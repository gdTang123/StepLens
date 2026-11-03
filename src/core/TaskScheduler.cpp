#include "core/TaskScheduler.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <stdexcept>
#include <utility>

namespace steplens {
namespace {

thread_local const TaskScheduler* t_runningOn = nullptr;

} // namespace

unsigned TaskScheduler::defaultThreadCount() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    // 硬件线程数 - 1，至少 2。hardware_concurrency 为 0 时按至少 2 处理。
    if (hardware <= 2) {
        return 2;
    }
    return hardware - 1;
}

TaskScheduler::TaskScheduler(unsigned threads, std::function<void()> onThreadStart)
    : m_threadCount(threads == 0 ? defaultThreadCount() : threads),
      m_onThreadStart(std::move(onThreadStart)) {
    if (m_threadCount == 0) {
        m_threadCount = defaultThreadCount();
    }
    m_workers.reserve(m_threadCount);
    for (unsigned index = 0; index < m_threadCount; ++index) {
        m_workers.emplace_back([this](const std::stop_token& stop) { workerMain(stop); });
    }
}

TaskScheduler::~TaskScheduler() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_cv.notify_all();
    m_workers.clear();
}

void TaskScheduler::enqueue(std::function<void()> job) {
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) {
            throw std::runtime_error("TaskScheduler is stopping");
        }
        m_queue.push_back(std::move(job));
    }
    m_cv.notify_one();
}

void TaskScheduler::workerMain(const std::stop_token& stop) {
    t_runningOn = this;
    struct ClearRunning {
        ~ClearRunning() {
            t_runningOn = nullptr;
        }
    } clearRunning;
    if (m_onThreadStart) {
        m_onThreadStart();
    }
    while (!stop.stop_requested()) {
        std::function<void()> job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, stop, [this]() { return m_stopping || !m_queue.empty(); });
            if ((stop.stop_requested() || m_stopping) && m_queue.empty()) {
                return;
            }
            if (m_queue.empty()) {
                continue;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            job();
        } catch (...) {
            // packaged_task 自己保存异常。这里只防止任务函数把异常扔出线程。
            continue;
        }
    }
}

void TaskScheduler::parallelFor(std::size_t count, const std::function<void(std::size_t)>& body,
                                const CancelToken& cancel, std::size_t grain) {
    assert(t_runningOn != this);
    if (count == 0 || !body) {
        return;
    }
    cancel.throwIfCancelled();
    if (grain == 0) {
        grain = 1;
    }

    unsigned workers = m_threadCount;
    if (count < workers) {
        workers = static_cast<unsigned>(count);
    }
    const std::size_t chunk = grain;

    std::atomic<std::size_t> cursor{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<bool> stopOthers{false};
    std::mutex errorMutex;
    std::exception_ptr error;

    std::vector<std::future<void>> jobs;
    jobs.reserve(workers);
    try {
        for (unsigned worker = 0; worker < workers; ++worker) {
            (void)worker;
            jobs.push_back(submit([&, chunk]() {
                while (!stopOthers.load(std::memory_order_relaxed)) {
                    if (cancel.isCancelled()) {
                        stopOthers.store(true, std::memory_order_relaxed);
                        return;
                    }
                    const std::size_t begin = cursor.fetch_add(chunk, std::memory_order_relaxed);
                    if (begin >= count) {
                        return;
                    }
                    const std::size_t end = std::min(count, begin + chunk);
                    for (std::size_t index = begin; index < end; ++index) {
                        if (stopOthers.load(std::memory_order_relaxed) || cancel.isCancelled()) {
                            stopOthers.store(true, std::memory_order_relaxed);
                            return;
                        }
                        try {
                            body(index);
                            completed.fetch_add(1, std::memory_order_relaxed);
                        } catch (...) {
                            stopOthers.store(true, std::memory_order_relaxed);
                            std::lock_guard lock(errorMutex);
                            if (!error) {
                                error = std::current_exception();
                            }
                            return;
                        }
                    }
                }
            }));
        }
    } catch (...) {
        for (auto& job : jobs) {
            job.wait();
        }
        throw;
    }

    for (auto& job : jobs) {
        job.get();
    }

    std::exception_ptr thrown;
    {
        std::lock_guard lock(errorMutex);
        thrown = error;
    }
    if (thrown) {
        std::rethrow_exception(thrown);
    }
    if (cancel.isCancelled() && completed.load(std::memory_order_relaxed) < count) {
        throw CancelledException{};
    }
}

} // namespace steplens
