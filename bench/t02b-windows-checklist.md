# T02b 执行清单：在老板的 Windows 电脑上复测 OCCT 8.0.1 / 7.9.3

> **已结束的复测步骤，不是当前构建说明。** 2026-11-08 产品锁定 **OCCT 7.9.3**。下面命令里的 8.0.1 是对照路径；默认构建见仓库根 README 的 `cmake --workflow --preset ci`。

> 版本 v1.1（2026-11-05）｜配套：架构文档 v1.2 §3.3、附录 D｜结果写入 `/workspace/bench/spike-m0b.md`
> 目的：① 8.0.1 在 Windows + vcpkg 下读超大 IGES（X-07）还崩不崩；② 两个版本 STEP 的速度和内存；③ Windows 内存口径与取消行为。**GPU 帧率不在这里测**（需要 Qt 和显示代码），挪到 T19 在团队电脑上测。
> 约定：每一步是**一条**可单独批准的命令（PowerShell）。🔒 = 需要管理员（UAC 弹窗）；其余普通权限。耗时都是估计值，未实测。

老板这台电脑请改看 [Windows 本地构建](../docs/bench/windows-local-build.md)：本机是 VS 2026（`D:\others\VS`，生成器 `Visual Studio 18 2026`），访问不了 github.com，不要用第 2、5、7 步里的 VS 2022 BuildTools 路径和 `Visual Studio 17 2022`。

## 老板提前准备

| 项 | 要求 |
|---|---|
| 时间 | 一次约 **2.5–4 小时**（大部分是无人值守的下载和编译），需要老板在场批准约 10 条命令 |
| 电源 / 睡眠 | 插电源；编译和测试期间不要睡眠（第 1 步会显示当前设置） |
| 磁盘 | C 盘至少 **50 GB** 空闲（VS 生成工具约 8 GB、两套 OCCT 编译约 10–20 GB、样例 1.6 GB，均为估计） |
| 网络 | 能访问 github.com、download.visualstudio.microsoft.com、downloads.steptools.com、twindisc.com；总下载约 **4–5 GB** |
| GitHub | 老板的 GitHub 账号能访问私有仓库 `gdTang123/StepLens`（只有第 3 步方式 A 需要） |
| 其他 | 测试期间关掉大型程序；杀毒软件若拦截 `spike.exe`，请临时放行 `C:\sl` 目录 |

## 步骤

### 第 1 步　前置检查（普通权限，约 10 秒）
做什么：打印系统、CPU、内存、显卡、磁盘、电源设置，**整段输出发回给团队**（写进 spike-m0b.md 的"环境"一节）。
```powershell
Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,@{n='RAM_GB';e={[math]::Round($_.TotalVisibleMemorySize/1MB,1)}},@{n='FreeRAM_GB';e={[math]::Round($_.FreePhysicalMemory/1MB,1)}} | Format-List
Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed | Format-List
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,@{n='VRAM_GB(最多显示4)';e={[math]::Round($_.AdapterRAM/1GB,1)}} | Format-List
Get-PhysicalDisk | Select-Object FriendlyName,MediaType,BusType,@{n='Size_GB';e={[math]::Round($_.Size/1GB)}} | Format-Table
Get-PSDrive C | Select-Object @{n='C_Free_GB';e={[math]::Round($_.Free/1GB,1)}}
powercfg /getactivescheme; winget --version; git --version 2>$null
```
通过条件：C 盘空闲 ≥ 50 GB、内存 ≥ 16 GB、`winget` 有版本号。不满足先停下来告诉团队。

### 第 2 步　安装 VS 2022 生成工具 + Git（🔒 管理员，约 15–40 分钟，下载约 2–3 GB）
做什么：装 MSVC 编译器、Windows SDK、VS 自带的 CMake（"使用 C++ 的桌面开发"工作负载及推荐组件 `Microsoft.VisualStudio.Workload.VCTools --includeRecommended`），以及 Git。**不装 Qt。**已装过 VS 2022（含 C++ 工作负载）或 Git 的可跳过对应一行。
```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements --override "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id Git.Git -e --accept-source-agreements --accept-package-agreements
```
完成后**关掉这个窗口，重新打开一个普通 PowerShell**（让 Git 进入 PATH）。

### 第 3 步　取 vcpkg 和摸底工具（普通权限，约 2–5 分钟，下载约 0.1 GB）
做什么：建 `C:\sl` 工作目录；按 vcpkg 发布标签 2026.10.16 浅取 vcpkg（`--filter=blob:none`，约 70 MB；旧版本 OCCT 端口按需下载）；取摸底工具到 `C:\sl\t02b`。方式 A、方式 B 二选一。

