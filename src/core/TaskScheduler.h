#pragma once

#include "core/CancelToken.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace steplens {

// 固定大小的工作线程池。m_mutex 保护 m_queue 和 m_stopping。
// parallelFor 不能在本池的工作线程里调用（Debug 下断言），否则会死锁。
// 析构会把队列里还没跑的任务跑完再 join。单个网格任务可以到数秒，后台释放可以到约一分钟。
// 进程退出不要靠这个析构：T11 应 TerminateProcess 或 quick_exit，让操作系统收内存。
// onThreadStart 在每个工作线程入口调用，先于队列里的第一个任务。core 不链接 OCCT。
// 跑 OCCT 的线程池必须用 occt::makeOcctTaskScheduler 创建，由它注入 OcctBoundary::armWorkerThread
// （OSD::SetThreadLocalSignal）。不要在调用方手写这个钩子。
class TaskScheduler {
public:
    explicit TaskScheduler(unsigned threads = defaultThreadCount(), std::function<void()> onThreadStart = {});
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    template <class F> auto submit(F&& fn) -> std::future<std::invoke_result_t<F>>;

    // grain 是每个线程一次连续领走的下标数，默认 1。T07 按大零件优先时不要把多个大任务捆在同一块里。
    // 某个下标抛异常时，其余下标在下一次检查点退出，异常重新抛给调用者。
    // 取消时抛 CancelledException，不把已经跑完的下标算成失败。
    void parallelFor(std::size_t count, const std::function<void(std::size_t)>& body,
                     const CancelToken& cancel, std::size_t grain = 1);

    [[nodiscard]] unsigned threadCount() const noexcept {
        return m_threadCount;
    }

    [[nodiscard]] static unsigned defaultThreadCount() noexcept;

private:
    void enqueue(std::function<void()> job);
    void workerMain(const std::stop_token& stop);

    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::deque<std::function<void()>> m_queue;
    bool m_stopping = false;
    unsigned m_threadCount = 0;
    std::function<void()> m_onThreadStart;
    std::vector<std::jthread> m_workers;
};

template <class F> auto TaskScheduler::submit(F&& fn) -> std::future<std::invoke_result_t<F>> {
    using ResultType = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<F>(fn));
    std::future<ResultType> future = task->get_future();
    enqueue([task]() { (*task)(); });
    return future;
}

} // namespace steplens
