# T09 最小视口冒烟：团队 Windows 独显机器

G1-5 的像素验收在这台机器上做。CI 只保证编译，并且在 `QT_QPA_PLATFORM=offscreen` 下确认进程还活着。无头 runner 没有可判定的 GPU 帧缓冲，**缺画面不得让 CI 变红**。

默认路径：`QOpenGLWidget` + `Aspect_NeutralWindow` + `OpenGl_FrameBuffer::InitWrapper`，中央控件里一个旋转的方块（蓝灰底，不是纯黑）。窗口尺寸取 FBO 视口，单位是物理像素，所以 100% 和 150% DPI 都按帧缓冲大小送给 OCCT。

备选路径：CMake `-DSTEPLENS_USE_WNT_WINDOW=ON`，改用 `WNT_Window` 包住 `winId()`。实现和默认路径在同一个 `ViewerWidget` 里，编译期切换。

## 日志和截图放哪

全部放在 `docs/bench/t09-gpu-smoke/`（这个目录随结果一起提交）：

| 文件 | 内容 |
|---|---|
| `dpi100-log.txt` | 100% 缩放下脚本的通过/失败行 |
| `dpi150-log.txt` | 150% 缩放下同样的日志 |
| `dpi100-frame-*.png` | 100% 的逐帧截图 |
| `dpi150-frame-*.png` | 150% 的逐帧截图 |
| `switch-note.md` | 只有改走 `WNT_Window` 时才写：为什么 NeutralWindow 路径在 24 小时内做不到 |

通过条件：每一帧都不是单色图，也不是全黑。脚本会抽样像素，单色或全黑记 `FAIL`。

## 机器

- Windows 10/11，有独显（记录 `Win32_VideoController` 的名字和驱动版本）。
- 已按仓库预设编出 `build\msvc\RelWithDebInfo\StepLens.exe`（`cmake --preset msvc`，`cmake --build --preset release`）。
- 不要设 `QT_QPA_PLATFORM=offscreen`。那个值会跳过三维视图，只留给 CI。

## 步骤

每个缩放各做一遍。先在「设置 → 系统 → 显示 → 缩放与布局」里改成目标缩放，注销或重启资源管理器后重新登录，再开一个新的 PowerShell。

### 1. 记环境和缩放

```powershell
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | Format-List
Get-ItemProperty "HKCU:\Control Panel\Desktop\WindowMetrics" | Select-Object AppliedDPI
```

`AppliedDPI`：96 是 100%，144 是 150%。不对就停，先改系统缩放。

### 2. 跑 50 次 resize + 10 次最大化/还原，并逐帧截图

在仓库根目录执行。把 `$DpiTag` 改成 `dpi100` 或 `dpi150`。脚本会启动 `StepLens.exe`，对主窗口做 50 次改变大小、10 次最大化/还原，每次用 `PrintWindow`（`PW_RENDERFULLCONTENT`）截客户区。

```powershell
$DpiTag = "dpi100"   # 150% 时改成 dpi150
$Root = "C:\path\to\StepLens"   # 仓库根
$Exe = Join-Path $Root "build\msvc\RelWithDebInfo\StepLens.exe"
$Out = Join-Path $Root "docs\bench\t09-gpu-smoke"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Log = Join-Path $Out "$DpiTag-log.txt"
"dpi_tag=$DpiTag" | Set-Content -Encoding utf8 $Log
$env:PATH = "$(Join-Path $Root 'build\msvc\vcpkg_installed\x64-windows\bin');$env:PATH"
$env:QT_PLUGIN_PATH = Join-Path $env:QTDIR "plugins"
if ($env:QT_QPA_PLATFORM -eq "offscreen") { throw "不要在 offscreen 下做像素验收" }

Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class SlGrab {
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
      for (int y = 0; y < h && single; y += stepY)
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

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 3
if ($proc.HasExited) { throw "StepLens 提前退出，代码 $($proc.ExitCode)" }
$hwnd = $proc.MainWindowHandle
$i = 0
$fail = 0
function Grab([string]$name) {
  $script:i++
  $path = Join-Path $Out ("{0}-frame-{1:D3}-{2}.png" -f $DpiTag, $script:i, $name)
  $reason = ""
  $ok = [SlGrab]::Save($hwnd, $path, [ref]$reason)
  $line = "{0} {1}" -f $(if ($ok) {"PASS"} else {"FAIL"}), $reason
  Add-Content -Encoding utf8 $Log $line
  if (-not $ok) { $script:fail++ }
}
1..50 | ForEach-Object {
  $w = 640 + ($_ % 10) * 40
  $h = 480 + ($_ % 7) * 30
  [void][SlGrab]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, $w, $h, 0x0040)
  Start-Sleep -Milliseconds 120
  Grab ("resize{0}" -f $_)
}
1..10 | ForEach-Object {
  [void][SlGrab]::ShowWindow($hwnd, 3)   # SW_MAXIMIZE
  Start-Sleep -Milliseconds 200
  Grab ("max{0}" -f $_)
  [void][SlGrab]::ShowWindow($hwnd, 9)   # SW_RESTORE
  Start-Sleep -Milliseconds 200
  Grab ("restore{0}" -f $_)
}
Stop-Process -Id $proc.Id -Force
"fails=$fail" | Add-Content -Encoding utf8 $Log
if ($fail -ne 0) { throw "$DpiTag 有 $fail 帧不合格，看 $Log" }
```

通过：`$Log` 里没有 `FAIL`，`fails=0`，并且 `docs/bench/t09-gpu-smoke/` 下有对应 png。两档缩放都要这样。

### 3. 24 小时内做不到时

改走 `WNT_Window`，不要空着结果：

```powershell
cmake --preset msvc -DSTEPLENS_USE_WNT_WINDOW=ON
cmake --build --preset release
```

然后重跑第 2 步。在 `docs/bench/t09-gpu-smoke/switch-note.md` 写明：NeutralWindow 路径失败的现象（全黑、单色、崩溃、InitWrapper 日志）、驱动和 DPI，以及切换后的 `fails=`。
