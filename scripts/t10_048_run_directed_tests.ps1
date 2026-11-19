param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$TimeoutMs = 600000,
    [int]$ReapMs = 30000,
    [int]$ScanMs = 15000
)
$ErrorActionPreference = "Stop"

function ConvertTo-Abs([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "empty path" }
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}
function Write-Meta([string]$File, [string]$Line) {
    Add-Content -LiteralPath $File -Value $Line -Encoding utf8
}
function Stop-OwnedProcess {
    param($Proc, [int]$ReapMs, [string]$Meta)
    if (-not $Proc) { return $true }
    $reaped = $false
    $hasExited = $false
    try { $hasExited = $Proc.HasExited } catch {
        Write-Meta $Meta "has_exited_query_error=$_"
        Write-Meta $Meta "unreaped=1"
        Write-Meta $Meta "kill_skipped=has_exited_unknown"
        return $false
    }
    if ($hasExited) { return $true }
    $id = $null
    try { $id = $Proc.Id } catch {
        Write-Meta $Meta "id_query_error=$_"
        Write-Meta $Meta "unreaped=1"
        Write-Meta $Meta "kill_skipped=id_unknown"
        return $false
    }
    $started = $null
    try { $started = $Proc.StartTime.ToString("o") } catch { }
    Write-Meta $Meta "kill_tree_id=$id"
    Write-Meta $Meta "kill_tree_start=$started"
    $tk = 1
    try {
        & taskkill.exe /PID $id /T /F | Out-String | ForEach-Object { Write-Meta $Meta "taskkill=$_".Trim() }
        $tk = $LASTEXITCODE
    } catch {
        Write-Meta $Meta "taskkill_exception=$_"
        try { Stop-Process -Id $id -Force -ErrorAction Stop } catch { Write-Meta $Meta "stop_process_error=$_" }
    }
    Write-Meta $Meta "taskkill_exit=$tk"
    try { $reaped = $Proc.WaitForExit($ReapMs) } catch { Write-Meta $Meta "reap_wait_error=$_" }
    try { $hasExited = $Proc.HasExited } catch { $hasExited = $false }
    Write-Meta $Meta "reaped=$( if ($reaped) { '1' } else { '0' } )"
    Write-Meta $Meta "has_exited=$( if ($hasExited) { '1' } else { '0' } )"
    if (-not $hasExited) {
        Write-Meta $Meta "unreaped=1"
    }
    return $hasExited
}

$OutDir = ConvertTo-Abs $OutDir
$BuildDir = ConvertTo-Abs $BuildDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$scratch = Join-Path $OutDir "scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter steplens_model_tests.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1
$meta = Join-Path $OutDir "tests-meta.txt"
function New-UniqueCopy([string]$Source, [string]$Prefix) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    $hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLower().Substring(0, 12)
    $leaf = [IO.Path]::GetFileName($Source)
    $dest = Join-Path $OutDir ("{0}-pid{1}-{2}-{3}" -f $Prefix, $PID, $hash, $leaf)
    Copy-Item -LiteralPath $Source -Destination $dest -Force
    Write-Meta $meta "copied=$dest"
}

if (-not $exe) {
    "exe_missing=1" | Set-Content -LiteralPath $meta -Encoding utf8
    exit 2
}
"cwd=$(Get-Location)" | Set-Content -LiteralPath $meta -Encoding utf8
Write-Meta $meta "outdir=$OutDir"
Write-Meta $meta "builddir=$BuildDir"
Write-Meta $meta "scratch=$scratch"
Write-Meta $meta "exe=$($exe.FullName)"
Write-Meta $meta ("exe_sha256=" + (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLower())

# Product-internal timeouts stay in the tests (10s EnqueueNotify + 5s reap, HangAfter 15s, HangBefore 2s).
$filter = "DocumentRelease.EnqueueNotifyIsCoveredByInFlightAcrossStop:DocumentRelease.HangBeforeFirstMainEntryFailsClosed:DocumentRelease.HangAfterFirstMainEntryCleanupFailsClosed"
Write-Meta $meta "filter=$filter"
Write-Meta $meta "timeout_ms=$TimeoutMs"
$xml = Join-Path $OutDir "gtest.xml"
$stdout = Join-Path $OutDir "tests-stdout.txt"
$stderr = Join-Path $OutDir "tests-stderr.txt"
$prefixBin = Join-Path $BuildDir "vcpkg_installed\x64-windows\bin"
if (Test-Path -LiteralPath $prefixBin) {
    $env:PATH = "$prefixBin;$env:PATH"
}
$stamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$mirror = Join-Path $OutDir ("lifecycle-mirror-pid{0}-{1}.log" -f $PID, $stamp)
$env:STEPLENS_DIAG_LOG = $mirror
$env:TEMP = $scratch
$env:TMP = $scratch
Write-Meta $meta "diag_log=$mirror"

$p = $null
$code = 2
try {
    $p = Start-Process -FilePath $exe.FullName `
        -ArgumentList @("--gtest_color=no", "--gtest_filter=$filter", "--gtest_output=xml:$xml") `
        -WorkingDirectory $OutDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -NoNewWindow
    if (-not $p) { throw "Start-Process returned no process" }
    $exited = $p.WaitForExit($TimeoutMs)
    if (-not $exited) {
        Write-Meta $meta "watchdog_timeout=1"
        $null = Stop-OwnedProcess -Proc $p -ReapMs $ReapMs -Meta $meta
        if ($p.HasExited) { Write-Meta $meta "gtest_exit=$($p.ExitCode)" } else { Write-Meta $meta "gtest_exit=watchdog_unreaped" }
        $code = 124
    } else {
        $code = $p.ExitCode
        Write-Meta $meta "gtest_exit=$code"
        Write-Meta $meta "has_exited=1"
    }
} catch {
    Write-Meta $meta "run_exception=$_"
    Write-Meta $meta "gtest_exit=2"
    $code = 2
} finally {
    if ($p) {
        $cleaned = Stop-OwnedProcess -Proc $p -ReapMs $ReapMs -Meta $meta
        if (-not $cleaned -and $code -eq 0) { $code = 124 }
    }
    $mirrorOk = (Test-Path -LiteralPath $mirror) -and ((Get-Item -LiteralPath $mirror).Length -gt 0)
    Write-Meta $meta "mirror_exists=$( if (Test-Path -LiteralPath $mirror) { '1' } else { '0' } )"
    Write-Meta $meta "mirror_bytes=$( if (Test-Path -LiteralPath $mirror) { (Get-Item -LiteralPath $mirror).Length } else { 0 } )"
    if (-not $mirrorOk) {
        Write-Meta $meta "incomplete=1"
        if ($code -eq 0) { $code = 2 }
    }
    # Bounded leftover scan of this run's dedicated scratch only; do not walk the machine TEMP.
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ScanMs)
    $scanned = 0
    $skipped = 0
    if (Test-Path -LiteralPath $scratch) {
        Get-ChildItem -LiteralPath $scratch -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "lifecycle*.log" } |
            ForEach-Object {
                if ([DateTime]::UtcNow -gt $deadline) { $skipped++; return }
                New-UniqueCopy $_.FullName "leftover"
                $scanned++
            }
    }
    Write-Meta $meta "scratch_copied=$scanned"
    if ($skipped -gt 0) { Write-Meta $meta "scratch_scan_incomplete=$skipped" }
    Remove-Item Env:STEPLENS_DIAG_LOG -ErrorAction SilentlyContinue
}
exit $code
