#include "core/ProgressSink.h"

namespace steplens {
namespace {

std::int64_t steadyNs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

} // namespace

void ProgressSink::touch() noexcept {
    m_lastChangeNs.store(steadyNs(), std::memory_order_relaxed);
}

void ProgressSink::reset(std::uint64_t fileBytes, ProgressEstimate estimate) noexcept {
    m_fileBytes = fileBytes;
    m_estimate = estimate;
    m_frozen.store(false, std::memory_order_relaxed);
    m_indeterminate.store(false, std::memory_order_relaxed);
    m_fraction.store(0.f, std::memory_order_relaxed);
    m_stage.store(LoadStage::Idle, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Idle, std::memory_order_relaxed);
    touch();
}

void ProgressSink::setReadingBytes(std::uint64_t bytesRead) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_stage.store(LoadStage::Reading, std::memory_order_relaxed);
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

void ProgressSink::setParsingElapsed(double elapsedSeconds) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_stage.store(LoadStage::Reading, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Parsing, std::memory_order_relaxed);
    setFraction(progressFromTimedBand(kReadingBandEnd, kParsingBandEnd, elapsedSeconds,
                                      m_estimate.parseSeconds(m_fileBytes)));
}

void ProgressSink::setTransfer(float occtFraction, double elapsedSeconds) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_stage.store(LoadStage::Transferring, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Transferring, std::memory_order_relaxed);
    setFraction(progressFromTransfer(occtFraction, elapsedSeconds, m_estimate.transferSeconds(m_fileBytes)));
}

void ProgressSink::setPresentFaceFraction(float faceFraction) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    m_stage.store(LoadStage::Presenting, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Presenting, std::memory_order_relaxed);
    setFraction(progressFromPresenting(faceFraction));
}

void ProgressSink::beginCancel() noexcept {
    m_frozen.store(true, std::memory_order_relaxed);
    m_stage.store(LoadStage::Cancelling, std::memory_order_relaxed);
    m_phase.store(ProgressPhase::Cancelling, std::memory_order_relaxed);
}

void ProgressSink::setStage(LoadStage stage) noexcept {
    m_stage.store(stage, std::memory_order_relaxed);
}

void ProgressSink::setFraction(float fraction) noexcept {
    if (m_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    fraction = clampUnit(fraction);
    const float previous = m_fraction.load(std::memory_order_relaxed);
    if (fraction < previous) {
        fraction = previous;
    }
    if (fraction != previous) {
        m_fraction.store(fraction, std::memory_order_relaxed);
        touch();
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
    return m_fraction.load(std::memory_order_relaxed);
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

} // namespace steplens
