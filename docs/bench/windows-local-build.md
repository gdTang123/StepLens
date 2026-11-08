# 老板电脑：Windows 本地构建与 T02b

> **产品默认是 OCCT 7.9.3**（2026-11-08 锁定）。第 1 段的 8.0.1 是对照复现，不是默认。主工程默认预设是 `msvc` / `ci`，清单为 `vcpkg-configs/occt-7.9.3`。

CI 已恢复。本地构建只作为备用方案，例如 CI 不可用，或老板想在本机复现时。下面第 0 到第 5 段只在这台电脑上跑 T02b 摸底（`bench/`）。**VS 2026 本地跑的 T02b 不能替代 windows-2022 / VS 2022 / MSVC 14.44 的 CI 回执，主工程未被覆盖。** 这几段编出来的是 `spike.exe` 和 `test_compat.exe`，没有主工程的 `StepLens.exe`，也没有默认预设 `msvc` 的配置输出、Ninja 全量构建日志和 `ctest` 结果。主工程要另在完整源码树上按文末「后续验收证据清单」留证；那一份证据仍要写明实际工具链，不能当成上述 CI 回执。

下面每一段用 **Windows PowerShell 5.1** 整段粘贴（开始菜单里的「Windows PowerShell」，不是 PowerShell 7）。先粘「0. 准备环境」，然后按顺序粘，**不要关这个窗口**。某一段报错就停在那一段，把窗口里从第一条 `error` / `fatal` 开始的原文发回团队。

现有 kit（`scripts/make_t02b_kit.py` 打出来的那份）只有 `KIT_COMMIT`、`bench\`、`src\occtsupport\`。不要在这份只含 bench 的 kit 上直接跑主工程预设 `msvc`、`release`、`unit` 或 workflow `ci`。这些预设写在仓库根的 `CMakePresets.json` 里，生成器是 Ninja Multi-Config，三元组是 `x64-windows`，默认 OCCT 7.9.3，还要仓库根的 `CMakeLists.txt`、`vcpkg-configs/occt-7.9.3`、Qt 和 `scripts\`。`bench/windows/run-t02b.ps1` 只到 `vcpkg_installed\x64-windows-release\bin` 找 OCCT 的 DLL。

清单 `t02b-windows-checklist.md` 第 2、5、7 步写的是 VS 2022 BuildTools 和生成器 `Visual Studio 17 2022`。这台机器不要用那三步。

## 这台机器

| 项 | 路径或取值 |
|---|---|
| Visual Studio | 2026（v18），安装在 `D:\others\VS`，自带 CMake 4.3 |
| 生成器 | `Visual Studio 18 2026`，平台 `-A x64` |
| Git | `D:\others\GIT\Git`（`git.exe` 在 `D:\others\GIT\Git\cmd`） |
| vcpkg | `C:\sl\vcpkg`，blobless 克隆（`--filter=blob:none`） |
| kit | 已解压在 `C:\sl\t02b` |
| 三元组 | `x64-windows-release` |
| 网络 | 访问不了 github.com。不要 `git clone`，也不要让 vcpkg 自己去下载 |

kit 的 zip 根上是 `KIT_COMMIT`、`bench\`、`src\occtsupport\`（`scripts/make_t02b_kit.py` 打出来的就是这样）。两种放法后面的命令都能认：

- `C:\sl\t02b\windows\run-t02b.ps1` 已经在（bench 目录本身就叫 `t02b`）
- 或者 `C:\sl\t02b\bench\windows\run-t02b.ps1`（zip 原样解压到 `C:\sl\t02b`）

CMake 会先把 bench 目录做 `REALPATH`。头文件用 bench 上一级的 `src\occtsupport\occt_compat.h`。扁平兜底是 `bench\occtsupport\occt_compat.h`。

8.0.1 用 bench 根上的 `vcpkg.json`（没有 `overrides`，基线即 8.0.1）。7.9.3 **不要**用仓库里的 `occt-7.9.3\vcpkg.json`（那份带 `overrides`，锁 `7.9.3#1`，要能上网）。7.9.3 用离线包里的 `manifest-793`（已经去掉 `overrides`）和 `overlay-ports`。

