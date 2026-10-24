@echo off
rem Local build helper: finds the newest Visual Studio (or Build Tools) through vswhere, sets up the
rem MSVC x64 environment, then builds the "release" preset (Ninja). No machine-specific paths.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [build] vswhere.exe not found: install Visual Studio with "Desktop development with C++".
  exit /b 1
)

set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [build] no Visual Studio installation with the C++ tools was found.
  exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

rem CMake and Ninja ship with Visual Studio: prefer them over whatever else is in PATH
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"

if not exist build\release\build.ninja (
  cmake --preset release || exit /b 1
)
cmake --build --preset release -- -k 0 || exit /b 1
echo [build] OK: build\release\ReSaverPP.exe and resaverpp-cli.exe
