# T09 独显冒烟结果

老板机上的两轮实机结果。G1-5 记为**通过（老板实机确认）**。逐帧截图和原始日志在下面两个目录里，文件名保持实机原样，没有重命名，也没有重新生成截图。

| 项 | 两轮相同的记录 |
|---|---|
| 机器 | 老板的 Windows 电脑。独立显卡 NVIDIA GeForce RTX 4070，驱动 32.0.16.1088 |
| 代码 | `c22bbbe`（`c22bbbe480637cd574f074c8d740f69f7f56f489`） |
| 构建 | OCCT 7.9.3，`STEPLENS_USE_WNT_WINDOW=OFF` |
| 帧数 | 每轮 70 帧（50 次改变大小 + 10 次最大化/还原） |
| 脚本抽样 | 两轮都是 `fails=0` |

日志里同时还列着 Intel(R) UHD Graphics 770，驱动 32.0.101.7088。独显记录是 RTX 4070。

## 100%：`run-dpi100/`

2026-11-08 19:36:10（+08:00）。`AppliedDPI=96`，文件名标签 `dpi100`，和 100% 缩放一致。70 帧，`fails=0`，日志里有一行 `visual_confirm=yes`。

同一份 `dpi100-log.txt` 在这行之后还有 `ERROR=人工画面确认未通过` 和 `incomplete_or_failed=true`。窗口里当时输入的是 `yes`。当时那版脚本把确认词做成大小写敏感，`yes` 过不了精确匹配，于是在已经写下 `visual_confirm=yes` 之后又追加了一笔失败。入库的 [`scripts/bench/t09-smoke.ps1`](../../../scripts/bench/t09-smoke.ps1) 已改成不区分大小写。这一轮是否算通过，跟 150% 那一轮一样，以老板 19:47 的书面确认为准。

## 150%：`run-dpi150-boss-confirmed/`

2026-11-08 19:43:53（+08:00）。老板把 Windows 系统缩放改成 150% 之后跑了这一轮，中间没有注销再登录。日志和文件名因此仍是 100% 那一套：

- 日志里的 `AppliedDPI` 是 **96**
- 脚本按 96 打上的标签是 **dpi100**。目录里的截图、日志、构建信息都叫 `dpi100-*`，没有 `dpi150-*`
- 70 帧，`fails=0`，日志 `visual_confirm=yes`

脚本这一轮打印和写入的标签是 dpi100。150% 成立，是因为老板书面确认，不是因为脚本读到了 150% 的 DPI。

老板确认时间是 2026-11-08 19:47（Asia/Shanghai），原话：「100和150测试都没问题」。G1-5 以这句书面确认为准。

之后如果要让脚本自己记成 150%（`AppliedDPI=144`，标签 `dpi150`），先在「设置 → 系统 → 屏幕」把运行 StepLens 的那块屏幕调成 150%，注销并重新登录，再开一个新的 Windows PowerShell 重跑。只关窗口，或者只重启资源管理器，注册表里的 `AppliedDPI` 不会变成 144。

## 目录

| 路径 | 内容 |
|---|---|
| `run-dpi100/` | 100% 的 70 张 png、`dpi100-log.txt`、`dpi100-build-info.json` |
| `run-dpi150-boss-confirmed/` | 系统缩放已改为 150%、但未注销重登的那一轮。文件名仍是 `dpi100-*` |

两份 `dpi100-build-info.json` 记录的 exe 都是 `C:\sl\StepLens\build\msvc\RelWithDebInfo\StepLens.exe`。png 在 `.gitattributes` 里按二进制入库。这两轮的 txt 和 json 也按二进制入库，保留老板机上的 UTF-8 BOM 和 CRLF。

## 脚本

入库副本是 [`scripts/bench/t09-smoke.ps1`](../../../scripts/bench/t09-smoke.ps1)，内容来自当时合过人工确认逻辑的 `2-smoke.ps1`。确认词不区分大小写：`yes` 和 `YES` 都算通过。

脚本找 exe 的顺序：

1. `build\msvc\src\app\RelWithDebInfo\StepLens.exe`（默认，按当前源码的实际输出）
2. 找不到再找 `build\msvc\RelWithDebInfo\StepLens.exe`

本轮构建信息里记到的是第 2 条。那台机器上编出来的 exe 在 `build\msvc\RelWithDebInfo`，不在 `src\app` 下面。

脚本把自己所在的目录当成仓库根，并且要求同级有 `CMakePresets.json`。按这个检查，它要放在仓库根目录再运行。直接执行 `scripts\bench\t09-smoke.ps1` 会被脚本拒绝。复跑时把这份脚本复制到仓库根（与 `CMakePresets.json` 同级）后再执行。

## 复跑

下面按老板这台电脑写。换机器时换成自己的 VS、Qt 和 vcpkg 路径。完整的逐步说明以当时发给老板的操作说明为准，这里只留复跑需要的口径。

1. 打开开始菜单里的 **Windows PowerShell**。不要选带 (x86) 的，也不要打开 PowerShell 7。
2. 进入 VS 的 x64 开发环境，在仓库根执行 `cmake --preset msvc`，再执行 `cmake --build --preset release`。成功时能找到上面两个路径之一的 `StepLens.exe`。
3. 在「设置 → 系统 → 屏幕」里，把**运行 StepLens 的那块屏幕**调到 100% 或 150%。保存正在编辑的文件，从开始菜单注销，重新登录同一个账户。不要只锁定，也不要只重启资源管理器。
4. 重新打开 Windows PowerShell。看 `HKCU:\Control Panel\Desktop\WindowMetrics` 的 `AppliedDPI`：96 是 100%，144 是 150%。脚本只认这两个数，并据此写入 `dpi100` 或 `dpi150`。不要手改注册表，也不要手填标签。
5. 不要设置 `QT_QPA_PLATFORM=offscreen`。离屏模式不能做像素验收。
6. 在系统的图形设置里，把这次用到的 `StepLens.exe` 选成高性能（独立显卡）。测试过程中不要遮挡窗口、最小化、锁屏，也不要断开远程桌面。
7. 脚本自动做 50 次改变大小和 10 次最大化/还原，共 70 帧。结束时问刚才是否看到蓝灰背景上的旋转方块，并且没有黑屏或纯色闪帧。看到了就输入 `yes`（大小写都行）。没看到就输入 `NO`。
8. 通过之后，桌面会出现 `t09-result-dpi100.zip` 或 `t09-result-dpi150.zip`。脚本写文件的位置是 `docs\bench\t09-gpu-smoke\` 根下，按 `dpi100-*` 或 `dpi150-*` 命名。同一标签再跑时，旧文件会被移进 `history-*` 子目录。这次入库为了让两轮 dpi100 标签的结果并存，分别放进了 `run-dpi100/` 和 `run-dpi150-boss-confirmed/`。

本轮没有改走 `WNT_Window`。