前提：`C:\sl\t02b-occt793-offline.zip` 已放在 `C:\sl\`。8.0.1 先前用同一个 `C:\sl\vcpkg` 编过，源码包还在 `C:\sl\vcpkg\downloads`。样例若已按清单第 4 步下过，在 `C:\sl\samples`。本机不要再从 github 拉 `screw.step`。

## 0. 准备环境

做什么：进 VS 2026 的 x64 开发环境，把 Git 加进 PATH，禁止 git 向 promisor remote 补对象，禁止 vcpkg 联网下源码。并记下 bench 目录 `$Bench`。

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -property installationPath
if (-not $vs) { $vs = 'D:\others\VS' }
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:Path = 'D:\others\GIT\Git\cmd;' + $env:Path
$env:GIT_NO_LAZY_FETCH = '1'
$env:X_VCPKG_ASSET_SOURCES = 'clear;x-block-origin'
Remove-Item Env:\VCPKG_MANIFEST_DIR -ErrorAction SilentlyContinue
Remove-Item Env:\VCPKG_OVERLAY_PORTS -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath 'C:\sl\t02b\windows\run-t02b.ps1') {
  $Bench = 'C:\sl\t02b'
} elseif (Test-Path -LiteralPath 'C:\sl\t02b\bench\windows\run-t02b.ps1') {
  $Bench = 'C:\sl\t02b\bench'
} else {
  throw '找不到 windows\run-t02b.ps1。kit 应在 C:\sl\t02b：要么直接有 windows\run-t02b.ps1，要么有 bench\windows\run-t02b.ps1。'
}
Write-Host "VS=$vs"
Write-Host "Bench=$Bench"
cmake --version
git --version
cmd /c "cl 2>&1" | Select-Object -First 2
Write-Host '环境就绪'
```

通过条件：`cmake --version` 是 4.3 这一路；`git --version` 有输出；`cl` 那两行是 VS 2026 的编译器，不是「找不到命令」。最后一行是 `环境就绪`。`$Bench` 停在本窗口里，后面每段都用它。

`VS=` 应是 `D:\others\VS`。若不是，或 `Launch-VsDevShell.ps1` 找不到，先执行 `$vs = 'D:\others\VS'`，再从 `& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation` 这一行贴到本段末尾。

## 1. 编译 OCCT 8.0.1（build801，对照，不是产品默认）

做什么：用 bench 自己的 `vcpkg.json` 配 `build801`，再以 RelWithDebInfo 编出 `spike.exe` 和 `test_compat.exe`。不传 `VCPKG_MANIFEST_DIR`，不传 `VCPKG_OVERLAY_PORTS`。`-DSTEPLENS_EXPECTED_OCCT=8.0.1` 要和这份清单一致，`test_compat` 对不上会停。

vcpkg 只编 Release（三元组 `x64-windows-release`）。`--clean-after-build` 编完会删掉端口的中间文件，已安装的包还在。

```powershell
if (-not $Bench) { throw '先粘贴「0. 准备环境」，不要关窗口。' }
if ($env:GIT_NO_LAZY_FETCH -ne '1') { throw 'GIT_NO_LAZY_FETCH 不是 1。回到「0. 准备环境」。' }
if ($env:X_VCPKG_ASSET_SOURCES -ne 'clear;x-block-origin') { throw 'X_VCPKG_ASSET_SOURCES 被改过。回到「0. 准备环境」。' }
Remove-Item Env:\VCPKG_MANIFEST_DIR -ErrorAction SilentlyContinue
Remove-Item Env:\VCPKG_OVERLAY_PORTS -ErrorAction SilentlyContinue
Set-Location $Bench
if (Test-Path build801) { Remove-Item -Recurse -Force build801 }
cmake -S . -B build801 -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\sl\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DVCPKG_INSTALL_OPTIONS=--clean-after-build "-DSTEPLENS_EXPECTED_OCCT=8.0.1"
if ($LASTEXITCODE -ne 0) { throw "cmake 配置 8.0.1 失败，退出码 $LASTEXITCODE" }
cmake --build build801 --config RelWithDebInfo
if ($LASTEXITCODE -ne 0) { throw "cmake 编译 8.0.1 失败，退出码 $LASTEXITCODE" }
$spike801 = Join-Path $Bench 'build801\RelWithDebInfo\spike.exe'
if (-not (Test-Path -LiteralPath $spike801)) { throw "没有 $spike801" }
Write-Host "8.0.1 编完：$spike801"
```

