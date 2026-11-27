# StepLens

内部使用的 STEP/IGES 轻量查看器。技术栈是 C++20、Qt 6.12、Open CASCADE Technology、CMake。平台是 Windows 10 1809 及以上与 Windows 11 x64，目标发布形式是绿色版 zip。

主窗口可以打开 STEP/IGES：菜单、工具栏、拖放或命令行 `StepLens.exe 文件路径`。加载时有阶段、进度和取消。读文件、建模型和三角化在后台；`SceneBuilder::pump()` 每 16 ms 在界面线程跑一截。模型显示之后可以旋转、平移、缩放，切换标准视图与显示模式。装配树可按名称过滤；距离/角度测量结果进入右侧列表（默认 mm 三位、角度两位），清除测量同时清标注和列表。截图可导出 PNG/JPG。软取消：打开 B 时保留 A，取消或失败不误销 A（见 `docs/prd/t65-soft-cancel-errata.md`）；加载完成后 Esc 不清除模型。文档的 `Close` 在后台线程做完。

**已在本机验证（不要扩大成全项目验收）**：004 已修复两千节点装配树单测退出挂起并回归通过；005 已在 TGD 真桌面完成 AppExit 6/6、`screw.step` 10 轮缩放 82 帧 0 空帧、374 MB Scania 加载 / 294 MB 取消恢复 / 0.78 s 退出实测；008 已完成角度标注 M1–M3（REVERSED 外法向、锐角弧、边-面正交投影、退化边不抛）及单测回归，并在临时便携目录（含 Qt/OCCT/vcpkg DLL，不含官方 VC Redist）清空开发 PATH 后启动，成功打开 `screw.step` 渲染并以退出码 0 干净退出。本机 GUI before/after 截图仅在真实可运行时采集，不造图。

**尚未完成 / 不要当成可交付**：含官方 VC Redist 的正式绿色版 zip（见下）、GitHub Actions 远程 CI 跑通、合并与发布。空帧/独显在其它机器上的稳定性、超大装配树 1500 s 超时门禁，仍不以本机一次实测代替全面验收。

后台读取走 `LoadSession` / `LoadCoordinator`，调用 `io` 的 `StepLoader` 和 `IgesLoader`，带进度和软取消，读完释放读取器。`model` 的 DocumentModel 已经实现（装配树、原型去重、实例、颜色分组、IGES 分块）。`mesh` 的 Tessellator 已经实现 T07 单级网格（方案 B，弦高系数 c = 0.002）：按原型并行三角化，每个原型只做一次，颜色组调用 `FillTriangles` / `FillFaceBoundaries`。`scene` 已经实现 T08：每个原型一份 `PrototypePrs`（不直接显示），实例用 `AIS_ConnectedInteractive` 带上变换；带实例色的实例单独一份 `PrototypePrs`，三角形数组仍共用。颜色组沿用 `MeshBatch`。`SelectionMap` 把点选到的实例对象映回 `NodeId`。创建按帧切开，默认预算 8 ms。还没做的是粗/细两级网格（T07+）和 `replaceMesh`（T08+）。core 里有结果类型、软取消、进度、线程池、日志和 Windows 崩溃转储。

架构文档是 v1.2。摸底报告在 `docs/bench/spike-m0.md`，Windows 复测清单在 `docs/bench/t02b-windows-checklist.md`。产品锁定 **OCCT 7.9.3**（2026-11-08；T02b 结论是 8.0.1 在 X-07 崩溃）。默认预设 `msvc` / `ci` 使用 `vcpkg-configs/occt-7.9.3`（overrides 锁 `7.9.3#1`），`STEPLENS_EXPECTED_OCCT=7.9.3`。根 `vcpkg.json` 不写 overrides，只给对照预设 `msvc-occt801`（基线端口当前是 8.0.1，不是默认）。版本差异只在 `src/occtsupport/occt_compat.h`。老板在自己电脑上跑的工具在 `bench/`。

## 文档

