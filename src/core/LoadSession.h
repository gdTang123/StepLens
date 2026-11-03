#pragma once

#include "core/CancelToken.h"
#include "core/ProgressSink.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace steplens {

// 一次打开文件的共享状态。GUI 和后台各持有一份 shared_ptr。
// cancel() 只请求停止并冻住进度，立刻返回。后台释放完调用 noteBackgroundReleased()，再丢掉自己的指针。
// 不要把 progress() 的引用存过 session 的生命周期。
class LoadSession {
public:
    explicit LoadSession(std::uint64_t fileBytes, ProgressEstimate estimate = {});

    void cancel() noexcept;
    void noteBackgroundReleased() noexcept;

    [[nodiscard]] bool backgroundReleased() const noexcept;
    [[nodiscard]] CancelToken token() const noexcept;
    [[nodiscard]] ProgressSink& progress() noexcept;
    [[nodiscard]] const ProgressSink& progress() const noexcept;

private:
    CancelSource m_source;
    ProgressSink m_progress;
    // 后台线程写，界面线程读。
    std::atomic<bool> m_released{false};
};

} // namespace steplens
