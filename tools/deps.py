#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 lib/base/config.h 的开关拉取并编译第三方依赖,产物就位到树内。

版本策略:deps/<name>/ 在就用它,不在才去拉远端最新的正式版。所以想固定版本就自己把
源码放进 deps/<name>/(不必是 git 克隆),想升级就删掉该目录再跑一次。

依赖源码克隆到 deps/<name>(不入版本控制),编译产物按仓库既有约定摆放:
    头文件 -> lib/<name>/     (与 -Ilib、MSVC 的 ../lib 对齐,#include <openssl/ssl.h> 直接命中)
    静态库 -> bin/            (与 -Lbin、MSVC 的 ../bin/ 对齐)
所以构建文件(mk.sh / 四个 .vcxproj)不需要任何改动。

用法(项目根目录运行):
    python3 tools/deps.py              # 按 config.h 的开关决定做哪几个
    python3 tools/deps.py clean        # 清构建残留与产物,保留 deps/ 下的源码
    python3 tools/deps.py m32          # 指定目标架构(m32/m64/arm64,也认 x86/x64/aarch64)
    python3 tools/deps.py debug        # 出 Debug 依赖(默认 Release)

做哪几个依赖只由 config.h 的 WITH_* 决定,不能在命令行点名单个。

库名带变体后缀「<名字>[d]_<x86|x64|arm64>」,如 libssl_x64.a / libssld_x86.lib /
libmimalloc_arm64.a。各变体在 bin/ 里互不覆盖,一个平台每种变体各编一次就够,
换架构或换 debug/release 不用先 clean。链接侧按同一规则拼名字:POSIX 由 mk.sh 拼 -l,
Windows 由 os.h 的 DEPS_LIB_SUFFIX 拼进两个 main.c 的 #pragma comment(lib)。
头不带后缀,各变体共用一份(装的是公开头,最后一次构建装上去的那份)。

Windows ARM64 的 OpenSSL 一律降到 /O1(见文件前部的 WIN_ARM64_OSSL_CFLAGS):
MSVC 在 /O2 下会把它编坏,TLS 握手必崩,与 OpenSSL 版本无关。

Windows 上 Debug 必须配 debug 依赖:mimalloc 是 Release(/MD)时会内嵌 MSVCRT,
链进 /MDd 的 Debug 程序就是 LNK4098 两套 CRT。后缀把这条从"靠约定"变成"名字对不上直接链不到"。
OpenSSL 静态库带 /Zl(不写默认库名),两种 CRT 都能链,给它 --debug 只是为了能跟进去调试。
"""
from __future__ import annotations

import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# Windows 控制台按 cp936/cp1252 编码,print 中文会抛 UnicodeEncodeError 把脚本打断;
# errors="replace" 保证终端不支持时只显示成问号,不中断
for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
CONFIG_H = ROOT / "lib" / "base" / "config.h"
DEPS_DIR = ROOT / "deps"
INC_DIR = ROOT / "lib"
LIB_DIR = ROOT / "bin"
# Windows ARM64 编 openssl 时额外塞给 Configure 的编译选项。
# MSVC 在 /O2 下会把 openssl 编坏:TLS 握手解析扩展时把小整数当指针解引用,进程直接
# 0xC0000005。3.5.5/3.5.8/4.0.2 都一样,换版本躲不掉,关汇编也没用;降到 /O1 即可,
# 仍是优化构建,实测握手与全套测试都过。置空则不加,完全按上游默认走
WIN_ARM64_OSSL_CFLAGS = "/O1"

# 每个依赖:仓库地址 + 由 config.h 哪个开关决定要不要做 + 版本 tag 的形状 + 清理规则。
# 不钉版本:deps/<name>/ 不在就按 tag_re 挑远端最新的正式版拉,在就原样用。
# 要固定版本,自己把源码放进 deps/<name>/ 即可,脚本不会去动它
DEPS = {
    "openssl": {
        "url": "https://github.com/openssl/openssl.git",
        "flag": "WITH_SSL",
        "tag_re": r"^openssl-(\d+)\.(\d+)\.(\d+)$",# 形如 openssl-3.5.8
        # 产物散在源码树里,先让上游 distclean 清,再按模式扫它漏下的
        "clean_make": ("distclean",),
        "clean_globs": ("configdata.pm", "makefile", "Makefile",
                        "**/*.obj", "**/*.o", "**/*.lib", "**/*.a", "**/*.pdb"),
    },
    "mimalloc": {
        "url": "https://github.com/microsoft/mimalloc.git",
        "flag": "WITH_MIMALLOC",
        "tag_re": r"^v(\d+)\.(\d+)\.(\d+)$",# 形如 v3.5.0
        # cmake 的 out-of-source:产物全在 build/ 下,删掉就彻底,用不上上游的 clean 目标
        "clean_dirs": ("build",),
    },
}

IS_WIN = os.name == "nt"
# 目标架构:mk.sh 风格的位置参数(m32/m64/arm64),不传则探测。
# 只有 openssl 用得上:它的 Windows 构建没有 ./config 自动探测,必须显式给 target。
# mimalloc 走 cmake,由所在 shell 的编译器决定目标,不传架构
ARCH_SPECS = {
    "m32": {"vc": "VC-WIN32", "cflag": "-m32"},
    "m64": {"vc": "VC-WIN64A", "cflag": "-m64"},
    "arm64": {"vc": "VC-WIN64-ARM", "cflag": None},
}
# 库名后缀里用的架构词。与 ARCH_SPECS 的键分开:那边是 mk 脚本风格的入参(m32/m64),
# 这边是文件名上看得懂的架构名(x86/x64),两套不混用
SFX_ARCH = {"m32": "x86", "m64": "x64", "arm64": "arm64"}
# 探测用:环境变量/CPU 名 -> ARCH_SPECS 的键。键一律小写,查表前先 lower()
# ——VS2015 的 vcvarsall 设的是 Platform=X64(大写),大小写敏感会漏
ARCH_ALIAS = {
    "x64": "m64", "amd64": "m64", "x86_64": "m64", "em64t": "m64",
    "arm64": "arm64", "aarch64": "arm64",
    "x86": "m32", "win32": "m32", "i386": "m32", "i686": "m32",
}
# 缺工具时打印的安装指引,一行一个平台
TOOL_HINTS = {
    "perl": {
        "win": "https://strawberryperl.com/ 或 choco install strawberryperl",
        "freebsd": "pkg install perl5",
        "linux": "dnf install perl 或 apt install perl",
        "darwin": "系统自带 /usr/bin/perl,若缺失用 brew install perl",
    },
    "nasm": {
        "win": "https://www.nasm.us/ 或 choco install nasm(装完加进 PATH)",
    },
    "cl": {
        "win": "在「x64 Native Tools Command Prompt for VS」里运行本脚本",
    },
    "perl-mods": {
        "win": "Strawberry Perl 自带这些模块;若用其他 perl 发行请补装",
        "freebsd": "pkg install perl5(base 系统的 perl 不完整)",
        "linux": "dnf install perl-core 或 apt install perl-modules",
        "darwin": "系统 perl 自带;若用 brew perl 请补装",
    },
    "cmake": {
        "win": "https://cmake.org/download/ 或 choco install cmake",
        "freebsd": "pkg install cmake",
        "linux": "dnf install cmake 或 apt install cmake",
        "darwin": "brew install cmake",
    },
    "git": {
        "win": "https://git-scm.com/download/win",
        "freebsd": "pkg install git",
        "linux": "dnf install git 或 apt install git",
        "darwin": "xcode-select --install",
    },
}


def detect_arch() -> str:
    """必须跟当前 cl.exe 的目标一致,而不是跟 CPU 一致——VS 开发者提示符会设 VSCMD_ARG_TGT_ARCH。
    退回 platform.machine() 只是兜底:它给的是本进程架构,32 位 python 跑在 x64 上会误报 x86"""
    for v in (os.environ.get("VSCMD_ARG_TGT_ARCH"), os.environ.get("Platform"),
              platform.machine()):
        if v and v.strip().lower() in ARCH_ALIAS:
            return ARCH_ALIAS[v.strip().lower()]
    return "m64"


def plat_key() -> str:
    if IS_WIN:
        return "win"
    s = platform.system().lower()
    if "freebsd" in s or "bsd" in s:
        return "freebsd"
    if s == "darwin":
        return "darwin"
    return "linux"


def _rm_readonly(func, path, _exc):
    """Windows 上 git 的 pack 文件带只读位,unlink 会 WinError 5;去掉再来一次"""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def rmtree(path: Path) -> None:
    # onexc 是 3.12 起的名字,onerror 在 3.12 被弃用;两边都要能跑
    if (3, 12) <= sys.version_info:
        shutil.rmtree(str(path), onexc=_rm_readonly)
    else:
        shutil.rmtree(str(path), onerror=_rm_readonly)


def run(cmd, cwd=None, env=None):
    printable = cmd if isinstance(cmd, str) else " ".join(str(c) for c in cmd)
    print(f"  $ {printable}")
    rc = subprocess.call(cmd, cwd=str(cwd) if cwd else None, env=env,
                         shell=isinstance(cmd, str))
    if 0 != rc:
        raise RuntimeError(f"命令失败(rc={rc}): {printable}")


def run_soft(cmd, cwd=None) -> int:
    """给清理用:上游 clean 目标失败不该中断整轮,后面还有模式兜底"""
    printable = " ".join(str(c) for c in cmd)
    print(f"  $ {printable}")
    rc = subprocess.call(cmd, cwd=str(cwd) if cwd else None)
    if 0 != rc:
        print(f"  [提示] {printable} 返回 {rc},改由模式清理接手")
    return rc


def out(cmd, cwd=None) -> str:
    try:
        b = subprocess.check_output(cmd, cwd=str(cwd) if cwd else None,
                                    stderr=subprocess.DEVNULL)
        return b.decode("utf-8", "replace").strip()
    except Exception:
        return ""


def need_tools(names: list) -> None:
    """缺一个就把该平台的安装指引全打出来再退出,不让它在半路上以别的错误崩掉"""
    key = plat_key()
    missing = [n for n in names if shutil.which(n) is None]
    if not missing:
        return
    sys.stderr.write(f"缺少工具: {', '.join(missing)}\n")
    for n in missing:
        hint = TOOL_HINTS.get(n, {}).get(key)
        if hint:
            sys.stderr.write(f"  {n}: {hint}\n")
    sys.exit(1)


# OpenSSL 的 Configure/Makefile.in 直接 use 这几个。RHEL 系把它们拆成独立包,
# perl 二进制在也未必有;Text::Template 不列——OpenSSL 自带 fallback
OPENSSL_PERL_MODS = ("FindBin", "IPC::Cmd", "Time::Piece")


def need_perl_mods(mods) -> None:
    missing = []
    for m in mods:
        if 0 != subprocess.call(["perl", f"-M{m}", "-e1"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL):
            missing.append(m)
    if not missing:
        return
    sys.stderr.write(f"perl 缺模块: {', '.join(missing)}\n")
    hint = TOOL_HINTS["perl-mods"].get(plat_key())
    if hint:
        sys.stderr.write(f"  {hint}\n")
    sys.exit(1)


# mimalloc 在 MSVC 上一律把 .c 当 C++ 编(它 CMakeLists 写死 MI_USE_CXX=ON,为了用
# C++ atomics),所以 C++ 的一致性决定能不能编过;VS2015 过不去(init.c 的 operator != 歧义)。
# 门槛取它自己 CMakeLists 里唯一的 MSVC 判断 1914;上游只自带并测试 VS2022 的工程
MSVC_MIN_FOR_MIMALLOC = 1914  # VS2017 15.7


def msvc_ver() -> int:
    """cl.exe 的版本 x100(19.00 -> 1900);探不到返 0 表示别拦,让编译器自己报"""
    try:
        pr = subprocess.run(["cl"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        txt = pr.stdout.decode("utf-8", "replace")
    except Exception:
        return 0
    m = re.search(r"Version (\d+)\.(\d+)", txt)
    if not m:
        return 0
    return int(m.group(1)) * 100 + int(m.group(2))


def make_cmd() -> str:
    """FreeBSD 的 bmake 跑 OpenSSL 生成的 Makefile 不稳,有 gmake 优先用"""
    if IS_WIN:
        return "nmake"
    if shutil.which("gmake"):
        return "gmake"
    return "make"


def njobs() -> int:
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except AttributeError:
        return max(1, os.cpu_count() or 1)


def read_flags() -> dict:
    """读 config.h 的 #define WITH_XXX N。与 mk.sh 同一份真相,不另设开关"""
    flags = {}
    if not CONFIG_H.is_file():
        sys.stderr.write(f"找不到 {CONFIG_H}\n")
        sys.exit(1)
    for line in CONFIG_H.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        parts = line.split()
        if 3 <= len(parts) and "#define" == parts[0] and parts[1].startswith("WITH_"):
            try:
                flags[parts[1]] = int(parts[2], 0)
            except ValueError:
                continue
    return flags


def latest_tag(spec: dict) -> str:
    """问远端要最新的正式版 tag。按 tag_re 过滤,alpha/beta/rc 这类预发布天然不匹配。
    版本号按数字逐段比,避免 3.5.10 被字符串序排到 3.5.9 前面"""
    txt = out(["git", "ls-remote", "--tags", "--refs", spec["url"]])
    if not txt:
        return ""
    pat = re.compile(spec["tag_re"])
    best_ver, best_tag = None, ""
    for line in txt.splitlines():
        ref = line.rsplit("/", 1)[-1].strip()
        m = pat.match(ref)
        if not m:
            continue
        ver = tuple(int(x) for x in m.groups())
        if best_ver is None or ver > best_ver:
            best_ver, best_tag = ver, ref
    return best_tag


def ensure_clone(name: str, spec: dict) -> Path:
    """目录在就原样用,不在才去拉远端最新的正式版。
    这条就是版本策略:想固定版本把源码放进 deps/<name>/,想升级先 clean 再跑"""
    src = DEPS_DIR / name
    if src.is_dir():
        desc = out(["git", "describe", "--tags", "--always"], cwd=src)
        print(f"  已有 {src.relative_to(ROOT)} ({desc or '非 git 源码'}),按现状使用")
        print( "         要换版本:删掉该目录再跑,会拉远端最新正式版")
        return src
    DEPS_DIR.mkdir(parents=True, exist_ok=True)
    tag = latest_tag(spec)
    cmd = ["git", "clone", "--depth", "1"]
    if tag:
        print(f"  远端最新正式版: {tag}")
        cmd += ["--branch", tag]
    else:
        print(f"  [警告] 没问到 {name} 的版本 tag,退回远端默认分支")
    cmd += [spec["url"], str(src)]
    run(cmd)
    return src


def lib_sfx(arch: str, dbg: bool) -> str:
    """库名后缀:debug 的 d 贴在名字后面,架构跟在下划线后,如 d_x64 / _arm64"""
    return ("d" if dbg else "") + "_" + SFX_ARCH[arch]


def copy_into(files: list, dest: Path, sfx: str, strip: str = "") -> list:
    """按 <名字><sfx>.<扩展名> 落地。strip 非空时先去掉上游给 debug 加的那段,
    免得 mimalloc-debug 变成 mimalloc-debugd_x64"""
    dest.mkdir(parents=True, exist_ok=True)
    done = []
    for f in files:
        name = f.name.replace(strip, "") if strip else f.name
        stem, dot, ext = name.rpartition(".")
        name = (stem + sfx + dot + ext) if dot else (name + sfx)
        shutil.copy2(str(f), str(dest / name))
        done.append(dest / name)
    return done


def build_openssl(src: Path, arch: str, forced: bool, dbg: bool) -> list:
    """no-shared 静态构建 + install_dev(只装头与库,跳过极慢的文档)"""
    prefix = src / "_prefix"
    opts = [f"--prefix={prefix}", f"--openssldir={prefix / 'ssl'}",
            "no-shared", "no-tests", "no-docs"]
    if dbg:
        opts.append("--debug")
    if IS_WIN:
        # 理由见 WIN_ARM64_OSSL_CFLAGS
        if "arm64" == arch and WIN_ARM64_OSSL_CFLAGS:
            opts.append(WIN_ARM64_OSSL_CFLAGS)
        run(["perl", "Configure", ARCH_SPECS[arch]["vc"]] + opts, cwd=src)
    else:
        # 不指定时让 ./config 自己探测;显式指定才附加 -m32/-m64,口径同 mk.sh 的 CFLAGS
        cflag = ARCH_SPECS[arch]["cflag"] if forced else None
        run(["./config"] + opts + ([cflag] if cflag else []), cwd=src)
    # config 出错也可能返回 0(实测缺 perl 模块时如此),故另验它该产出的文件。
    # 只查 configdata.pm:makefile 在 Windows 上是小写,两平台同名的只有这个
    if not (src / "configdata.pm").is_file():
        raise RuntimeError("config 没产出 configdata.pm,真实原因见上面的输出(常见:perl 模块不全)")
    mk = make_cmd()
    if IS_WIN:
        run([mk], cwd=src)
        run([mk, "install_dev"], cwd=src)
    else:
        run([mk, f"-j{njobs()}"], cwd=src)
        run([mk, "install_dev"], cwd=src)

    inc = prefix / "include" / "openssl"
    if not inc.is_dir():
        raise RuntimeError(f"install_dev 没产出头文件目录: {inc}")
    dst_inc = INC_DIR / "openssl"
    if dst_inc.is_dir():
        rmtree(dst_inc)
    shutil.copytree(str(inc), str(dst_inc))
    # 库名各平台不同(Windows 上带 _x64 之类后缀),不猜,把 prefix 里实际产出的捞出来
    libs = []
    for pat in ("*crypto*.a", "*ssl*.a", "*crypto*.lib", "*ssl*.lib"):
        for d in ("lib", "lib64"):
            libs += sorted((prefix / d).glob(pat)) if (prefix / d).is_dir() else []
    if not libs:
        raise RuntimeError(f"在 {prefix} 下找不到 crypto/ssl 静态库")
    return [dst_inc] + copy_into(libs, LIB_DIR, lib_sfx(arch, dbg))


def build_mimalloc(src: Path, arch: str, forced: bool, dbg: bool) -> list:
    """只要静态库,且必须 MI_OVERRIDE=OFF:srey 在 memory.c 显式调 mi_*,不劫持 libc malloc。
    arch/forced 不用:mimalloc 的目标由所在 shell 的编译器决定"""
    build = src / "build"
    if IS_WIN:
        ver = msvc_ver()
        if 0 != ver and ver < MSVC_MIN_FOR_MIMALLOC:
            raise RuntimeError(
                f"当前 cl {ver // 100}.{ver % 100:02d} 太老,mimalloc 编不过"
                f"(上游只自带 VS2022 的工程);Windows 上请把 lib/base/config.h 的 "
                f"WITH_MIMALLOC 改回 0(openssl 不受影响)")
    # 不传 -A:那是 VS 生成器专用的,Ninja(Windows 上 cmake 常见的默认)会直接报错。
    # 目标架构由所在 shell 的 cl.exe 决定,与 srey 自身的构建同一来源
    cfgtype = "Debug" if dbg else "Release"
    run(["cmake", "-S", ".", "-B", "build", f"-DCMAKE_BUILD_TYPE={cfgtype}",
         "-DMI_OVERRIDE=OFF", "-DMI_BUILD_SHARED=OFF",
         "-DMI_BUILD_OBJECT=OFF", "-DMI_BUILD_TESTS=OFF"], cwd=src)
    # -j 必须带数字:FreeBSD 的 bmake 不接受裸 -j
    run(["cmake", "--build", "build", "--config", cfgtype, "-j", str(njobs())], cwd=src)

    hdr = src / "include" / "mimalloc.h"
    if not hdr.is_file():
        raise RuntimeError(f"找不到 {hdr}")
    # 只拷这一个公共头:它仅 include <stddef.h>/<stdbool.h>,不依赖 include/mimalloc/ 内部头
    dst_inc = INC_DIR / "mimalloc"
    dst_inc.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(hdr), str(dst_inc / "mimalloc.h"))
    libs = []
    for pat in ("libmimalloc*.a", "mimalloc*.lib"):
        libs += sorted(build.glob(pat))
        libs += sorted(build.glob(f"{cfgtype}/{pat}"))
    if not libs:
        raise RuntimeError(f"在 {build} 下找不到 mimalloc 静态库")
    # Debug 时上游把库名改成 mimalloc-debug(见其 CMakeLists 的 mi_libname),
    # 先抹掉它再按本项目的规则贴 d_<arch>,免得出来 mimalloc-debugd_x64
    return [dst_inc / "mimalloc.h"] + copy_into(libs, LIB_DIR, lib_sfx(arch, dbg), "-debug")


