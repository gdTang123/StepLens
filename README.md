# StepLens

内部使用的 STEP/IGES 轻量查看器。技术栈是 C++20、Qt 6.12、Open CASCADE Technology、CMake。平台是 Windows 10/11 x64，发布形式是绿色版 zip。

窗口仍是空的：状态栏写着「尚未打开文件」，界面还没有接到打开文件。后台已经能读 STEP/IGES。`LoadSession` / `LoadCoordinator` 调用 `io` 的 `StepLoader` 和 `IgesLoader`，带进度和软取消，读完释放读取器。`model` 的 DocumentModel 已经实现（装配树、原型去重、实例、颜色分组、IGES 分块）。`mesh` 的 Tessellator 已经实现 T07 单级网格（方案 B，弦高系数 c = 0.002）：按原型并行三角化，每个原型只做一次，颜色组调用 `FillTriangles` / `FillFaceBoundaries`。`scene` 已经实现 T08：每个原型一份 `PrototypePrs`（不直接显示），实例用 `AIS_ConnectedInteractive` 带上变换；带实例色的实例单独一份 `PrototypePrs`，三角形数组仍共用。颜色组沿用 `MeshBatch`。`SelectionMap` 把点选到的实例对象映回 `NodeId`。创建按帧切开，默认预算 8 ms。还没做的是粗/细两级网格（T07+）、`replaceMesh`（T08+）和装配树界面（T10）。core 里有结果类型、软取消、进度、线程池、日志和 Windows 崩溃转储。

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

样例文件不要提交。GrabCAD 和 Twin Disc 的模型有版权和转贴限制，下载到本地目录，用环境变量 `STEPLENS_SAMPLES` 告诉测试程序。`scripts/fetch_samples.py` 要到 T03 才实现。

`STEPLENS_SAMPLES` 里如果有 X-02（`Scania-Engine-V8-XT-Turbo.step`），网格测试 `Tessellator.X02InstanceTrianglesStayWithinBudget` 会加载并要求按实例展开的三角形数 ≤ 5,000,000。没有这份文件时该测试跳过，并写出跳过原因，不会假装已经量过。有样例时可以只跑这一条：

```bash
STEPLENS_SAMPLES=/path/to/samples ctest --test-dir build/occt-contract -R 'Tessellator.X02' --output-on-failure
```

## 环境

在 **x64 Native Tools Command Prompt for VS 2022** 里构建（或者先运行 `vcvars64.bat`）。预设使用 Ninja Multi-Config，终端里必须能找到 `cl` 和 `ninja`。

| 工具 | 要求 |
|---|---|
| Visual Studio 2022 | MSVC v143，x64，勾选「使用 C++ 的桌面开发」和「C++ CMake 工具」（带 Ninja） |
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

打一个带 Qt 运行时的 zip（还不含 OCCT DLL 和 VC++ 运行库，那些是 T17）：

```powershell
cmake --install build\msvc --config RelWithDebInfo --prefix dist-stage
python scripts\package.py --prefix dist-stage --qt-dir $env:QTDIR --output dist\StepLens-0.1.0-windows-x64.zip
```

其他预设：`msvc-asan`（AddressSanitizer）、`msvc-tbb`（给 OCCT 打开 oneTBB，默认构建不启用），都继承 7.9.3。`msvc-occt801` 是 OCCT 8.0.1 对照，不是默认。日常用 `msvc` 或 `cmake --workflow --preset ci`。

## 目录

```text
src/    core io model mesh scene viewer measure ui pipeline app
tests/  unit（GoogleTest）以及以后的 integration / ui / data
cmake/  编译选项、OCCT 工具包列表、安装规则、overlay ports
docs/   prd、design、adr、bench
tools/  bench、stepgen（尚未实现）
scripts/  package.py；fetch_samples.py 仍是占位
```

模块依赖以架构文档第 4 节为准：`core`、`io`、`model`、`mesh` 不依赖 Qt GUI；只有 `scene`、`viewer`、`measure` 会碰 OCCT 的显示类。`io` 已实现 STEP/IGES 后台读取。`model` 已实现 DocumentModel（装配树、原型去重、实例、颜色分组、IGES 分块）。`mesh` 已实现 T07 单级 Tessellator（方案 B，c = 0.002），不是占位。`scene` 已实现 T08（SceneBuilder、PrototypePrs、SelectionMap），不是占位。T07+ 的粗细两级和 T08+ 的 `replaceMesh` 不在这一版。`measure`、`ui`、`pipeline` 仍是占位翻译单元。`SceneBuilder` 不继承 `QObject`：无头契约测试不链 Qt，GUI 线程的调用方每个事件循环调用一次 `pump()`。

## 许可证

内部使用，保留所有权利，见 [LICENSE](LICENSE)。Qt、OCCT、FreeType 只动态链接，许可证文本在做绿色版（T17）时放进 `licenses/`。
