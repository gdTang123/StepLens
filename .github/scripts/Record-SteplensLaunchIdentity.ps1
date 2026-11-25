# 记录即将启动或刚启动过的 StepLens.exe，以及 PE 里 CodeView RSDS 指向的 PDB。
# 缺 exe、缺 RSDS、缺 PDB 或哈希失败时退出 3，并仍写出清单。不改产品退出码。
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][ValidateSet("before", "after")][string]$Phase,
    [Parameter(Mandatory = $true)][string]$CheckoutSha,
    [Parameter(Mandatory = $true)][string]$CheckoutTree,
    [Parameter(Mandatory = $true)][string]$IdentityFile
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$manifestPath = Join-Path $OutDir "launch-identity-$Phase.txt"
$modulePath = Join-Path $OutDir "modules-$Phase.txt"
$status = "ok"
$pdbPath = ""
$pdbHash = ""
$pdbGuid = ""
$pdbAge = ""
$exeHash = ""

function Write-Manifest {
    $identityRaw = "missing"
    $identityHash = ""
    if (Test-Path -LiteralPath $IdentityFile) {
        $identityBytes = [System.IO.File]::ReadAllBytes($IdentityFile)
        $identityHash = [System.BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash($identityBytes)).Replace("-", "").ToLowerInvariant()
        [System.IO.File]::WriteAllBytes((Join-Path $OutDir "steplens-build-identity-$Phase.txt"), $identityBytes)
        $identityRaw = [System.Text.Encoding]::UTF8.GetString($identityBytes)
    } else {
        $script:status = "identity-missing"
    }
    $lines = @(
        "phase=$Phase"
        "checkout_sha=$CheckoutSha"
        "checkout_tree=$CheckoutTree"
        "exe_path=$Exe"
        "exe_sha256=$exeHash"
        "pdb_path=$pdbPath"
        "pdb_sha256=$pdbHash"
        "pdb_guid=$pdbGuid"
        "pdb_age=$pdbAge"
        "status=$status"
        "identity_sha256=$identityHash"
        "identity_begin"
        $identityRaw
        "identity_end"
    )
    [System.IO.File]::WriteAllLines($manifestPath, $lines)
    $module = "main exe=$Exe sha256=$exeHash pdb=$pdbPath pdb_sha256=$pdbHash pdb_guid=$pdbGuid pdb_age=$pdbAge status=$status"
    [System.IO.File]::WriteAllText($modulePath, $module + "`n")
    Write-Host $module
}

