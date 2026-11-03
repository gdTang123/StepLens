#include "core/ProgressSink.h"

namespace steplens {
namespace {

std::int64_t steadyNs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

constexpr double kNsPerSecond = 1'000'000'000.0;

} // namespace

void ProgressSink::touch() noexcept {
    m_lastChangeNs.store(steadyNs(), std::memory_order_relaxed);
}

void ProgressSink::reset(std::uint64_t fileBytes, ProgressEstimate estimate) noexcept {
    m_fileBytes = fileBytes;
    m_estimate = estimate;
    m_frozen.store(false, std::memory_order_relaxed);
    m_timedPhase.store(false, std::memory_order_relaxed);
    m_indeterminate.store(false, std::memory_order_relaxed);
    m_fraction.store(0.f, std::memory_order_relaxed);
    m_stage.store(LoadStage::Idle, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Idle, std::memory_order_relaxed);
    m_phaseEstimateSeconds.store(0.0, std::memory_order_relaxed);
    const auto now = steadyNs();
    m_loadStartNs.store(now, std::memory_order_relaxed);
    m_phaseStartNs.store(now, std::memory_order_relaxed);
    touch();
}

void ProgressSink::enterTimedPhase(ProgressPhase phase, float bandStart, float bandEnd,
                                   double estimateSeconds) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_phase.store(phase, std::memory_order_relaxed);
    m_timedBandStart.store(bandStart, std::memory_order_relaxed);
    m_timedBandEnd.store(bandEnd, std::memory_order_relaxed);
    m_phaseEstimateSeconds.store(estimateSeconds, std::memory_order_relaxed);
    m_phaseStartNs.store(steadyNs(), std::memory_order_relaxed);
    m_timedPhase.store(true, std::memory_order_relaxed);
    setFraction(bandStart);
}

float ProgressSink::timedFractionNow() const noexcept {
    if (!m_timedPhase.load(std::memory_order_relaxed)) {
        return 0.f;
    }
    const auto start = m_phaseStartNs.load(std::memory_order_relaxed);
    const double elapsed = static_cast<double>(steadyNs() - start) / kNsPerSecond;
    return progressFromTimedBand(m_timedBandStart.load(std::memory_order_relaxed),
                                 m_timedBandEnd.load(std::memory_order_relaxed), elapsed,
                                 m_phaseEstimateSeconds.load(std::memory_order_relaxed));
}

void ProgressSink::setReadingBytes(std::uint64_t bytesRead) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_timedPhase.store(false, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::ReadingBytes, std::memory_order_relaxed);
    float fileFraction = 1.f;
    if (m_fileBytes > 0) {
        if (bytesRead > m_fileBytes) {
            bytesRead = m_fileBytes;
        }
        fileFraction = static_cast<float>(static_cast<double>(bytesRead) / static_cast<double>(m_fileBytes));
    }
    setFraction(progressFromReadingBytes(fileFraction));
}

void ProgressSink::enterParsing() noexcept {
    enterTimedPhase(ProgressPhase::Parsing, kReadingBandEnd, kParsingBandEnd,
                    m_estimate.parseSeconds(m_fileBytes));
}

void ProgressSink::setParsingElapsed(double elapsedSeconds) noexcept {
    if (m_phase.load(std::memory_order_relaxed) != ProgressPhase::Parsing) {
        enterParsing();
    }
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    setFraction(progressFromTimedBand(kReadingBandEnd, kParsingBandEnd, elapsedSeconds,
                                      m_estimate.parseSeconds(m_fileBytes)));
}

void ProgressSink::enterTransfer() noexcept {
    enterTimedPhase(ProgressPhase::Transferring, kParsingBandEnd, kTransferBandEnd,
                    m_estimate.transferSeconds(m_fileBytes));
}

