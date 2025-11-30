#!/bin/sh
#***********************************************
# File Name  : mk
# Description: make file
# Make By    :
# Date Time  :2011/06/15 
# UsAge      :mk.sh  mk.sh x86/x64 mk.sh test mk.sh x86/x64 test mk.sh clean
#***********************************************
#config.h
LUA=0
SSLLIB=""
WK="awk"
OSNAME=`uname`
if [ "$OSNAME" = "SunOS" ]
then
	WK="nawk"
fi
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
            SSLLIB="-lssl -lcrypto"
        fi
    fi
done < `pwd`/lib/base/config.h
#程序的名称 文件夹
MAINDIR="srey"
PROGRAMNAME="srey"
MAINFILE="main.c"
Dir="lib lib/base lib/utils lib/containers lib/crypt lib/event lib/protocol lib/protocol/mongo lib/protocol/mysql lib/protocol/mqtt lib/srey lib/thread"
if [ $LUA -eq 1 ]
then
    Dir=$Dir" lualib lualib/lua lualib/lfs lualib/luacjson lualib/pb"
fi
if [ "$1" = "test" ] || [ "$2" = "test" ]
then
    MAINDIR="test"
    PROGRAMNAME="test"
    Dir=$Dir" test"
else
    Dir=$Dir" srey srey/cjson srey/tasks"
    if [ $LUA -eq 1 ]
    then
        Dir=$Dir" srey/lbind"
    fi
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
#不参与编译的文件,MAINFILE+其他
EXCEPTL=$MAINFILE" "$TESTFILE
MAKEFILEPATH=`pwd`
LIBPATH="-L$MAKEFILEPATH/$RSTPATH"
CC="gcc -std=gnu99"
GCC="g++"
ARCH="ar -rv"
INCLUDEPATH=""
OBJFILE=""
CFLAGS="-Wall -O2"
if [ "$1" = "x64" ] || [ "$1" = "x86" ]
then
    if [ "$1" = "x64" ]
    then
        CFLAGS=$CFLAGS" -m64"
    fi
else
    OS_Version=`uname -m`
    if [ "$OS_Version" = "x86_64" ] || [ "$OS_Version" = "amd64" ]
    then
        CFLAGS=$CFLAGS" -m64"
    fi
fi
if [ "$OSNAME" == "Darwin" ]
then
    CFLAGS=$CFLAGS" -Wno-unused-function"
fi
if [ "$OSNAME" == "AIX" ]
then
    CFLAGS=$CFLAGS" -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast"
fi
LIBDIR=$Dir
#先clean
rm -rf *.o
for EachSub in $LIBDIR
do
	rm -rf $MAKEFILEPATH/$EachSub/*.o
done
cd $RSTPATH    
rm -rf lib$LIBNAME.a
rm -rf $PROGRAMNAME
cd $MAKEFILEPATH
#clean
if [ "$1" = "clean" ]
then
    exit 0
fi
GetIncludePath()
{
    for EachSub in $LIBDIR
    do
        INCLUDEPATH=$INCLUDEPATH" -I$MAKEFILEPATH/$EachSub"
    done
    INCLUDEPATH=`echo $INCLUDEPATH|$WK '{gsub(/^\s+|\s+$/, "");print}'`
    echo ---------------------Dir---------------------------
    echo $LIBDIR
    echo ---------------------Include Dir---------------------------
    echo $INCLUDEPATH
}
IsExcePTL()
{
    for EachExec in $EXCEPTL
    do
        if [ "$EachExec" = "$1" ]
        then
            return 1
        fi
    done
    return 0
}
Make()
{
    for EachSub in $LIBDIR
    do
        echo ---------------------$EachSub--------------------------- 
        cd $EachSub
        if [ "$?" != "0" ]
        then
            exit 1
        fi
		SourceFile=`ls *.cpp 2>/dev/null`
        for EachFile in $SourceFile
        do
            IsExcePTL $EachFile
            if [ "$?" = "0" ]
            then
                echo "$GCC $CFLAGS -c $EachFile"
                $GCC $CFLAGS -c $EachFile $INCLUDEPATH
                if [ "$?" != "0" ]
                then
                    echo "---------------------Error---------------------"
                    exit 1
                fi
            fi
        done
        SourceFile=`ls *.c 2>/dev/null`
        for EachFile in $SourceFile
        do
            IsExcePTL $EachFile
            if [ "$?" = "0" ]
            then
                echo "$CC $CFLAGS -c $EachFile"
                $CC $CFLAGS -c $EachFile $INCLUDEPATH
                if [ "$?" != "0" ]
                then
                    echo "---------------------Error---------------------"
                    exit 1
                fi
            fi
        done
	RstFile=`ls *.o 2>/dev/null`
	for EachFile in $RstFile
        do
            if [ -f "$MAKEFILEPATH/$EachFile" ]
            then
                rm -rf $MAKEFILEPATH/$EachFile
            fi
            mv $EachFile $MAKEFILEPATH>/dev/null
        done
        cd $MAKEFILEPATH
    done
    echo ---------------------Make .a file--------------------------- 
    cd $MAKEFILEPATH
    OBJFILE=`ls *.o 2>/dev/null`
    echo "$ARCH lib$LIBNAME.a `echo $OBJFILE`"
    $ARCH lib$LIBNAME.a `echo $OBJFILE`
    rm -rf *.o
    mv lib$LIBNAME.a $RSTPATH
}

GetIncludePath
Make
mkmaindir=$MAKEFILEPATH/$MAINDIR
mkmaincpp=$MAINFILE
proname=$PROGRAMNAME
cd $mkmaindir
echo ---------------------Make program file---------------------------
echo "$CC $CFLAGS $mkmaincpp $INCLUDEPATH $LIBPATH $INCLUDELIB -l$LIBNAME -o $proname $SSLLIB"
$CC $CFLAGS $mkmaincpp $INCLUDEPATH $LIBPATH $INCLUDELIB -l$LIBNAME -o $proname $SSLLIB
if [ "$?" != "0" ]
then
    echo "---------------------Error---------------------"
    exit 1
fi
mv $proname $MAKEFILEPATH/$RSTPATH
cd $MAKEFILEPATH
exit 0
