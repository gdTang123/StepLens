#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace steplens {

// 架构 6.5。比例来自摸底：读取短、转换长。时间估算超时后本阶段停在 95%，不倒退、不假装完成。
inline constexpr float kReadingBandEnd = 0.08f;
inline constexpr float kParsingBandEnd = 0.20f;
inline constexpr float kTransferBandEnd = 0.90f;
inline constexpr float kPresentBandEnd = 1.0f;
inline constexpr float kStageTimeCap = 0.95f;
inline constexpr double kDefaultParseSecondsPerMib = 0.030;
inline constexpr double kDefaultTransferSecondsPerMib = 0.15;
inline constexpr std::uint64_t kBytesPerMib = 1024ull * 1024ull;
inline constexpr double kDefaultEstimateAlpha = 0.3;

enum class LoadStage : std::uint8_t {
    Idle,
    Reading,
    Transferring,
    BuildingModel,
    MeshingCoarse,
    Presenting,
    Refining,
    Ready,
    Cancelling,
    Failed
};

// 进度条上的四段，比 LoadStage 更细（解析结构仍算在 Reading 里）。
enum class ProgressPhase : std::uint8_t {
    Idle,
    ReadingBytes,
    Parsing,
    Transferring,
    Presenting,
    Cancelling,
    Failed,
    Ready
};

[[nodiscard]] constexpr std::string_view progressPhaseLabel(ProgressPhase phase) noexcept {
    switch (phase) {
    case ProgressPhase::ReadingBytes:
        return "读取文件";
    case ProgressPhase::Parsing:
        return "解析结构";
    case ProgressPhase::Transferring:
        return "转换几何";
    case ProgressPhase::Presenting:
        return "生成显示";
    case ProgressPhase::Idle:
    case ProgressPhase::Cancelling:
    case ProgressPhase::Failed:
    case ProgressPhase::Ready:
        break;
    }
    return "";
}

[[nodiscard]] inline double secondsForBytes(std::uint64_t fileBytes, double secondsPerMib) noexcept {
    const double mib = static_cast<double>(fileBytes) / static_cast<double>(kBytesPerMib);
    const double seconds = mib * secondsPerMib;
    if (!(seconds > 0.0)) {
        return 0.0;
    }
    return seconds;
}

[[nodiscard]] constexpr float clampUnit(float value) noexcept {
    if (!(value > 0.f)) {
        return 0.f;
    }
    if (value > 1.f) {
        return 1.f;
    }
    return value;
}

// 预估时间驱动的阶段内进度，封顶 95%。estimate <= 0 时停在阶段起点。
[[nodiscard]] inline float cappedTimeLocal(double elapsedSeconds, double estimateSeconds) noexcept {
    if (!(estimateSeconds > 0.0) || !(elapsedSeconds > 0.0)) {
        return 0.f;
    }
    const double ratio = elapsedSeconds / estimateSeconds;
    if (!(ratio > 0.0)) {
        return 0.f;
    }
    const float local = static_cast<float>(ratio);
    if (!(local < kStageTimeCap)) {
        return kStageTimeCap;
    }
    return local;
}

[[nodiscard]] inline float bandFraction(float bandStart, float bandEnd, float local) noexcept {
    local = clampUnit(local);
    return bandStart + (bandEnd - bandStart) * local;
}

[[nodiscard]] inline float progressFromReadingBytes(float fileFraction) noexcept {
    return bandFraction(0.f, kReadingBandEnd, clampUnit(fileFraction));
}

[[nodiscard]] inline float progressFromTimedBand(float bandStart, float bandEnd, double elapsedSeconds,
                                                 double estimateSeconds) noexcept {
    return bandFraction(bandStart, bandEnd, cappedTimeLocal(elapsedSeconds, estimateSeconds));
}

// max(OCCT 位置, 时间比例)，再封顶 95%。OCCT 在取消后会跳到 100%，调用方应先 beginCancel。
[[nodiscard]] inline float progressFromTransfer(float occtFraction, double elapsedSeconds,
                                                double estimateSeconds) noexcept {
    const float fromOcct = clampUnit(occtFraction);
    const float fromTime = cappedTimeLocal(elapsedSeconds, estimateSeconds);
    float local = fromOcct > fromTime ? fromOcct : fromTime;
    if (local > kStageTimeCap) {
        local = kStageTimeCap;
    }
    return bandFraction(kParsingBandEnd, kTransferBandEnd, local);
}

[[nodiscard]] inline float progressFromPresenting(float faceFraction) noexcept {
    return bandFraction(kTransferBandEnd, kPresentBandEnd, clampUnit(faceFraction));
}

// 系数存在内存里。写进 StepLens.ini 是 T14 的事。observe 由加载线程在一次加载结束后独占调用。
struct ProgressEstimate {
    double parseSecondsPerMib = kDefaultParseSecondsPerMib;
    double transferSecondsPerMib = kDefaultTransferSecondsPerMib;

