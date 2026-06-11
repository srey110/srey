#!/bin/sh
#***********************************************
# File Name  : mk
# Description: make file
# Make By    :
# Date Time  :2011/06/15
# Usage: sh mk.sh [target] [options...]
#
#   target:
#     (空)    编译 srey（默认）
#     all     编译 srey + test
#     test    编译 test
#     clean   清理构建产物
#
#   options:
#     m64     强制 64-bit（-m64），未设置则自动检测
#     m32     强制 32-bit（-m32），未设置则自动检测
#     debug   调试模式（-O0 -g3），默认不开启
#     asan    ASan/UBSan 检测（-fsanitize=address,undefined），默认不开启
#             macOS ARM64 须与 debug 同用，否则协程切栈时 ASan 可能产生误报
#     tsan    ThreadSanitizer 检测（-fsanitize=thread），与 asan 互斥
#***********************************************
LUA=0
EXTRALIB=""
WK="awk"
#平台
OSNAME=`uname`
if [ "$OSNAME" = "SunOS" ]
then
	WK="nawk"
fi
#读取config.h的配置
while read line
do
    val=`echo $line|$WK -F ' ' '{print $2}'`
    if [ "$val" = "WITH_LUA" ]
    then
        LUA=`echo $line|$WK -F ' ' '{print int($3)}'`
    fi
    if [ "$val" = "WITH_SSL" ]
    then
        if [ `echo $line|$WK -F ' ' '{print int($3)}'` -eq 1 ]
        then
            EXTRALIB="-lssl -lcrypto"
        fi
    fi
done < `pwd`/lib/base/config.h
# 共享库目录（参与 libsrey.a；srey 与 test 二进制共用）
SHARED_DIR="lib lib/base lib/utils lib/containers lib/crypt lib/event lib/serial lib/srey lib/thread lib/path"
SHARED_DIR=$SHARED_DIR" lib/protocol lib/protocol/mongo lib/protocol/mqtt lib/protocol/mysql lib/protocol/pgsql lib/protocol/smtp"
SHARED_DIR=$SHARED_DIR" lib/services"
if [ $LUA -eq 1 ]
then
    SHARED_DIR=$SHARED_DIR" lualib lualib/lua lualib/lbind lualib/lfs lualib/luacjson lualib/pb"
fi
# 应用专属目录（仅参与各自二进制链接，不进入 libsrey.a）
SREY_EXTRA_DIR="srey srey/cjson"
TEST_EXTRA_DIR="test"
# 解析 target（第一个参数）
BUILD_TARGET="srey"
case "$1" in
    all|test|clean) BUILD_TARGET="$1"; shift ;;
esac
# 解析 options（剩余参数）
ARCH_OPT=""
WITH_ASAN=0
WITH_TSAN=0
WITH_DEBUG=0
for a in "$@"
do
    case "$a" in
        m64|m32) ARCH_OPT=$a ;;
        debug)   WITH_DEBUG=1 ;;
        asan)    WITH_ASAN=1 ;;
        tsan)    WITH_TSAN=1 ;;
    esac
done
if [ $WITH_ASAN -eq 1 ] && [ $WITH_TSAN -eq 1 ]
then
    echo "Error: asan 与 tsan 不能同时启用"
    exit 1
fi
if [ "$OSNAME" = "Darwin" ] && [ "$ARCH_OPT" = "m32" ]
then
    echo "Error: macOS 不支持 m32（Apple 已移除 32-bit 支持）"
    exit 1
fi
#包含库
INCLUDELIB="-lpthread -lm"
if [ "$OSNAME" != "Darwin" ]
then
    INCLUDELIB=$INCLUDELIB" -lrt"
fi
if [ "$OSNAME" = "Linux" ]
then
    INCLUDELIB=$INCLUDELIB" -ldl"
fi
if [ "$OSNAME" = "SunOS" ]
then
	INCLUDELIB=$INCLUDELIB" -lsocket -lnsl"
fi
#结果存放路径
RSTPATH="bin"
#中间库文件名
LIBNAME="srey"
MAKEFILEPATH=`pwd`
LIBPATH="-L$MAKEFILEPATH/$RSTPATH"
if [ "$OSNAME" = "FreeBSD" ]
then
    CC="clang -std=gnu99"
    CXX="clang++"
