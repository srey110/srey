#!/bin/sh
# popen2 测试辅助脚本
# 无参数：仅退出，用于测试无管道模式
# 参数 r ：输出固定字符串，用于测试只读模式
# 参数 rw：从 stdin 读一行并回显，用于测试读写模式
case "$1" in
    r)
        printf 'hello popen\n'
        ;;
    rw)
        read -r line
        printf '%s\n' "$line"
        ;;
    *)
        ;;
esac
exit 0
