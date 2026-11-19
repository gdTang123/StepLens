param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$TimeoutMs = 600000,
    [int]$ReapMs = 30000
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter steplens_model_tests.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1
$meta = Join-Path $OutDir "tests-meta.txt"
function Write-Meta([string]$Line) {
    Add-Content -LiteralPath $meta -Value $Line -Encoding utf8
}
function New-UniqueCopy([string]$Source, [string]$Prefix) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    $hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLower().Substring(0, 12)
    $leaf = [IO.Path]::GetFileName($Source)
    $dest = Join-Path $OutDir ("{0}-pid{1}-{2}-{3}" -f $Prefix, $PID, $hash, $leaf)
    Copy-Item -LiteralPath $Source -Destination $dest -Force
    Write-Meta "copied=$dest"
}

if (-not $exe) {
    "exe_missing=1" | Set-Content -LiteralPath $meta -Encoding utf8
    exit 2
}
"exe=$($exe.FullName)" | Set-Content -LiteralPath $meta -Encoding utf8
Write-Meta ("exe_sha256=" + (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLower())

# Product-internal timeouts stay in the tests (10s EnqueueNotify + 5s reap, HangAfter 15s, HangBefore 2s).
# This bound is only the outer controller around the whole gtest process (static dtors included).
$filter = "DocumentRelease.EnqueueNotifyIsCoveredByInFlightAcrossStop:DocumentRelease.HangBeforeFirstMainEntryFailsClosed:DocumentRelease.HangAfterFirstMainEntryCleanupFailsClosed"
Write-Meta "filter=$filter"
Write-Meta "timeout_ms=$TimeoutMs"
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
Write-Meta "diag_log=$mirror"

$p = $null
$code = 2
try {
    $p = Start-Process -FilePath $exe.FullName `
        -ArgumentList @("--gtest_color=no", "--gtest_filter=$filter", "--gtest_output=xml:$xml") `
        -WorkingDirectory $OutDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -NoNewWindow
    if (-not $p) { throw "Start-Process returned no process" }
    $exited = $p.WaitForExit($TimeoutMs)
    if (-not $exited) {
        Write-Meta "watchdog_timeout=1"
        try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { Write-Meta "kill_error=$_" }
        $reaped = $false
        try { $reaped = $p.WaitForExit($ReapMs) } catch { Write-Meta "reap_wait_error=$_" }
        Write-Meta "reaped=$( if ($reaped) { '1' } else { '0' } )"
        Write-Meta "has_exited=$( if ($p.HasExited) { '1' } else { '0' } )"
        if ($p.HasExited) { Write-Meta "gtest_exit=$($p.ExitCode)" } else { Write-Meta "gtest_exit=watchdog_unreaped" }
        $code = 124
    } else {
        $code = $p.ExitCode
        Write-Meta "gtest_exit=$code"
        Write-Meta "has_exited=1"
    }
} catch {
    Write-Meta "run_exception=$_"
    Write-Meta "gtest_exit=2"
    $code = 2
} finally {
    Write-Meta "mirror_exists=$( if (Test-Path -LiteralPath $mirror) { '1' } else { '0' } )"
    # Best-effort leftovers after TempDir cleanup; unique names, no first-5 cap, no same-name overwrite.
    $temps = @()
    if ($env:TEMP) { $temps += $env:TEMP }
    if ($env:TMP) { $temps += $env:TMP }
    foreach ($root in ($temps | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "lifecycle-r1.log" -or $_.Name -like "lifecycle*.log" } |
            ForEach-Object { New-UniqueCopy $_.FullName "leftover" }
    }
    Remove-Item Env:STEPLENS_DIAG_LOG -ErrorAction SilentlyContinue
}
exit $code