通过条件：最后一行是 `8.0.1 编完：` 加上 `spike.exe` 的路径。同目录还有 `test_compat.exe`。

这一段若报 `x-block-origin`，是 `C:\sl\vcpkg\downloads` 里没有 8.0.1 当时留下的源码包。不要改掉 `X_VCPKG_ASSET_SOURCES`，也不要去开 github。把报错里的文件名发回团队。下面的 `t02b-occt793-deps-sources.zip` 只补 7.9.3 缺的 zlib / freetype，不能拿来代替 8.0.1 的包。

## 2. 编译 OCCT 7.9.3（build793，离线 overlay）

做什么：核对离线包 SHA256，解压到 vcpkg 的 downloads，用包内清单和 overlay 端口配置 `build793`。这段命令与已经验证到端口解析的离线脚本一致；工作目录用上面的 `$Bench`（脚本里写的 `C:\sl\t02b` 就是 `$Bench` 等于 `C:\sl\t02b` 的那种放法）。

离线包 SHA256：`9B68B2F44B26289E804F089C7C6B7B274A0E5DC3E3F53414BF39E652B995A31C`

解压后必须有：

- `C:\sl\vcpkg\downloads\steplens-occt793-offline\manifest-793\vcpkg.json`（无 `overrides`）
- `C:\sl\vcpkg\downloads\steplens-occt793-offline\overlay-ports`

`X_VCPKG_ASSET_SOURCES=clear;x-block-origin` 让 vcpkg 缺文件就立刻失败，不去联网。`GIT_NO_LAZY_FETCH=1` 让 blobless 克隆缺对象时不要回 github 取。

```powershell
if (-not $Bench) { throw '先粘贴「0. 准备环境」，不要关窗口。' }
$zip = 'C:\sl\t02b-occt793-offline.zip'
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '9B68B2F44B26289E804F089C7C6B7B274A0E5DC3E3F53414BF39E652B995A31C') { throw '离线包 SHA256 不对，别继续' }
Expand-Archive -Path $zip -DestinationPath C:\sl\vcpkg\downloads -Force
$off = 'C:\sl\vcpkg\downloads\steplens-occt793-offline'
if (-not (Test-Path -LiteralPath (Join-Path $off 'manifest-793\vcpkg.json'))) { throw "解压后没有 $off\manifest-793\vcpkg.json" }
if (-not (Test-Path -LiteralPath (Join-Path $off 'overlay-ports'))) { throw "解压后没有 $off\overlay-ports" }
$env:X_VCPKG_ASSET_SOURCES = 'clear;x-block-origin'
$env:GIT_NO_LAZY_FETCH = '1'
Set-Location $Bench
if (Test-Path build793) { Remove-Item -Recurse -Force build793 }
cmake -S . -B build793 -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\sl\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DVCPKG_INSTALL_OPTIONS=--clean-after-build "-DVCPKG_MANIFEST_DIR=$off\manifest-793" "-DVCPKG_OVERLAY_PORTS=$off\overlay-ports" "-DSTEPLENS_EXPECTED_OCCT=7.9.3"
if ($LASTEXITCODE -ne 0) { throw "cmake 配置 7.9.3 失败，退出码 $LASTEXITCODE。若含 x-block-origin 且点名 zlib 或 freetype，先粘贴「2b」，再从本段的 Remove-Item build793 那一行重跑。" }
cmake --build build793 --config RelWithDebInfo
if ($LASTEXITCODE -ne 0) { throw "cmake 编译 7.9.3 失败，退出码 $LASTEXITCODE" }
$spike793 = Join-Path $Bench 'build793\RelWithDebInfo\spike.exe'
if (-not (Test-Path -LiteralPath $spike793)) { throw "没有 $spike793" }
Write-Host "7.9.3 编完：$spike793"
```

