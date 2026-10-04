@echo off
rem Builds and tests the game server on Windows with the Visual Studio toolchain.
rem   build.cmd            configure + build + test (debug)
rem   build.cmd release    the same for release
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=debug

rem Any Visual Studio 2026 edition with the C++ tools (Community, Build Tools, ...), found by vswhere.
set VSDIR=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio C++ tools not found: install Visual Studio 2026 Build Tools with "Desktop development with C++" & exit /b 1)
if not defined VCPKG_ROOT set VCPKG_ROOT=%VSDIR%\VC\vcpkg
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
python ..\Protocol\generator\gen.py --check || (echo Protocol code is stale: run python Protocol\generator\gen.py & exit /b 1)
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
ctest --preset %PRESET% || exit /b 1
