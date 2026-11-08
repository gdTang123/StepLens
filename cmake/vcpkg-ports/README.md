# overlay ports

给 OCCT 打补丁或改编译选项时，把端口放在这个目录下，例如 `cmake/vcpkg-ports/opencascade/`。当前没有补丁，vcpkg 使用基线里的官方端口。

基线是 vcpkg 标签 **2026.10.16**，提交 `9eec002df1cd109f8f104cd966a763daf9751489`（2026-10-17）。该提交上的 `opencascade` 端口版本是 **8.0.1**（port-version 0）。这是基线事实，不是产品默认。

产品默认是 **OCCT 7.9.3**。默认预设把 `VCPKG_MANIFEST_DIR` 指到 `vcpkg-configs/occt-7.9.3`，那里的 `overrides` 锁 `7.9.3#1`。根 `vcpkg.json` 不写 overrides，只给对照预设 `msvc-occt801`。

`port-version` 1 是这个基线的版本库里 7.9.3 的最新端口修订，写成 `"7.9.3#1"`。版本差异放在 `src/occtsupport/occt_compat.h`。
