# 第三方许可证副本

正文是上游文件的逐字副本，来源和版本写在 `SOURCES.txt`。这些副本不是法律审查结论。仓库根上的 `LICENSE` 没有改。

| 文件 | 内容 |
|---|---|
| `Qt-6.12.0-LGPL-3.0-only.txt` | Qt 6.12.0 附带的 LGPL-3.0 |
| `Qt-6.12.0-GPL-3.0-only.txt` | 上一份写明并入的 GPL-3.0 条款。不是把 StepLens 改成 GPL |
| `OCCT-7.9.3-LICENSE_LGPL_21.txt` | OCCT 7.9.3 的 LGPL-2.1 |
| `OCCT-7.9.3-OCCT_LGPL_EXCEPTION.txt` | OCCT 7.9.3 的 LGPL 例外 |
| `occt-7.9.3-vcpkg-port/` | vcpkg 端口 7.9.3#1 的 portfile 和它打入的补丁 |
| `FreeType-2.14.3-FTL.TXT` | FreeType 2.14.3 的 FTL |
| `freetype-2.14.3-vcpkg-port/` | 该基线端口里默认会打入的补丁，不含可选的 subpixel 补丁 |
| `zlib-1.3.2-LICENSE.txt` | zlib 1.3.2 |
| `bzip2-1.0.8-LICENSE.txt` | bzip2 1.0.8 |
| `libpng-1.6.59-LICENSE.txt` | libpng 1.6.59 |
| `brotli-1.2.0-LICENSE.txt` | brotli 1.2.0 |
| `zlib-vcpkg-port/`、`bzip2-vcpkg-port/`、`libpng-vcpkg-port/`、`brotli-vcpkg-port/` | 同一基线的端口和补丁。哪些补丁在 Windows 上打入，看其中的 portfile |

MSVC 的再分发说明不在这里。`scripts/package.py` 从本机官方再发行目录读取 `Redist.txt`。目录可以新于工具集，不能更旧。找不到、读不到工具集版本，或文件不在 `Redist.txt` 里，就停止。不另写许可证，也不从系统目录或网络拿 DLL。
