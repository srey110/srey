#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查并补全 .c / .h 文件的 UTF-8 BOM。

srey 约定所有 .c/.h 文件以 UTF-8 BOM(EF BB BF)开头。第三方库(如 lualib/lua)
升级替换源码后 BOM 会丢失,用本脚本重新补全即可。

用法:
    python3 add_bom.py                 # 扫当前目录,补全所有缺 BOM 的 .c/.h
    python3 add_bom.py lib lualib      # 只扫指定目录(或文件)
    python3 add_bom.py --check         # 只检查不修改,有缺失则退出码 1(CI 用)
    python3 add_bom.py --check lualib  # 只检查指定目录

默认跳过 .git / build* / .vs / x64 / Debug / Release / obj 等非源码目录,
以及 tools/deps.py 拉下或装进来的第三方树(deps/、lib/openssl、lib/mimalloc)。
"""
import os
import sys

BOM = b"\xef\xbb\xbf"
# 跳过的目录:版本控制 / 构建产物 / IDE 缓存
SKIP_EXACT = {".git", ".vs", "x64", "Win32", "Debug", "Release", "obj", "ipch"}
# deps.py 管的第三方树,按仓库里的位置跳(lib/ 下其余目录是项目源码)
_ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
SKIP_TREES = {os.path.join(_ROOT, *p) for p in (("deps",), ("lib", "openssl"), ("lib", "mimalloc"))}


def _skip_dir(dirpath, name):
    return (name in SKIP_EXACT or name.startswith("build")
            or os.path.realpath(os.path.join(dirpath, name)) in SKIP_TREES)


def iter_sources(roots):
    # roots 重叠时(如 `add_bom.py lib/base/bytes.c lib`)同一个文件会被产出多次,
    # 补 BOM 那步无条件写,补两次就是双 BOM,编译直接挂。按真实路径去重
    seen = set()

    def _emit(path):
        real = os.path.realpath(path)
        if real in seen:
            return False
        seen.add(real)
        return True

    for root in roots:
        if os.path.isfile(root):
            if root.endswith((".c", ".h")) and _emit(root):
                yield root
            continue
        for dirpath, dirnames, names in os.walk(root):
            # 原地裁剪,os.walk 不再进入被跳过的目录
            dirnames[:] = [d for d in dirnames if not _skip_dir(dirpath, d)]
            for n in names:
                if n.endswith((".c", ".h")):
                    p = os.path.join(dirpath, n)
                    if _emit(p):
                        yield p


def has_bom(path):
    with open(path, "rb") as f:
        return f.read(3) == BOM


def add_bom(path):
    with open(path, "rb") as f:
        data = f.read()
    with open(path, "wb") as f:
        f.write(BOM + data)


def main():
    argv = sys.argv[1:]
    check_only = "--check" in argv
    roots = [a for a in argv if not a.startswith("-")] or ["."]

    total = 0
    missing = []
    for path in iter_sources(roots):
        total += 1
        if not has_bom(path):
            missing.append(path)

    if check_only:
        for p in missing:
            print("缺 BOM:", p)
        print("扫描 %d 个 .c/.h, 缺 BOM %d 个" % (total, len(missing)))
        sys.exit(1 if missing else 0)

    for p in missing:
        add_bom(p)
        print("已补 BOM:", p)
    print("扫描 %d 个 .c/.h, 本次补 %d 个, 其余已有" % (total, len(missing)))


if __name__ == "__main__":
    main()
