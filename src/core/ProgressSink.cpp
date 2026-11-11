#include "core/ProgressSink.h"

namespace steplens {
namespace {

std::int64_t steadyNs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

constexpr double kNsPerSecond = 1'000'000'000.0;

} // namespace

void ProgressSink::touchLocked() noexcept {
    m_lastChangeNs = steadyNs();
}

void ProgressSink::setFractionLocked(float fraction) noexcept {
    fraction = clampUnit(fraction);
    if (m_frozen) {
        return;
    }
    if (fraction > m_fraction) {
        m_fraction = fraction;
        touchLocked();
    }
}

void ProgressSink::publishPhaseLocked(ProgressPhase phase) noexcept {
    if (m_phase == phase) {
        return;
    }
    m_phase = phase;
    if (m_phaseHook != nullptr) {
        m_phaseHook(phase, m_phaseHookUser);
    }
}

void ProgressSink::setPhaseHook(PhaseHook hook, void* user) noexcept {
    std::lock_guard lock(m_mutex);
    m_phaseHook = hook;
    m_phaseHookUser = user;
}

void ProgressSink::enterTimedPhaseLocked(ProgressPhase phase, float bandStart, float bandEnd,
                                         double estimateSeconds) noexcept {
    if (m_frozen) {
        return;
    }
    publishPhaseLocked(phase);
    m_timedBandStart = bandStart;
    m_timedBandEnd = bandEnd;
    m_phaseEstimateSeconds = estimateSeconds;
    m_phaseStartNs = steadyNs();
    m_timedPhase = true;
    setFractionLocked(bandStart);
}

float ProgressSink::fractionLocked() const noexcept {
    float value = m_fraction;
    if (!m_frozen && m_timedPhase) {
        const double elapsed = static_cast<double>(steadyNs() - m_phaseStartNs) / kNsPerSecond;
        const float timed =
            progressFromTimedBand(m_timedBandStart, m_timedBandEnd, elapsed, m_phaseEstimateSeconds);
        if (timed > value) {
            value = timed;
        }
    }
    if (value < m_published) {
        value = m_published;
    } else {
        m_published = value;
    }
    return value;
}

void ProgressSink::reset(std::uint64_t fileBytes, ProgressEstimate estimate) noexcept {
    std::lock_guard lock(m_mutex);
    m_fileBytes = fileBytes;
    m_estimate = estimate;
    m_frozen = false;
    m_timedPhase = false;
    m_indeterminate = false;
    m_fraction = 0.f;
    m_published = 0.f;
    m_stage = LoadStage::Idle;
    publishPhaseLocked(ProgressPhase::Idle);
    m_phaseEstimateSeconds = 0.0;
    m_timedBandStart = 0.f;
    m_timedBandEnd = 0.f;
    const auto now = steadyNs();
    m_loadStartNs = now;
    m_phaseStartNs = now;
    touchLocked();
}

void ProgressSink::setReadingBytes(std::uint64_t bytesRead) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_frozen) {
        return;
    }
    m_timedPhase = false;
    publishPhaseLocked(ProgressPhase::ReadingBytes);
    float fileFraction = 1.f;
    if (m_fileBytes > 0) {
        if (bytesRead > m_fileBytes) {
            bytesRead = m_fileBytes;
        }
        fileFraction = static_cast<float>(static_cast<double>(bytesRead) / static_cast<double>(m_fileBytes));
    }
    setFractionLocked(progressFromReadingBytes(fileFraction));
}

void ProgressSink::enterParsing() noexcept {
    std::lock_guard lock(m_mutex);
    enterTimedPhaseLocked(ProgressPhase::Parsing, kReadingBandEnd, kParsingBandEnd,
                          m_estimate.parseSeconds(m_fileBytes));
}

