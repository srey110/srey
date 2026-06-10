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

默认跳过 .git / build* / .vs / x64 / Debug / Release / obj 等非源码目录。
"""
import os
import sys

BOM = b"\xef\xbb\xbf"
# 跳过的目录:版本控制 / 构建产物 / IDE 缓存
SKIP_EXACT = {".git", ".vs", "x64", "Win32", "Debug", "Release", "obj", "ipch"}


def _skip_dir(name):
    return name in SKIP_EXACT or name.startswith("build")


def iter_sources(roots):
    for root in roots:
        if os.path.isfile(root):
            if root.endswith((".c", ".h")):
                yield root
            continue
        for dirpath, dirnames, names in os.walk(root):
            # 原地裁剪,os.walk 不再进入被跳过的目录
            dirnames[:] = [d for d in dirnames if not _skip_dir(d)]
            for n in names:
                if n.endswith((".c", ".h")):
                    yield os.path.join(dirpath, n)


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