else
    CC="gcc -std=gnu99"
    CXX="g++"
fi
# macOS ar 不支持 -D（确定性模式），Linux/SunOS 支持
if [ "$OSNAME" = "Darwin" ]
then
    ARCH="ar rcs"
else
    ARCH="ar rcsD"
fi
CFLAGS="-Wall -Wextra -Wshadow -Wstrict-prototypes -Wold-style-definition"
CFLAGS=$CFLAGS" -Wpointer-arith -Werror=implicit-function-declaration"
# 暴露 glibc GNU 扩展（epoll_create1 / F_GETPIPE_SZ 等），对非 glibc 平台无副作用
CFLAGS=$CFLAGS" -D_GNU_SOURCE"
# 决定优化级别和调试符号
if [ $WITH_DEBUG -eq 1 ]
then
    CFLAGS=$CFLAGS" -O0 -g3"
else
    CFLAGS=$CFLAGS" -O2"
    # LTO：GCC 需要 gcc-ar；Clang（macOS/Darwin）直接用 ar，
    # 链接时透明处理 bitcode。SunOS/AIX 不启用 LTO。
    if [ "$OSNAME" != "SunOS" ] && [ "$OSNAME" != "AIX" ]
    then
        if [ "$OSNAME" != "Darwin" ] && command -v gcc-ar >/dev/null 2>&1
        then
            CFLAGS=$CFLAGS" -flto=$(nproc 2>/dev/null || echo 4)"
            ARCH="gcc-ar rcsD"
        else
            CFLAGS=$CFLAGS" -flto"
            if [ "$OSNAME" = "FreeBSD" ] && command -v llvm-ar >/dev/null 2>&1
            then
                ARCH="llvm-ar rcsD"
            fi
        fi
    fi
    # _FORTIFY_SOURCE=2 需要 -O2 以上才生效
    CFLAGS=$CFLAGS" -D_FORTIFY_SOURCE=2"
fi
if [ $WITH_ASAN -eq 1 ]
then
    CFLAGS=$CFLAGS" -fsanitize=address,undefined -fno-omit-frame-pointer"
fi
if [ $WITH_TSAN -eq 1 ]
then
    CFLAGS=$CFLAGS" -fsanitize=thread -fno-omit-frame-pointer -D_MCO_USE_TSAN"
fi
if [ -n "$ARCH_OPT" ]
then
    if [ "$ARCH_OPT" = "m64" ]
    then
        CFLAGS=$CFLAGS" -m64"
    elif [ "$ARCH_OPT" = "m32" ]
    then
        CFLAGS=$CFLAGS" -m32"
    fi
else
    MACHINE_ARCH=`uname -m`
    if [ "$MACHINE_ARCH" = "x86_64" ] || [ "$MACHINE_ARCH" = "amd64" ]
    then
        CFLAGS=$CFLAGS" -m64"
    fi
fi
if [ "$OSNAME" = "Darwin" ]
then
    CFLAGS=$CFLAGS" -Wno-unused-function"
fi
if [ "$OSNAME" = "AIX" ]
then
    CFLAGS=$CFLAGS" -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast"
fi
# 根据构建模式确定额外目录列表（all 模式两套都要）
case "$BUILD_TARGET" in
    srey) EXTRA_DIRS_ALL="$SREY_EXTRA_DIR" ;;
    test) EXTRA_DIRS_ALL="$TEST_EXTRA_DIR" ;;
    all)  EXTRA_DIRS_ALL="$SREY_EXTRA_DIR $TEST_EXTRA_DIR" ;;
esac
# include 路径覆盖共享 + 全部参与的额外目录
INCLUDEPATH=""
for EachSub in $SHARED_DIR $EXTRA_DIRS_ALL
do
    INCLUDEPATH=$INCLUDEPATH" -I$MAKEFILEPATH/$EachSub"