方式 B **不要**用 2026-11-04 的旧 `t02b-kit.zip`（缺 `src/occtsupport`，解压后 cmake 找不到头文件）。团队在 **main HEAD** 上、仓库根目录执行一条命令现打：`python scripts/make_t02b_kit.py`。生成 `dist/t02b-kit-<12位SHA>.zip`。zip 根上是 `KIT_COMMIT`（一行完整 commit SHA）、`bench\`（与该提交的 `bench/` 相同）、`src\occtsupport\`。核对：`KIT_COMMIT` 里的 SHA 必须等于打包当时的 `git rev-parse HEAD`（在 main 上打包时，就是当时的 main HEAD）。老板用下面命令块里的 `Get-Content` 读出这一行，和团队随 zip 发来的 SHA 比对。脚本启动时打印 `kit_commit=<sha>`，并写入 `results\<Tag>\kit_commit.txt`。方式 A 在仓库里跑时，脚本用 `git rev-parse HEAD` 与 `KIT_COMMIT`（旁边有这份文件时）核对，不一致就打中文提示。`bench/` 或 `windows/run-t02b.ps1` 再改过（包括本清单后续缺陷修复合入之后），在当时的 main HEAD 上把这条命令再跑一次。
```powershell
New-Item -ItemType Directory -Force C:\sl | Out-Null; Set-Location C:\sl
git clone --filter=blob:none --branch 2026.10.16 https://github.com/microsoft/vcpkg C:\sl\vcpkg
C:\sl\vcpkg\bootstrap-vcpkg.bat -disableMetrics
# 方式 A（推荐）：首次会弹浏览器登录 GitHub。t02b 是指向仓库 bench 的目录联接。
git clone https://github.com/gdTang123/StepLens C:\sl\StepLens; cmd /c mklink /J C:\sl\t02b C:\sl\StepLens\bench
# 方式 B：把上面命令打好的 t02b-kit-<12位SHA>.zip 放到 C:\sl 后，去掉下面三行的注释再执行（与方式 A 二选一）。
# Expand-Archive -LiteralPath C:\sl\t02b-kit-<12位SHA>.zip -DestinationPath C:\sl\t02b-kit
# cmd /c mklink /J C:\sl\t02b C:\sl\t02b-kit\bench
# Get-Content -LiteralPath C:\sl\t02b-kit\KIT_COMMIT
```
通过条件：`C:\sl\vcpkg\vcpkg.exe`、`C:\sl\t02b\vcpkg.json`、`C:\sl\t02b\occt-7.9.3\vcpkg.json` 都存在。方式 B 还要有 `C:\sl\t02b-kit\src\occtsupport\occt_compat.h`，且 `KIT_COMMIT` 与团队给出的完整 SHA 一致。

### 第 4 步　下载样例并校验（普通权限，约 5–20 分钟，必需 1.06 GB + 可选 0.53 GB）
做什么：从 PRD 样例清单（`samples.md`）的原始地址下载；X-07 是判定项，X-02/L-03 是 STEP 对比主样例，X-01 补充；X-06/X-08（另两个超大 IGES）可选。另外下载 OCCT 自带的小文件 `screw.step`（约 87 KB，88,552 字节），第 8 步用它测「文件夹名里有中文和空格」时能不能打开。
```powershell
New-Item -ItemType Directory -Force C:\sl\samples | Out-Null; Set-Location C:\sl\samples
$f = @{
 'PX13226.IGS'='https://twindisc.com/downloads/PX13226.IGS|0b4e8039a6612c003f38ca3e36f645dfede2f56aa0ba4623b1e0557182cb9a9b'
 'Scania-Engine-V8-XT-Turbo.step'='https://downloads.steptools.com/docs/stpfiles/bigassy/Scania-Engine-V8-XT-Turbo.step|3b02d0b151aa393db2a9c741b243ca34b05b973619cbe638f9a1d00cffede818'
 'NissanGT-R.STEP'='https://downloads.steptools.com/docs/stpfiles/bigassy/NissanGT-R.STEP|c64a4ad7540ab80f7db072781e8eab034bdbe8f0506a2421dbdf139a5f9e998c'
 'Scania-8x4.stp'='https://downloads.steptools.com/docs/stpfiles/bigassy/Scania-8x4.stp|573aa7da05462dee25a430b28a6869906775ae5e0126308956661c1e1da421b7'
 '1026064M.IGS'='https://twindisc.com/downloads/1026064M.IGS|'   # 可选，300,590,516 字节
 '1027247E.IGS'='https://twindisc.com/downloads/1027247E.IGS|'   # 可选，256,710,594 字节
 'screw.step'='https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/V7_9_3/data/step/screw.step|4b3649a4f5c4f05c7a06a402a91fe2fd7e3cba1615520fbd8c62a62610ad3e69'
}
foreach ($n in $f.Keys) { $u,$h = $f[$n].Split('|'); if (-not (Test-Path $n)) { curl.exe -L --fail -o $n $u }
  $ok = (-not $h) -or ((Get-FileHash $n -Algorithm SHA256).Hash -eq $h.ToUpper()); "{0,-34} {1,12:N0} 字节  {2}" -f $n,(Get-Item $n).Length,($(if($ok){'OK'}else{'哈希不符!'})) }
