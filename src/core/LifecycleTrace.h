#pragma once

#include <string_view>

namespace steplens {

// 独立于 Log 的进程退出取证通道，文件句柄与状态故意存活到进程结束。
void initializeLifecycleTrace();
void lifecycleTrace(std::string_view event, std::string_view identity = {}, int exitCode = -1) noexcept;

} // namespace steplens
