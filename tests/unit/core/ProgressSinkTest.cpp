#include "core/ProgressSink.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

constexpr std::uint64_t kFileBytes = 100ull * steplens::kBytesPerMib;

} // namespace

TEST(ProgressMath, BandsMatchSpikeRatios) {
    EXPECT_FLOAT_EQ(steplens::progressFromReadingBytes(0.f), 0.f);
    EXPECT_FLOAT_EQ(steplens::progressFromReadingBytes(0.5f), 0.04f);
    EXPECT_FLOAT_EQ(steplens::progressFromReadingBytes(1.f), steplens::kReadingBandEnd);

    const double parseEstimate = 0.030 * 100.0;
    EXPECT_FLOAT_EQ(steplens::progressFromTimedBand(steplens::kReadingBandEnd, steplens::kParsingBandEnd, 0.0,
                                                    parseEstimate),
                    steplens::kReadingBandEnd);
    const float halfway = steplens::progressFromTimedBand(
        steplens::kReadingBandEnd, steplens::kParsingBandEnd, parseEstimate * 0.5, parseEstimate);
    EXPECT_NEAR(halfway, 0.14f, 0.0001f);
    const float overtime = steplens::progressFromTimedBand(
        steplens::kReadingBandEnd, steplens::kParsingBandEnd, parseEstimate * 4.0, parseEstimate);
    EXPECT_LT(overtime, steplens::kParsingBandEnd);
    EXPECT_NEAR(overtime, 0.08f + 0.12f * steplens::kStageTimeCap, 0.0001f);

    const double transferEstimate = 0.15 * 100.0;
    const float fromTime = steplens::progressFromTransfer(0.f, transferEstimate * 0.5, transferEstimate);
    EXPECT_NEAR(fromTime, 0.55f, 0.0001f);
    const float fromOcct = steplens::progressFromTransfer(0.8f, 0.0, transferEstimate);
    EXPECT_NEAR(fromOcct, 0.20f + 0.70f * 0.8f, 0.0001f);
    const float capped = steplens::progressFromTransfer(1.f, transferEstimate * 5.0, transferEstimate);
    EXPECT_NEAR(capped, 0.20f + 0.70f * steplens::kStageTimeCap, 0.0001f);
    EXPECT_LT(capped, steplens::kTransferBandEnd);

    EXPECT_FLOAT_EQ(steplens::progressFromPresenting(0.f), steplens::kTransferBandEnd);
    EXPECT_FLOAT_EQ(steplens::progressFromPresenting(1.f), steplens::kPresentBandEnd);
    EXPECT_EQ(steplens::progressPhaseLabel(steplens::ProgressPhase::Parsing), "解析结构");
}

TEST(ProgressEstimate, ExponentialAverage) {
    steplens::ProgressEstimate estimate;
    estimate.observe(0.060, 0.30, 0.5);
    EXPECT_DOUBLE_EQ(estimate.parseSecondsPerMib, 0.045);
    EXPECT_DOUBLE_EQ(estimate.transferSecondsPerMib, 0.225);
    EXPECT_DOUBLE_EQ(estimate.parseSeconds(kFileBytes), 4.5);
}

TEST(ProgressSink, MonotonicBandsAndIgnoresCancelJump) {
    steplens::ProgressSink sink;
    sink.reset(kFileBytes);
    sink.setReadingBytes(kFileBytes / 2);
    EXPECT_FLOAT_EQ(sink.fraction(), 0.04f);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::ReadingBytes);

    sink.setReadingBytes(kFileBytes);
    EXPECT_FLOAT_EQ(sink.fraction(), steplens::kReadingBandEnd);

    sink.setParsingElapsed(0.0);
    EXPECT_FLOAT_EQ(sink.fraction(), steplens::kReadingBandEnd);
    sink.setParsingElapsed(1000.0);
    const float stalled = sink.fraction();
    EXPECT_LT(stalled, steplens::kParsingBandEnd);
    sink.setParsingElapsed(0.0);
    EXPECT_FLOAT_EQ(sink.fraction(), stalled);

    sink.setTransfer(0.1f, 0.0);
    EXPECT_GT(sink.fraction(), stalled);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::Transferring);
    EXPECT_EQ(sink.stage(), steplens::LoadStage::Idle);
    sink.setStage(steplens::LoadStage::BuildingModel);
    const float duringTransfer = sink.fraction();

    sink.beginCancel();
    EXPECT_TRUE(sink.cancelFrozen());
    EXPECT_EQ(sink.stage(), steplens::LoadStage::Cancelling);
    sink.setTransfer(1.f, 1000.0);
    sink.setPresentFaceFraction(1.f);
    EXPECT_FLOAT_EQ(sink.fraction(), duringTransfer);
}

TEST(ProgressSink, PresentingReachesEndAndStallTimer) {
    steplens::ProgressSink sink;
    sink.reset(kFileBytes);
    sink.setPresentFaceFraction(1.f);
    EXPECT_FLOAT_EQ(sink.fraction(), 1.f);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::Presenting);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(sink.unchangedFor(std::chrono::milliseconds(5)));
    EXPECT_FALSE(sink.unchangedFor(std::chrono::hours(1)));
}

TEST(ProgressSink, AdvancesDuringCallbackGap) {
    steplens::ProgressSink sink;
    sink.reset(steplens::kBytesPerMib, steplens::ProgressEstimate{0.05, 0.05});
    sink.enterParsing();
    const float before = sink.fraction();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const float after = sink.fraction();
    EXPECT_GT(after, before);
    EXPECT_LT(after, steplens::kParsingBandEnd);
    EXPECT_GT(sink.elapsedSinceLoadStart(), 0.0);
}

TEST(ProgressSink, ConcurrentWritersDoNotGoBackwards) {
    steplens::ProgressSink sink;
    sink.reset(steplens::kBytesPerMib);
    constexpr int kSteps = 400;
    std::atomic<int> regressions{0};
    std::atomic<bool> done{false};
    std::thread reader([&]() {
        float previous = 0.f;
        while (!done.load()) {
            const float current = sink.fraction();
            if (current + 0.000001f < previous) {
                regressions.fetch_add(1);
            }
            if (current > previous) {
                previous = current;
            }
        }
    });
    std::thread left([&]() {
        for (int step = 0; step < kSteps; ++step) {
            sink.setPresentFaceFraction(static_cast<float>(step) / static_cast<float>(kSteps));
        }
    });
    std::thread right([&]() {
        for (int step = 0; step < kSteps; ++step) {
            sink.setPresentFaceFraction(static_cast<float>(step) / static_cast<float>(kSteps));
        }
    });
    left.join();
    right.join();
    done.store(true);
    reader.join();
    EXPECT_EQ(regressions.load(), 0);
    EXPECT_GT(sink.fraction(), 0.99f);
}
