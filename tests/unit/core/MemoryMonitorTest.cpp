#include "core/MemoryMonitor.h"

#include <gtest/gtest.h>

namespace steplens {

TEST(MemoryMonitorTest, EstimatesConfiguredStepAndIgesMultipliers) {
    const MemoryMonitor monitor({2.0, 3.0});
    EXPECT_EQ(monitor.estimatePeakBytes(100, true), 200U);
    EXPECT_EQ(monitor.estimatePeakBytes(100, false), 300U);
}

TEST(MemoryMonitorTest, ConfirmsOnlyWhenEstimateExceedsAvailableHardLimit) {
    EXPECT_EQ(MemoryMonitor::preflight(59, 100, .60, .80), MemoryMonitor::Preflight::Open);
    EXPECT_EQ(MemoryMonitor::preflight(80, 100, .60, .80), MemoryMonitor::Preflight::Open);
    EXPECT_EQ(MemoryMonitor::preflight(81, 100, .60, .80), MemoryMonitor::Preflight::Confirm);
    EXPECT_EQ(MemoryMonitor::preflight(95, 100, .60, .80), MemoryMonitor::Preflight::Confirm);
}

TEST(MemoryMonitorTest, AllowsOpenWhenAvailableMemoryIsUnknown) {
    EXPECT_EQ(MemoryMonitor::preflight(1000, 0, .60, .80), MemoryMonitor::Preflight::Open);
}

TEST(MemoryMonitorTest, SamplesPhysicalMemoryOnThisPlatform) {
    const auto s = MemoryMonitor().sample();
    EXPECT_GT(s.systemTotalPhys, 0U);
    EXPECT_LE(s.systemAvailPhys, s.systemTotalPhys);
}

} // namespace steplens
