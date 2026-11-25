# 非 offscreen 的 Viewer 帧检查。缺图、空图、整幅单色都失败。不跳过。
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Writer,
    [Parameter(Mandatory = $true)][string]$DumpDir,
    [Parameter(Mandatory = $true)][string]$IdentityFile,
    [Parameter(Mandatory = $true)][string]$CheckoutSha,
    [Parameter(Mandatory = $true)][string]$CheckoutTree
)

$ErrorActionPreference = "Stop"

function Assert-NotSolidPng {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Label 不存在：$Path"
    }
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -le 0) {
        throw "$Label 是空文件"
    }
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        if ($bitmap.Width -lt 2 -or $bitmap.Height -lt 2) {
            throw "$Label 尺寸 $($bitmap.Width)x$($bitmap.Height)，不能当帧通过"
        }
        $first = $bitmap.GetPixel(0, 0).ToArgb()
        $solid = $true
        for ($y = 0; $y -lt $bitmap.Height; $y++) {
            for ($x = 0; $x -lt $bitmap.Width; $x++) {
                if ($bitmap.GetPixel($x, $y).ToArgb() -ne $first) {
                    $solid = $false
                    break
                }
            }
            if (-not $solid) { break }
        }
        if ($solid) {
            throw "$Label 整幅单色 $first，不能当帧通过"
        }
    } finally {
        $bitmap.Dispose()
    }
}

if ($env:QT_QPA_PLATFORM -match "offscreen") {
    throw "QT_QPA_PLATFORM=offscreen。拒绝用离屏空窗当像素通过。"
}
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue

if ($CheckoutSha -notmatch "^[0-9a-f]{40}$" -or $CheckoutTree -notmatch "^[0-9a-f]{40}$") {
    throw "checkout SHA/tree 不是 40 位十六进制"
}
if (-not (Test-Path -LiteralPath $Exe)) { throw "未找到 $Exe" }
if (-not (Test-Path -LiteralPath $Writer)) { throw "未找到 $Writer" }
if (-not (Test-Path -LiteralPath $IdentityFile)) { throw "未找到构建身份 $IdentityFile" }

New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null
$model = Join-Path $DumpDir "box-10x20x30.step"
& $Writer $model
if ($LASTEXITCODE -ne 0) { throw "写已知盒子失败，退出码 $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $model)) { throw "没有写出 $model" }
$modelHash = (Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash.ToLowerInvariant()

$env:STEPLENS_DUMP_SHOTS = $DumpDir
$env:STEPLENS_LIFECYCLE_LOG = Join-Path $DumpDir "lifecycle.log"
$stdout = Join-Path $DumpDir "stdout.txt"
$stderr = Join-Path $DumpDir "stderr.txt"
$proc = Start-Process -FilePath $Exe -ArgumentList @($model) -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (-not $proc.WaitForExit(60000)) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    throw "StepLens 60 秒内没有退出。没有可用帧缓冲就失败，不跳过，也不改用 offscreen。"
}
if ($proc.ExitCode -ne 0) {
    throw "StepLens 退出码 $($proc.ExitCode)。stderr 在 $stderr"
}

$frameNotePath = Join-Path $DumpDir "framebuffer.txt"
if (-not (Test-Path -LiteralPath $frameNotePath)) { throw "没有 framebuffer.txt" }
$frameNote = (Get-Content -LiteralPath $frameNotePath -Raw).Trim()
if ($frameNote -ne "ok") { throw "帧缓冲没有写成：$frameNote" }
Assert-NotSolidPng -Path (Join-Path $DumpDir "framebuffer.png") -Label "framebuffer.png"
Assert-NotSolidPng -Path (Join-Path $DumpDir "model.png") -Label "model.png"

$frameHash = (Get-FileHash -LiteralPath (Join-Path $DumpDir "framebuffer.png") -Algorithm SHA256).Hash.ToLowerInvariant()
$grabHash = (Get-FileHash -LiteralPath (Join-Path $DumpDir "model.png") -Algorithm SHA256).Hash.ToLowerInvariant()
Copy-Item -LiteralPath $IdentityFile -Destination (Join-Path $DumpDir "steplens-build-identity.txt") -Force
$receipt = @(
    "checkout_sha=$CheckoutSha"
    "checkout_tree=$CheckoutTree"
    "model=box-10x20x30.step"
    "model_sha256=$modelHash"
    "model_source=BRepPrimAPI_MakeBox(10,20,30)"
    "framebuffer_sha256=$frameHash"
    "grab_view_sha256=$grabHash"
    "qpa=windows"
) -join "`n"
Set-Content -LiteralPath (Join-Path $DumpDir "view-shot-receipt.txt") -Value ($receipt + "`n") -Encoding utf8NoBOM
Write-Host $receipt
