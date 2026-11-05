<#
  T02b：在 Windows 上跑一个 OCCT 版本的全部测试（不需要管理员权限）。
  用法（在 C:\sl\t02b 下，Windows PowerShell 5.1）：
    powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 801 -Build build801
    powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 793 -Build build793
  输出：results\<Tag>\<编号>.txt（key=value）、.err（stderr，崩溃栈在这里）、崩溃时 <编号>.dmp
        results\<Tag>\env.txt（仅在需要时出现：pdb_missing=、skipped=、transfer_time_s_from=）
  开跑前检查 spike.pdb 和 spike 会加载的 TK*.pdb，缺了只警告、不中断。
  test_compat.exe 退出码不是 0 就立刻停止，后面的测试都不跑。
  耗时：每个版本大约 45–70 分钟（X-07 若崩溃会更短；比旧版多一次 X-02 解析取消和一次小文件中文路径）
#>
param(
  [Parameter(Mandatory)] [string]$Tag,
  [Parameter(Mandatory)] [string]$Build,
  [string]$Samples = "C:\sl\samples",
  [string]$CnDir = "C:\sl\样例 目录",
  [switch]$Quick        # 只跑 X-07 一次（先确认崩不崩）
)
$ErrorActionPreference = "Stop"
# 7.2 起如果打开这个开关，外部程序退出码不是 0 会直接变成终止错误，后面的中文说明就出不来。
# Windows PowerShell 5.1 没有这个变量，设一下也无害。
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
  $PSNativeCommandUseErrorActionPreference = $false
}

$ScrewUrl = "https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/V7_9_3/data/step/screw.step"
$ScrewSha = "4B3649A4F5C4F05C7A06A402A91FE2FD7E3CBA1615520FBD8C62A62610AD3E69"

function Join-Under([string[]]$parts) {
  $path = $parts[0]
  for ($i = 1; $i -lt $parts.Count; $i++) {
    $path = Join-Path $path $parts[$i]
  }
  return $path
}

function Add-EnvLine([string]$line) {
  $utf8 = New-Object System.Text.UTF8Encoding $false
  [IO.File]::AppendAllText((Join-Path $out "env.txt"), ($line + "`r`n"), $utf8)
}

# PowerShell 5.1 的 Start-Process -ArgumentList 会把数组用空格拼起来，自己不加引号。
# 路径里有空格或中文时会被拆开。这里按 Windows 命令行规则给每一段加上引号。
function Quote-ProcessArgument([string]$text) {
  if ($null -eq $text) { $text = "" }
  $escaped = $text -replace '(\\*)"', '$1$1\"'
  $escaped = $escaped -replace '(\\+)$', '$1$1'
  return '"' + $escaped + '"'
}

function Read-ResultKey([string]$path, [string]$key) {
  if (-not (Test-Path -LiteralPath $path)) { return $null }
  $prefix = $key + "="
  foreach ($line in @(Get-Content -LiteralPath $path)) {
    if ($null -ne $line -and $line.StartsWith($prefix)) {
      return $line.Substring($prefix.Length).Trim()
    }
  }
  return $null
}

function Read-Exact([IO.FileStream]$fs, [int]$count) {
  $buf = New-Object byte[] $count
  $got = 0
  while ($got -lt $count) {
    $n = $fs.Read($buf, $got, $count - $got)
    if ($n -le 0) { return $null }
    $got += $n
  }
  return ,$buf
}

function Read-U16At([IO.FileStream]$fs, [int64]$pos) {
  $fs.Position = $pos
  $buf = Read-Exact $fs 2
  if ($null -eq $buf) { return $null }
  return [BitConverter]::ToUInt16($buf, 0)
}

function Read-U32At([IO.FileStream]$fs, [int64]$pos) {
  $fs.Position = $pos
  $buf = Read-Exact $fs 4
  if ($null -eq $buf) { return $null }
  return [int64][BitConverter]::ToUInt32($buf, 0)
}

function Read-I32At([IO.FileStream]$fs, [int64]$pos) {
  $fs.Position = $pos
  $buf = Read-Exact $fs 4
  if ($null -eq $buf) { return $null }
  return [BitConverter]::ToInt32($buf, 0)
}

