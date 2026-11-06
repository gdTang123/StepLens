# T02b Windows 复测结论（spike-m0b）

## 文档信息

- 文档：`bench/spike-m0b.md`
- kit：`e502584`（`KIT_COMMIT=e5025844885bdad264927c2360f12e86bb3ff4a6`）
- 样例：X-07=`PX13226.IGS`（`C:\sl\samples\PX13226.IGS`）
- 对比版本：OCCT 8.0.1（801） vs OCCT 7.9.3（793）
- 7.9.3 构建：周严离线 overlay port；CMake 生成器离线脚本指定为 `"Visual Studio 18 2026"`（见 `t02b-occt793-offline-v2.ps1:18`；实际 configure 日志未归档）
- kit SHA `e5025844885bdad264927c2360f12e86bb3ff4a6` 来自 `res/{801,793}/kit_commit.txt`，`env.txt` 未归档；其余环境信息未采集。
- 第 1 步机器硬件详情（CPU 型号/内存/磁盘等）：未采集；X-07 日志仅记录 `threads_hw=28`。

## 1. 结论

按老板 2026-11-04 定版规则：**8.0.1 读超大 IGES 崩，且 7.9.3 本批 17 项全部正常退出 → 选用 OCCT 7.9.3**。

- OCCT 8.0.1 在 X-07=`PX13226.IGS` 上崩溃：`-1073741819`，minidump 确认 `0xC0000005`，读地址 `0x38`，崩溃帧指向 `TKDEIGES.dll!IGESCAFControl_Reader::Transfer+0x174`，位置 `IGESCAFControl_Reader.cxx:204`。
- OCCT 7.9.3 的兼容单测 `PASS`，X-07 成功（合计 `69.0 s`），取消测试 `7/7` 成功。注意：X-07 两个版本当前归档各只测了 1 次，不能推广为所有 IGES 均稳定。
- 速度差已标出：X-02 合计中位数 801=`48.8 s` / 793=`52.4 s`，793 约慢 `7.5%`（按未舍入中位数 48.7579 s / 52.3946 s 计算）；L-03 合计中位数 801=`67.1 s` / 793=`72.0 s`，793 约慢 `7.3%`。均未到 `≥20%`；按规则即使 STEP 慢 `≥20%` 仍用 7.9.3（IGES 是 P0），但需在文中标出速度差。

**最终选择：OCCT 7.9.3。**

## 2. 8.0.1 vs 7.9.3 对比表

兼容单测：OCCT 8.0.1 `PASS`；OCCT 7.9.3 `PASS`。