void ProgressSink::setTransfer(float occtFraction, double elapsedSeconds) noexcept {
    if (m_phase.load(std::memory_order_relaxed) != ProgressPhase::Transferring) {
        enterTransfer();
    }
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    // 存 OCCT 位置。时间推进由 fraction() 在读的时候算，这样没有回调的空档也会往前走。
    const float fromOcct = progressFromTransfer(occtFraction, 0.0, m_estimate.transferSeconds(m_fileBytes));
    const float fromCaller =
        progressFromTransfer(0.f, elapsedSeconds, m_estimate.transferSeconds(m_fileBytes));
    setFraction(fromOcct > fromCaller ? fromOcct : fromCaller);
}

void ProgressSink::setPresentFaceFraction(float faceFraction) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_timedPhase.store(false, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Presenting, std::memory_order_relaxed);
    setFraction(progressFromPresenting(faceFraction));
}

void ProgressSink::beginCancel() noexcept {
    m_frozen.store(true, std::memory_order_relaxed);
    const float written = m_fraction.load(std::memory_order_relaxed);
    const float timed = timedFractionNow();
    m_fraction.store(timed > written ? timed : written, std::memory_order_relaxed);
    m_timedPhase.store(false, std::memory_order_relaxed);
    m_stage.store(LoadStage::Cancelling, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Cancelling, std::memory_order_relaxed);
}

void ProgressSink::setStage(LoadStage stage) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_stage.store(stage, std::memory_order_relaxed);
}

void ProgressSink::setFraction(float fraction) noexcept {
    fraction = clampUnit(fraction);
    float previous = m_fraction.load(std::memory_order_relaxed);
    while (fraction > previous) {
        if (m_frozen.load(std::memory_order_relaxed)) {
            return;
        }
        if (m_fraction.compare_exchange_weak(previous, fraction, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            touch();
            return;
        }
    }
}

void ProgressSink::setIndeterminate(bool indeterminate) noexcept {
    m_indeterminate.store(indeterminate, std::memory_order_relaxed);
}

LoadStage ProgressSink::stage() const noexcept {
    return m_stage.load(std::memory_order_relaxed);
}

ProgressPhase ProgressSink::phase() const noexcept {
    return m_phase.load(std::memory_order_relaxed);
}

float ProgressSink::fraction() const noexcept {
    const float written = m_fraction.load(std::memory_order_relaxed);
    if (m_frozen.load(std::memory_order_relaxed)) {
        return written;
    }
    const float timed = timedFractionNow();
    return timed > written ? timed : written;
}

bool ProgressSink::indeterminate() const noexcept {
    return m_indeterminate.load(std::memory_order_relaxed);
}

bool ProgressSink::cancelFrozen() const noexcept {
    return m_frozen.load(std::memory_order_relaxed);
}

bool ProgressSink::unchangedFor(std::chrono::milliseconds budget) const noexcept {
    const auto changedAt = m_lastChangeNs.load(std::memory_order_relaxed);
    if (changedAt == 0 || budget.count() <= 0) {
        return false;
    }
    const auto elapsedNs = steadyNs() - changedAt;
    const auto budgetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(budget).count();
    return elapsedNs >= budgetNs;
}

double ProgressSink::elapsedSinceLoadStart() const noexcept {
    const auto start = m_loadStartNs.load(std::memory_order_relaxed);
    if (start == 0) {
        return 0.0;
    }
    return static_cast<double>(steadyNs() - start) / kNsPerSecond;
}

bool ProgressSink::overEstimate() const noexcept {
    if (!m_timedPhase.load(std::memory_order_relaxed)) {
        return false;
    }
    const double estimate = m_phaseEstimateSeconds.load(std::memory_order_relaxed);
    if (!(estimate > 0.0)) {
        return false;
    }
    const auto start = m_phaseStartNs.load(std::memory_order_relaxed);
    const double elapsed = static_cast<double>(steadyNs() - start) / kNsPerSecond;
    return elapsed > estimate;
}

} // namespace steplens