```
通过条件：前 4 个大文件和 `screw.step` 都显示 `OK`（哈希来自团队已下载的同名文件；`screw.step` 的哈希是 OCCT V7_9_3 这份，和 V8_0_1 里的文件相同）。可选的两个 IGES 下载失败可以忽略。

### 第 5 步　编译 OCCT 8.0.1 版摸底工具（普通权限，约 20–45 分钟，**估计**）
做什么：打开 VS 开发环境；CMake 配置时 vcpkg 按 `vcpkg.json`（基线 2026.10.16，不写 overrides = 8.0.1）**只编 Release**（`x64-windows-release` 三元组）的 OCCT 和 FreeType，编完删除中间文件；再编 `spike.exe` 和 `test_compat.exe`（RelWithDebInfo，带 PDB，崩溃栈可显示函数名）。
```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force   # 只对当前窗口生效，允许运行 VS 的环境脚本
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Set-Location C:\sl\t02b
cmake -S . -B build801 -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\sl\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DVCPKG_INSTALL_OPTIONS=--clean-after-build "-DSTEPLENS_EXPECTED_OCCT=8.0.1"
cmake --build build801 --config RelWithDebInfo
```
通过条件：最后一行没有 `error`，`C:\sl\t02b\build801\RelWithDebInfo\spike.exe` 存在。失败时把窗口最后 50 行发回团队。

### 第 6 步　先测关键项：8.0.1 读 X-07（普通权限，约 1–2 分钟）
做什么：只跑 X-07 一次，立刻知道 8.0.1 在 Windows 上崩不崩。崩溃时 `results\801\X-07.err` 里有 `crash_frame_00=...` 调用栈，同目录有 `X-07.dmp`。
```powershell
Set-Location C:\sl\t02b; powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 801 -Build build801 -Quick; Get-Content results\801\X-07.txt -Tail 4; Select-String -Path results\801\X-07.err -Pattern '^crash_' | Select-Object -First 12
```
看结果：`exit=0` 且有 `prototypes=32711` = **不崩**；`exit=-1073741819`（0xC0000005）并打印 `crash_frame_*` = **崩溃**。

开跑前脚本会先看 `spike.pdb` 和 spike 实际会加载的那些 `TK*.pdb` 在不在。窗口如果出现**黄字**「缺少符号文件」，测试**还会继续**，但万一崩溃，调用栈里可能没有函数名。`results\801\env.txt` 里会有一行 `pdb_missing=缺了哪些文件`。pdb 都在的话，不会有这一行。如果 `results\801` 里已经有上次的结果，脚本会先把它整个挪到 `results\archive\801-日期时间`，再新建空的 `results\801`。看结果仍看 `results\801`，不要去翻旧目录，除非要比对上一次。

如果出现**红字**「兼容单测没有通过」，脚本会**立刻停**，X-07 也不会跑。这通常是 OCCT 版本配错了：编译时期望的 8.0.1 或 7.9.3，和实际编进去的不是同一个。请把窗口文字和 `results\801\test_compat.txt` 发回团队，不要自己改配置。

### 第 7 步　编译 OCCT 7.9.3 版（普通权限，约 20–45 分钟，**估计**）
做什么：同第 5 步，但用 `occt-7.9.3\vcpkg.json`（同一基线 + `overrides` 锁 `7.9.3#1`），通过 `VCPKG_MANIFEST_DIR` 切换，产物放在 `build793`，两套互不影响。（第 5 步的窗口没关的话前两行可省略。）
```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force   # 只对当前窗口生效，允许运行 VS 的环境脚本
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Set-Location C:\sl\t02b
cmake -S . -B build793 -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\sl\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DVCPKG_INSTALL_OPTIONS=--clean-after-build -DVCPKG_MANIFEST_DIR=C:\sl\t02b\occt-7.9.3 "-DSTEPLENS_EXPECTED_OCCT=7.9.3"
cmake --build build793 --config RelWithDebInfo
```