BUILDERS = {"openssl": build_openssl, "mimalloc": build_mimalloc}
# 各依赖构建所需的工具,在克隆之前查——缺工具时不该已经拉完几百 MB 源码才报错
BUILD_TOOLS = {
    "openssl": ["perl", "nasm", "cl"] if IS_WIN else ["perl", "cc"],
    "mimalloc": ["cmake"],
}
# 每个依赖的产物特征:头文件锚点 + bin/ 下库名的通配。跳过判定与 clean 共用这一份
ARTIFACTS = {
    "openssl": {
        "inc": INC_DIR / "openssl",
        "anchor": INC_DIR / "openssl" / "ssl.h",
        "libs": ("libssl{sfx}.a", "libcrypto{sfx}.a", "*ssl{sfx}.lib", "*crypto{sfx}.lib"),
    },
    "mimalloc": {
        "inc": INC_DIR / "mimalloc",
        "anchor": INC_DIR / "mimalloc" / "mimalloc.h",
        "libs": ("libmimalloc{sfx}.a", "mimalloc{sfx}.lib"),
    },
}


# 旧版本在 bin/ 下记过"这套依赖是什么变体",库名带后缀之后各变体可以共存,这个标记没用了。
# 见到就顺手删掉(clean 里做),不再读也不再写
STALE_VARIANT_FILE = LIB_DIR / ".deps_variant"