# 读 PE 导入表里的 DLL 名字。不是 PE（或读失败）就返回空数组，调用方只检查 spike.pdb。
function Get-PeImportDlls([string]$path) {
  $names = @()
  $fs = $null
  try {
    $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    if ($fs.Length -lt 64) { return }
    $mz = Read-U16At $fs 0
    if ($null -eq $mz -or $mz -ne 0x5A4D) { return }
    $peOff = Read-I32At $fs 0x3C
    if ($null -eq $peOff -or $peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return }
    $sig = Read-U32At $fs $peOff
    if ($null -eq $sig -or $sig -ne 0x4550) { return }
    $sections = Read-U16At $fs ($peOff + 6)
    $optSize = Read-U16At $fs ($peOff + 20)
    if ($null -eq $sections -or $null -eq $optSize -or $optSize -lt 96) { return }
    $optStart = [int64]$peOff + 24
    $magic = Read-U16At $fs $optStart
    $ddStart = [int64]0
    if ($magic -eq 0x20B) {
      $ddStart = $optStart + 112
    } elseif ($magic -eq 0x10B) {
      $ddStart = $optStart + 96
    } else {
      return
    }
    $nRva = Read-U32At $fs ($ddStart - 4)
    if ($null -eq $nRva -or $nRva -lt 2) { return }
    $impRva = Read-U32At $fs ($ddStart + 8)
    if ($null -eq $impRva -or $impRva -eq 0) { return }

    $secStart = $optStart + $optSize
    $secList = @()
    for ($i = 0; $i -lt $sections; $i++) {
      $base = $secStart + (40 * $i)
      if (($base + 24) -gt $fs.Length) { break }
      $vsize = Read-U32At $fs ($base + 8)
      $va = Read-U32At $fs ($base + 12)
      $rawSize = Read-U32At $fs ($base + 16)
      $rawPtr = Read-U32At $fs ($base + 20)
      $secList += [pscustomobject]@{ VA = $va; VSize = $vsize; Raw = $rawPtr; RawSize = $rawSize }
    }

    $off = [int64]-1
    foreach ($s in $secList) {
      $span = $s.VSize
      if ($s.RawSize -gt $span) { $span = $s.RawSize }
      if ($span -le 0) { continue }
      if ($impRva -ge $s.VA -and $impRva -lt ($s.VA + $span)) {
        $off = $s.Raw + ($impRva - $s.VA)
        break
      }
    }
    if ($off -lt 0) { return }

    for ($n = 0; $n -lt 4096; $n++) {
      if (($off + 20) -gt $fs.Length) { break }
      $oft = Read-U32At $fs $off
      $td = Read-U32At $fs ($off + 4)
      $fwd = Read-U32At $fs ($off + 8)
      $nameRva = Read-U32At $fs ($off + 12)
      $ft = Read-U32At $fs ($off + 16)
      if ($null -eq $nameRva) { break }
      if ($oft -eq 0 -and $td -eq 0 -and $fwd -eq 0 -and $nameRva -eq 0 -and $ft -eq 0) { break }
      if ($nameRva -ne 0) {
        $nameOff = [int64]-1
        foreach ($s in $secList) {
          $span = $s.VSize
          if ($s.RawSize -gt $span) { $span = $s.RawSize }
          if ($span -le 0) { continue }
          if ($nameRva -ge $s.VA -and $nameRva -lt ($s.VA + $span)) {
            $nameOff = $s.Raw + ($nameRva - $s.VA)
            break
          }
        }
        if ($nameOff -ge 0 -and $nameOff -lt $fs.Length) {
          # 不用一次读 1 字节再放进管道：PowerShell 会把长度为 1 的 byte[] 拆开，0 字节会被吃掉。
          $fs.Position = $nameOff
          $raw = New-Object byte[] 260
          $nread = $fs.Read($raw, 0, 260)
          $end = 0
          while ($end -lt $nread -and $raw[$end] -ne 0) { $end++ }
          if ($end -gt 0) {
            $names += [Text.Encoding]::ASCII.GetString($raw, 0, $end)
          }
        }
      }
      $off += 20
    }
  } catch {
    return
  } finally {
    if ($null -ne $fs) { $fs.Dispose() }
  }
  # 逐个输出。return ,$array 会让调用方的 foreach 拿到「一个数组」而不是每个 DLL 名。
  return $names
}

