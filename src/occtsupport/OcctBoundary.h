#pragma once

// 访问违例的捕获只放在 OcctBoundary.cpp。那个翻译单元单独用 /EHa。
// 其它模块保持 /EHsc，不能把 /EHa 和 /EHsc 写进同一个编译命令。

#include "core/Result.h"

#include <string_view>

namespace steplens::occt {

// 接住访问违例之后进程内存不再可信。标记一旦置上就不再清掉。
inline constexpr std::string_view kProcessContaminatedUserMessage = "进程已污染";

[[nodiscard]] bool processContaminated() noexcept;

// 工作线程里调用 OCCT 的入口。TaskScheduler::onThreadStart 注入本函数。
// 实现是 OSD::SetThreadLocalSignal(OSD_SignalMode_Set, false)：打开 SEH 转换，不打开浮点异常。
class OcctBoundary {
public:
    static void armWorkerThread();

    // 测试缝：本线程已经执行过几次 armWorkerThread。生产代码不读它。
    // 计数发生在钩子内部，所以只调用 SetThreadLocalSignal 之外的空函数不会增加。
    [[nodiscard]] static int armCountOnThisThread() noexcept;

    // 在当前线程触发一次空指针写入，并收成 Result。进程不退出。
    // 成功接住时 code 为 Internal，userMessage 为 kProcessContaminatedUserMessage，并置
    // processContaminated()。
    [[nodiscard]] static Result<int> probeNullPointerAccess();
};

} // namespace steplens::occt
