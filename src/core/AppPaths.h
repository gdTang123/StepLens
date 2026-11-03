#pragma once

#include <filesystem>
#include <string_view>

namespace steplens {

[[nodiscard]] std::filesystem::path executableDirectory();

// 先试可执行文件旁边的 folderName。写不进去时用
// %LOCALAPPDATA%\StepLens\folderName（其他系统用用户数据目录）。
[[nodiscard]] std::filesystem::path defaultWritableDirectory(std::string_view folderName);

} // namespace steplens