def found_libs(name: str, sfx: str = "*") -> list:
    """sfx 给具体后缀就是查某个变体,默认 * 是全部变体(clean 用)"""
    got = []
    for pat in ARTIFACTS[name]["libs"]:
        got += sorted(LIB_DIR.glob(pat.format(sfx=sfx)))
    return got


def already_done(name: str, sfx: str) -> bool:
    """头和库都在才算做过。只认头会在 bin/*.a 被清掉后误判成已完成。
    查的是本次这个变体的库名,所以先编 release 再编 debug 不会被判成已完成"""
    return ARTIFACTS[name]["anchor"].exists() and 0 != len(found_libs(name, sfx))


def clean_src(name: str, spec: dict, src: Path) -> None:
    """清源码树里的构建残留,保留源码本身(自己下载的源码没有 .git,所以不能靠 git clean)。
    先跑上游的 clean 目标,再按模式扫一遍它漏下的——openssl 的 distclean 实测会留下
    providers/legacy.lib 这类孤立产物,而残留的 .obj 会让下次构建走增量、编出混合版本的库"""
    has_mk = any((src / n).is_file() for n in ("makefile", "Makefile"))
    for tgt in spec.get("clean_make", ()):
        # makefile 是 configure 生成的,没有它说明还没构建过,自然没有残留
        if has_mk:
            run_soft([make_cmd(), tgt], cwd=src)
    for d in spec.get("clean_dirs", ()):
        path = src / d
        if path.is_dir():
            print(f"  删除 {path.relative_to(ROOT)}")
            rmtree(path)
    left = 0
    for pat in spec.get("clean_globs", ()):
        for f in src.glob(pat):
            if f.is_file():
                f.unlink()
                left += 1
    if 0 != left:
        print(f"  另清掉 {left} 个上游 clean 漏下的产物")


