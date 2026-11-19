# Captures MSVC / SDK / runtime identity for 1111-048. Read-only after a configure/build.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Invoke-Logged([string]$Name, [scriptblock]$Block) {
    $out = Join-Path $OutDir $Name
    $text = @()
    try {
        $text = & $Block 2>&1 | ForEach-Object { "$_" }
        $global:LASTEXITCODE
    } catch {
        $text += "ERROR: $_"
    }
    Set-Content -LiteralPath $out -Value $text -Encoding utf8
}

$cl = Get-Command cl -ErrorAction SilentlyContinue
"cl=$(if ($cl) { $cl.Source } else { 'MISSING' })" | Set-Content (Join-Path $OutDir "cl-path.txt") -Encoding utf8
if ($cl) {
    Invoke-Logged "cl-Bv.txt" { cmd /c "cl /Bv" }
    Invoke-Logged "cl-help-head.txt" { cmd /c "cl" }
}

Invoke-Logged "cmake-version.txt" { cmake --version }
Invoke-Logged "ninja-version.txt" { ninja --version }
Invoke-Logged "where-tools.txt" {
    where.exe cl
    where.exe ninja
    where.exe link
    where.exe cmake
}

$cache = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path -LiteralPath $cache) {
    Copy-Item -LiteralPath $cache -Destination (Join-Path $OutDir "CMakeCache.txt")
    Select-String -LiteralPath $cache -Pattern "CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS|CMAKE_BUILD_TYPE|STEPLENS_|OpenCASCADE|VCPKG|MSVC" |
        ForEach-Object { $_.Line } |
        Set-Content (Join-Path $OutDir "CMakeCache-selected.txt") -Encoding utf8
}

Get-ChildItem -LiteralPath $BuildDir -Recurse -Include *.rsp,*.ninja -ErrorAction SilentlyContinue |
    Select-Object -First 80 FullName, Length |
    Out-String |
    Set-Content (Join-Path $OutDir "rsp-ninja-index.txt") -Encoding utf8

$rspDir = Join-Path $OutDir "rsp"
New-Item -ItemType Directory -Force -Path $rspDir | Out-Null
Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter "*.rsp" -ErrorAction SilentlyContinue |
    Select-Object -First 40 |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $rspDir $_.Name) }

# DocumentRelease compile command if ninja can print it
$docObjHint = "DocumentRelease.cpp"
Invoke-Logged "ninja-document-release-commands.txt" {
    if (Test-Path -LiteralPath (Join-Path $BuildDir "build.ninja")) {
        Push-Location $BuildDir
        try {
            ninja -t commands steplens_model_tests
        } finally {
            Pop-Location
        }
    }
}

$prefixCandidates = @(
    (Join-Path $BuildDir "vcpkg_installed\x64-windows"),
    (Join-Path (Get-Location) "build\t10-048-diag\vcpkg_installed\x64-windows"),
    (Join-Path (Get-Location) "vcpkg_installed\x64-windows")
)
$prefix = $prefixCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($prefix) {
    "prefix=$prefix" | Set-Content (Join-Path $OutDir "vcpkg-prefix.txt") -Encoding utf8
    $ver = Join-Path $prefix "include\opencascade\Standard_Version.hxx"
    if (Test-Path -LiteralPath $ver) {
        Copy-Item $ver (Join-Path $OutDir "Standard_Version.hxx")
    }
    $bin = Join-Path $prefix "bin"
    if (Test-Path -LiteralPath $bin) {
        Get-ChildItem -LiteralPath $bin -Filter "TK*.dll" |
            ForEach-Object {
                $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
                "{0}  {1}  {2}" -f $hash, $_.Length, $_.Name
            } | Set-Content (Join-Path $OutDir "tk-dll-sha256.txt") -Encoding utf8
    }
} else {
    "vcpkg prefix not found" | Set-Content (Join-Path $OutDir "vcpkg-prefix.txt") -Encoding utf8
}

Get-Date -Format "yyyy-MM-ddTHH:mm:ssK" | Set-Content (Join-Path $OutDir "captured-at.txt") -Encoding utf8
