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

// 固定大小的工作线程池。parallelFor 只能从池外的线程调用，不能在池内的任务里等待本池。
// m_mutex 保护 m_queue 和 m_stopping。工作线程在 workerMain 里取任务。
class TaskScheduler {
public:
    explicit TaskScheduler(unsigned threads = defaultThreadCount());
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    template <class F> auto submit(F&& fn) -> std::future<std::invoke_result_t<F>>;

    // 按块把 [0, n) 分给线程池。某个下标抛异常时，其余下标在下一次检查点退出，异常重新抛给调用者。
    // 取消时抛 CancelledException，不把已经跑完的下标算成失败。
    void parallelFor(std::size_t count, const std::function<void(std::size_t)>& body,
                     const CancelToken& cancel);

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
