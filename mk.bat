@echo off
rem ***********************************************
rem   Windows 命令行构建（不需要 IDE），参数与 mk.sh 对齐。
rem   须在 VS 开发者命令提示符里运行（要 msbuild 在 PATH 上）。
rem
rem   用法: mk.bat [target] [options]
rem
rem   target:
rem     (空)    编译 srey（默认）
rem     all     编译 srey + test
rem     test    编译 test
rem     clean   清理构建产物
rem
rem   options:
rem     m64     64-bit（默认）
rem     m32     32-bit
rem     debug   Debug 配置，默认 Release
rem
rem   第三方依赖(openssl/mimalloc)由 tools\deps.py 准备，配置必须跟这里一致:
rem   编 debug 就要先 python3 tools\deps.py clean 再 deps.py debug，
rem   否则 mimalloc 的 /MD 撞上程序的 /MDd，链接期 LNK4098 两套 CRT。
rem ***********************************************
setlocal enabledelayedexpansion

set "TARGET=srey"
set "CONFIG=Release"
set "PLATFORM=x64"
set "MSTARGET=srey"
rem 记录 config/platform 是否被显式指定:clean 不指定时清全部四组,与 mk.sh 的"无条件清干净"对齐
set "CFGSET="
set "PLATSET="

rem 逐个吃参数。set / shift / goto 一律不放进 ( ) 块:
rem 块内的 %~1 在进块前就展开了,shift 对同块内后续引用不生效
:parse
if "%~1"=="" goto parsed
set "OK="
if /i "%~1"=="all"   set "TARGET=all"   & set "MSTARGET=srey;test" & set "OK=1"
if /i "%~1"=="test"  set "TARGET=test"  & set "MSTARGET=test"      & set "OK=1"
if /i "%~1"=="clean" set "TARGET=clean"                            & set "OK=1"
if /i "%~1"=="debug" set "CONFIG=Debug"  & set "CFGSET=1"           & set "OK=1"
if /i "%~1"=="m32"   set "PLATFORM=x86"  & set "PLATSET=1"          & set "OK=1"
if /i "%~1"=="m64"   set "PLATFORM=x64"  & set "PLATSET=1"          & set "OK=1"
if not defined OK echo 未知参数 %~1；target: all, test, clean；options: m32, m64, debug& exit /b 1
shift
goto parse
:parsed

where msbuild >nul 2>&1
if errorlevel 1 echo 找不到 msbuild，请在「x64 Native Tools Command Prompt for VS」里运行本脚本& exit /b 1

rem 读 config.h 的开关打印出来,与 mk.sh / deps.py 同一份真相
for /f "tokens=2,3" %%a in ('findstr /r /c:"^#define WITH_" lib\base\config.h') do echo   %%a = %%b

if /i "%TARGET%"=="clean" goto doclean

rem 依赖变体核对:bin\ 一次只放一套,Debug 程序(/MDd)链 Release 的 mimalloc(/MD)
rem 就是 LNK4098 两套 CRT。标记由 tools\deps.py 写,没有就不拦(可能是手工摆的)
set "WANTARCH=m64"
if /i "%PLATFORM%"=="x86" set "WANTARCH=m32"
set "WANTVAR=%WANTARCH% release"
set "DEPSARG=%WANTARCH%"
if /i "%CONFIG%"=="Debug" set "WANTVAR=%WANTARCH% debug"
if /i "%CONFIG%"=="Debug" set "DEPSARG=%WANTARCH% debug"
if exist bin\.deps_variant (
    set /p HAVEVAR=<bin\.deps_variant
    if /i not "!HAVEVAR!"=="!WANTVAR!" (
        echo   [警告] bin\ 里的依赖是「!HAVEVAR!」,本次要编的是「!WANTVAR!」
        echo          python3 tools\deps.py clean
        echo          python3 tools\deps.py !DEPSARG!
    )
)

echo ====================== Build %MSTARGET% %CONFIG%^|%PLATFORM% ======================
msbuild srey.sln /t:%MSTARGET% /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /v:normal /nologo > mk_build.log 2>&1
set "RC=!errorlevel!"
type mk_build.log

rem 告警不是硬错误,退出码 0 也可能有告警,必须数一遍(见 CLAUDE.md)
set "NWARN=0"
for /f %%n in ('findstr /r /c:" warning [A-Z][A-Z]*[0-9][0-9]*:" mk_build.log ^| find /c /v ""') do set "NWARN=%%n"
echo ------------------------------------------------------
echo   退出码: !RC!    告警: !NWARN!    完整日志: mk_build.log
if not "!NWARN!"=="0" echo   有告警,按项目约定要清干净
exit /b !RC!

:doclean
rem 没点名 debug/m32 就把四组全清掉:msbuild 的 /t:Clean 只作用于指定的那一组,
rem 而 mk.sh clean 是无条件删产物,不对齐的话"清过了"是假的
if defined CFGSET goto cleanone
if defined PLATSET goto cleanone
for %%c in (Debug Release) do for %%p in (x64 x86) do (
    echo ====================== Clean %%c %%p ======================
    msbuild srey.sln /t:Clean /p:Configuration=%%c /p:Platform=%%p /v:minimal /nologo
)
echo   四组配置均已清理。第三方依赖不在此列,用 python3 tools\deps.py clean
exit /b 0

:cleanone
echo ====================== Clean %CONFIG% %PLATFORM% ======================
msbuild srey.sln /t:Clean /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /v:minimal /nologo
exit /b !errorlevel!
