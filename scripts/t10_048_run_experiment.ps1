param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$TimeoutMs = 30000,
    [int]$ReapMs = 30000
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
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$src = ConvertTo-Abs (Join-Path (Get-Location) "tests\diag\static-dtor-join-gate.cpp")
$exe = Join-Path $OutDir "static-dtor-join-gate.exe"
$stdout = Join-Path $OutDir "experiment-stdout.txt"
$stderr = Join-Path $OutDir "experiment-stderr.txt"
$meta = Join-Path $OutDir "experiment-meta.txt"
$log = Join-Path $OutDir "static-dtor-join-gate.log"

"cwd=$(Get-Location)" | Set-Content -LiteralPath $meta -Encoding utf8
Write-Meta $meta "outdir=$OutDir"
Write-Meta $meta "src=$src"
Write-Meta $meta "log=$log"
Write-Meta $meta "cl=$( if (Get-Command cl -ErrorAction SilentlyContinue) { (Get-Command cl).Source } else { 'MISSING' } )"

$clExit = 2
try {
    cmd /c "cl /nologo /EHsc /std:c++17 /O2 /W4 /Fe:`"$exe`" `"$src`" /Fo:`"$OutDir\static-dtor-join-gate.obj`"" `
        > (Join-Path $OutDir "experiment-cl-stdout.txt") 2> (Join-Path $OutDir "experiment-cl-stderr.txt")
    $clExit = $LASTEXITCODE
} catch {
    Write-Meta $meta "cl_exception=$_"
    $clExit = 2
}
Write-Meta $meta "cl_exit=$clExit"
if ($clExit -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Write-Meta $meta "run_skipped=1"
    Write-Meta $meta "run_exit=2"
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
        Write-Meta $meta "watchdog_timeout_ms=$TimeoutMs"
        $null = Stop-OwnedProcess -Proc $p -ReapMs $ReapMs -Meta $meta
        if ($p.HasExited) { Write-Meta $meta "run_exit=$($p.ExitCode)" } else { Write-Meta $meta "run_exit=watchdog_unreaped" }
        $code = 124
    } else {
        $code = $p.ExitCode
        Write-Meta $meta "run_exit=$code"
        Write-Meta $meta "watchdog_timeout_ms=$TimeoutMs"
        Write-Meta $meta "has_exited=1"
    }
} catch {
    Write-Meta $meta "run_exception=$_"
    Write-Meta $meta "run_exit=2"
    $code = 2
} finally {
    if ($p) {
        $cleaned = Stop-OwnedProcess -Proc $p -ReapMs $ReapMs -Meta $meta
        if (-not $cleaned -and $code -eq 0) { $code = 124 }
    }
    Remove-Item Env:STEPLENS_DIAG_EXPERIMENT_LOG -ErrorAction SilentlyContinue
    $logOk = (Test-Path -LiteralPath $log) -and ((Get-Item -LiteralPath $log).Length -gt 0)
    Write-Meta $meta "log_exists=$( if (Test-Path -LiteralPath $log) { '1' } else { '0' } )"
    Write-Meta $meta "log_bytes=$( if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 } )"
    if (-not $logOk) {
        Write-Meta $meta "incomplete=1"
        if ($code -eq 0) { $code = 2 }
    }
}
exit $code