### 第 8 步　两个版本的完整测试（普通权限，约 45–70 分钟，无人值守）
做什么：每个版本依次跑下面这些，每项单独进程、串行，避免互相抢内存。脚本会把 OCCT 的 DLL 目录加进这一次的 PATH，老板不用自己设。

1. 检查 pdb。缺了只出黄字，并在 `results\<版本>\env.txt` 写 `pdb_missing=...`，然后继续。
2. 兼容单测。退出码不是 0 就红字停下，说明 vcpkg 清单或 CMake 预设里的 OCCT 版本很可能和实际链接的不一致（期望是 8.0.1 或 7.9.3）。后面的测试一项都不跑。
3. X-07 →（可选 X-06/X-08）→ X-02、L-03 各 3 次 → X-01 1 次。
4. X-02 读取阶段 10/50/90% 取消。
5. X-02 解析盲区取消：文件读完、程序还在建模型时，再等 2 秒请求取消。只有取消请求真的赶在 `ReadStream` 返回之前，结果里才是 `cancel_stage=parsing` 和 `cancel_requested=1`。文件太小、读取先结束，则是 `cancel_stage=not_covered`、`cancel_requested=0`，这不是崩溃。读取本身失败时退出码不是 0。
6. X-02 转换阶段 20/50/80% 取消。用的是第 1 次的 `transfer_time_s`；第 1 次没有这个数（比如崩了）就改用第 2 次、再没有就用第 3 次。三次都没有，就跳过，并在 `env.txt` 写 `skipped=transfer_cancel`。不会因为找不到这个数而自己报错退出。
7. 把 `screw.step` 复制到 `C:\sl\样例 目录\screw.step`（文件夹名里有中文和空格），再跑一次 bench。成功时这一项的 `exit=0`。样例没下到或哈希不对就跳过，并写 `skipped=cn_path`，不影响前面已经跑完的结果。
```powershell
Set-Location C:\sl\t02b; powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 801 -Build build801; powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 793 -Build build793
```
期间不要用电脑做别的重活。

### 第 9 步　打包结果发回团队（普通权限，约 10 秒）
做什么：把两个版本的 `results`（文本、调用栈、崩溃转储）和第 1 步的环境信息打成一个 zip，通过微信/邮件/网盘发给团队。不含样例文件。
```powershell
Set-Location C:\sl\t02b; Compress-Archive -Force -Path results\* -DestinationPath C:\sl\t02b-results.zip; Get-Item C:\sl\t02b-results.zip | Select-Object FullName,Length
```

### 第 10 步　清理（等团队确认结果可用后再做）
| 项 | 命令 | 权限 / 耗时 |
|---|---|---|
| 删工作目录、vcpkg、样例、编译产物 | `Remove-Item -Recurse -Force C:\sl` | 普通，约 1 分钟 |
| 删 vcpkg 二进制缓存 | `Remove-Item -Recurse -Force "$env:LOCALAPPDATA\vcpkg"` | 普通，几秒 |
| （可选）卸载 VS 生成工具、Git | `winget uninstall --id Microsoft.VisualStudio.2022.BuildTools -e; winget uninstall --id Git.Git -e` | 🔒，约 5 分钟 |
| （仅做过附加步骤 A 时）删 WER 设置 | `Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\spike.exe"` | 🔒，几秒 |

## 附加步骤（只在需要时做）