done
INCLUDEPATH=`echo $INCLUDEPATH|$WK '{gsub(/^\s+|\s+$/, "");print}'`
#先 clean（清理共享目录 + 所有额外目录 + 顶层 *.o + 产物）
rm -rf *.o
for EachSub in $SHARED_DIR $SREY_EXTRA_DIR $TEST_EXTRA_DIR
do
    rm -rf $MAKEFILEPATH/$EachSub/*.o
done
cd $RSTPATH
rm -rf lib$LIBNAME.a srey test
cd $MAKEFILEPATH
if [ "$BUILD_TARGET" = "clean" ]
then
    exit 0
fi
# 编译单个目录里的 .c/.cpp（跳过 _skip_main 指定的文件，常用于排除 main.c）
CompileDir()
{
    _dir=$1
    _skip_main=$2
    cd $_dir
    if [ "$?" != "0" ]; then exit 1; fi
    for EachFile in `ls *.cpp 2>/dev/null`
    do
        if [ "$EachFile" != "$_skip_main" ]; then
            echo "$CXX $CFLAGS -c $EachFile"
            $CXX $CFLAGS -c $EachFile $INCLUDEPATH || { echo ""------------------Error"------------------"; exit 1; }
        fi
    done
    for EachFile in `ls *.c 2>/dev/null`
    do
        if [ "$EachFile" != "$_skip_main" ]; then
            echo "$CC $CFLAGS -c $EachFile"
            $CC $CFLAGS -c $EachFile $INCLUDEPATH || { echo ""------------------Error"------------------"; exit 1; }
        fi
    done
    cd $MAKEFILEPATH
}
# 构建共享 libsrey.a：编译 SHARED_DIR 下所有 .c/.cpp，归档到 bin/libsrey.a
BuildSharedLib()
{
    echo "====================== Build shared lib$LIBNAME.a ======================"
    for EachSub in $SHARED_DIR
    do
        echo ---------------------$EachSub---------------------------
        CompileDir $EachSub ""
        for EachFile in `ls $MAKEFILEPATH/$EachSub/*.o 2>/dev/null`
        do
            mv $EachFile $MAKEFILEPATH/ >/dev/null
        done
    done
    cd $MAKEFILEPATH
    OBJFILE=`ls *.o 2>/dev/null`
    echo "$ARCH lib$LIBNAME.a `echo $OBJFILE`"
    $ARCH lib$LIBNAME.a `echo $OBJFILE` || exit 1
    rm -rf *.o
    mv lib$LIBNAME.a $RSTPATH
}
# 构建一个二进制：参数 1=主目录, 2=主文件名, 3=产物名, 4=额外目录列表
BuildBinary()
{
    _maindir=$1
    _mainfile=$2
    _proname=$3
    _extra_dirs=$4
    echo "====================== Build $_proname ======================"
    # 1) 编译额外目录中非 main 的 .c/.cpp → .o
    EXTRA_OBJS=""
    for EachSub in $_extra_dirs
    do
        echo ---------------------$EachSub---------------------------
        CompileDir $EachSub "$_mainfile"
        for EachFile in `ls $MAKEFILEPATH/$EachSub/*.o 2>/dev/null`
        do
            EXTRA_OBJS=$EXTRA_OBJS" "$EachFile
        done
    done
    # 2) 编译主文件并链接
    cd $MAKEFILEPATH/$_maindir || exit 1
    echo ---------------------Make $_proname---------------------------
    echo "$CC $CFLAGS $_mainfile $EXTRA_OBJS $INCLUDEPATH $LIBPATH $INCLUDELIB -l$LIBNAME -o $_proname $EXTRALIB"
    $CC $CFLAGS $_mainfile $EXTRA_OBJS $INCLUDEPATH $LIBPATH $INCLUDELIB -l$LIBNAME -o $_proname $EXTRALIB \
        || { echo "---------Error---------"; exit 1; }
    mv $_proname $MAKEFILEPATH/$RSTPATH
    cd $MAKEFILEPATH
    # 3) 清理额外目录的 .o，避免污染下一轮 BuildBinary
    for EachSub in $_extra_dirs
    do
        rm -rf $MAKEFILEPATH/$EachSub/*.o
    done
}

BuildSharedLib
case "$BUILD_TARGET" in
    srey)
        BuildBinary "srey" "main.c" "srey" "$SREY_EXTRA_DIR"
        ;;
    test)
        BuildBinary "test" "main.c" "test" "$TEST_EXTRA_DIR"
        ;;
    all)
        BuildBinary "srey" "main.c" "srey" "$SREY_EXTRA_DIR"
        BuildBinary "test" "main.c" "test" "$TEST_EXTRA_DIR"
        ;;
esac
exit 0
