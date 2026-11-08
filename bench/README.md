# StepLens M0 spike 原型（T02）

- 源码：`src/spike.cpp`（单文件 C++17），构建：`CMakeLists.txt`
- 运行脚本：`run_all.sh`（串行跑完全部基准与取消测试），原始输出：`results/*.txt`（key=value）
- 报告：`spike-m0.md`

## 构建（本机，conda-forge OCCT）

产品默认是 OCCT 7.9.3。8.0.1 只作对照。

```bash
# 默认 7.9.3
export PATH=/opt/mm/occt793/bin:$PATH
cmake -S . -B build793 -DCMAKE_PREFIX_PATH=/opt/mm/occt793 -DCMAKE_CXX_COMPILER=/opt/mm/occt793/bin/x86_64-conda-linux-gnu-g++ -DSTEPLENS_EXPECTED_OCCT=7.9.3
cmake --build build793
# 对照 8.0.1（非默认）：-DCMAKE_PREFIX_PATH=/opt/mm/occt -B build801 -DSTEPLENS_EXPECTED_OCCT=8.0.1
```
## 运行

默认跑 7.9.3 的 `build793`。对照 8.0.1 时把 `occt793` 换成 `occt`，`build793` 换成 `build801`。

```bash
export LD_LIBRARY_PATH=/opt/mm/occt793/lib
./build793/spike bench <file> [--no-mesh]
./build793/spike cancel-read <file> <已读字节比例 0-1>
./build793/spike cancel-transfer <file> <进度位置 0-1 | 秒数 >1>
./build793/spike cancel-parse <file> [delay_s]   # EOF 后再等 delay_s（默认 2）才请求取消；赶上了解析才打印 cancel_stage=parsing
# 环境变量：SPIKE_NO_PROGRESS=1（Transfer 不挂进度）、SPIKE_IGES_NO_ATTRS=1（IGES 不读颜色/名称）
```

## Windows 复测（T02b，v1.2 新增）
- 执行清单：`t02b-windows-checklist.md`（= 架构文档 v1.2 附录 D），在老板的 Windows 电脑上逐条执行。
- 默认清单：`occt-7.9.3/vcpkg.json`（基线 2026.10.16 + `overrides` 锁 `7.9.3#1`），用 `-DVCPKG_MANIFEST_DIR` 指向它，并传 `-DSTEPLENS_EXPECTED_OCCT=7.9.3`。
- 对照清单：根目录 `vcpkg.json`（同一基线、不写 overrides，当前端口是 8.0.1，不是默认）。对照构建才传 `-DSTEPLENS_EXPECTED_OCCT=8.0.1`。
- `compat/test_compat.cpp`：版本敏感点单测。版本分支在仓库的 `src/occtsupport/occt_compat.h`，不在本目录再放一份。
- `windows/run-t02b.ps1`：按清单第 6、8 步运行；`windows/summarize.py results\801 results\793`：生成 spike-m0b.md 的结果表。
- 方式 B 的 zip：在仓库根目录执行 `python scripts/make_t02b_kit.py`（打包当前提交的 `bench/` 与 `src/occtsupport`，并写入 `KIT_COMMIT`）。`bench/` 或这个脚本再改过，就在 main HEAD 上重新打一次。
