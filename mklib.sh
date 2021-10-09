#!/bin/sh
#***********************************************
# File Name  : mklib
# Description: 编译所需库
# Make By    :
# Date Time  :2011/06/15 
#***********************************************

UsAge="UsAge:\"./mklib.sh\""

OSName=`uname`
X64=""
OS_Version=`uname -m`
if [ "$OS_Version" = "x86_64" ]
then
	X64="x64"
	echo "x86_64"
fi

LibPath=`pwd`
LibPath=$LibPath/
RstPath="bin/x86"
echo current path $LibPath
if [ "$X64" = "x64" ]
then
    RstPath="bin/x64"
fi

DepLib=depend

#编译---------------------------包信息---------------------------
cppunit_tar=cppunit-1.13.2.tar.gz
cppunit_path=cppunit-1.13.2


cd $LibPath$DepLib
rm -rf $cppunit_path

if [ "$OSName" = "SunOS" ]
then
    gzcat $cppunit_tar | tar xvf -
else
    tar -xvzf $cppunit_tar
fi

cd $LibPath$DepLib/$cppunit_path
if [ "$X64" = "x64" ]
then
    ./configure --enable-shared=no --enable-static=yes CFLAGS=-m64
else
    ./configure --enable-shared=no --enable-static=yes
fi
make

mv $LibPath$DepLib/$cppunit_path/src/cppunit/.libs/libcppunit.a $LibPath$RstPath/libcppunit.a
