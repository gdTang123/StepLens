# StepLens

内部使用的 STEP/IGES 轻量查看器。技术栈是 C++20、Qt 6.12、Open CASCADE Technology、CMake。平台是 Windows 10/11 x64，发布形式是绿色版 zip。

程序能打开一个空窗口，状态栏写着「尚未打开文件」，还不能读 STEP/IGES。core 里已经有结果类型、软取消、进度、线程池、日志和 Windows 崩溃转储。

架构文档是 v1.1。它建议 v0.1 改用 OCCT 7.9.3，但 Windows 复测还没做完，`vcpkg.json` 里仍然锁着 8.0.1。

## 文档

| 文档 | 路径 |
|---|---|
| 需求 PRD v0.2 | [docs/prd/step-viewer-mvp-prd.md](docs/prd/step-viewer-mvp-prd.md) |
| 样例清单 | [docs/prd/samples.md](docs/prd/samples.md) |
| 架构设计 v1.1 | [docs/design/step-viewer-architecture.md](docs/design/step-viewer-architecture.md) |
| M0 摸底报告 | [docs/bench/spike-m0.md](docs/bench/spike-m0.md) |
| 样例扫描脚本和结果 | [docs/design/data/](docs/design/data/) |

样例文件不要提交。GrabCAD 和 Twin Disc 的模型有版权和转贴限制，下载到本地目录，用环境变量 `STEPLENS_SAMPLES` 告诉测试程序。`scripts/fetch_samples.py` 要到 T03 才实现。

## 环境

在 **x64 Native Tools Command Prompt for VS 2022** 里构建（或者先运行 `vcvars64.bat`）。预设使用 Ninja Multi-Config，终端里必须能找到 `cl` 和 `ninja`。

| 工具 | 要求 |
|---|---|
| Visual Studio 2022 | MSVC v143，x64，勾选「使用 C++ 的桌面开发」和「C++ CMake 工具」（带 Ninja） |
| CMake | 3.25 或更高。预设 schema v6 需要 3.25 |
| vcpkg | 设置 `VCPKG_ROOT`。端口锁在基线 `2026.10.16`（`9eec002df1cd109f8f104cd966a763daf9751489`），工具本身可以用这个标签或更新的版本 |
| Qt | **6.12.0** 官方预编译包 `msvc2022_64`，不走 vcpkg。组件要有 Core、Gui、Widgets、OpenGL、OpenGLWidgets、Concurrent、Test，以及 Linguist（qttools）。把安装目录设为 `QTDIR`，例如 `C:\Qt\6.12.0\msvc2022_64` |

第一次配置会按清单编译 OCCT 8.0.1 和 GoogleTest。OCCT 编译很久，编完后 vcpkg 的二进制缓存会让下次快很多。

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

其他预设：`msvc-asan`（AddressSanitizer）、`msvc-tbb`（给 OCCT 打开 oneTBB，默认构建不启用）。日常用 `msvc`。

## 目录

```text
src/    core io model mesh scene viewer measure ui pipeline app
tests/  unit（GoogleTest）以及以后的 integration / ui / data
cmake/  编译选项、OCCT 工具包列表、安装规则、overlay ports
docs/   prd、design、adr、bench
tools/  bench、stepgen（尚未实现）
scripts/  package.py；fetch_samples.py 仍是占位
```

模块依赖以架构文档第 4 节为准：`core`、`io`、`model`、`mesh` 不依赖 Qt GUI；只有 `scene`、`viewer`、`measure` 会碰 OCCT 的显示类。T01 里除 `app` 和 `core` 之外的库只有一个占位翻译单元，避免空的静态库。

## 许可证

内部使用，保留所有权利，见 [LICENSE](LICENSE)。Qt、OCCT、FreeType 只动态链接，许可证文本在做绿色版（T17）时放进 `licenses/`。