def clean() -> int:
    for n, spec in DEPS.items():
        src = DEPS_DIR / n
        if src.is_dir():
            print(f"清理 {src.relative_to(ROOT)} 的构建残留(保留源码)")
            clean_src(n, spec, src)
        inc = ARTIFACTS[n]["inc"]
        if inc.is_dir():
            print(f"删除 {inc.relative_to(ROOT)}")
            rmtree(inc)
        for f in found_libs(n):
            print(f"删除 {f.relative_to(ROOT)}")
            f.unlink()
    if STALE_VARIANT_FILE.is_file():
        print(f"删除 {STALE_VARIANT_FILE.relative_to(ROOT)}(旧版本留下的变体标记,已废弃)")
        STALE_VARIANT_FILE.unlink()
    return 0


def usage() -> int:
    sys.stdout.write(__doc__ or "")
    return 0


def main() -> int:
    # 参数一律裸词,同 mk.sh:clean / debug / 架构(m32/m64/arm64),顺序无关
    args = sys.argv[1:]
    if "-h" in args or "--help" in args or "help" in args:
        return usage()
    do_clean = "clean" in args
    dbg = "debug" in args
    # 架构既认 mk 脚本的 m32/m64/arm64,也认 x86/x64/aarch64 这些同义写法,省得记两套词
    archs = [ARCH_ALIAS.get(a.lower(), a) for a in args if a in ARCH_SPECS or a.lower() in ARCH_ALIAS]
    if 1 < len(set(archs)):
        sys.stderr.write(f"架构参数只能给一个,收到: {', '.join(archs)}\n")
        return 1
    for a in args:
        if a not in ("clean", "debug") and a not in ARCH_SPECS and a.lower() not in ARCH_ALIAS:
            sys.stderr.write(f"未知参数 {a};可用: clean, debug, "
                             f"{', '.join(ARCH_SPECS)}(或 {', '.join(sorted(set(ARCH_ALIAS)))})\n")
            return 1
    arch_forced = 0 != len(archs)
    arch = archs[0] if arch_forced else detect_arch()

    if do_clean:
        return clean()

    need_tools(["git"])
    flags = read_flags()
    todo = [n for n, sp in DEPS.items() if 0 != flags.get(sp["flag"], 0)]
    for n, sp in DEPS.items():
        if 0 == flags.get(sp["flag"], 0):
            print(f"跳过 {n}: config.h 里 {sp['flag']} = 0")
    if not todo:
        print("没有需要处理的依赖。改 lib/base/config.h 的 WITH_* 开关。")
        return 0
    sfx = lib_sfx(arch, dbg)
    print(f"目标架构: {arch}" + ("(指定)" if arch_forced else "(探测)")
          + f"  构建类型: {'Debug' if dbg else 'Release'}"
          + f"  库名后缀: {sfx}")

    report = []
    for name in todo:
        spec = DEPS[name]
        # 版本取自 deps/<name>/ 的实际源码:不钉版本之后,"要拉哪个"已不是定值,
        # 报出来的必须是这次真正在用的那份
        src_dir = DEPS_DIR / name
        if src_dir.is_dir():
            ver = out(["git", "describe", "--tags", "--always"], cwd=src_dir) or "非 git 源码"
        else:
            ver = "待拉取"
        print(f"===== {name} ({ver}) =====")
        if already_done(name, sfx):
            print(f"  该变体产物已存在,跳过(要重编先 clean)")
            report.append((name, "已存在", [ARTIFACTS[name]["inc"]] + found_libs(name, sfx)))
            continue
        need_tools(BUILD_TOOLS[name])
        if "openssl" == name:
            need_perl_mods(OPENSSL_PERL_MODS)
        src = ensure_clone(name, spec)
        desc = out(["git", "describe", "--tags", "--always"], cwd=src) or "未知版本"
        produced = BUILDERS[name](src, arch, arch_forced, dbg)
        report.append((name, desc, produced))

    print("\n===== 依赖清单 =====")
    for name, ver, produced in report:
        print(f"{name:10s} {ver}")
        for p in produced:
            try:
                print(f"           -> {p.relative_to(ROOT)}")
            except ValueError:
                print(f"           -> {p}")
    print("\n提示: 头在 lib/ 下、库在 bin/ 下,mk.sh 与 .vcxproj 的搜索路径本来就覆盖这两处。")
    print(f"      本次库名后缀是 {sfx},换架构或换 debug/release 直接再跑一次,不会互相覆盖。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:
        sys.stderr.write(f"\n失败: {e}\n")
        sys.exit(1)
    except KeyboardInterrupt:
        sys.stderr.write("\n已中断\n")
        sys.exit(130)
