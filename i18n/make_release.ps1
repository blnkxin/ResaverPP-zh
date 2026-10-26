# 打本地发布包（Nexus / 网盘发的那个）：两个 exe + 许可/说明 + 汉化说明 + 版本与哈希 → 7z + sha256
#
#   pwsh -File i18n/make_release.ps1 -Version 22
#   pwsh -File i18n/make_release.ps1 -Version 22 -ExeDir build\vs\Release -Note "GitHub Actions"
#
# 需要先编好 ReSaverPP 和 resaverpp-cli，并写好 i18n\汉化说明.md（或旧版的 build\Nexus-*\汉化说明.md）。
# 产出：build\release\ReSaverPP-zh-CN-v<版本>.7z 与同名 .sha256，以及 build\release\ReSaverPP-zh-CN-v<版本>\ 目录。
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ExeDir = "build\vs\Release",
    [string]$BuildMethod = "",
    [string]$Note = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot                      # 仓库根目录
$release = Join-Path $root "build\release"
$stage = Join-Path $release "ReSaverPP-zh-CN-v$Version"
New-Item -ItemType Directory -Force $stage | Out-Null

# ---- 两个 exe ----------------------------------------------------------------------------------
foreach ($exe in @("ReSaverPP.exe", "resaverpp-cli.exe")) {
    $from = Join-Path (Join-Path $root $ExeDir) $exe
    if (-not (Test-Path $from)) { throw "找不到 $from，先编译 ReSaverPP 与 resaverpp-cli" }
    Copy-Item $from $stage -Force
}

# ---- 文档 --------------------------------------------------------------------------------------
foreach ($doc in @("LICENSE", "NOTICE.md", "THIRD-PARTY-NOTICES.md", "README.zh-CN.md")) {
    Copy-Item (Join-Path $root $doc) $stage -Force
}
$explain = Join-Path $root "i18n\汉化说明.md"                  # 常驻副本；没有就用打包目录里已有的
if (Test-Path $explain) {
    Copy-Item $explain (Join-Path $stage "汉化说明.md") -Force
} elseif (-not (Test-Path (Join-Path $stage "汉化说明.md"))) {
    Write-Warning "没找到 i18n\汉化说明.md，发布包里会缺少用户向说明"
}

# ---- 工具链（写进 版本与哈希.txt） --------------------------------------------------------------
if (-not $BuildMethod) {
    $cache = Join-Path $root "$ExeDir\..\CMakeCache.txt"
    $gen = if (Test-Path $cache) {
        (Select-String -Path $cache -Pattern "^CMAKE_GENERATOR:INTERNAL=(.+)$").Matches.Groups[1].Value
    } else { "" }
    $BuildMethod = if ($gen) { "MSVC（$gen）Release，静态链接运行库（/MT）" } else { "MSVC Release，静态链接运行库（/MT）" }
}

# ---- 版本与哈希.txt ----------------------------------------------------------------------------
$gui = Get-FileHash (Join-Path $stage "ReSaverPP.exe") -Algorithm SHA256
$cli = Get-FileHash (Join-Path $stage "resaverpp-cli.exe") -Algorithm SHA256
$guiLen = (Get-Item (Join-Path $stage "ReSaverPP.exe")).Length
$cliLen = (Get-Item (Join-Path $stage "resaverpp-cli.exe")).Length
$built = (Get-Date).ToString("yyyy-MM-dd")

$lines = @(
    "ReSaver++ 简体中文版 v$Version",
    "========================================",
    "",
    "对应上游：Tonycubed2/ResaverPP v$Version",
    "构建日期：$built",
    "构建方式：$BuildMethod",
    "汉化分支：https://github.com/blnkxin/ResaverPP-zh",
    "",
    "文件                    大小（字节）    SHA256",
    ("ReSaverPP.exe            {0}        {1}" -f $guiLen, $gui.Hash),
    ("resaverpp-cli.exe        {0}        {1}" -f $cliLen, $cli.Hash),
    "",
    "校验方法（任选其一）：",
    "  certutil -hashfile ReSaverPP.exe SHA256",
    "  PowerShell:  Get-FileHash .\ReSaverPP.exe -Algorithm SHA256",
    "",
    "说明：",
    "- 两个 exe 都没有数字签名，首次运行会经过 Windows SmartScreen / 杀软的信誉检查，属预期行为；",
    "  详见「汉化说明.md」里的一节。",
    "- 发布包不包含 ReSaverHelper.dll（上游的可选 SKSE 插件）。没装它时「实时游戏」与移植页的",
    "  「快照 / 移植进游戏」按钮是灰的，其它功能完全不受影响；装了之后游戏里的提示也是中文",
    "  （需要中文汉化包自带的中文字体）。",
    "- 每次重新编译都会得到不同的哈希，这是正常的。"
)
if ($Note) { $lines += @("", "备注：$Note") }
$lines -join "`r`n" | Set-Content -LiteralPath (Join-Path $stage "版本与哈希.txt") -Encoding utf8

# ---- 7z（进到目录里压，否则包里会多一层目录名） -------------------------------------------------
$seven = Join-Path $env:ProgramFiles "7-Zip\7z.exe"
if (-not (Test-Path $seven)) { $seven = (Get-Command 7z -ErrorAction Stop).Source }
$archive = Join-Path $release "ReSaverPP-zh-CN-v$Version.7z"
if (Test-Path $archive) { Remove-Item $archive -Force }
Push-Location $stage
try {
    & $seven a -t7z -mx=9 -m0=lzma2 $archive * | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z 打包失败（exit $LASTEXITCODE）" }
} finally {
    Pop-Location
}
$zipHash = (Get-FileHash $archive -Algorithm SHA256).Hash
"7z SHA256: $zipHash" | Set-Content "$archive.sha256" -Encoding utf8

Get-ChildItem $stage | Select-Object Name, Length | Format-Table -AutoSize
Get-Item $archive | Select-Object Name, Length
"7z SHA256: $zipHash"