function Resolve-DllName([string]$name, [string[]]$dirs) {
  foreach ($dir in $dirs) {
    $candidate = Join-Path $dir $name
    if (Test-Path -LiteralPath $candidate) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  return $null
}

function Find-NamedPdb([string]$leaf, [string[]]$dirs) {
  foreach ($dir in $dirs) {
    $candidate = Join-Path $dir $leaf
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return $null
}

function Get-MissingPdbs([string]$exePath, [string[]]$dirs) {
  $missing = @()
  $spikeLeaf = [IO.Path]::GetFileNameWithoutExtension($exePath) + ".pdb"
  if ($null -eq (Find-NamedPdb $spikeLeaf $dirs)) { $missing += $spikeLeaf }

  $queue = @($exePath)
  $seen = @{}
  while ($queue.Count -gt 0) {
    $mod = [string]$queue[0]
    if ($queue.Count -le 1) { $queue = @() } else { $queue = @($queue[1..($queue.Count - 1)]) }
    if ($seen.ContainsKey($mod)) { continue }
    $seen[$mod] = $true
    foreach ($dll in @(Get-PeImportDlls $mod)) {
      if ([string]::IsNullOrWhiteSpace($dll)) { continue }
      if ($dll -notmatch '^(?i)TK.+\.dll$') { continue }
      $pdbLeaf = [IO.Path]::GetFileNameWithoutExtension($dll) + ".pdb"
      $resolved = Resolve-DllName $dll $dirs
      $pdbDirs = @($dirs)
      if ($resolved) {
        $pdbDirs = @((Split-Path -Parent $resolved)) + @($dirs)
        if (-not $seen.ContainsKey($resolved)) { $queue += $resolved }
      }
      if ($null -eq (Find-NamedPdb $pdbLeaf $pdbDirs)) { $missing += $pdbLeaf }
    }
  }
  return @($missing | Sort-Object -Unique)
}

function Run([string]$id, [string[]]$a) {
  $t0 = Get-Date
  Write-Host ("== {0}  {1:HH:mm:ss}" -f $id, $t0)
  $quoted = @()
  foreach ($item in $a) { $quoted += (Quote-ProcessArgument ([string]$item)) }
  $argLine = $quoted -join " "
  $outTxt = Join-Path $out "$id.txt"
  $outErr = Join-Path $out "$id.err"
  $p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $out -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $outTxt -RedirectStandardError $outErr
  if (-not $p.HasExited) { $null = $p.WaitForExit() }
  try { $p.Refresh() } catch { }
  $code = $p.ExitCode
  if ($null -eq $code) { $code = -1 }
  $wall = ((Get-Date) - $t0).TotalSeconds
  $wallText = $wall.ToString("0.0", [Globalization.CultureInfo]::InvariantCulture)
  $utf8 = New-Object System.Text.UTF8Encoding $false
  [IO.File]::AppendAllText($outTxt, ("exit={0}`r`nwall_s={1}`r`n" -f $code, $wallText), $utf8)
  $dump = Join-Path $out "spike-crash.dmp"
  if (Test-Path -LiteralPath $dump) {
    Move-Item -LiteralPath $dump -Destination (Join-Path $out "$id.dmp") -Force
  }
  if ($code -ne 0) {
    Write-Host ("   exit=0x{0:X8}  见 {1}" -f $code, $outErr) -ForegroundColor Yellow
  }
}

function Get-ScrewStep {
  if (-not (Test-Path -LiteralPath $Samples)) {
    New-Item -ItemType Directory -Force -Path $Samples | Out-Null
  }
  $dest = Join-Path $Samples "screw.step"
  $hashOk = $false
  if (Test-Path -LiteralPath $dest) {
    $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    if ($hash -eq $ScrewSha) { $hashOk = $true }
  }
  if (-not $hashOk) {
    Write-Host "正在下载 screw.step（中文路径测试用的小样例）..."
    & curl.exe -L --fail --retry 2 -o $dest $ScrewUrl
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dest)) {
      if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force }
      return $null
    }
    $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    if ($hash -ne $ScrewSha) { return $null }
  }
  return $dest
}

