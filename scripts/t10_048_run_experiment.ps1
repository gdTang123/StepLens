param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$TimeoutMs = 30000
)
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$src = Join-Path (Get-Location) "tests\diag\static-dtor-join-gate.cpp"
$exe = Join-Path $OutDir "static-dtor-join-gate.exe"
$stdout = Join-Path $OutDir "experiment-stdout.txt"
$stderr = Join-Path $OutDir "experiment-stderr.txt"
$meta = Join-Path $OutDir "experiment-meta.txt"

"cl=$( (Get-Command cl -ErrorAction SilentlyContinue).Source )" | Set-Content $meta -Encoding utf8
cmd /c "cl /nologo /EHsc /std:c++17 /O2 /W4 /Fe:`"$exe`" `"$src`" /Fo:`"$OutDir\static-dtor-join-gate.obj`"" > (Join-Path $OutDir "experiment-cl-stdout.txt") 2> (Join-Path $OutDir "experiment-cl-stderr.txt")
$clExit = $LASTEXITCODE
Add-Content $meta "cl_exit=$clExit"
if ($clExit -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Add-Content $meta "run_skipped=1"
    Add-Content $meta "run_exit="
    exit $clExit
}

$p = Start-Process -FilePath $exe -WorkingDirectory $OutDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -NoNewWindow
$exited = $p.WaitForExit($TimeoutMs)
if (-not $exited) {
    Add-Content $meta "watchdog_timeout_ms=$TimeoutMs"
    try { Stop-Process -Id $p.Id -Force } catch { Add-Content $meta "kill_error=$_" }
    Start-Sleep -Milliseconds 200
    Add-Content $meta "run_exit=watchdog"
    exit 124
}
Add-Content $meta "run_exit=$($p.ExitCode)"
Add-Content $meta "watchdog_timeout_ms=$TimeoutMs"
exit $p.ExitCode
