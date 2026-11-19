param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$TimeoutMs = 30000,
    [int]$ReapMs = 30000
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$src = Join-Path (Get-Location) "tests\diag\static-dtor-join-gate.cpp"
$exe = Join-Path $OutDir "static-dtor-join-gate.exe"
$stdout = Join-Path $OutDir "experiment-stdout.txt"
$stderr = Join-Path $OutDir "experiment-stderr.txt"
$meta = Join-Path $OutDir "experiment-meta.txt"
$log = Join-Path $OutDir "static-dtor-join-gate.log"
function Write-Meta([string]$Line) {
    Add-Content -LiteralPath $meta -Value $Line -Encoding utf8
}

"cl=$( if (Get-Command cl -ErrorAction SilentlyContinue) { (Get-Command cl).Source } else { 'MISSING' } )" |
    Set-Content -LiteralPath $meta -Encoding utf8

$clExit = 2
try {
    cmd /c "cl /nologo /EHsc /std:c++17 /O2 /W4 /Fe:`"$exe`" `"$src`" /Fo:`"$OutDir\static-dtor-join-gate.obj`"" `
        > (Join-Path $OutDir "experiment-cl-stdout.txt") 2> (Join-Path $OutDir "experiment-cl-stderr.txt")
    $clExit = $LASTEXITCODE
} catch {
    Write-Meta "cl_exception=$_"
    $clExit = 2
}
Write-Meta "cl_exit=$clExit"
if ($clExit -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Write-Meta "run_skipped=1"
    Write-Meta "run_exit=2"
    if ($clExit -eq 0) { exit 2 }
    exit $clExit
}

$p = $null
$code = 2
try {
    $env:STEPLENS_DIAG_EXPERIMENT_LOG = $log
    $p = Start-Process -FilePath $exe -WorkingDirectory $OutDir -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru -NoNewWindow
    if (-not $p) { throw "Start-Process returned no process" }
    $exited = $p.WaitForExit($TimeoutMs)
    if (-not $exited) {
        Write-Meta "watchdog_timeout_ms=$TimeoutMs"
        try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { Write-Meta "kill_error=$_" }
        $reaped = $false
        try { $reaped = $p.WaitForExit($ReapMs) } catch { Write-Meta "reap_wait_error=$_" }
        Write-Meta "reaped=$( if ($reaped) { '1' } else { '0' } )"
        Write-Meta "has_exited=$( if ($p.HasExited) { '1' } else { '0' } )"
        if ($p.HasExited) {
            Write-Meta "run_exit=$($p.ExitCode)"
        } else {
            Write-Meta "run_exit=watchdog_unreaped"
        }
        exit 124
    }
    $code = $p.ExitCode
    Write-Meta "run_exit=$code"
    Write-Meta "watchdog_timeout_ms=$TimeoutMs"
    Write-Meta "has_exited=1"
} catch {
    Write-Meta "run_exception=$_"
    Write-Meta "run_exit=2"
    exit 2
} finally {
    Remove-Item Env:STEPLENS_DIAG_EXPERIMENT_LOG -ErrorAction SilentlyContinue
    Write-Meta "log_exists=$( if (Test-Path -LiteralPath $log) { '1' } else { '0' } )"
}
exit $code