# 开发机点源上面的函数做自测时设 STEPLENS_T02B_DOTSOURCE=1。老板用 -File 运行不会设置它。
function Start-T02bRun {
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Under @($root, $Build, "RelWithDebInfo", "spike.exe")
$compatExe = Join-Under @($root, $Build, "RelWithDebInfo", "test_compat.exe")
$occtBin = Join-Under @($root, $Build, "vcpkg_installed", "x64-windows-release", "bin")
$out  = Join-Under @($root, "results", $Tag)
New-Item -ItemType Directory -Force -Path $out | Out-Null

$exeDir = Split-Path -Parent $exe
$env:PATH = $exeDir + [IO.Path]::PathSeparator + $occtBin + [IO.Path]::PathSeparator + $env:PATH
$env:_NT_SYMBOL_PATH = $exeDir + [IO.Path]::PathSeparator + $occtBin

if (-not (Test-Path -LiteralPath $exe)) {
  Write-Host "找不到 spike.exe：$exe" -ForegroundColor Red
  Write-Host "请先按清单第 5 步或第 7 步编译。这一版的测试不会开始。" -ForegroundColor Red
  exit 1
}

$searchDirs = @($exeDir, $occtBin)
$missingPdbs = @(Get-MissingPdbs $exe $searchDirs)
if ($missingPdbs.Count -gt 0) {
  $pdbLine = "pdb_missing=" + ($missingPdbs -join ",")
  Add-EnvLine $pdbLine
  Write-Host ("警告：缺少符号文件 {0}。" -f ($missingPdbs -join "、")) -ForegroundColor Yellow
  Write-Host "如果测试进程崩溃，调用栈里不会有函数名，只剩地址，后面不好查。测试仍会继续。请不要删掉 exe 和 OCCT DLL 旁边的 pdb。" -ForegroundColor Yellow
}

if (-not (Test-Path -LiteralPath $compatExe)) {
  Write-Host "找不到 test_compat.exe：$compatExe" -ForegroundColor Red
  Write-Host "请先按清单第 5 步或第 7 步编译。后面的测试不会跑。" -ForegroundColor Red
  exit 1
}

# 0) 版本与兼容单测。退出码不是 0 就停，避免清单和实际 OCCT 版本对不上还继续跑。
$compatRaw = & $compatExe
$compatCode = $LASTEXITCODE
$compatLines = @()
if ($null -ne $compatRaw) {
  $compatLines = @($compatRaw | Where-Object { $null -ne $_ })
}
$compatText = ""
if ($compatLines.Count -gt 0) { $compatText = ($compatLines -join "`r`n") + "`r`n" }
$compatText += "exit=$compatCode`r`n"
$utf8 = New-Object System.Text.UTF8Encoding $false
[IO.File]::WriteAllText((Join-Path $out "test_compat.txt"), $compatText, $utf8)
foreach ($line in $compatLines) { Write-Host $line }
if ($null -eq $compatCode -or $compatCode -ne 0) {
  Write-Host "兼容单测 test_compat.exe 没有通过，已停止。后面的读取、取消和中文路径测试都不会跑。" -ForegroundColor Red
  Write-Host "退出码：$compatCode 。多半是 OCCT 的 vcpkg 清单或 CMake 预设配错了。" -ForegroundColor Red
  Write-Host "test_compat 对比的是编译时期望的版本（-DSTEPLENS_EXPECTED_OCCT，8.0.1 或 7.9.3）和实际链接到的 OCCT 版本，对不上就会失败。" -ForegroundColor Red
  Write-Host "请核对这一次用的 vcpkg.json（8.0.1）或 occt-7.9.3\vcpkg.json（7.9.3），以及 cmake 的 -DSTEPLENS_EXPECTED_OCCT、-DVCPKG_MANIFEST_DIR 是不是同一套，然后重新编译。不要改别的配置。" -ForegroundColor Red
  if ($compatLines.Count -gt 0) {
    Write-Host "test_compat 输出里应有 occt_version=实际版本 expected=期望版本，把 results\$Tag\test_compat.txt 发回团队。" -ForegroundColor Red
  }
  exit $compatCode
}

$S = @{ X07 = (Join-Path $Samples "PX13226.IGS"); X02 = (Join-Path $Samples "Scania-Engine-V8-XT-Turbo.step");
        L03 = (Join-Path $Samples "NissanGT-R.STEP"); X01 = (Join-Path $Samples "Scania-8x4.stp");
        X06 = (Join-Path $Samples "1026064M.IGS");   X08 = (Join-Path $Samples "1027247E.IGS") }

# 1) 超大 IGES：X-07（关键判定项）
Run "X-07" @("bench", $S.X07)
if ($Quick) { return }
# 2) X-06 / X-08（可选，文件存在才跑）
foreach ($k in "X06","X08") { if (Test-Path -LiteralPath $S[$k]) { Run ($k.Insert(1,"-")) @("bench", $S[$k]) } }
# 3) STEP 性能：X-02、L-03 各 3 次，X-01 1 次（第 1 次含冷读）
foreach ($i in 1..3) { Run "X-02-r$i" @("bench", $S.X02); Run "L-03-r$i" @("bench", $S.L03) }
Run "X-01" @("bench", $S.X01)
# 4) 取消：X-02 读取阶段 10/50/90%；文件读完后的解析盲区；转换阶段 20/50/80%
foreach ($f in "0.1","0.5","0.9") { Run "cancel-read-X02-$f" @("cancel-read", $S.X02, $f) }
Run "cancel-parse-X02" @("cancel-parse", $S.X02, "2")

$transfer = $null
$transferFrom = $null
foreach ($i in 1,2,3) {
  $id = "X-02-r$i"
  $raw = Read-ResultKey (Join-Path $out "$id.txt") "transfer_time_s"
  if ([string]::IsNullOrWhiteSpace($raw)) { continue }
  $parsed = 0.0
  $styles = [Globalization.NumberStyles]::Float
  $culture = [Globalization.CultureInfo]::InvariantCulture
  $okParse = [double]::TryParse($raw, $styles, $culture, [ref]$parsed)
  if (-not $okParse) { continue }
  $transfer = $parsed
  $transferFrom = $id
  break
}
if ($null -eq $transfer) {
  Add-EnvLine "skipped=transfer_cancel"
  Write-Host "警告：X-02 三次运行都没有 transfer_time_s（可能崩溃或读取失败），已跳过转换阶段取消测试。" -ForegroundColor Yellow
} else {
  if ($transferFrom -ne "X-02-r1") {
    $shown = $transfer.ToString([Globalization.CultureInfo]::InvariantCulture)
    Add-EnvLine ("transfer_time_s_from={0}" -f $transferFrom)
    Add-EnvLine ("transfer_time_s={0}" -f $shown)
    Write-Host ("转换取消改用 {0} 的 transfer_time_s={1}（第 1 次结果里没有这个数）。" -f $transferFrom, $shown) -ForegroundColor Yellow
  }
  foreach ($p in 0.2,0.5,0.8) {
    $seconds = [math]::Round($transfer * $p, 1)
    $secondsText = $seconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    Run "cancel-transfer-X02-$p" @("cancel-transfer", $S.X02, $secondsText)
  }
}

# 5) 中文目录 + 空格。样例是 OCCT 自带的 screw.step，体积很小。
$screw = Get-ScrewStep
if ($null -eq $screw) {
  Add-EnvLine "skipped=cn_path"
  Write-Host "警告：没有拿到校验通过的 screw.step，已跳过中文路径测试。请检查网络，或按清单第 4 步重新下载。" -ForegroundColor Yellow
} else {
  New-Item -ItemType Directory -Force -Path $CnDir | Out-Null
  $cnFile = Join-Path $CnDir "screw.step"
  Copy-Item -LiteralPath $screw -Destination $cnFile -Force
  Run "cn-path" @("bench", $cnFile)
}
Write-Host "== 完成：$out"
}

if ($env:STEPLENS_T02B_DOTSOURCE -ne "1") {
  Start-T02bRun
}