| 文档 | 路径 |
|---|---|
| 需求 PRD v0.2 | [docs/prd/step-viewer-mvp-prd.md](docs/prd/step-viewer-mvp-prd.md) |
| 样例清单 | [docs/prd/samples.md](docs/prd/samples.md) |
| 架构设计 | [docs/design/step-viewer-architecture.md](docs/design/step-viewer-architecture.md) |
| M0 摸底报告 | [docs/bench/spike-m0.md](docs/bench/spike-m0.md) |
| T02b Windows 复测清单 | [docs/bench/t02b-windows-checklist.md](docs/bench/t02b-windows-checklist.md) |
| 样例扫描脚本和结果 | [docs/design/data/](docs/design/data/) |

样例文件不要提交。GrabCAD 和 Twin Disc 的模型有版权和转贴限制，下载到本地目录，用环境变量 `STEPLENS_SAMPLES` 告诉测试程序。一条命令拉齐 `docs/prd/samples.md` 并做 sha256 校验：

```bash
STEPLENS_SAMPLES=/path/to/samples python3 scripts/fetch_samples.py
```

`STEPLENS_SAMPLES` 里如果有 X-02（`Scania-Engine-V8-XT-Turbo.step`），网格测试 `Tessellator.X02InstanceTrianglesStayWithinBudget` 会加载并要求按实例展开的三角形数 ≤ 5,000,000。没有这份文件时该测试跳过，并写出跳过原因，不会假装已经量过。有样例时可以只跑这一条：

```bash
STEPLENS_SAMPLES=/path/to/samples ctest --test-dir build/occt-contract -R 'Tessellator.X02' --output-on-failure
```

基准命令（不带界面）。`--repeat 3` 对每次的耗时、计数和可重置的内存峰值取中位数。T07 只有方案 B 一级网格，JSON 里 `fine_triangle_count` 为 0。

```bash
steplens-bench --file X-02 --stages all --repeat 3 --json out.json
```

特大档用 `scripts/make_big_step.py` 把现有 STEP 拼成约 500 MiB 和约 1 GiB 的文件，写到 `local/bench/`，不要提交。超大文件只在团队机器上跑，不进托管 CI。

## 环境

在 **x64 Native Tools Command Prompt for VS 2022** 里构建（或者先运行 `vcvars64.bat`）。预设使用 Ninja Multi-Config，终端里必须能找到 `cl` 和 `ninja`。

| 工具 | 要求 |
|---|---|
| Visual Studio 2022 | MSVC v143，x64，勾选「使用 C++ 的桌面开发」和「C++ CMake 工具」（带 Ninja）。这是 GitHub Actions `windows-2022` 的 CI 基线；本机 TGD 开发环境是 VS 2026 / 工具集 14.51（VC145）。BenchCli 依赖 Python；TGD 默认 PATH 没有可用的 `python`（本机另有 conda Python，但不在默认 PATH），因此不要把「本机默认环境 BenchCli 已绿」写成事实。 |
| CMake | 3.25 或更高。预设 schema v6 需要 3.25 |
| vcpkg | 设置 `VCPKG_ROOT`。端口锁在基线 `2026.10.16`（`9eec002df1cd109f8f104cd966a763daf9751489`），工具本身可以用这个标签或更新的版本 |
| Qt | **6.12.0** 官方预编译包 `msvc2022_64`，不走 vcpkg。组件要有 Core、Gui、Widgets、OpenGL、OpenGLWidgets、Concurrent、Test，以及 Linguist（qttools）。把安装目录设为 `QTDIR`，例如 `C:\Qt\6.12.0\msvc2022_64` |

第一次配置会按 `vcpkg-configs/occt-7.9.3` 编译 OCCT 7.9.3 和 GoogleTest。OCCT 编译很久，编完后 vcpkg 的二进制缓存会让下次快很多。对照预设 `msvc-occt801` 才使用根清单里的基线端口（当前 8.0.1），不进默认 workflow。

只编 core 并跑单元测试（不需要 Qt、OCCT、MSVC），可以加 `-DSTEPLENS_CORE_ONLY=ON`。CI 的 Ubuntu 任务用这个选项，并打开 ThreadSanitizer。

## 构建

