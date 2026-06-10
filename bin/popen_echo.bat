@echo off
rem popen2 测试辅助脚本
rem 无参数：仅退出，用于测试无管道模式
rem 参数 r ：输出固定字符串，用于测试只读模式
rem 参数 rw：从 stdin 读一行并回显，用于测试读写模式
if "%1"=="r"  goto do_r
if "%1"=="rw" goto do_rw
exit /b 0

:do_r
echo hello popen
exit /b 0

:do_rw
set /p line=
echo %line%
exit /b 0
