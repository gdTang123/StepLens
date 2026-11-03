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
    m_released.store(true, std::memory_order_release);
}

bool LoadSession::backgroundReleased() const noexcept {
    return m_released.load(std::memory_order_acquire);
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