| 版本 | 运行 | 结果 | 读取 s | 转换 s | 释放 s | 方案B网格 s | 合计 s | 读取峰值工作集 MB | 全程峰值工作集 MB | 全程峰值提交 MB | 最长 UserBreak 间隔 s | 原型数 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 8.0.1 | L-03-r1 | 成功 | 5.5 | 48.7 | 0.8 | 3.7 | 67.1 | 816 | 881 | 928 | 8.28 | 101 |
| 8.0.1 | L-03-r2 | 成功 | 5.4 | 48.7 | 0.8 | 3.7 | 67.0 | 816 | 880 | 956 | 8.24 | 101 |
| 8.0.1 | L-03-r3 | 成功 | 5.5 | 48.9 | 0.8 | 3.8 | 67.3 | 816 | 879 | 959 | 8.35 | 101 |
| 8.0.1 | X-01 | 成功 | 7.0 | 37.4 | 0.7 | 5.5 | 63.0 | 1577 | 1577 | 1656 | 1.03 | 191 |
| 8.0.1 | X-02-r1 | 成功 | 8.3 | 28.8 | 0.6 | 3.3 | 48.7 | 1867 | 1867 | 1942 | 0.71 | 159 |
| 8.0.1 | X-02-r2 | 成功 | 8.2 | 29.1 | 0.6 | 3.3 | 48.8 | 1867 | 1867 | 1942 | 0.71 | 159 |
| 8.0.1 | X-02-r3 | 成功 | 8.1 | 29.5 | 0.6 | 3.3 | 49.2 | 1867 | 1867 | 1942 | 0.71 | 159 |
| 8.0.1 | X-07 | 崩溃 -1073741819 @ TKDEIGES.dll!IGESCAFControl_Reader::Transfer+0x174（IGESCAFControl_Reader.cxx:204） | 3.1 | — | — | — | — | 848 | 848 | — | — | — |
| 8.0.1 | cn-path | 成功 | 0.0 | 0.0 | 0.0 | 0.0 | 0.1 | 19 | 26 | 14 | 0.00 | 1 |
| 7.9.3 | L-03-r1 | 成功 | 5.5 | 50.2 | 0.9 | 4.8 | 72.0 | 831 | 904 | 936 | 8.45 | 101 |
| 7.9.3 | L-03-r2 | 成功 | 5.4 | 55.1 | 0.8 | 4.7 | 76.6 | 830 | 904 | 968 | 13.53 | 101 |
| 7.9.3 | L-03-r3 | 成功 | 5.4 | 48.6 | 0.9 | 4.9 | 70.1 | 831 | 903 | 970 | 8.00 | 101 |
| 7.9.3 | X-01 | 成功 | 6.9 | 38.8 | 0.8 | 7.4 | 70.5 | 1598 | 1598 | 1679 | 1.04 | 191 |
| 7.9.3 | X-02-r1 | 成功 | 7.9 | 32.6 | 0.6 | 4.1 | 54.3 | 1882 | 1882 | 1957 | 0.75 | 159 |
| 7.9.3 | X-02-r2 | 成功 | 7.9 | 30.9 | 0.6 | 4.0 | 52.4 | 1882 | 1882 | 1957 | 0.74 | 159 |
| 7.9.3 | X-02-r3 | 成功 | 8.0 | 30.7 | 0.7 | 4.0 | 52.3 | 1882 | 1882 | 1957 | 0.73 | 159 |
| 7.9.3 | X-07 | 成功 | 3.1 | 50.6 | 0.5 | 3.9 | 69.0 | 867 | 1531 | 1664 | 0.42 | 32711 |
| 7.9.3 | cn-path | 成功 | 0.0 | 0.0 | 0.0 | 0.0 | 0.1 | 19 | 25 | 14 | 0.00 | 1 |

### 精简判定表

| 判定项 | OCCT 8.0.1 | OCCT 7.9.3 | 说明 |
|---|---|---|---|
| 兼容单测 | PASS | PASS | 两者均 PASS |
| X-07（`PX13226.IGS`） | 崩溃：`-1073741819`；`0xC0000005`；read `0x38`；`TKDEIGES.dll!IGESCAFControl_Reader::Transfer+0x174` | 成功；合计 `69.0 s`；原型数 `32711` | 按 2026-11-04 规则选 7.9.3 |
| X-02 合计中位数 | `48.8 s`（3 次） | `52.4 s`（3 次） | 793 约慢 `7.5%`（按未舍入中位数），<`20%` |
| L-03 合计中位数 | `67.1 s`（3 次） | `72.0 s`（3 次） | 793 约慢 `7.3%`，<`20%` |
| 取消测试是否全过 | 是（7/7） | 是（7/7） | summary 中取消测试均成功 |


### 取消测试对比（summary）

#### OCCT 8.0.1

| 取消测试 | 结果 | 阶段 | 请求时刻 s | 返回延迟 ms | 释放 ms | 释放后工作集 MB |
|---|---|---|---|---|---|---|
| cancel-parse-X02 | 成功 | parsing / req=1 | 4.6 | 3802.44 | 535 | 22 |
| cancel-read-X02-0.1 | 成功 | — | 0.3 | 6.4062 | 0 | 18 |
| cancel-read-X02-0.5 | 成功 | — | 1.4 | 18.2663 | 0 | 18 |
| cancel-read-X02-0.9 | 成功 | — | 2.3 | 30.9719 | 0 | 18 |
| cancel-transfer-X02-0.2 | 成功 | — | 5.8 | 159.171 | 565 | 28 |
| cancel-transfer-X02-0.5 | 成功 | — | 14.4 | 36.4805 | 631 | 30 |
| cancel-transfer-X02-0.8 | 成功 | — | 23.1 | 15.4565 | 751 | 30 |

#### OCCT 7.9.3

