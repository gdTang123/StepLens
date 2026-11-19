param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter steplens_model_tests.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1
$meta = Join-Path $OutDir "tests-meta.txt"
if (-not $exe) {
    "exe_missing=1" | Set-Content $meta -Encoding utf8
    exit 2
}
"exe=$($exe.FullName)" | Set-Content $meta -Encoding utf8
Add-Content $meta ("exe_sha256=" + (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLower())

# Original internal timeouts stay in the tests (10s EnqueueNotify wait + 5s reap, HangAfter 15s, HangBefore 2s).
# No extra short outer kill. This process is allowed to run up to 10 minutes by the workflow step.
$filter = "DocumentRelease.EnqueueNotifyIsCoveredByInFlightAcrossStop:DocumentRelease.HangBeforeFirstMainEntryFailsClosed:DocumentRelease.HangAfterFirstMainEntryCleanupFailsClosed"
Add-Content $meta "filter=$filter"
$xml = Join-Path $OutDir "gtest.xml"
$stdout = Join-Path $OutDir "tests-stdout.txt"
$stderr = Join-Path $OutDir "tests-stderr.txt"
$prefixBin = Join-Path $BuildDir "vcpkg_installed\x64-windows\bin"
if (Test-Path -LiteralPath $prefixBin) {
    $env:PATH = "$prefixBin;$env:PATH"
}
$p = Start-Process -FilePath $exe.FullName `
    -ArgumentList @("--gtest_color=no", "--gtest_filter=$filter", "--gtest_output=xml:$xml") `
    -WorkingDirectory $OutDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -NoNewWindow
$p.WaitForExit()
Add-Content $meta "gtest_exit=$($p.ExitCode)"
# Best-effort copy of leftover lifecycle logs (TempDir usually unlinks them; stdout still has the body on fail).
Get-ChildItem -LiteralPath $env:TEMP -Recurse -Filter "lifecycle-r1.log" -ErrorAction SilentlyContinue |
    Select-Object -First 5 |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $OutDir ("leftover-" + $_.Name)) }
exit $p.ExitCode
