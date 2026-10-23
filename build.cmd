@echo off
rem Local build helper: MSVC (Build Tools 2022) + Ninja (bundled with VS 2026)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars failed & exit /b 1 )
set "PATH=D:\app\Microsoft Visual Studio\2026\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
if not exist build\release\build.ninja (
  cmake --preset release || exit /b 1
)
cmake --build --preset release -- -k 0 || exit /b 1
echo [build] OK