void ProgressSink::setParsingElapsed(double elapsedSeconds) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_phase != ProgressPhase::Parsing) {
        enterTimedPhaseLocked(ProgressPhase::Parsing, kReadingBandEnd, kParsingBandEnd,
                              m_estimate.parseSeconds(m_fileBytes));
    }
    if (m_frozen) {
        return;
    }
    setFractionLocked(progressFromTimedBand(kReadingBandEnd, kParsingBandEnd, elapsedSeconds,
                                            m_estimate.parseSeconds(m_fileBytes)));
}

void ProgressSink::enterTransfer() noexcept {
    std::lock_guard lock(m_mutex);
    enterTimedPhaseLocked(ProgressPhase::Transferring, kParsingBandEnd, kTransferBandEnd,
                          m_estimate.transferSeconds(m_fileBytes));
}

void ProgressSink::setTransfer(float occtFraction, double elapsedSeconds) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_phase != ProgressPhase::Transferring) {
        enterTimedPhaseLocked(ProgressPhase::Transferring, kParsingBandEnd, kTransferBandEnd,
                              m_estimate.transferSeconds(m_fileBytes));
    }
    if (m_frozen) {
        return;
    }
    const double estimateSeconds = m_estimate.transferSeconds(m_fileBytes);
    const float fromOcct = progressFromTransfer(occtFraction, 0.0, estimateSeconds);
    const float fromCaller = progressFromTransfer(0.f, elapsedSeconds, estimateSeconds);
    setFractionLocked(fromOcct > fromCaller ? fromOcct : fromCaller);
}

void ProgressSink::setPresentFaceFraction(float faceFraction) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_frozen) {
        return;
    }
    m_timedPhase = false;
    publishPhaseLocked(ProgressPhase::Presenting);
    setFractionLocked(progressFromPresenting(faceFraction));
}

void ProgressSink::beginCancel() noexcept {
    std::lock_guard lock(m_mutex);
    const float shown = fractionLocked();
    m_fraction = shown;
    m_published = shown;
    m_frozen = true;
    m_timedPhase = false;
    m_stage = LoadStage::Cancelling;
    publishPhaseLocked(ProgressPhase::Cancelling);
    touchLocked();
}

void ProgressSink::setStage(LoadStage stage) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_frozen) {
        return;
    }
    m_stage = stage;
}

void ProgressSink::setFraction(float fraction) noexcept {
    std::lock_guard lock(m_mutex);
    setFractionLocked(fraction);
}

void ProgressSink::setIndeterminate(bool indeterminate) noexcept {
    std::lock_guard lock(m_mutex);
    m_indeterminate = indeterminate;
}

LoadStage ProgressSink::stage() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_stage;
}

ProgressPhase ProgressSink::phase() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_phase;
}

float ProgressSink::fraction() const noexcept {
    std::lock_guard lock(m_mutex);
    return fractionLocked();
}

bool ProgressSink::indeterminate() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_indeterminate;
}

bool ProgressSink::cancelFrozen() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_frozen;
}

bool ProgressSink::unchangedFor(std::chrono::milliseconds budget) const noexcept {
    std::lock_guard lock(m_mutex);
    if (m_lastChangeNs == 0 || budget.count() <= 0) {
        return false;
    }
    const auto elapsedNs = steadyNs() - m_lastChangeNs;
    const auto budgetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(budget).count();
    return elapsedNs >= budgetNs;
}

double ProgressSink::elapsedSinceLoadStart() const noexcept {
    std::lock_guard lock(m_mutex);
    if (m_loadStartNs == 0) {
        return 0.0;
    }
    return static_cast<double>(steadyNs() - m_loadStartNs) / kNsPerSecond;
}

bool ProgressSink::overEstimate() const noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_timedPhase || !(m_phaseEstimateSeconds > 0.0)) {
        return false;
    }
    const double elapsed = static_cast<double>(steadyNs() - m_phaseStartNs) / kNsPerSecond;
    return elapsed > m_phaseEstimateSeconds;
}

} // namespace steplens