function Get-RSDS {
    param([byte[]]$Bytes)
    function U16([int]$Offset) { [BitConverter]::ToUInt16($Bytes, $Offset) }
    function U32([int]$Offset) { [BitConverter]::ToUInt32($Bytes, $Offset) }
    if ($Bytes.Length -lt 0x40 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { return $null }
    $lfanew = [int](U32 0x3C)
    if (($lfanew + 24) -ge $Bytes.Length -or (U32 $lfanew) -ne 0x00004550) { return $null }
    $coff = $lfanew + 4
    $sectionCount = [int](U16 ($coff + 2))
    $optionalSize = [int](U16 ($coff + 16))
    $optional = $coff + 20
    if (($optional + 2) -ge $Bytes.Length) { return $null }
    $magic = U16 $optional
    if ($magic -eq 0x20b) { $directories = $optional + 112 }
    elseif ($magic -eq 0x10b) { $directories = $optional + 96 }
    else { return $null }
    $sectionTable = $optional + $optionalSize
    function Convert-Rva([uint32]$Rva) {
        for ($index = 0; $index -lt $sectionCount; $index++) {
            $section = $sectionTable + ($index * 40)
            if (($section + 24) -ge $Bytes.Length) { return -1 }
            $virtualSize = U32 ($section + 8)
            $virtualAddress = U32 ($section + 12)
            $rawSize = U32 ($section + 16)
            $rawPointer = U32 ($section + 20)
            $span = [Math]::Max($virtualSize, $rawSize)
            if ($Rva -ge $virtualAddress -and $Rva -lt ($virtualAddress + $span)) {
                return [int]($rawPointer + ($Rva - $virtualAddress))
            }
        }
        return -1
    }
    if (($directories + 52) -ge $Bytes.Length) { return $null }
    # DataDirectory[6] is Debug.
    $debugRva = U32 ($directories + 48)
    $debugSize = U32 ($directories + 52)
    if ($debugRva -eq 0 -or $debugSize -lt 28) { return $null }
    $debugOffset = Convert-Rva $debugRva
    if ($debugOffset -lt 0) { return $null }
    $count = [int]($debugSize / 28)
    for ($index = 0; $index -lt $count; $index++) {
        $entry = $debugOffset + ($index * 28)
        if (($entry + 28) -ge $Bytes.Length) { break }
        if ((U32 ($entry + 12)) -ne 2) { continue }
        $raw = [int](U32 ($entry + 24))
        if ($raw -le 0) { $raw = Convert-Rva (U32 ($entry + 20)) }
        if ($raw -lt 0 -or ($raw + 24) -ge $Bytes.Length) { continue }
        if ($Bytes[$raw] -ne 0x52 -or $Bytes[$raw + 1] -ne 0x53 -or $Bytes[$raw + 2] -ne 0x44 -or $Bytes[$raw + 3] -ne 0x53) { continue }
        $data1 = (U32 ($raw + 4)).ToString("x8")
        $data2 = (U16 ($raw + 8)).ToString("x4")
        $data3 = (U16 ($raw + 10)).ToString("x4")
        $tail = ""
        for ($byte = 0; $byte -lt 8; $byte++) { $tail += $Bytes[$raw + 12 + $byte].ToString("x2") }
        $guid = "$data1-$data2-$data3-$($tail.Substring(0, 4))-$($tail.Substring(4))"
        $age = U32 ($raw + 20)
        $pathBytes = New-Object System.Collections.Generic.List[byte]
        for ($cursor = $raw + 24; $cursor -lt $Bytes.Length -and $Bytes[$cursor] -ne 0; $cursor++) {
            $pathBytes.Add($Bytes[$cursor])
        }
        $path = [System.Text.Encoding]::UTF8.GetString($pathBytes.ToArray())
        return [pscustomobject]@{ Guid = $guid; Age = $age; Path = $path }
    }
    return $null
}

if ($CheckoutSha -notmatch "^[0-9a-f]{40}$" -or $CheckoutTree -notmatch "^[0-9a-f]{40}$") {
    $status = "checkout-invalid"
    Write-Manifest
    exit 3
}
if (-not (Test-Path -LiteralPath $Exe)) {
    $status = "exe-missing"
    Write-Manifest
    exit 3
}
$exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLowerInvariant()
$image = [System.IO.File]::ReadAllBytes($Exe)
$rsds = Get-RSDS -Bytes $image
if ($null -eq $rsds -or [string]::IsNullOrWhiteSpace($rsds.Path)) {
    $status = "pdb-rsds-missing"
    Write-Manifest
    exit 3
}
$pdbGuid = $rsds.Guid
$pdbAge = [string]$rsds.Age
$candidate = $rsds.Path
if (-not [System.IO.Path]::IsPathRooted($candidate)) {
    $candidate = Join-Path (Split-Path -Parent $Exe) $candidate
}
$pdbPath = [System.IO.Path]::GetFullPath($candidate)
if (-not (Test-Path -LiteralPath $pdbPath)) {
    $status = "pdb-missing"
    Write-Manifest
    exit 3
}
$pdbHash = (Get-FileHash -LiteralPath $pdbPath -Algorithm SHA256).Hash.ToLowerInvariant()
Copy-Item -LiteralPath $Exe -Destination (Join-Path $OutDir "StepLens-$Phase.exe") -Force
Copy-Item -LiteralPath $pdbPath -Destination (Join-Path $OutDir "StepLens-$Phase.pdb") -Force
Write-Manifest
if ($status -ne "ok") { exit 3 }
exit 0
