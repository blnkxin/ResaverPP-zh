@echo off
rem One-click build launcher for players. This file is deliberately ASCII-only: cmd.exe reads .cmd files
rem with the console code page, so Chinese text here turns into broken commands on some systems.
rem All the text and the work live in build-player.ps1 (UTF-8 with BOM, read correctly by PowerShell).
setlocal
cd /d "%~dp0"
rem Launch Windows PowerShell with its own module path: when this is started from a PowerShell 7
rem terminal, the inherited PSModulePath points at 7's modules and 5.1 then cannot find its own
rem cmdlets (Get-FileHash and friends).
set "PSModulePath=%SystemRoot%\system32\WindowsPowerShell\v1.0\Modules"
set "PS1=%~dp0build-player.ps1"
if not exist "%PS1%" (
    echo [error] build-player.ps1 is missing next to this file.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
if errorlevel 1 (
    echo.
    echo [error] the build did not finish - see the messages above.
    pause
    exit /b 1
)
exit /b 0
