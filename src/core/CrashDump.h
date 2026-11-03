#pragma once

#include "core/Result.h"

#include <filesystem>

namespace steplens {

// Windows 上安装未处理异常过滤器，写出 MiniDump。其他系统只保证接口能调用，不生成 .dmp。
// core 不链接 OCCT，所以这里不调用 OSD::SetSignal；等进程已经链上 OCCT 再在入口调用。
// 过滤器只读 install 时写好的目录缓冲区，崩溃路径上不再分配 C++ 对象。
class CrashDump {
public:
    static void install(const std::filesystem::path& directory);
    static void installDefault();

    // 在进程内触发一次可捕获的异常并写出转储，调用方继续运行。用来验证转储内容，不是闪退。
    [[nodiscard]] static Result<std::filesystem::path>
    captureSimulatedCrash(const std::filesystem::path& directory);

    // 真正的未处理崩溃。会结束进程。单元测试不要调用。
    [[noreturn]] static void crashForDump();
};

} // namespace steplens
