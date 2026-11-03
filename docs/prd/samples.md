# STEP 模型轻量查看器 MVP：测试样例下载清单（配套 PRD v0.2 第 7 节）

整理日期：2026-11-03 ｜ 整理人：产品经理

## 说明

- 本清单是 PRD v0.2 第 7 节"测试样例集"的下载附件，编号与 PRD 表格一一对应。PRD 表格写的是来源页面，这里写的是可以直接下载的文件地址。
- 大小口径：按 Windows 资源管理器的算法，1 MB = 1024 × 1024 字节；括号里是精确字节数。
- 核验方法（2026-11-03 当天完成）：
  - 直接下载链接：用 `curl -sIL` 确认返回 HTTP 200，大小取 `Content-Length`；
  - GitHub 文件：用 GitHub API 读 `size` 字段，并实际下载确认；
  - NIST 压缩包里的文件：下载压缩包后用 `unzip -l` 读出单个文件大小；
  - Zenodo 文件：用 Zenodo API 读 `size` 字段，并实际下载确认。
- 零件数：已下载的 STEP 文件，用文本统计 `PRODUCT` 实体数（≈ 零件/组件定义数）和 `NEXT_ASSEMBLY_USAGE_OCCURRENCE` 实体数（≈ 装配实例数），只是粗略统计，最终以程序装配树显示为准。IGES 没有统一的装配结构，零件数一律写"待测"。
- 本次已下载到工作机 `/workspace/samples/dl/` 的文件（共约 2.1 GB）：S-01～S-07、S-11～S-13、M-01～M-04、L-01～L-03、X-01～X-05；S-08～S-10 所在的 NIST-PMI-STEP-Files.zip 已下载到 `/workspace/samples/`。其余文件（S-14、M-05～M-07、L-04～L-07、X-06～X-08，均为 Twin Disc 的 IGES）只核验了链接和大小，没有下载。
- 所有文件只用于内部测试，不对外转发、不二次发布。GrabCAD 来源的模型版权归原上传者所有；Twin Disc 网站条款允许个人和业务用途下载，但不允许转贴到其他网站，也不允许修改内容。

## 小文件（≤ 10 MB）

| 编号 | 文件名 | 直接下载链接 |
|---|---|---|
| S-01 | screw.step | https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/master/data/step/screw.step |
| S-02 | linkrods.step | https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/master/data/step/linkrods.step |
| S-03 | as1-ac-214.stp | https://raw.githubusercontent.com/FreeCAD/FreeCAD/main/data/tests/Step/as1-ac-214.stp |
| S-04 | Schenkel.stp | https://raw.githubusercontent.com/FreeCAD/FreeCAD/main/data/examples/Schenkel.stp |
| S-05 | as1_pe.stp | https://downloads.steptools.com/docs/stpfiles/ap203/as1_pe.stp |
| S-06 | moon_buggy_asm.stp | https://downloads.steptools.com/docs/stpfiles/ap203/moon_buggy_asm.stp |
| S-07 | 123Block_Color.stp | https://downloads.steptools.com/docs/stpfiles/ap203e2/123Block_Color.stp |
| S-08～S-10 | nist_ctc_02_asme1_ap242-e2.stp、nist_ftc_09_asme1_ap242-e1.stp、nist_stc_10_asme1_ap242-e2.stp | 先下载压缩包 NIST-PMI-STEP-Files.zip（13,976,599 字节）再解压：https://www.nist.gov/system/files/documents/noindex/2024/06/19/NIST-PMI-STEP-Files.zip |
| S-11 | ROTOR-201NAL-Z7.STEP | https://downloads.steptools.com/docs/stpfiles/bigassy/ROTOR-201NAL-Z7.STEP |
| S-12 | bearing.iges | https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/master/data/iges/bearing.iges |
| S-13 | hammer.iges | https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/master/data/iges/hammer.iges |
| S-14 | 1030490B.igs | https://twindisc.com/downloads/1030490B.igs |

## 中文件（10–50 MB）