**A. 内置崩溃处理没打出调用栈**（`.err` 里没有 `crash_frame_*`，例如栈溢出时）：让 Windows 自己写完整转储（🔒，几秒），然后重跑第 6 步。转储在 `C:\sl\dumps`，连同 `build801\RelWithDebInfo\` 下的 exe、dll、pdb 一起发回团队，由团队用 WinDbg 分析（老板电脑上不用装 WinDbg）。
```powershell
New-Item -ItemType Directory -Force C:\sl\dumps | Out-Null; $k='HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\spike.exe'; New-Item -Force $k | Out-Null; Set-ItemProperty $k DumpFolder 'C:\sl\dumps'; Set-ItemProperty $k DumpType 2 -Type DWord; Set-ItemProperty $k DumpCount 3 -Type DWord
```

**B. 第 5/7 步 vcpkg 报错**：把 `C:\sl\vcpkg\buildtrees\opencascade\` 下最新的 `*-err.log` 和窗口最后 50 行发回团队，不要自己改配置。

## 记录模板（第 9 步的 zip 由团队用 `windows\summarize.py` 自动生成下表，老板不用手填）

| 项 | 8.0.1 | 7.9.3 |
|---|---|---|
| 兼容单测 test_compat | PASS / FAIL | PASS / FAIL |
| X-07 结果（成功 / 崩溃 + 栈顶函数） | | |
| X-06 / X-08（可选） | | |
| X-02 合计中位数（s，3 次） | | |
| L-03 合计中位数（s，3 次） | | |
| X-01 合计（s） | | |
| X-02 读取 + 转换（s，第 1 次） | | |
| X-02 峰值工作集 / 峰值提交（MB） | | |
| 最长 UserBreak 间隔（X-02 / L-03，s） | | |
| 读取取消 3 次：返回延迟（ms）/ 是否崩溃 | | |
| 解析盲区取消：`cancel_stage`（parsing 或 not_covered）/ `cancel_requested` / 退出码 | | |
| 转换取消 3 次：返回延迟（ms）/ 释放（ms）/ 是否崩溃；若跳过，写 skipped | | |
| 中文路径 `C:\sl\样例 目录\screw.step` 的 exit | | |
| `env.txt` 里的 `pdb_missing`（没有这行就是 pdb 齐全） | | |
| `env.txt` 里的 `skipped`（没有这行就是都跑了） | | |

## spike-m0b.md 的格式（团队填写）
1. **环境**：第 1 步输出原文（CPU、内存、显卡、磁盘、系统版本）；vcpkg 基线 `2026.10.16`；三元组 `x64-windows-release`；MSVC 版本（`cl` 首行）。
2. **结果表**：`python windows\summarize.py results\801 results\793` 的输出（每个版本一张阶段耗时/内存表 + 一张取消表；取消表多一列「阶段」。解析盲区只有 `cancel_stage=parsing` 且 `cancel_requested=1` 才算覆盖到；`not_covered` 在结果列显示「未覆盖」，退出码不是 0 显示「失败」，不要只看进程有没有崩溃。表前面如果有旁注，就是 `env.txt` 里的 `pdb_missing`、`skipped`、`transfer_time_s_from`）。
3. **崩溃分析**（如有）：调用栈前 15 帧原文、异常码、触发时的阶段；与 Linux 摸底（`IGESCAFControl_Reader::Transfer`，约 50 s）是否一致。
4. **与 Linux 摸底对比**：系数 k = Windows X-02「读取 + 转换」÷ 53.0 s；内存比 = Windows X-02 峰值提交 ÷ 1,876 MB（Linux 峰值 RSS）。
5. **定版结论**：按下面规则，写明选哪个版本、依据哪几行数据。
6. **未做 / 异常项**。

## 定版规则（已作废。老板 2026-11-04 的过渡规则：8.0.1 不崩就用 8.0.1，仍崩就用 7.9.3。2026-11-08 已按后一种锁定）

| 情况 | 结论 |
|---|---|
| 8.0.1 读 X-07（以及做了的 X-06/X-08）**全部成功** | **用 8.0.1**。另外：若 8.0.1 的 X-02 或 L-03 合计中位数比 7.9.3 **慢 ≥ 20%**，在 spike-m0b.md 里标出并报老板（此条是复测前的规则，2026-11-08 已作废；产品锁定 7.9.3） |
| 8.0.1 读任一超大 IGES **崩溃**，7.9.3 全部成功 | **用 7.9.3**。若 7.9.3 的 STEP 比 8.0.1 **慢 ≥ 20%**，仍用 7.9.3（IGES 是 P0），但第 6 节时间目标按 7.9.3 的数据重算并报老板 |
| 两个版本都崩，或 7.9.3 在任一 STEP 样例上失败 | **不定版**，报老板。候选：8.0.1 + 改用 `IGESControl_Reader`（不带 CAF）试 0.5 天；IGES 放到子进程隔离（约 4–5 人天） |
| 兼容单测 FAIL，或任一取消测试崩溃 | 先修再定版（属于我们的代码问题，不影响版本判断） |
| `env.txt` 里有 `skipped=transfer_cancel` | X-02 三次都没有转换耗时，转换取消没跑。先看 X-02 是否崩溃或读取失败，不要把它当成两个 OCCT 版本谁快谁慢的依据 |
| `env.txt` 里有 `pdb_missing` | 测试可以照跑，但崩溃栈可能没有函数名。把缺的 pdb 补上再看崩溃才好查 |
| 拿到 8.0.1 的崩溃栈 | 团队整理复现材料，**是否向 OCCT 官方报 bug 由老板决定**（对外发帖） |
