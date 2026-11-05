#include "core/LoadCoordinator.h"

#include <chrono>
#include <utility>

namespace steplens {

LoadCoordinator::LoadCoordinator() : m_thread([this] { threadMain(); }) {}

LoadCoordinator::~LoadCoordinator() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_hasPending = false;
        m_pending = {};
        if (m_active) {
            m_active->cancel();
        }
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void LoadCoordinator::submit(LoadRequest request) {
    std::lock_guard lock(m_mutex);
    if (m_stop) {
        return;
    }
    if (m_running || m_hasPending) {
        if (m_active) {
            m_active->cancel();
        }
        m_pending = std::move(request);
        m_hasPending = true;
        if (!m_running) {
            m_cv.notify_all();
        }
        return;
    }
    m_pending = std::move(request);
    m_hasPending = true;
    m_cv.notify_all();
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