| 取消测试 | 结果 | 阶段 | 请求时刻 s | 返回延迟 ms | 释放 ms | 释放后工作集 MB |
|---|---|---|---|---|---|---|
| cancel-parse-X02 | 成功 | parsing / req=1 | 4.6 | 3398.49 | 555 | 22 |
| cancel-read-X02-0.1 | 成功 | — | 0.2 | 9.4059 | 0 | 18 |
| cancel-read-X02-0.5 | 成功 | — | 1.4 | 22.1299 | 0 | 18 |
| cancel-read-X02-0.9 | 成功 | — | 2.3 | 29.7534 | 0 | 18 |
| cancel-transfer-X02-0.2 | 成功 | — | 6.5 | 1.7327 | 546 | 29 |
| cancel-transfer-X02-0.5 | 成功 | — | 16.3 | 13.1869 | 659 | 29 |
| cancel-transfer-X02-0.8 | 成功 | — | 26.0 | 19.6992 | 791 | 29 |

## 3. X-07 崩溃栈（8.0.1）

- 样例：X-07=`PX13226.IGS`，路径 `C:\sl\samples\PX13226.IGS`
- 版本：`occt_version_at_start=8.0.1`，`occt_version=8.0.1`
- 运行：`mode=bench`，`threads_hw=28`，`exit=-1073741819`，`wall_s=42.4`
- 读取：`read_status=1`，`read_time_s=3.13207`，`read_ok=1`，`read_rss_mb=356.395`，`read_peak_rss_mb=848.383`，`read_peak_commit_mb=857.188`
- minidump：`ExceptionCode=EXCEPTION_ACCESS_VIOLATION (0xC0000005)`；`ExceptionAddress=0x7ff9dc1f33a4`；`ExceptionInformation: read (0) of address 0x38`（near-null deref）；`Module: C:\sl\t02b\build801\RelWithDebInfo\TKDEIGES.dll`
- minidump 说明：`PEB parse failed; no full symbolized stack from minidump; use crash_frame_* from X-07.txt`。完整符号栈以 X-07.txt/err 的 `crash_frame_*` 为准。

| 帧 | 符号/位置 |
|---|---|
| `crash_frame_00` | `TKDEIGES.dll!IGESCAFControl_Reader::Transfer+0x174 C:\sl\vcpkg\buildtrees\opencascade\src\V8_0_1-2f604eca2a.clean\src\DataExchange\TKDEIGES\IGESCAFControl\IGESCAFControl_Reader.cxx:204` |
| `crash_frame_01` | `spike.exe!run+0x14ea C:\sl\t02b\src\spike.cpp:744` |
| `crash_frame_02` | `spike.exe!main+0x2dc C:\sl\t02b\src\spike.cpp:929` |
| `crash_frame_03` | `spike.exe!__scrt_common_main_seh+0x10f D:\a\_work\1\s\src\vctools\crt\vcstartup\src\startup\exe_common.inl:288` |
| `crash_frame_04` | `KERNEL32.DLL!BaseThreadInitThunk+0x17 :0` |
| `crash_frame_05` | `ntdll.dll!RtlUserThreadStart+0x2c :0` |

## 4. Windows 环境踩坑

### 4.1 VS2026 生成器

- CMake 生成器离线脚本指定为 `"Visual Studio 18 2026"`，架构 `-A x64`（见 `t02b-occt793-offline-v2.ps1:18`）；实际 configure 日志未归档。
- 脚本用 `vswhere.exe` 定位 VS 安装路径：`"${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -property installationPath`。
- 未采集：VS2026 具体版本号、安装 workload、其他生成器对比/失败日志。

### 4.2 vcpkg blobless

- 清单第 3 步使用：`git clone --filter=blob:none --branch 2026.10.16 https://github.com/microsoft/vcpkg C:\sl\vcpkg`，约 `70 MB`；旧版本 OCCT 端口按需下载。
- 坑：vcpkg 用 `--filter=blob:none` 克隆后，编 7.9.3 时若无 overlay/离线源，旧 port 的 blob 会触发 promisor 联网。
- 防联网：
  - `GIT_NO_LAZY_FETCH=1`：禁止 git 去 promisor remote 现取对象。
  - `X_VCPKG_ASSET_SOURCES='clear;x-block-origin'`：禁止 vcpkg 联网下载，缺文件立即报错。

