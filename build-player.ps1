# ReSaver++ 简体中文版：一键编译（Windows PowerShell 5.1+）
#
# 由同目录的「一键编译.cmd」调用；也可以右键本文件 →「使用 PowerShell 运行」。
# 做的事：找 Visual Studio 的 C++ 工具链（没有就调 winget 装 Build Tools）→ 编译 → 把两个 exe
# 复制到 out\ 并打印 SHA256。所有编译逻辑都在仓库自己的 build.cmd 里，本文件只管环境和体验。

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $root

function Step($text) { Write-Host "  $text" -ForegroundColor Cyan }
function Ok($text) { Write-Host "  $text" -ForegroundColor Green }
function Note($text) { Write-Host "    $text" -ForegroundColor DarkGray }

function Get-Sha256([string]$path) {
    # .NET instead of Get-FileHash: that cmdlet lives in a module, and Windows PowerShell 5.1 can
    # fail to autoload its modules when it was started from a PowerShell 7 environment.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Find-VS {
    $vswhere = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $vswhere) { return $null }
    $dir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $dir) { return $null }
    return ($dir | Select-Object -First 1)
}

function Show-ManualInstall {
    Write-Host ""
    Write-Host "  [!] 没法自动安装编译器，请手动安装：" -ForegroundColor Yellow
    Note "下载：https://aka.ms/vs/17/release/vs_buildtools.exe"
    Note "安装时勾选「使用 C++ 的桌面开发」，装完重新运行本脚本。"
}

function End-Fail {
    Write-Host ""
    Write-Host "  编译没有完成。常见原因：" -ForegroundColor Yellow
    Note "1. 没有点「是」授予管理员权限（安装编译器需要）；"
    Note "2. 网络不稳，Visual Studio 安装包下载失败——重跑本脚本即可（会接着装）；"
    Note "3. 磁盘空间不足（约 5 GB）；"
    Note "4. 路径里有特殊字符时，试着把整个文件夹移到纯英文路径再编译。"
    Write-Host ""
    if (-not $env:RESAVERPP_NO_PAUSE) { Read-Host "  按回车键退出" | Out-Null }
    exit 1
}

Write-Host ""
Write-Host "    ReSaver++ 简体中文版：一键编译" -ForegroundColor White
Write-Host "    ------------------------------------------------"
Write-Host "    产物：本目录 out\ReSaverPP.exe 和 out\resaverpp-cli.exe"
Write-Host "    首次运行：需要联网安装 Visual Studio Build Tools（约 2-3 GB，10-30 分钟）"
Write-Host "    已经装过编译器：每次编译约 2-5 分钟"
Write-Host "    需要：Windows 10/11 x64、约 5 GB 可用磁盘空间"
Write-Host ""

# ---- 1. 编译器 ------------------------------------------------------------------------------
$vs = Find-VS
if (-not $vs) {
    Step "[1/3] 没找到 Visual Studio 的 C++ 工具链，准备用 winget 安装 Build Tools..."
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Show-ManualInstall
        End-Fail
    }
    & winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
        --accept-package-agreements --accept-source-agreements `
        --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "  [!] 安装没有成功（可能被取消，或者没有管理员权限）。" -ForegroundColor Yellow
        Show-ManualInstall
        End-Fail
    }
    $vs = Find-VS
    if (-not $vs) {
        Write-Host ""
        Write-Host "  [!] 安装完成了，但找不到编译器：请重启电脑后再运行本脚本。" -ForegroundColor Yellow
        End-Fail
    }
}
Note "编译器：$vs"

# ---- 2. 编译 --------------------------------------------------------------------------------
# rc.exe（编译 exe 的版本信息）只认 ANSI 路径：源码放在含中文的目录里会直接失败。
# 这种情况改到 %PUBLIC%\ReSaverPP-build（纯英文）里编，编完把 exe 拷回来。
$buildRoot = $root
$tempBuild = $false
if ($root -match '[^\x00-\x7F]') {
    $asciiBase = $env:PUBLIC
    if (-not $asciiBase -or $asciiBase -match '[^\x00-\x7F]') { $asciiBase = 'C:\ProgramData' }
    $buildRoot = Join-Path $asciiBase 'ReSaverPP-build'
    $tempBuild = $true
    Step "[2/3] 当前路径含中文，rc.exe 编不过：改到 $buildRoot 里编译（源码会同步一份过去）"
} else {
    Step "[2/3] 编译中，请稍等（首次 2-5 分钟）..."
}

if ($tempBuild) {
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $root $buildRoot /MIR /XD "$root\build" "$root\out" "$root\.git" /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) {
            Write-Host "  [!] 没法把源码复制到 $buildRoot（robocopy 退出码 $LASTEXITCODE）" -ForegroundColor Yellow
            End-Fail
        }
    } else {
        Copy-Item -LiteralPath (Join-Path $root '*') -Destination $buildRoot -Recurse -Force
    }
}

& cmd /c "`"$(Join-Path $buildRoot 'build.cmd')`""
if ($LASTEXITCODE -ne 0) { End-Fail }

# ---- 3. 收集产物与哈希 ----------------------------------------------------------------------
$out = Join-Path $root 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$names = @('ReSaverPP.exe', 'resaverpp-cli.exe')
foreach ($name in $names) {
    $src = Join-Path $buildRoot "build\release\$name"
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Host "  [!] 编译结束了，但没找到 $src" -ForegroundColor Yellow
        End-Fail
    }
    Copy-Item -LiteralPath $src -Destination $out -Force
}

Ok "[3/3] 完成。产物在：$out"
Write-Host ""
foreach ($name in $names) {
    $p = Join-Path $out $name
    $size = (Get-Item -LiteralPath $p).Length
    $hash = Get-Sha256 $p
    Write-Host ("     {0,-18} {1,12:N0} 字节" -f $name, $size)
    Write-Host ("     {0,-18} SHA256 {1}" -f '', $hash)
}
Write-Host ""
Write-Host "  说明：" -ForegroundColor White
Note "- 双击 out\ReSaverPP.exe 就能用；这是你自己编的，通常不会触发 SmartScreen。"
Note "- 想重新编译：再运行本脚本（只编译改动过的文件，很快）。"
if ($tempBuild) { Note "- 编译缓存放在 $buildRoot，不需要时可以整个删掉。" }
Note "- 编译失败时，把上面的报错整段复制给作者或贴到 issue 里。"
Write-Host ""

if (-not $env:RESAVERPP_NO_PAUSE) {
    Read-Host "  按回车键打开 out 文件夹" | Out-Null
    Start-Process explorer.exe $out
}
exit 0
