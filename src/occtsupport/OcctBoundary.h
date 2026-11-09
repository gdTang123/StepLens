#pragma once

// 访问违例的捕获只放在 OcctBoundary.cpp。那个翻译单元单独用 /EHa。
// 其它模块保持 /EHsc，不能把 /EHa 和 /EHsc 写进同一个编译命令。

#include "core/Result.h"
#include "core/TaskScheduler.h"

#include <memory>
#include <string_view>

namespace steplens::occt {

// 接住访问违例之后进程内存不再可信。标记一旦置上就不再清掉。
inline constexpr std::string_view kProcessContaminatedUserMessage = "进程已污染";

[[nodiscard]] bool processContaminated() noexcept;

// 工作线程里调用 OCCT 的入口。实现是 OSD::SetThreadLocalSignal(OSD_SignalMode_Set, false)：
// 打开 SEH 转换，不打开浮点异常。
// 跑 OCCT 的线程池必须用 makeOcctTaskScheduler 创建，由它在线程入口调用 armWorkerThread。
// 不要自己构造 TaskScheduler 再手写这个钩子。core 不链接 OCCT，所以工厂放在这里。
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

// 唯一允许创建“跑 OCCT 的线程池”的入口。onThreadStart 固定为 OcctBoundary::armWorkerThread。
// 网格线程池必须走这里，不能自己构造 TaskScheduler。
// T05 的加载不进这个池：LoadCoordinator 的工作线程在调用 OCCT 之前，由加载器调用 armWorkerThread()。
[[nodiscard]] std::unique_ptr<TaskScheduler>
makeOcctTaskScheduler(unsigned threads = TaskScheduler::defaultThreadCount());

} // namespace steplens::occt
