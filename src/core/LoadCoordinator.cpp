#include "core/LoadCoordinator.h"

#include <chrono>
#include <utility>

namespace steplens {

LoadCoordinator::LoadCoordinator() : m_thread([this] { threadMain(); }) {}

LoadCoordinator::~LoadCoordinator() {
    std::shared_ptr<LoadSession> cancelSession;
    LoadRequest displaced;
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        displaced = std::move(m_pending);
        m_hasPending = false;
        cancelSession = m_active;
    }
    displaced = {};
    if (cancelSession) {
        cancelSession->cancel();
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void LoadCoordinator::submit(LoadRequest request) {
    std::shared_ptr<LoadSession> cancelSession;
    LoadRequest displaced;
    bool shouldNotify = false;
    {
        std::lock_guard lock(m_mutex);
        if (!m_stop) {
            if (m_running || m_hasPending) {
                cancelSession = m_active;
                displaced = std::move(m_pending);
            }
            m_pending = std::move(request);
            m_hasPending = true;
            shouldNotify = !m_running;
        }
    }
    // 旧请求的析构和 request_stop 的同步回调都可能重入 submit。锁已经放下。
    displaced = {};
    if (cancelSession) {
        cancelSession->cancel();
    }
    if (shouldNotify) {
        m_cv.notify_all();
    }
}

void LoadCoordinator::waitUntilIdle() {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return !m_running && !m_hasPending; });
}

int LoadCoordinator::liveBackgroundSessions() const noexcept {
    return m_live.load(std::memory_order_acquire);
}

void LoadCoordinator::threadMain() {
    for (;;) {
        LoadRequest job;
        std::shared_ptr<LoadSession> session;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || m_hasPending; });
            if (m_stop && !m_hasPending) {
                m_running = false;
                m_cv.notify_all();
                return;
            }
            job = std::move(m_pending);
            m_pending = {};
            m_hasPending = false;
            session = std::make_shared<LoadSession>(job.fileBytes, job.estimate);
            m_active = session;
            m_running = true;
        }

        m_live.fetch_add(1, std::memory_order_acq_rel);
        try {
            // core 不链接 OCCT，这条线程的入口不能调用 OcctBoundary::armWorkerThread。
            // 在这里跑 STEP/IGES 的加载器会在调用 OCCT 之前自己 arm。
            if (job.run) {
                job.run(session);
            }
        } catch (...) {
            session->cancel();
        }
        if (!session->backgroundReleased()) {
            session->noteBackgroundReleased();
        }
        // 谓词已经为真时立刻返回，把“释放完成”走通知，而不是空转等标志。
        static_cast<void>(session->waitForBackgroundReleased(std::chrono::milliseconds(0)));
        m_live.fetch_sub(1, std::memory_order_acq_rel);

        {
            std::lock_guard lock(m_mutex);
            if (m_active == session) {
                m_active.reset();
            }
            if (!m_hasPending) {
                m_running = false;
                m_cv.notify_all();
            }
        }
    }
}

} // namespace steplens
