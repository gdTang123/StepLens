#include "core/ProgressSink.h"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>

namespace {

// G1-3：把 bench/td-wei/core/race.cpp 收成 gtest。
// 写线程在同一次加载里走过 Parsing→Transfer 和 Transfer→Presenting。
// 读线程只统计 seq 稳定的同一次加载内部的回退。
constexpr int kLoadsPerCase = 200000;
constexpr int kSpinIters = 300;
constexpr std::uint64_t kRaceFileBytes = 100ull << 20;

void spinPause() noexcept {
    volatile int sink = 0;
    for (int step = 0; step < kSpinIters; ++step) {
        sink += step;
    }
}

long dropsWithinOneLoad(double parseSecondsPerMib, double transferSecondsPerMib, long* readsOut) {
    steplens::ProgressSink sink;
    steplens::ProgressEstimate estimate;
    estimate.parseSecondsPerMib = parseSecondsPerMib;
    estimate.transferSecondsPerMib = transferSecondsPerMib;

    std::atomic<bool> stop{false};
    std::atomic<long> seq{0};
    std::atomic<long> drops{0};
    std::atomic<long> reads{0};

    std::thread writer([&]() {
        for (int load = 0; load < kLoadsPerCase; ++load) {
            seq.fetch_add(1);
            sink.reset(kRaceFileBytes, estimate);
            seq.fetch_add(1);
            sink.enterParsing();
            spinPause();
            sink.enterTransfer();
            spinPause();
            sink.setPresentFaceFraction(0.f);
        }
        stop.store(true);
    });

    std::thread reader([&]() {
        float previous = -1.f;
        long currentSeq = -1;
        while (!stop.load()) {
            const long before = seq.load();
            const float fraction = sink.fraction();
            const long after = seq.load();
            reads.fetch_add(1);
            if (before != after || (before & 1L) != 0L) {
                currentSeq = -1;
                continue;
            }
            if (before != currentSeq) {
                currentSeq = before;
                previous = fraction;
                continue;
            }
            if (fraction + 1e-6f < previous) {
                drops.fetch_add(1);
            } else {
                previous = fraction;
            }
        }
    });

    writer.join();
    reader.join();
    *readsOut = reads.load();
    return drops.load();
}

void expectNoDrops(double parseSecondsPerMib, double transferSecondsPerMib) {
    long reads = 0;
    const long drops = dropsWithinOneLoad(parseSecondsPerMib, transferSecondsPerMib, &reads);
    EXPECT_GT(reads, 0);
    EXPECT_EQ(drops, 0) << "parse_est=" << parseSecondsPerMib << " transfer_est=" << transferSecondsPerMib
                        << " reads=" << reads << " drops_within_one_load=" << drops;
}

} // namespace

TEST(ProgressSinkRace, NoDropWithinLoadWhenTransferEstimateIsLarge) {
    expectNoDrops(1e-9, 1000.0);
}

TEST(ProgressSinkRace, NoDropWithinLoadWhenBothEstimatesAreTiny) {
    expectNoDrops(1e-9, 1e-9);
}
