@echo off
rem SKSE DLL for SE 1.5.97, AE 1.6.x and 1.7.x.
rem Output: build\windows\x64\releasedbg\ImUsingThisNow.dll
rem "build.bat test" makes a self-test build in build-test\ instead (never ship it).
rem Needs VS 2022 Build Tools (MSVC 14.44) and xmake: on PATH, or in a
rem shared\tools folder two levels up; CommonLibSSE-NG is the
rem lib\commonlibsse-ng submodule.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -vcvars_ver=14.44 >nul
set TOOLS=%~dp0..\..\shared\tools
if exist "%TOOLS%\xmake" (
    set "PATH=%TOOLS%\xmake;%PATH%"
    set "XMAKE_GLOBALDIR=%TOOLS%\xmake-cache"
)
rem Strip this machine's folders from file names compiled into the DLL
rem (__FILE__ / source_location). Passed through cl's CL variable because
rem xmake can't keep a flag with a space in it as one argument.
for %%I in ("%~dp0..\..") do set CL=/d1trimfile:"%%~fI\\"
cd /d "%~dp0"
rem VR is off until its openvr headers are fetched and someone can test VR.
set SELFTEST=n
set OUT=build
if /i "%~1"=="test" (set SELFTEST=y& set OUT=build-test)
xmake f -c -p windows -a x64 -m releasedbg --skyrim_vr=n --selftest=%SELFTEST% -o %OUT% -y || exit /b 1
xmake -y || exit /b 1
