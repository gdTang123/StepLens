#include "core/LoadSession.h"

namespace steplens {

LoadSession::LoadSession(std::uint64_t fileBytes, ProgressEstimate estimate) {
    m_progress.reset(fileBytes, estimate);
}

void LoadSession::cancel() noexcept {
    m_source.requestStop();
    m_progress.beginCancel();
}

void LoadSession::noteBackgroundReleased() noexcept {
    {
        std::lock_guard lock(m_mutex);
        m_released = true;
    }
    m_releasedCv.notify_all();
}

bool LoadSession::backgroundReleased() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_released;
}

bool LoadSession::waitForBackgroundReleased(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(m_mutex);
    return m_releasedCv.wait_for(lock, timeout, [this] { return m_released; });
}

CancelToken LoadSession::token() const noexcept {
    return m_source.token();
}

ProgressSink& LoadSession::progress() noexcept {
    return m_progress;
}

const ProgressSink& LoadSession::progress() const noexcept {
    return m_progress;
}

} // namespace steplens
