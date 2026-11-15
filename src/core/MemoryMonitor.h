#pragma once

#include <cstdint>

namespace steplens {

class MemoryMonitor final {
public:
    struct Sample {
        std::uint64_t processCommit = 0;
        std::uint64_t processPeak = 0;
        std::uint64_t systemAvailPhys = 0;
        std::uint64_t systemTotalPhys = 0;
    };

    struct EstimateCoefficients {
        // 9.0: Architecture §6.3 extrapolation; upper end of the observed ~6–9× range.
        // IGES has no separate evidence, so it uses the same multiplier.
        double step;
        double iges;
    };

    enum class Preflight { Open, Confirm, Block };

    [[nodiscard]] Sample sample() const;
    [[nodiscard]] std::uint64_t estimatePeakBytes(std::uint64_t fileBytes, bool isStep) const;
    [[nodiscard]] static Preflight preflight(std::uint64_t estimatedPeak, std::uint64_t totalPhys,
                                             double softLimitRatio, double hardLimitRatio);
    explicit MemoryMonitor(EstimateCoefficients coefficients = {9.0, 9.0}) : m_coefficients(coefficients) {}

private:
    EstimateCoefficients m_coefficients;
};

} // namespace steplens
