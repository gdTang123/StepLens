# 两个 DPI 各运行一次。只能使用普通 Windows PowerShell。
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw '本脚本只能在有桌面的 Windows 独显电脑上运行。' }
if ([IntPtr]::Size -ne 8 -or $PSVersionTable.PSEdition -eq 'Core') { throw '请从开始菜单打开 Windows PowerShell（不要选 x86，也不要使用 PowerShell 7 / pwsh）。' }
$Root = $PSScriptRoot
if (-not (Test-Path "$Root\CMakePresets.json")) { throw '请把 2-smoke.ps1 放在 StepLens 仓库根目录，与 CMakePresets.json 同级；不要放在下载、桌面或 scripts 子目录。' }
$Exe = $null
foreach ($relative in @('build\msvc\src\app\RelWithDebInfo\StepLens.exe','build\msvc\RelWithDebInfo\StepLens.exe')) {
    $candidate = Join-Path $Root $relative
    if (Test-Path $candidate) { $Exe = $candidate; break }
}
if (-not $Exe) { throw '没找到 RelWithDebInfo 版 StepLens.exe。请先在仓库根目录完成 cmake --preset msvc 和 cmake --build --preset release。默认实际输出在 build\msvc\src\app\RelWithDebInfo，不要根据旧说明猜路径。' }
if (-not $env:QTDIR) { $env:QTDIR = [Environment]::GetEnvironmentVariable('QTDIR','User') }
if (-not $env:QTDIR) { $env:QTDIR = [Environment]::GetEnvironmentVariable('QTDIR','Machine') }
# 注销后新开窗口时，也可从本次 CMake 配置恢复 Qt 的真实目录。
if (-not $env:QTDIR -and (Test-Path "$Root\build\msvc\CMakeCache.txt")) {
    foreach ($line in Get-Content "$Root\build\msvc\CMakeCache.txt") {
        if ($line -match '^Qt6_DIR:[^=]+=(.+)$') {
            $qtRoot = Join-Path $Matches[1] '..\..\..'
            if (Test-Path $qtRoot) { $env:QTDIR = (Resolve-Path $qtRoot).Path }
        }
    }
}
if (-not $env:QTDIR -or -not (Test-Path "$env:QTDIR\bin\Qt6Core.dll") -or -not (Test-Path "$env:QTDIR\plugins\platforms\qwindows.dll")) { throw 'QTDIR 未设置或 Qt 目录不完整。请按操作说明的 Qt 检查步骤，在此窗口重新设置 QTDIR；应指向含 bin\Qt6Core.dll 和 plugins\platforms\qwindows.dll 的 MSVC x64 Qt 目录。' }
$occt = Join-Path $Root 'build\msvc\vcpkg_installed\x64-windows'
# 兼容显式指定 VCPKG_INSTALLED_DIR 的配置，优先读取当前 CMake 缓存。
$cachefile = Join-Path $Root 'build\msvc\CMakeCache.txt'
$installed = $null
$triplet = 'x64-windows'
if (Test-Path $cachefile) {
    foreach ($line in Get-Content $cachefile) {
        if ($line -match '^VCPKG_INSTALLED_DIR:[^=]+=(.+)$') { $installed = $Matches[1] }
        if ($line -match '^VCPKG_TARGET_TRIPLET:[^=]+=(.+)$') { $triplet = $Matches[1] }
    }
}
if ($installed) { $occt = Join-Path $installed $triplet }
if (-not (Test-Path "$occt\bin\TKOpenGl.dll")) { throw "没找到当前构建的 OCCT 运行库：$occt\bin\TKOpenGl.dll。请先确认配置和编译都成功；不要复制 T02b 8.0.1 的 DLL。" }
$commit = '未读取到（请同时提供 git rev-parse HEAD 输出）'
if (Get-Command git.exe -ErrorAction SilentlyContinue) {
    $gitHead = & git.exe -C $Root rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $commit = "$gitHead" }
}
$info = [pscustomobject]@{ Commit=$commit; QtDir=$env:QTDIR; OcctPrefix=$occt; Exe=$Exe; ExeSHA256=(Get-FileHash $Exe -Algorithm SHA256).Hash; WindowPath='请对照 CMakeCache 中 STEPLENS_USE_WNT_WINDOW，默认 OFF' }
if ($env:QT_QPA_PLATFORM -and $env:QT_QPA_PLATFORM -ne 'windows') { throw "QT_QPA_PLATFORM 当前是 $env:QT_QPA_PLATFORM，不能做像素验收。在同一窗口执行 Remove-Item Env:QT_QPA_PLATFORM 后重跑。" }
function Get-T09Desktop {
    $desktop = [Environment]::GetFolderPath('Desktop')
    if (-not $desktop -or -not (Test-Path $desktop)) { throw '找不到当前用户桌面目录，请确认已正常登录 Windows。' }
    return $desktop
}
$dpi = (Get-ItemProperty 'HKCU:\Control Panel\Desktop\WindowMetrics' -ErrorAction Stop).AppliedDPI
switch ($dpi) {
    96 { $DpiTag = 'dpi100' }
    144 { $DpiTag = 'dpi150' }
    default { throw "AppliedDPI=$dpi，不是 96（100%）或 144（150%）。设置→系统→屏幕/显示→缩放，选择 100% 或 150%，注销重登后再开新的 Windows PowerShell。不要只关闭窗口，也不要只重启资源管理器。" }
}
Write-Host "本轮：$DpiTag，AppliedDPI=$dpi。请保持 StepLens 可见，不要锁屏、遮挡、拖动窗口或远程断开。"
$Out = Join-Path $Root 'docs\bench\t09-gpu-smoke'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$old = @(Get-ChildItem $Out -File -Filter "$DpiTag-*")
if ($old.Count -gt 0) {
    $history = Join-Path $Out ("history-{0}-{1}" -f $DpiTag,(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $history | Out-Null
    $old | Move-Item -Destination $history
}
$Log = Join-Path $Out "$DpiTag-log.txt"
"dpi_tag=$DpiTag`r`nAppliedDPI=$dpi`r`ncommit=$($info.Commit)`r`nexe_sha256=$($info.ExeSHA256)`r`nwindow_path=see_CMakeCache_below`r`ntime_shanghai=$([DateTimeOffset]::Now.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy-MM-dd HH:mm:ss zzz'))" | Set-Content -Encoding utf8 $Log
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | Format-List | Out-String | Add-Content -Encoding utf8 $Log
$info | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Out "$DpiTag-build-info.json")
if (Test-Path $cachefile) { Select-String -Path $cachefile -Pattern '^(STEPLENS_USE_WNT_WINDOW|STEPLENS_EXPECTED_OCCT|CMAKE_BUILD_TYPE|CMAKE_CONFIGURATION_TYPES|CMAKE_GENERATOR|VCPKG_TARGET_TRIPLET):' | ForEach-Object { $_.Line } | Add-Content -Encoding utf8 $Log }
$env:QTDIR = $info.QtDir
$env:PATH = "$(Split-Path $Exe -Parent);$($info.QtDir)\bin;$($info.OcctPrefix)\bin;$env:PATH"
$env:QT_PLUGIN_PATH = Join-Path $info.QtDir 'plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $info.QtDir 'plugins\platforms'
$proc = $null
$script:i = 0
$script:fail = 0
$success = $false
$problem = $null
try {
    if (-not ('SlGrab' -as [type])) {
        Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class SlGrab {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr context);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static bool Save(IntPtr hwnd, string path, out string reason) {
    RECT rc; GetClientRect(hwnd, out rc);
    int w = rc.R - rc.L, h = rc.B - rc.T;
    if (w < 32 || h < 32) { reason = "client too small"; return false; }
    using (var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
      using (var g = Graphics.FromImage(bmp)) {
        IntPtr hdc = g.GetHdc();
        bool ok = PrintWindow(hwnd, hdc, 2);
        g.ReleaseHdc(hdc);
        if (!ok) { reason = "PrintWindow failed"; return false; }
      }
      int stepX = Math.Max(1, w / 16), stepY = Math.Max(1, h / 16);
      Color first = bmp.GetPixel(w / 2, h / 2);
      bool single = true, allBlack = true;
      for (int y = 0; y < h; y += stepY)
        for (int x = 0; x < w; x += stepX) {
          Color c = bmp.GetPixel(x, y);
          if (c.ToArgb() != first.ToArgb()) single = false;
          if (c.R != 0 || c.G != 0 || c.B != 0) allBlack = false;
        }
      bmp.Save(path, ImageFormat.Png);
      if (allBlack) { reason = "all black"; return false; }
      if (single) { reason = "single color"; return false; }
      reason = "ok";
      return true;
    }
  }
}
"@
    }
    try { [void][SlGrab]::SetProcessDpiAwarenessContext([IntPtr](-4)) } catch { Write-Host '此系统无法设置脚本 DPI 感知，保留系统设置；记录后继续。'; "dpi_awareness_warning=$($_.Exception.Message)" | Add-Content -Encoding utf8 $Log }
    $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe -Parent) -PassThru
    for ($wait = 0; $wait -lt 100; $wait++) {
        Start-Sleep -Milliseconds 100
        $proc.Refresh()
        if ($proc.HasExited) { throw "StepLens 提前退出，代码 $($proc.ExitCode)。请拍下缺 DLL、Qt 平台插件或崩溃弹窗。" }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    $hwnd = $proc.MainWindowHandle
    if ($hwnd -eq [IntPtr]::Zero) { throw '等待 10 秒仍没有 StepLens 主窗口。请拍下屏幕并发送日志。' }
    Start-Sleep -Seconds 3
    Write-Host '现在请看 StepLens 中央区域：应出现蓝灰背景和旋转方块。脚本马上自动调整大小，共截 70 帧。'
    Start-Sleep -Seconds 5
    function Grab([string]$name) {
        if ($proc.HasExited) { throw 'StepLens 在截图中途退出。' }
        $script:i++
        $path = Join-Path $Out ('{0}-frame-{1:D3}-{2}.png' -f $DpiTag, $script:i, $name)
        $reason = ''
        $ok = [SlGrab]::Save($hwnd, $path, [ref]$reason)
        $line = '{0} {1} {2}' -f $(if ($ok) {'PASS'} else {'FAIL'}), $name, $reason
        $line | Add-Content -Encoding utf8 $Log
        Write-Host ("[{0}/70] {1}" -f $script:i, $line)
        if (-not $ok) { $script:fail++ }
    }
    1..50 | ForEach-Object {
        $w = 640 + ($_ % 10) * 40
        $h = 480 + ($_ % 7) * 30
        if (-not [SlGrab]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, $w, $h, 0x0040)) { throw '改变窗口大小失败，停止验收。' }
        Start-Sleep -Milliseconds 120
        Grab ('resize{0}' -f $_)
    }
    1..10 | ForEach-Object {
        [void][SlGrab]::ShowWindow($hwnd, 3)
        Start-Sleep -Milliseconds 200
        Grab ('max{0}' -f $_)
        [void][SlGrab]::ShowWindow($hwnd, 9)
        Start-Sleep -Milliseconds 200
        Grab ('restore{0}' -f $_)
    }
    "frames=$script:i`r`nfails=$script:fail" | Add-Content -Encoding utf8 $Log
    if ($script:i -ne 70 -or $script:fail -ne 0) { throw "$DpiTag 失败：已截图 $script:i/70，失败 $script:fail 帧。" }
    $visual = Read-Host '刚才确实看到蓝灰背景上的旋转方块，且没有黑屏/纯色闪帧吗？是则输入 YES 后回车；否则输入 NO 后回车'
    "visual_confirm=$visual" | Add-Content -Encoding utf8 $Log
    if ($visual -notmatch '^\s*yes\s*$') { throw '人工画面确认未通过。即使 fails=0，也不能视为本轮通过。' }
    $success = $true
} catch {
    $problem = $_.Exception.Message
    "ERROR=$problem`r`nincomplete_or_failed=true`r`nframes_so_far=$script:i`r`nfailed_frames_so_far=$script:fail" | Add-Content -Encoding utf8 $Log
    Write-Host "本轮未通过：$problem" -ForegroundColor Red
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    $Zip = Join-Path (Get-T09Desktop) "t09-result-$DpiTag.zip"
    if (Test-Path $Zip) { Move-Item $Zip ($Zip + '.' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.bak') }
    try {
        Compress-Archive -Path "$Out\*" -DestinationPath $Zip -CompressionLevel Optimal
        Write-Host "结果已打包到桌面：$Zip"
    } catch {
        $success = $false
        $problem = "压缩结果失败：$($_.Exception.Message)；请直接发送 $Out 文件夹。"
        Write-Host $problem -ForegroundColor Red
    }
}
if ($success) {
    Write-Host "通过 fails=0；$DpiTag 共 70 帧，人工画面确认 YES。" -ForegroundColor Green
    Write-Host '若这是 100%：继续操作说明中的 150% 一轮；若两轮都已做完：把桌面两个 t09-result-*.zip 发回聊天。'
} else {
    throw "未通过：失败帧数 $script:fail；已执行 $script:i/70 帧；$problem。请发桌面本轮 zip 和报错截图，不要自行切换 WNT_Window。"
}