| 编号 | 文件名 | 直接下载链接 |
|---|---|---|
| M-01 | Rocky_House.stp | https://downloads.steptools.com/docs/stpfiles/bigassy/Rocky_House.stp |
| M-02 | SO101 Assembly.step | https://raw.githubusercontent.com/TheRobotStudio/SO-ARM100/main/STEP/SO101/SO101%20Assembly.step |
| M-03 | YARR_assembly.STEP | https://raw.githubusercontent.com/ledsandchips/YARR/master/cad_models/YARR_assembly.STEP |
| M-04 | Cruise_Assembly.stp | https://downloads.steptools.com/docs/stpfiles/bigassy/Cruise_Assembly.stp |
| M-05 | SP218P001.IGS | https://twindisc.com/downloads/SP218P001.IGS |
| M-06 | 1026868C.IGS | https://twindisc.com/downloads/1026868C.IGS |
| M-07 | PX13298F.IGS | https://twindisc.com/downloads/PX13298F.IGS |

## 大文件（50–200 MB）

| 编号 | 文件名 | 直接下载链接 |
|---|---|---|
| L-01 | UMC-500_SS_Solid_Model_2019-06_r1.stp | https://downloads.steptools.com/docs/stpfiles/bigassy/UMC-500_SS_Solid_Model_2019-06_r1.stp |
| L-02 | Ai-14R.stp | https://downloads.steptools.com/docs/stpfiles/bigassy/Ai-14R.stp |
| L-03 | NissanGT-R.STEP | https://downloads.steptools.com/docs/stpfiles/bigassy/NissanGT-R.STEP |
| L-04 | PX12820C.igs | https://twindisc.com/downloads/PX12820C.igs |
| L-05 | 1025454D.IGS | https://twindisc.com/downloads/1025454D.IGS |
| L-06 | 1028862.IGS | https://twindisc.com/downloads/1028862.IGS |
| L-07 | 1028499B.IGS | https://twindisc.com/downloads/1028499B.IGS |

## 超大文件（200 MB 以上）

| 编号 | 文件名 | 直接下载链接 |
|---|---|---|
| X-01 | Scania-8x4.stp | https://downloads.steptools.com/docs/stpfiles/bigassy/Scania-8x4.stp |
| X-02 | Scania-Engine-V8-XT-Turbo.step | https://downloads.steptools.com/docs/stpfiles/bigassy/Scania-Engine-V8-XT-Turbo.step |
| X-03 | LUPA-A1000.STEP | https://zenodo.org/records/8156350/files/LUPA-A1000.STEP?download=1 |
| X-04 | 1026064M.STEP | https://twindisc.com/downloads/1026064M.STEP |
| X-05 | 1027247E.STEP | https://twindisc.com/downloads/1027247E.STEP |
| X-06 | 1026064M.IGS | https://twindisc.com/downloads/1026064M.IGS |
| X-07 | PX13226.IGS | https://twindisc.com/downloads/PX13226.IGS |
| X-08 | 1027247E.IGS | https://twindisc.com/downloads/1027247E.IGS |

## 来源页面

- OCCT 自带数据：https://github.com/Open-Cascade-SAS/OCCT/tree/master/data （仓库许可 LGPL-2.1）
- FreeCAD 测试与示例文件：https://github.com/FreeCAD/FreeCAD/tree/main/data （仓库许可 LGPL-2.1）
- STEP Tools 样例文件：https://downloads.steptools.com/docs/stpfiles/ （大装配见 bigassy/index.html，页面注明原始来源为 GrabCAD）
- NIST MBE PMI 测试模型：https://www.nist.gov/ctl/smart-connected-systems-division/smart-connected-manufacturing-systems-group/mbe-pmi-0 （NIST 公开数据，许可见 https://www.nist.gov/open/license ）
- Zenodo LUPA（俄勒冈州立大学开源波浪能装置）：https://zenodo.org/records/8156350 （CC-BY-4.0）
- TheRobotStudio SO-ARM100：https://github.com/TheRobotStudio/SO-ARM100 （Apache-2.0）
- YARR：https://github.com/ledsandchips/YARR （GPL-2.0）
- Twin Disc 公开下载目录：https://twindisc.com/downloads/ （网站条款：https://twindisc.com/terms-conditions/ ）