通过条件：最后一行是 `7.9.3 编完：` 加上 `spike.exe` 的路径。配置行里的三个 `-D` 必须同时在：`VCPKG_MANIFEST_DIR` 指向 `$off\manifest-793`，`VCPKG_OVERLAY_PORTS` 指向 `$off\overlay-ports`，`STEPLENS_EXPECTED_OCCT=7.9.3`。不要改成 `-DVCPKG_MANIFEST_DIR=$Bench\occt-7.9.3`。

### 2b. 仅当 x-block-origin 拦住 zlib / freetype

cmake 配置报 `x-block-origin blocks`，并且名字里有 `madler-zlib`、`zlib` 或 `freetype` 时，才做这一段。先把 `t02b-occt793-deps-sources.zip` 放到 `C:\sl\`。

SHA256：`7C02066B2BC9BDF2F5216186B8A50385633D0AF004C226743280840A1D070020`

```powershell
$deps = 'C:\sl\t02b-occt793-deps-sources.zip'
if ((Get-FileHash $deps -Algorithm SHA256).Hash -ne '7C02066B2BC9BDF2F5216186B8A50385633D0AF004C226743280840A1D070020') { throw '依赖包 SHA256 不对' }
Expand-Archive -Path $deps -DestinationPath C:\sl\vcpkg\downloads -Force
Write-Host '依赖源码已解压。不要关掉 X_VCPKG_ASSET_SOURCES。回到第 2 段，从 Remove-Item build793 那一行重跑。'
```

重跑前窗口里仍要是 `clear;x-block-origin`。不要为了绕过报错把这个变量清空。其他说不上来的包名，把整段报错发回团队，不要自己换端口。

## 3. 跑 bench/windows/run-t02b.ps1

做什么：每个版本单独起进程。脚本参数就是这三个：`-Tag`（`801` 或 `793`）、`-Build`（目录名 `build801` 或 `build793`，相对 bench 根，不是完整路径）、可选开关 `-Quick`（只跑 X-07 一次）。样例目录默认 `-Samples C:\sl\samples`，中文路径默认 `-CnDir "C:\sl\样例 目录"`。用 `powershell -File` 启动。不要点源（开发机自测才设 `STEPLENS_T02B_DOTSOURCE=1`）。

`-Quick` 先看 8.0.1 的 X-07 崩不崩。完整测试不带 `-Quick`：兼容单测、X-07、可选的 X-06/X-08、X-02 与 L-03 各 3 次、X-01、取消、中文路径。同一 `Tag` 再跑时，脚本先把已有的 `results\<Tag>` 挪到 `results\archive\<Tag>-时间`。所以完整测试跑完之后，要看的是新的 `results\801` 和 `results\793`，不是 archive 里 Quick 那次。

X-07 用的大文件是 `C:\sl\samples\PX13226.IGS`。没有样例就不要跑。

完整跑（不带 `-Quick`）走到中文路径时，`run-t02b.ps1` 的 `Get-ScrewStep` 会检查 `C:\sl\samples\screw.step`（默认 `-Samples C:\sl\samples`，文件名必须是 `screw.step`）。文件不在，或 SHA256 不是 `4B3649A4F5C4F05C7A06A402A91FE2FD7E3CBA1615520FBD8C62A62610AD3E69`（OCCT V7_9_3 这份，88,552 字节）时，脚本会执行 `curl.exe -L --fail --retry 2`，地址是 `https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/V7_9_3/data/step/screw.step`。`GIT_NO_LAZY_FETCH` 和 `X_VCPKG_ASSET_SOURCES` 管的是 git 和 vcpkg，管不到这一步。`-Quick` 在 X-07 之后就返回，不会下载这个文件。

预先放到 `C:\sl\samples\screw.step`，文件名就是 `screw.step`，并用上面的 SHA256 核对通过，完整跑才不会去访问 raw.githubusercontent.com。脚本自己会把它复制到 `C:\sl\样例 目录\screw.step`。curl 失败或下下来的哈希不对时，才在 `env.txt` 写 `skipped=cn_path` 并继续，这不是崩溃。

先看崩不崩：

```powershell
if (-not $Bench) { throw '先粘贴「0. 准备环境」，不要关窗口。' }
Set-Location $Bench
powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 801 -Build build801 -Quick
powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 793 -Build build793 -Quick
```

再跑完整测试（会把上面的 Quick 结果归档）：

```powershell
if (-not $Bench) { throw '先粘贴「0. 准备环境」，不要关窗口。' }
Set-Location $Bench
powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 801 -Build build801
powershell -ExecutionPolicy Bypass -File windows\run-t02b.ps1 -Tag 793 -Build build793
```

窗口出现红字「兼容单测没有通过」时脚本会立刻停，后面的读取都不会跑。把 `results\<Tag>\test_compat.txt` 发回团队。多半是 8.0.1 用成了 7.9.3 的清单，或 7.9.3 用了带 `overrides` 的 `occt-7.9.3\vcpkg.json`，`-DSTEPLENS_EXPECTED_OCCT` 和实际链上的 OCCT 不一致。不要改脚本。

黄字「缺少符号文件」只说明 pdb 不齐，测试还会继续。`results\<Tag>\env.txt` 里会有 `pdb_missing=`。

## 4. 结果放在哪

下面这些文件只是 T02b 摸底结果。它们不是 windows-2022 / VS 2022 / MSVC 14.44 的 CI 回执，也不覆盖主工程。主工程的验收证据见第 6 节，而且不要拿 `C:\sl\t02b` 这份 kit 去跑。

都在 `$Bench\results\` 下（`$Bench` 是 `C:\sl\t02b` 时就是 `C:\sl\t02b\results`；kit 原样解压时是 `C:\sl\t02b\bench\results`）。

| 文件 | 内容 |
|---|---|
| `results\<Tag>\<编号>.txt` | `key=value`，末尾有 `exit=` 和 `wall_s=` |
| `results\<Tag>\<编号>.err` | 该进程的 stderr。崩溃栈是 `crash_frame_00=` 这一行起 |
| `results\<Tag>\<编号>.dmp` | 只有崩溃且写出了转储时才有。脚本把 `spike-crash.dmp` 改成这个名字 |
| `results\<Tag>\test_compat.txt` | 版本单测。`exit=0` 才算过 |
| `results\<Tag>\kit_commit.txt` | `kit_commit=` / `git_head=` |
| `results\<Tag>\env.txt` | 有需要才出现：`pdb_missing=`、`skipped=`、`transfer_time_s_from=` |
| `results\archive\<Tag>-时间\` | 同一 Tag 上一次的整目录 |

`<Tag>` 是 `801` 或 `793`。可执行文件在 `$Bench\build801\RelWithDebInfo\spike.exe` 和 `$Bench\build793\RelWithDebInfo\spike.exe`。OCCT DLL 在对应的 `build801\vcpkg_installed\x64-windows-release\bin` 或 `build793\...` 下。

打包发回（不含样例）：

```powershell
if (-not $Bench) { throw '先粘贴「0. 准备环境」，不要关窗口。' }
Set-Location $Bench
Compress-Archive -Force -Path results\* -DestinationPath C:\sl\t02b-results.zip
Get-Item C:\sl\t02b-results.zip | Select-Object FullName, Length
```

zip 在 `C:\sl\t02b-results.zip`。团队用 `python windows\summarize.py results\801 results\793` 出表，老板不用手填。

## 5. 常见错误

### checkout-index / promisor remote

`C:\sl\vcpkg` 是 blobless 克隆。端口文件缺 blob 时，git 会向 promisor remote（github.com）要对象。这台机器连不上。日志里常见 `checkout-index`、`promisor remote`、`Unable to read sha1`、`lazy fetch`。

第 0 段已经设了 `$env:GIT_NO_LAZY_FETCH = '1'`。设了之后 git 不会再去拉，而是马上失败。不要去掉这个变量，也不要改成在线 `git fetch` 或重新 `git clone`。把从 `fatal` 或 `error:` 起的原文发回团队。需要对象的话由团队补进离线包，不在这台电脑上现取。

### x-block-origin

`$env:X_VCPKG_ASSET_SOURCES` 必须保持 `clear;x-block-origin`。vcpkg 发现 downloads 里没有源码包，就会报 `x-block-origin blocks ...`，而不会打开浏览器或重试 github。

- 7.9.3 的报错点名 `madler-zlib`、`zlib` 或 `freetype`：做第 2b 段，然后只重跑第 2 段里 `Remove-Item build793` 之后的命令。
- 8.0.1 或其他包名：不要清空这个变量。把完整报错发回团队。

### 0xC0000005

访问违例。`results\<Tag>\X-07.txt`（或其他编号的 txt）里是 `exit=-1073741819`。窗口黄字是 `exit=0xC0000005`，后面写了 `.err` 的路径。这两个是同一个退出码。

请把崩溃那一项的文件原样发回，不要只发窗口最后几行：

- `results\<Tag>\<编号>.err`
- `results\<Tag>\<编号>.dmp`（没有这个文件也要在回复里写明「没有 dmp」）

X-07 不崩的看 txt：`exit=0`，并且有 `prototypes=32711`。

`.err` 里没有 `crash_frame_` 时，调用栈没记下来。需要的话按清单附加步骤 A，用管理员打开 `spike.exe` 的 LocalDumps（转储目录 `C:\sl\dumps`），然后重跑。那一步不在这里重复。重跑后把 `C:\sl\dumps` 里的转储和对应的 `.err` 一起发回。

### 找不到 occt_compat.h

cmake 停在「找不到 occtsupport/occt_compat.h」。说明 `$Bench` 的上一级没有 `src\occtsupport\occt_compat.h`，bench 目录里也没有扁平的 `occtsupport\occt_compat.h`。看第 0 段打印的 `Bench=`。kit 原样解压时 bench 是 `C:\sl\t02b\bench`，头文件应在 `C:\sl\t02b\src\occtsupport\occt_compat.h`。不要把 `cmake -S` 指到 `C:\sl\t02b` 的 zip 根上（那里没有 `CMakeLists.txt`，除非第 0 段已经把 `$Bench` 判成 `C:\sl\t02b`）。

## 6. 后续验收证据清单

这一节不在第 0 到第 5 段里做。现有 kit 只有 bench，没有仓库根的 `CMakeLists.txt`、`CMakePresets.json`、`vcpkg-configs\occt-7.9.3` 和 `scripts\`。不要在 `C:\sl\t02b` 上直接跑下面的预设。

等有完整源码树时，在仓库根做。预设名以 `CMakePresets.json` 为准。这是产品默认路径，OCCT **7.9.3**，不是 8.0.1 对照：

- 配置预设 `msvc`：继承 `base`，生成器 Ninja Multi-Config，构建目录 `build/msvc`，三元组 `x64-windows`，`VCPKG_MANIFEST_DIR` 指向 `${sourceDir}/vcpkg-configs/occt-7.9.3`，`STEPLENS_EXPECTED_OCCT` 为 `7.9.3`。需要环境变量 `VCPKG_ROOT` 和 `QTDIR`。
- 构建预设 `release`：配置类型 RelWithDebInfo，对应 `msvc`。
- 测试预设 `unit`：同一构建目录，排除标签 `bench|large`。
- workflow `ci` 就是上面三步。对照预设 `msvc-occt801` 不在这里跑。

先 `Set-Location` 到仓库根（这一层要有 `CMakePresets.json`），再删掉旧的 `build\msvc`，然后配置、全量构建。这样 `build-msvc.log` 才是干净的 Ninja 全量日志。命令和 CI 里 `windows-build` 的 `cmake --workflow --preset ci` 相同；本机没有 `pwsh` 时用 Windows PowerShell 5.1 的 `powershell -File`。脚本参数以仓库为准：`scripts/check_build_log.ps1` 只有必填参数 `-Log`；`scripts/verify_occt_link.ps1` 必填 `-BuildDir`，链接报告用 `-Report`。

```powershell
git rev-parse HEAD
cmake --version
ninja --version
cmd /c "cl 2>&1" | Select-Object -First 2
if (Test-Path build\msvc) { Remove-Item -Recurse -Force build\msvc }
cmake --preset msvc
if ($LASTEXITCODE -ne 0) { throw "cmake --preset msvc 失败，退出码 $LASTEXITCODE" }
cmd /c "cmake --build --preset release > build-msvc.log 2>&1"
$buildCode = $LASTEXITCODE
Get-Content -LiteralPath build-msvc.log
if ($buildCode -ne 0) { throw "cmake --build --preset release 失败，退出码 $buildCode" }
powershell -ExecutionPolicy Bypass -File scripts\check_build_log.ps1 -Log build-msvc.log
if ($LASTEXITCODE -ne 0) { throw "check_build_log.ps1 未通过，退出码 $LASTEXITCODE" }
ctest --preset unit
if ($LASTEXITCODE -ne 0) { throw "ctest --preset unit 失败，退出码 $LASTEXITCODE" }
powershell -ExecutionPolicy Bypass -File scripts\verify_occt_link.ps1 -BuildDir build\msvc -Report build\msvc\occt-link-report.txt
if ($LASTEXITCODE -ne 0) { throw "verify_occt_link.ps1 未通过，退出码 $LASTEXITCODE" }
```

把下面六项原文留下。VS 2026 上跑的，在工具链那一项写明 `cl` 首行；不要把这份记录标成 windows-2022 / VS 2022 / MSVC 14.44 的 CI 回执。

| 证据 | 从哪来 | 留下什么 |
|---|---|---|
| 源码 SHA 和工具链版本 | `git rev-parse HEAD`、`cmake --version`、`ninja --version`、`cl` 首行 | 四条原文，并写明机器和 Visual Studio 版本 |
| configure 输出里的 7.9.3 版本行 | `cmake --preset msvc` 的输出 | `OpenCASCADE 7.9.3` 这一行。它来自仓库根 `CMakeLists.txt` 的 `message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}")` |
| 干净的 Ninja 全量构建日志 | 删除 `build\msvc` 之后的 `build-msvc.log` | 整份日志。预设生成器是 Ninja Multi-Config，这次是全量，不是增量 |
| ctest 用例数量 | `ctest --preset unit` 的结尾汇总 | 含用例总数的那一行（`tests passed` 里的总数），不要只写「通过」 |
| `check_build_log.ps1` 的结果里 D9025=0 | `scripts\check_build_log.ps1 -Log build-msvc.log` | 通过时打印「构建日志里的编译器命令行覆盖警告: 0」，退出码 0。脚本统计的就是日志里的 `D9025`，这一行即 D9025=0 |
| `verify_occt_link.ps1` 的报告 | `-BuildDir build\msvc -Report build\msvc\occt-link-report.txt` | 脚本打印的 `---- occt-link-report ----` 整段，以及最后一行 `dumpbin: 除 StepLens.exe 外没有 TKOpenGl.dll`。报告文件是 `build\msvc\occt-link-report.txt` |
