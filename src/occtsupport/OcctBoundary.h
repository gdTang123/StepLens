#pragma once

// 访问违例的捕获只放在 OcctBoundary.cpp。那个翻译单元单独用 /EHa。
// 其它模块保持 /EHsc，不能把 /EHa 和 /EHsc 写进同一个编译命令。

#include "core/Result.h"
#include "core/TaskScheduler.h"

#include <memory>
#include <string_view>

#if defined(_WIN32)
// 必须在 namespace/class 之外前向声明，与 Windows <eh.h>/winnt.h 的 struct 标签为同一实体。
// 写在 OcctBoundary 类体内会形成 OcctBoundary::_EXCEPTION_POINTERS，
// 与::_EXCEPTION_POINTERS、_se_translator_function、测试 sehSentinel 和无捕获 lambda 均不兼容。
struct _EXCEPTION_POINTERS;
#endif

namespace steplens::occt {

// 接住访问违例之后进程内存不再可信。标记一旦置上就不再清掉。
inline constexpr std::string_view kProcessContaminatedUserMessage = "进程已污染";

[[nodiscard]] bool processContaminated() noexcept;

// 工作线程里调用 OCCT 的入口。实现是 OSD::SetThreadLocalSignal(OSD_SignalMode_Set, false)：
// 打开 SEH 转换，不打开浮点异常。官方 Windows 实现会覆盖当前线程 SEH translator，
// 并执行 _fpreset/_clearfp/_controlfp，不会自动恢复调用者的 FP 控制与译码器。
// 跑 OCCT 的线程池必须用 makeOcctTaskScheduler 创建，由它在线程入口调用 armWorkerThread，
// 使该政策在工作线程寿命内保持。不要自己构造 TaskScheduler 再手写这个钩子。
// 卫审已有条件认可：实际 GetApplication 线程短暂 arm，同 xcaf 锁内初始化，离开 scope
// 恢复调用者 FP/SEH。不要求 GUI/普通 wait 永久 arm，也不禁止 GUI 首入。
// ScopedThreadSignal 按实例保存，支持嵌套与按线程独立。保存成功后才 arm；保存失败不改变
// 环境、不 GetApplication。析构 noexcept，恢复失败 fail-closed（诊断后 terminate），
// 不在展开中再抛。new/分配失败时尚未改环境，不得报已污染。
// 工作线程仍用 armWorkerThread() 保持线程寿命内的政策。
// 不能倒退成裸 GetApplication，也不能把 Interface_Static 搬到 worker。
// core 不链接 OCCT，所以工厂放在这里。
class OcctBoundary {
public:
    static void armWorkerThread();

    // 非工作线程上短暂执行 OCCT 初始化（如 GetApplication）时使用。
    // 构造时保存本线程 FP/SEH，再调用 armWorkerThread；析构时恢复保存的状态。
    class ScopedThreadSignal {
    public:
        ScopedThreadSignal();
        ~ScopedThreadSignal() noexcept;
        ScopedThreadSignal(const ScopedThreadSignal&) = delete;
        ScopedThreadSignal& operator=(const ScopedThreadSignal&) = delete;

    private:
        struct SavedState;
        SavedState* m_saved = nullptr;
        static bool save(SavedState& state);
        static bool restore(const SavedState& state) noexcept;
    };

    // 测试缝：迫使下一次 ScopedThreadSignal 保存失败（返回 false 则不 arm）。生产不调用。
    static void setScopedThreadSignalSaveHookForTesting(bool (*hook)());
    // 测试缝：arm 成功后、构造完成前调用，可抛以验证回滚。生产不调用。
    static void setScopedThreadSignalArmHookForTesting(void (*hook)());
    // 恢复失败已 fail-closed 并 terminate 前可见。分配失败未改环境时此位仍为 false。
    [[nodiscard]] static bool scopedThreadSignalRestoreFailed() noexcept;

    // 测试缝：本线程已经执行过几次 armWorkerThread。生产代码不读它。
    // 计数发生在钩子内部，所以只调用 SetThreadLocalSignal 之外的空函数不会增加。
    [[nodiscard]] static int armCountOnThisThread() noexcept;

#if defined(_WIN32)
    using SehTranslatorForTesting = void (*)(unsigned, struct ::_EXCEPTION_POINTERS*);
    // 测试缝：在本 /EHa 翻译单元内交换当前线程 SEH translator。生产不调用。
    // /EHsc 测试用它安装 sentinel 并核验 ScopedThreadSignal 已恢复调用者译码器，
    // 不得在测试 TU 里直接调用 _set_se_translator（C4535 需要 /EHa）。
    // 不删除 sentinel，不削弱 FP/SEH 恢复合同。
    static SehTranslatorForTesting exchangeSehTranslatorForTesting(SehTranslatorForTesting next);
#endif

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