    [[nodiscard]] double parseSeconds(std::uint64_t fileBytes) const noexcept {
        return secondsForBytes(fileBytes, parseSecondsPerMib);
    }

    [[nodiscard]] double transferSeconds(std::uint64_t fileBytes) const noexcept {
        return secondsForBytes(fileBytes, transferSecondsPerMib);
    }

    void observe(double parseRate, double transferRate, double alpha = kDefaultEstimateAlpha) noexcept {
        if (!(alpha >= 0.0) || alpha > 1.0) {
            alpha = kDefaultEstimateAlpha;
        }
        if (parseRate > 0.0) {
            parseSecondsPerMib = alpha * parseRate + (1.0 - alpha) * parseSecondsPerMib;
        }
        if (transferRate > 0.0) {
            transferSecondsPerMib = alpha * transferRate + (1.0 - alpha) * transferSecondsPerMib;
        }
    }
};

// 工作线程写阶段和 OCCT 给出的位置。没有回调的空档里，fraction() 用阶段开始时间和预估耗时自己往前走。
// 定时阶段的 band、起点、预估和 timedPhase 必须整组快照。分开的 relaxed 原子量会在
// Parsing→Transfer、Transfer/Reading→Presenting 两个窗口里被读端拆开，fraction() 就会倒退。
// 这里用一把 mutex：TSan 认得锁；seqlock 保护普通字段仍会被 TSan 当成数据竞争。
// fraction() 再对同一次加载上一次返回的比例做单调钳位。reset() 清掉钳位，下一次加载可以从 0 开始。
// 冻结后忽略后续写入。进度写端只改 ProgressPhase。LoadStage 由 LoadPipeline 调用 setStage。
class ProgressSink {
public:
    void reset(std::uint64_t fileBytes, ProgressEstimate estimate = {}) noexcept;

    void setReadingBytes(std::uint64_t bytesRead) noexcept;
    // 流读完、进入没有回调的解析段时调用。之后 GUI 轮询 fraction() 就会按时间往前走。
    void enterParsing() noexcept;
    void setParsingElapsed(double elapsedSeconds) noexcept;
    void enterTransfer() noexcept;
    void setTransfer(float occtFraction, double elapsedSeconds) noexcept;
    void setPresentFaceFraction(float faceFraction) noexcept;

    // 软取消：冻住当前显示值，忽略 OCCT 随后跳到 100% 的那一次。不等待后台。
    void beginCancel() noexcept;

    void setStage(LoadStage stage) noexcept;
    void setFraction(float fraction) noexcept;
    void setIndeterminate(bool indeterminate) noexcept;

    [[nodiscard]] LoadStage stage() const noexcept;
    [[nodiscard]] ProgressPhase phase() const noexcept;
    [[nodiscard]] float fraction() const noexcept;
    [[nodiscard]] bool indeterminate() const noexcept;
    [[nodiscard]] bool cancelFrozen() const noexcept;
    [[nodiscard]] bool unchangedFor(std::chrono::milliseconds budget) const noexcept;
    [[nodiscard]] double elapsedSinceLoadStart() const noexcept;
    [[nodiscard]] bool overEstimate() const noexcept;

    // 加载线程在 reset 之后独占读取。不要在 GUI 线程和 reset 并发调用。
    [[nodiscard]] std::uint64_t fileBytes() const noexcept {
        return m_fileBytes;
    }
    [[nodiscard]] const ProgressEstimate& estimate() const noexcept {
        return m_estimate;
    }

private:
    void touchLocked() noexcept;
    void setFractionLocked(float fraction) noexcept;
    void enterTimedPhaseLocked(ProgressPhase phase, float bandStart, float bandEnd,
                               double estimateSeconds) noexcept;
    [[nodiscard]] float fractionLocked() const noexcept;

    // 保护定时阶段快照、已写入比例、阶段、冻结标志，以及同一次加载的已发布比例。
    mutable std::mutex m_mutex;
    mutable float m_published = 0.f;

    LoadStage m_stage = LoadStage::Idle;
    ProgressPhase m_phase = ProgressPhase::Idle;
    float m_fraction = 0.f;
    bool m_indeterminate = false;
    bool m_frozen = false;
    bool m_timedPhase = false;
    std::int64_t m_lastChangeNs = 0;
    std::int64_t m_loadStartNs = 0;
    std::int64_t m_phaseStartNs = 0;
    float m_timedBandStart = 0.f;
    float m_timedBandEnd = 0.f;
    double m_phaseEstimateSeconds = 0.0;

    std::uint64_t m_fileBytes = 0;
    ProgressEstimate m_estimate{};
};

} // namespace steplens
