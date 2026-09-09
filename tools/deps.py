#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 lib/base/config.h 的开关拉取并编译第三方依赖,产物就位到树内。

依赖源码克隆到 deps/<name>(不入版本控制),编译产物按仓库既有约定摆放:
    头文件 -> lib/<name>/     (与 -Ilib、MSVC 的 ../lib 对齐,#include <openssl/ssl.h> 直接命中)
    静态库 -> bin/            (与 -Lbin、MSVC 的 ../bin/ 对齐)
所以构建文件(mk.sh / 四个 .vcxproj)不需要任何改动。

用法(项目根目录运行):
    python3 tools/deps.py              # 按 config.h 的开关决定做哪几个
    python3 tools/deps.py clean        # 删掉 deps/ 下的克隆与产物;要重编就先 clean
    python3 tools/deps.py m32          # 指定目标架构(mk.sh 风格);不传就按系统探测
    python3 tools/deps.py debug        # 出 Debug 依赖(默认 Release)

做哪几个依赖只由 config.h 的 WITH_* 决定,不能在命令行点名单个——产物名
(libssl.a / mimalloc.lib 等)不含架构也不含 debug/release,bin/ 只能存一套,
换架构或换 debug/release 都要先 clean 再整轮重来。

Windows 上 Debug 必须配 debug 依赖:mimalloc 是 Release(/MD)时会内嵌 MSVCRT,
链进 /MDd 的 Debug 程序就是 LNK4098 两套 CRT。OpenSSL 静态库带 /Zl(不写默认库名),
两种 CRT 都能链,给它 --debug 只是为了能跟进去调试。
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

ROOT = Path(__file__).resolve().parent.parent
CONFIG_H = ROOT / "lib" / "base" / "config.h"
DEPS_DIR = ROOT / "deps"
INC_DIR = ROOT / "lib"
LIB_DIR = ROOT / "bin"

# 每个依赖:仓库地址 + 钉住的版本 + 由 config.h 哪个开关决定要不要做。
# tag 缺省(或置 None)则不带 --branch,取远端默认分支
DEPS = {
    "openssl": {
        "url": "https://github.com/openssl/openssl.git",
        "tag": "openssl-3.5.5",
        "flag": "WITH_SSL",
    },
    "mimalloc": {
        "url": "https://github.com/microsoft/mimalloc.git",
        "tag": "v3.5.0",
        "flag": "WITH_MIMALLOC",
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


def ensure_clone(name: str, spec: dict) -> Path:
    """目录不存在则克隆;已存在只核对版本,不动它——用户可能在里面改过东西"""
    src = DEPS_DIR / name
    tag = spec.get("tag")
    if src.is_dir():
        desc = out(["git", "describe", "--tags", "--always"], cwd=src)
        if tag and desc and not desc.startswith(tag):
            print(f"  [警告] {name} 已存在但版本是 {desc},期望 {tag};按现状使用")
            print( "         要换版本请先 python3 tools/deps.py clean")
        else:
            print(f"  已有 {src.relative_to(ROOT)} ({desc or '未知版本'})")
        return src
    DEPS_DIR.mkdir(parents=True, exist_ok=True)
    cmd = ["git", "clone", "--depth", "1"]
    if tag:
        cmd += ["--branch", tag]
    cmd += [spec["url"], str(src)]
    run(cmd)
    return src


def copy_into(files: list, dest: Path, strip: str = "") -> list:
    """strip 非空时从文件名里去掉那一段——上游给 debug 构建加的后缀在这里抹平"""
    dest.mkdir(parents=True, exist_ok=True)
    done = []
    for f in files:
        name = f.name.replace(strip, "") if strip else f.name
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
    return [dst_inc] + copy_into(libs, LIB_DIR)


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
    # 而 srey 的 pragma 只认 mimalloc.lib;bin/ 一次只放一套,拷过去时统一成规范名
    return [dst_inc / "mimalloc.h"] + copy_into(libs, LIB_DIR, "-debug")


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
        "libs": ("libssl*.a", "libcrypto*.a", "*ssl*.lib", "*crypto*.lib"),
    },
    "mimalloc": {
        "inc": INC_DIR / "mimalloc",
        "anchor": INC_DIR / "mimalloc" / "mimalloc.h",
        "libs": ("libmimalloc*.a", "mimalloc*.lib"),
    },
}


# bin/ 一次只放一套依赖,把"这套是什么变体"记下来。不记的话:已有 Release 产物时
# 请求 debug 会被 already_done 判成"已完成"而跳过,静默拿 Release 顶包——
# Windows 上就是 LNK4098 两套 CRT,正是这套工具链最初要解决的问题
VARIANT_FILE = LIB_DIR / ".deps_variant"


def variant_str(arch: str, dbg: bool) -> str:
    return f"{arch} {'debug' if dbg else 'release'}"


def check_variant(want: str) -> None:
    """标记缺失时放行(旧产物或手工摆的),只在明确不一致时拦"""
    if not VARIANT_FILE.is_file():
        return
    have = VARIANT_FILE.read_text(encoding="utf-8", errors="replace").strip()
    if have and have != want:
        sys.stderr.write(f"bin/ 里已有的依赖是「{have}」,与本次请求的「{want}」不一致。\n"
                         f"  产物名不含架构与 debug/release,一次只能存一套;\n"
                         f"  先 python3 tools/deps.py clean 再重来\n")
        sys.exit(1)


def found_libs(name: str) -> list:
    got = []
    for pat in ARTIFACTS[name]["libs"]:
        got += sorted(LIB_DIR.glob(pat))
    return got


def already_done(name: str) -> bool:
    """头和库都在才算做过。只认头会在 bin/*.a 被清掉后误判成已完成"""
    return ARTIFACTS[name]["anchor"].exists() and 0 != len(found_libs(name))


def clean() -> int:
    for n in DEPS:
        src = DEPS_DIR / n
        if src.is_dir():
            print(f"删除 {src.relative_to(ROOT)}")
            rmtree(src)
        inc = ARTIFACTS[n]["inc"]
        if inc.is_dir():
            print(f"删除 {inc.relative_to(ROOT)}")
            rmtree(inc)
        for f in found_libs(n):
            print(f"删除 {f.relative_to(ROOT)}")
            f.unlink()
    if VARIANT_FILE.is_file():
        print(f"删除 {VARIANT_FILE.relative_to(ROOT)}")
        VARIANT_FILE.unlink()
    if DEPS_DIR.is_dir() and not any(DEPS_DIR.iterdir()):
        DEPS_DIR.rmdir()
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
    archs = [a for a in args if a in ARCH_SPECS]
    if 1 < len(archs):
        sys.stderr.write(f"架构参数只能给一个,收到: {', '.join(archs)}\n")
        return 1
    for a in args:
        if a not in ("clean", "debug") and a not in ARCH_SPECS:
            sys.stderr.write(f"未知参数 {a};可用: clean, debug, {', '.join(ARCH_SPECS)}\n")
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
    want = variant_str(arch, dbg)
    check_variant(want)
    print(f"目标架构: {arch}" + ("(指定)" if arch_forced else "(探测)")
          + f"  构建类型: {'Debug' if dbg else 'Release'}")

    report = []
    for name in todo:
        spec = DEPS[name]
        print(f"===== {name} ({spec.get('tag') or '默认分支'}) =====")
        if already_done(name):
            print("  产物已存在,跳过(要重编先 clean)")
            report.append((name, "已存在", [ARTIFACTS[name]["inc"]] + found_libs(name)))
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
    LIB_DIR.mkdir(parents=True, exist_ok=True)
    VARIANT_FILE.write_text(want + "\n", encoding="utf-8")
    print("\n提示: 头在 lib/ 下、库在 bin/ 下,mk.sh 与 .vcxproj 的搜索路径本来就覆盖这两处。")
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
