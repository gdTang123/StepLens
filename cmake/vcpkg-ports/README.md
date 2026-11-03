# overlay ports

给 OCCT 打补丁或改编译选项时，把端口放在这个目录下，例如 `cmake/vcpkg-ports/opencascade/`。当前没有补丁，vcpkg 使用基线里的官方端口。

基线是 vcpkg 标签 **2026.10.16**，提交 `9eec002df1cd109f8f104cd966a763daf9751489`（2026-10-17）。该提交上的 `opencascade` 端口版本就是 **8.0.1**（port-version 0）。`vcpkg.json` 里的 `overrides` 再锁一次，避免基线升级时悄悄换版本。

回退到 7.9.3 时，把 overrides 改成：

```json
{ "name": "opencascade", "version": "7.9.3", "port-version": 1 }
```

`port-version` 1 是这个基线的版本库里 7.9.3 的最新端口修订。同时按架构 3.3 增加 `occt_compat.h`。这一步要等 M0 spike，现在不要改。
