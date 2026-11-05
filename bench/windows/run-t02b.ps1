<#
  T02b：在 Windows 上跑一个 OCCT 版本的全部测试（不需要管理员权限）。
  用法（在 C:\sl\t02b 下）：
    powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 801 -Build build801
    powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 793 -Build build793
  输出：results\<Tag>\<编号>.txt（key=value）、.err（stderr，崩溃栈在这里）、崩溃时 <编号>.dmp
  耗时：每个版本约 15–20 分钟（X-07 若崩溃会更短）
#>
param(
  [Parameter(Mandatory)] [string]$Tag,
  [Parameter(Mandatory)] [string]$Build,
  [string]$Samples = "C:\sl\samples",
  [switch]$Quick        # 只跑 X-07 一次（先确认崩不崩）
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "$Build\RelWithDebInfo\spike.exe"
$out  = Join-Path $root "results\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
# G1-8：kit 读上一级或本目录的 KIT_COMMIT；在 git 仓库里与 HEAD 核对。写入 results\<Tag>\kit_commit.txt
function Get-BenchReal([string]$path) {
  $item = Get-Item -LiteralPath $path -Force
  $target = $null
  if ($item.PSObject.Properties.Name -contains 'Target' -and $item.Target) {
    $target = @($item.Target)[0]
  } elseif ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
    $parent = Split-Path -Parent $item.FullName
    $leaf = [regex]::Escape($item.Name)
    foreach ($line in @(& cmd.exe /d /c "dir /AL `"$parent`"")) {
      if ($line -match ('\s' + $leaf + '\s+\[(.+)\]\s*$')) { $target = $Matches[1]; break }
    }
  }
  if ($target) {
    $target = ([string]$target).Trim().TrimEnd('\', '/')
    if (-not [IO.Path]::IsPathRooted($target)) { $target = Join-Path (Split-Path -Parent $item.FullName) $target }
    return $target
  }
  return $item.FullName
}
$benchReal = Get-BenchReal $root
$kitCommit = $null
foreach ($dir in @((Split-Path -Parent $benchReal), (Split-Path -Parent $root), $benchReal, $root)) {
  if (-not $dir) { continue }
  $kitPath = Join-Path $dir "KIT_COMMIT"
  if (Test-Path -LiteralPath $kitPath) {
    $kitCommit = ([IO.File]::ReadAllText($kitPath)).Trim()
    break
  }
}
$gitHead = $null
if (Get-Command git -ErrorAction SilentlyContinue) {
  foreach ($dir in @($benchReal, $root)) {
    if (-not $dir) { continue }
    try {
      $probe = & git -C $dir rev-parse HEAD 2>$null
      if ($LASTEXITCODE -eq 0 -and $probe) { $gitHead = "$probe".Trim(); break }
    } catch { $gitHead = $null }
  }
}
if ($kitCommit) { Write-Host "kit_commit=$kitCommit" }
if ($gitHead) { Write-Host "git_head=$gitHead" }
if ($kitCommit -and $gitHead -and ($kitCommit.ToLower() -ne $gitHead.ToLower())) {
  Write-Host "KIT_COMMIT 与当前 git 提交不一致：KIT_COMMIT=$kitCommit，git HEAD=$gitHead。请改到 KIT_COMMIT 对应的提交再跑，或用 scripts/make_t02b_kit.py 按 main HEAD 重新打包。" -ForegroundColor Yellow
} elseif ($kitCommit -and $kitCommit -notmatch '^[0-9a-fA-F]{40}$') {
  Write-Host "KIT_COMMIT 不是 40 位提交号：$kitCommit" -ForegroundColor Yellow
} elseif (-not $kitCommit -and -not $gitHead) {
  Write-Host "未找到 KIT_COMMIT，当前目录也不在 git 仓库内，无法确认这次摸底对应的提交。" -ForegroundColor Yellow
}
$metaLines = @()
if ($kitCommit) { $metaLines += "kit_commit=$kitCommit" }
if ($gitHead) { $metaLines += "git_head=$gitHead" }
if ($metaLines.Count -gt 0) {
  $utf8 = New-Object System.Text.UTF8Encoding $false
  [IO.File]::WriteAllText((Join-Path $out "kit_commit.txt"), (($metaLines -join "`n") + "`n"), $utf8)
}
# 崩溃栈符号化：exe 目录（含 OCCT DLL）+ vcpkg 安装目录下的 PDB
$env:_NT_SYMBOL_PATH = "$(Split-Path $exe);$(Join-Path $root "$Build\vcpkg_installed\x64-windows-release\bin")"

function Run([string]$id, [string[]]$a) {
  $t0 = Get-Date
  Write-Host ("== {0}  {1:HH:mm:ss}" -f $id, $t0)
  $p = Start-Process -FilePath $exe -ArgumentList $a -WorkingDirectory $out -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$out\$id.txt" -RedirectStandardError "$out\$id.err"
  Add-Content "$out\$id.txt" ("exit={0}`nwall_s={1:N1}" -f $p.ExitCode, ((Get-Date) - $t0).TotalSeconds)
  if (Test-Path "$out\spike-crash.dmp") { Move-Item -Force "$out\spike-crash.dmp" "$out\$id.dmp" }
  if ($p.ExitCode -ne 0) { Write-Host ("   exit=0x{0:X8}  见 $out\$id.err" -f $p.ExitCode) -ForegroundColor Yellow }
}

# 0) 版本与兼容单测
& (Join-Path $root "$Build\RelWithDebInfo\test_compat.exe") | Tee-Object "$out\test_compat.txt"

$S = @{ X07 = "$Samples\PX13226.IGS"; X02 = "$Samples\Scania-Engine-V8-XT-Turbo.step";
        L03 = "$Samples\NissanGT-R.STEP"; X01 = "$Samples\Scania-8x4.stp";
        X06 = "$Samples\1026064M.IGS";   X08 = "$Samples\1027247E.IGS" }

# 1) 超大 IGES：X-07（关键判定项）
Run "X-07" @("bench", $S.X07)
if ($Quick) { return }
# 2) X-06 / X-08（可选，文件存在才跑）
foreach ($k in "X06","X08") { if (Test-Path $S[$k]) { Run ($k.Insert(1,"-")) @("bench", $S[$k]) } }
# 3) STEP 性能：X-02、L-03 各 3 次，X-01 1 次（第 1 次含冷读）
foreach ($i in 1..3) { Run "X-02-r$i" @("bench", $S.X02); Run "L-03-r$i" @("bench", $S.L03) }
Run "X-01" @("bench", $S.X01)
# 4) 取消：X-02 读取阶段 10/50/90%；转换阶段 20/50/80%（按第 1 次的转换耗时换算成秒）
foreach ($f in "0.1","0.5","0.9") { Run "cancel-read-X02-$f" @("cancel-read", $S.X02, $f) }
$T = [double]((Select-String -Path "$out\X-02-r1.txt" -Pattern '^transfer_time_s=(.*)$').Matches[0].Groups[1].Value)
foreach ($p in 0.2,0.5,0.8) { $s = [math]::Round($T * $p, 1); Run "cancel-transfer-X02-$p" @("cancel-transfer", $S.X02, "$s") }
Write-Host "== 完成：$out"
