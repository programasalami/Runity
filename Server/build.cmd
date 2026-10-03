@echo off
rem Builds and tests the game server on Windows with the Visual Studio toolchain.
rem   build.cmd            configure + build + test (debug)
rem   build.cmd release    the same for release
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=debug

set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VCPKG_ROOT set VCPKG_ROOT=%VSDIR%\VC\vcpkg
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
python ..\Protocol\generator\gen.py --check || (echo Protocol code is stale: run python Protocol\generator\gen.py & exit /b 1)
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
ctest --preset %PRESET% || exit /b 1
