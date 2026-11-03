# 第三方许可证

T17 打包时把下列文本放进绿色版的 `licenses/` 目录。现在还没有随仓库分发这些组件，所以这里不复制许可证正文。

| 组件 | 许可证 | 发布时要附的内容 |
|---|---|---|
| Qt 6 开源版 | LGPLv3 | 许可证文本、关于对话框里的声明、该版本 Qt 源码的获取方式 |
| Open CASCADE Technology | LGPL-2.1 + OCCT 例外 | `LICENSE_LGPL_21.txt`、`OCCT_LGPL_EXCEPTION.txt`。给 OCCT 打过补丁时，仓库里的 overlay patch 就是修改说明 |
| FreeType | FTL 或 GPL-2.0，本项目按 FTL | FTL 文本 |
| oneTBB | Apache-2.0 | 只有 `msvc-tbb` 预设启用时才需要 |
| MSVC 运行库 | 随 Visual Studio 的再分发说明 | app-local 部署说明 |

样例模型不进仓库，也不进 zip。
