#pragma once

namespace steplens {

// Ready 时 refreshUi 是否应跳过写入「已完成」，以免盖住 measureMessage 阶段提示。
[[nodiscard]] inline bool measureModeBlocksReadyCompleted(bool pointDistanceActive,
                                                          bool angleActive) noexcept {
    return pointDistanceActive || angleActive;
}

} // namespace steplens
