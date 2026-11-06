# 核对 TKOpenGl 没有链进 viewer / app 以外的目标。
#
# 1. 读 CMake 写出的 occt-link-report.txt（链接闭包，不依赖 dumpbin）。
# 2. 对构建目录里我们自己的 exe/dll 跑 dumpbin /dependents。
#    vcpkg_installed 下的 OCCT 原件跳过。StepLens.exe 允许出现 TKOpenGl.dll。
#
# 在 x64 Native Tools 或已经跑过 ilammy/msvc-dev-cmd 的环境里：
#   pwsh -File scripts/verify_occt_link.ps1 -BuildDir build/msvc -Report build/msvc/occt-link-report.txt
# 只扫二进制、没有报告时（例如 bench）：
#   pwsh -File scripts/verify_occt_link.ps1 -BuildDir build/bench

param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$Report = ""
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "找不到构建目录 $BuildDir"
    exit 1
}

if ($Report -ne "") {
    if (-not (Test-Path -LiteralPath $Report)) {
        Write-Host "找不到链接报告 $Report"
        exit 1
    }
    Write-Host "---- occt-link-report ----"
    Get-Content -LiteralPath $Report | ForEach-Object { Write-Host $_ }
    $allowed = @("steplens_occt_vis", "steplens_viewer", "StepLens")
    $viewerOk = $false
    $visOk = $false
    foreach ($line in Get-Content -LiteralPath $Report) {
        if ($line -notmatch '^(\S+)\s+tkopengl=(yes|no)$') {
            continue
        }
        $name = $Matches[1]
        $has = $Matches[2] -eq "yes"
        if ($name -eq "steplens_viewer" -and $has) { $viewerOk = $true }
        if ($name -eq "steplens_occt_vis" -and $has) { $visOk = $true }
        if ($has -and ($allowed -notcontains $name)) {
            Write-Host "$name 的链接闭包含有 TKOpenGl"
            exit 1
        }
    }
    if (-not $viewerOk -or -not $visOk) {
        Write-Host "报告里 viewer 或 occt_vis 没有链上 TKOpenGl"
        exit 1
    }
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    Write-Host "找不到 dumpbin.exe。请从 x64 Native Tools 命令行运行。"
    exit 1
}

$root = (Resolve-Path -LiteralPath $BuildDir).Path
$bins = @(Get-ChildItem -LiteralPath $root -Recurse -File -Include *.exe, *.dll |
    Where-Object { $_.FullName -notmatch '\\vcpkg_installed\\' })
if ($bins.Count -eq 0) {
    Write-Host "$BuildDir 里没有 exe/dll"
    exit 1
}

$bad = @()
foreach ($bin in $bins) {
    $text = & dumpbin.exe /nologo /dependents $bin.FullName | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Host "dumpbin 失败: $($bin.FullName)"
        exit 1
    }
    # 第一行是 “Dump of file …TKOpenGl.dll”，不能把它当成依赖。
    $hit = $false
    foreach ($dumpLine in ($text -split "`r?`n")) {
        if ($dumpLine -match 'Dump of file') { continue }
        if ($dumpLine -match 'TKOpenGl\.dll') { $hit = $true }
    }
    $rel = $bin.FullName.Substring($root.Length).TrimStart('\', '/')
    if ($hit) {
        Write-Host "TKOpenGl.dll  YES  $rel"
        if ($bin.Name -ne "StepLens.exe") {
            $bad += $rel
        }
    } else {
        Write-Host "TKOpenGl.dll  no   $rel"
    }
}

if ($bad.Count -gt 0) {
    Write-Host "这些二进制依赖 TKOpenGl.dll: $($bad -join ', ')"
    exit 1
}

Write-Host "dumpbin: 除 StepLens.exe 外没有 TKOpenGl.dll"
exit 0
