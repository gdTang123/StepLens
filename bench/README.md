# StepLens M0 spike 原型（T02）

- 源码：`src/spike.cpp`（单文件 C++17），构建：`CMakeLists.txt`
- 运行脚本：`run_all.sh`（串行跑完全部基准与取消测试），原始输出：`results/*.txt`（key=value）
- 报告：`spike-m0.md`

## 构建（本机，conda-forge OCCT）
```bash
export PATH=/opt/mm/occt/bin:$PATH
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/mm/occt -DCMAKE_CXX_COMPILER=/opt/mm/occt/bin/x86_64-conda-linux-gnu-g++
cmake --build build
# 7.9.3 对照版：-DCMAKE_PREFIX_PATH=/opt/mm/occt793 -B build793
```
## 运行
```bash
export LD_LIBRARY_PATH=/opt/mm/occt/lib
./build/spike bench <file> [--no-mesh]
./build/spike cancel-read <file> <已读字节比例 0-1>
./build/spike cancel-transfer <file> <进度位置 0-1 | 秒数 >1>
# 环境变量：SPIKE_NO_PROGRESS=1（Transfer 不挂进度）、SPIKE_IGES_NO_ATTRS=1（IGES 不读颜色/名称）
```

## Windows 复测（T02b，v1.2 新增）
- 执行清单：`t02b-windows-checklist.md`（= 架构文档 v1.2 附录 D），在老板的 Windows 电脑上逐条执行。
- 两份清单：根目录 `vcpkg.json`（基线 2026.10.16 → OCCT 8.0.1）；`occt-7.9.3/vcpkg.json`（同一基线 + `overrides` 锁 `7.9.3#1`），用 `-DVCPKG_MANIFEST_DIR` 切换。
- `compat/test_compat.cpp`：版本敏感点单测（`-DSTEPLENS_EXPECTED_OCCT=8.0.1|7.9.3`）。版本分支在仓库的 `src/occtsupport/occt_compat.h`，不在本目录再放一份。
- `windows/run-t02b.ps1`：按清单第 6、8 步运行；`windows/summarize.py results\801 results\793`：生成 spike-m0b.md 的结果表。
