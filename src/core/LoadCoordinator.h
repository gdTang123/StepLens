#pragma once

#include "core/LoadSession.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace steplens {

// 单槽 Last-Request-Wins。
// 任意时刻最多一个后台 LoadSession 在执行。A 还在释放时再提交 B、再提交 C，只保留 C。
// C 的 run 在 A 调用 noteBackgroundReleased() 之后才开始。槽位空出来靠条件变量，不空转。
// run 必须在取消后返回。可以自己调用 noteBackgroundReleased；没调用时协调器补上。
class LoadCoordinator {
public:
    struct LoadRequest {
        std::uint64_t fileBytes = 0;
        ProgressEstimate estimate{};
        std::function<void(const std::shared_ptr<LoadSession>&)> run;
    };

    LoadCoordinator();
    ~LoadCoordinator();

    LoadCoordinator(const LoadCoordinator&) = delete;
    LoadCoordinator& operator=(const LoadCoordinator&) = delete;

    void submit(LoadRequest request);
    void waitUntilIdle();
    [[nodiscard]] int liveBackgroundSessions() const noexcept;

private:
    void threadMain();

    std::mutex m_mutex;
    std::condition_variable m_cv;
    LoadRequest m_pending{};
    bool m_hasPending = false;
    bool m_running = false;
    bool m_stop = false;
    std::shared_ptr<LoadSession> m_active;
    std::atomic<int> m_live{0};
    std::thread m_thread;
};

} // namespace steplens