### 4.3 离线 overlay

- 包：`t02b-occt793-offline.zip`，含 `Open-Cascade-SAS-OCCT-V7_9_3.tar.gz` + `steplens-occt793-offline/`。
- 解压目标：`C:\sl\vcpkg\downloads`。
- `Open-Cascade-SAS-OCCT-V7_9_3.tar.gz`：`vcpkg_from_github` 期望的文件名，SHA512 与端口 portfile 一致（`65935a2f…aa0aa67`）。
- overlay 端口：`overlay-ports\opencascade`，来自 microsoft/vcpkg 中 `opencascade 7.9.3#1` 的端口；git tree `9a763d6711b422dea8fd72603ef8f89ff2386983`（已核对；含缺失的 blob `c84985e7 = dependencies.patch`）。
- manifest：`manifest-793\vcpkg.json` 与 kit 的 `occt-7.9.3\vcpkg.json` 相同，只去掉 overrides（版本由 overlay 提供）。
- 依赖：`freetype/zlib/libpng/bzip2/brotli/opengl-registry/egl-registry` 与 8.0.1 完全相同，正常会命中二进制缓存；源码备份在另一个 deps 包。
- 依赖缺包时（`t02b-occt793-deps-sources.zip` SHA256=`7C02066B2BC9BDF2F5216186B8A50385633D0AF004C226743280840A1D070020`）解压到 downloads 后重跑配置：
  ```powershell
  if ((Get-FileHash C:\sl\t02b-occt793-deps-sources.zip -Algorithm SHA256).Hash -ne '7C02066B2BC9BDF2F5216186B8A50385633D0AF004C226743280840A1D070020') { throw '依赖包 SHA256 不对' }
  Expand-Archive -Path C:\sl\t02b-occt793-deps-sources.zip -DestinationPath C:\sl\vcpkg\downloads -Force
  ```
- CMake 关键参数：
  - `-G "Visual Studio 18 2026" -A x64`
  - `-DCMAKE_TOOLCHAIN_FILE=C:\sl\vcpkg\scripts\buildsystems\vcpkg.cmake`
  - `-DVCPKG_TARGET_TRIPLET=x64-windows-release`
  - `-DVCPKG_INSTALL_OPTIONS=--clean-after-build`
  - `-DVCPKG_MANIFEST_DIR=$off\manifest-793`
  - `-DVCPKG_OVERLAY_PORTS=$off\overlay-ports`
  - `-DSTEPLENS_EXPECTED_OCCT=7.9.3`
- 若 CMake 配置报 `"x-block-origin blocks ... madler-zlib/freetype/..."`：将 `t02b-occt793-deps-sources.zip` 放到 `C:\sl\` 后按脚本说明重跑。

## 5. 未采集项

| 项 | 状态 |
|---|---|
| 第 1 步机器硬件详情（OS/CPU 型号/内存容量/显卡/磁盘，清单 env 输出） | 未采集（`results/*/env.txt` 未随结果带回；kit SHA 来自 `res/{801,793}/kit_commit.txt`，另仅有 `threads_hw=28`） |
| VS2026 / MSVC 具体版本号（`cl` 首行） | 未采集 |
| `pdb_missing` / `skipped` 旁注 | 未采集（summary 未列） |
| X-06 / X-08（可选超大 IGES） | 未跑 |
| minidump 完整符号栈（超出 crash_frame_*） | 未采集（`minidump` 库 PEB 解析失败；栈以 `X-07.txt`/`X-07.err` 的 `crash_frame_00`…`05` 为准） |
| 离线包 zip 的 SHA256（本次复用核对值） | 脚本写明 `t02b-occt793-offline.zip` = `9B68B2F44B26289E804F089C7C6B7B274A0E5DC3E3F53414BF39E652B995A31C`（来自离线脚本，非 Windows 当场重算） |

---

原始 summarize：`/workspace/bench/t02b-windows-summary-20261107.md`（由 `bench/windows/summarize.py` 生成）。结果目录：`results/801`、`results/793`（本归档副本在执行机 `/workspace/bench/res/`）。