把下面两个路径换成自己机器上的目录，然后执行这一条：

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"; $env:QTDIR = "C:\Qt\6.12.0\msvc2022_64"; cmake --workflow --preset ci
```

cmd：

```bat
set VCPKG_ROOT=C:\src\vcpkg&& set QTDIR=C:\Qt\6.12.0\msvc2022_64&& cmake --workflow --preset ci
```

这条命令会配置、用 RelWithDebInfo 编译，并跑单元测试。产物：

```text
build\msvc\RelWithDebInfo\StepLens.exe
```

开发机上直接运行（Qt 的 bin 要在 `PATH` 里）：

```powershell
$env:PATH = "$env:QTDIR\bin;$env:PATH"
.\build\msvc\RelWithDebInfo\StepLens.exe
```

打绿色版 zip（Qt、按 dumpbin 收集的 vcpkg DLL、不旧于工具集的官方 VC 再发行库）。2026-11-28 在 TGD（VS 2026 / VC145，官方再发行目录同级无 `Redist.txt`）用本机已有 conda Python 3.10.16 对临时前缀试跑 `scripts/package.py --require-redist`，退出码 1（`windeployqt` 与 OCCT DLL 收集成功，但脚本因找不到 `Redist.txt` 拒绝复制 VC 运行库）。含官方 VC Redist 的正式绿色版 zip 仍待 GitHub Actions `windows-2022` / VC143 环境产出。同日在临时便携目录（cmake --install + windeployqt + 从 vcpkg 收集的 OCCT/vcpkg DLL，不含 VC Redist）将进程 PATH 限制为 `C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem` 并清空 `QT_PLUGIN_PATH` / `QML2_IMPORT_PATH` / `QT_QPA_PLATFORM_PLUGIN_PATH` 后启动，成功打开 `C:\sl\samples\screw.step` 并完成渲染，退出码 0、无残留进程。示例命令：

```powershell
cmake --install build\msvc --config RelWithDebInfo --prefix dist-stage
python scripts\package.py --prefix dist-stage --qt-dir $env:QTDIR --vcpkg-bin build\msvc\vcpkg_installed\x64-windows\bin --output dist\StepLens-0.1.0-windows-x64.zip
```

其他预设：`msvc-asan`（AddressSanitizer）、`msvc-tbb`（给 OCCT 打开 oneTBB，默认构建不启用），都继承 7.9.3。`msvc-occt801` 是 OCCT 8.0.1 对照，不是默认。日常用 `msvc` 或 `cmake --workflow --preset ci`。

## 目录

```text
src/    core io model mesh scene viewer measure ui pipeline app
tests/  unit（GoogleTest）以及以后的 integration / ui / data
cmake/  编译选项、OCCT 工具包列表、安装规则、overlay ports
docs/   prd、design、adr、bench
tools/  bench（steplens-bench）、stepgen（尚未实现，T24 / v0.2）
scripts/  package.py、fetch_samples.py、make_big_step.py
```

模块依赖以架构文档第 4 节为准：`core`、`io`、`model`、`mesh` 不依赖 Qt GUI；只有 `scene`、`viewer`、`measure` 会碰 OCCT 的显示类。`io` 已实现 STEP/IGES 后台读取。`model` 已实现 DocumentModel（装配树、原型去重、实例、颜色分组、IGES 分块）。`mesh` 已实现 T07 单级 Tessellator（方案 B，c = 0.002），不是占位。`scene` 已实现 T08（SceneBuilder、PrototypePrs、SelectionMap），不是占位。T07+ 的粗细两级和 T08+ 的 `replaceMesh` 不在这一版。`pipeline` 已经串起打开、软取消提交/回滚和后台释放，不是占位。`ui` 有加载遮罩、装配树过滤、属性/测量列表、程序绘制工具栏图标。`measure` 已接距离/角度计算与列表，不是占位。`SceneBuilder` 不继承 `QObject`：无头契约测试不链 Qt，GUI 线程的调用方每个事件循环调用一次 `pump()`。

## 许可证

内部使用，保留所有权利，见 [LICENSE](LICENSE)。Qt、OCCT、FreeType 及其默认传递依赖的许可证副本在 [packaging/licenses](packaging/licenses)，来源见那里的 `SOURCES.txt`。这不是法律审查结论。
