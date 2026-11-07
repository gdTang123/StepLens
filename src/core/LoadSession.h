#pragma once

#include "core/CancelToken.h"
#include "core/ProgressSink.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace steplens {

// 一次打开文件的共享状态。GUI 和后台各持有一份 shared_ptr。
// cancel() 只请求停止并冻住进度，立刻返回。后台释放完调用 noteBackgroundReleased()，再丢掉自己的指针。
// noteBackgroundReleased() 会唤醒 waitForBackgroundReleased()，调用方不必轮询。
// 不要把 progress() 的引用存过 session 的生命周期。
class LoadSession {
public:
    explicit LoadSession(std::uint64_t fileBytes, ProgressEstimate estimate = {});

    void cancel() noexcept;
    void noteBackgroundReleased() noexcept;

    [[nodiscard]] bool backgroundReleased() const noexcept;
    // 等到后台释放。timeout 耗尽返回 false，不在这里空转。
    [[nodiscard]] bool waitForBackgroundReleased(std::chrono::milliseconds timeout) const;
    [[nodiscard]] CancelToken token() const noexcept;
    [[nodiscard]] ProgressSink& progress() noexcept;
    [[nodiscard]] const ProgressSink& progress() const noexcept;

private:
    CancelSource m_source;
    ProgressSink m_progress;
    // m_mutex 保护 m_released。条件变量和这个标志一起用，避免通知丢失。
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_releasedCv;
    bool m_released = false;
};

} // namespace steplens
