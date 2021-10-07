#!/bin/sh
#***********************************************
# File Name  : mk
# Description: make file
# Make By    :
# Date Time  :2011/06/15 
#***********************************************

UsAge="UsAge:\"./mk.sh\" or \"./mk.sh test\" or \"./mk.sh clean\""

#生成程序的名称
PROGRAMNAME="srey"
PROTESTNAME="test"
#文件夹
Dir="lib lib/sha1 lib/md5"
#main函数所在文件夹
MAINDIR="srey"
TESTDIR="test"
#main函数所在文件
MAINFILE="srey.cpp"
TESTFILE="test.cpp"

#附加包含库
INCLUDELIB="-lrt -lpthread"
if [ "$1" = "test" ]
then
    Dir=$Dir" depend/cppunit-1.13.2/include "$TESTDIR
	INCLUDELIB=$INCLUDELIB" -lcppunit"
else
    Dir=$Dir" "$MAINDIR
fi
#系统位数
OSNAME=`uname`
X64=""
OS_Version=`uname -m`
echo $OSNAME $OS_Version
if [ "$OS_Version" = "x86_64" ]
then
	X64="x64"
	echo "x86_64"
fi
#结果存放路径
RSTPATH="bin/x86"
if [ "$X64" = "x64" ]
then
    RSTPATH="bin/x64"
fi

#中间库文件名
LIBNAME="srey"
#不参与编译的文件,MAINFILE+其他
EXCEPTL=$MAINFILE" "$TESTFILE

MAKEFILEPATH=`pwd`
LIBPATH="-L$MAKEFILEPATH/$RSTPATH"
CC="gcc"
GCC="g++"
ARCH="ar -rv"
INCLUDEPATH=""
OBJFILE=""
CFLAGS=""
if [ "$1" = "test" ]
then
    CFLAGS=$CFLAGS" -O0 -g -Wall"
else
    CFLAGS=$CFLAGS" -O3 -g -Wall"
fi
if [ "$X64" = "x64" ]
then
    CFLAGS=$CFLAGS" -m64"
fi

LIBDIR=$Dir

GetIncludePath()
{
    for EachSub in $LIBDIR
    do
        INCLUDEPATH=$INCLUDEPATH" -I$MAKEFILEPATH/$EachSub"
    done
    
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
		
		ExtFlags=""
		SourceFile=`ls *.cpp 2>/dev/null`
        for EachFile in $SourceFile
        do
            IsExcePTL $EachFile
            if [ "$?" = "0" ]
            then
                echo "$GCC $CFLAGS $ExtFlags -c $EachFile"
                $GCC $CFLAGS $ExtFlags -c $EachFile $INCLUDEPATH
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
                echo "$CC $CFLAGS $ExtFlags -c $EachFile"
                $CC $CFLAGS $ExtFlags -c $EachFile $INCLUDEPATH
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

Clean()
{
    rm -rf *.o
    for EachSub in $LIBDIR
    do
	    rm -rf $MAKEFILEPATH/$EachSub/*.o
    done

    cd $RSTPATH    
    echo "start rm lib$LIBNAME.a"
    rm -rf lib$LIBNAME.a

    echo "start rm $PROGRAMNAME"
    rm -rf $PROGRAMNAME

	echo "start rm $PROTESTNAME"
	rm -rf $PROTESTNAME
    cd $MAKEFILEPATH
}

while [ 1 = 1 ]
do
    if [ $# -eq 1 ]
    then
        if [ "$1" = "clean" ]
        then
            Clean
            exit 0
        fi
		if [ "$1" = "test" ]
        then
            break
        fi
        
        echo "$UsAge"
        exit 1
    elif [ $# -gt 1 ]
    then
        echo "$UsAge"
        exit 1
    else
        break
    fi
done

GetIncludePath
Make

if [ "$1" = "test" ]
then
    mkmaindir=$MAKEFILEPATH/$TESTDIR
	mkmaincpp=$TESTFILE
	proname=$PROTESTNAME
else
    mkmaindir=$MAKEFILEPATH/$MAINDIR
	mkmaincpp=$MAINFILE
	proname=$PROGRAMNAME
fi

cd $mkmaindir

echo ---------------------Make program file---------------------------
echo "Include lib is:$INCLUDELIB"
echo "$GCC $CFLAGS -o $proname $mkmaincpp $LIBPATH $LIBAPP -l$LIBNAME $INCLUDELIB"
$GCC $CFLAGS -o $proname $mkmaincpp $LIBPATH $LIBAPP -l$LIBNAME $INCLUDELIB $INCLUDEPATH
if [ "$?" != "0" ]
then
    echo "---------------------Error---------------------"
    exit 1
fi

mv $proname $MAKEFILEPATH/$RSTPATH
cd $MAKEFILEPATH

exit 0
