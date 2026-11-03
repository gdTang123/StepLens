#include "core/TaskScheduler.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <vector>

TEST(TaskScheduler, DefaultThreadCountAtLeastTwo) {
    EXPECT_GE(steplens::TaskScheduler::defaultThreadCount(), 2u);
}

TEST(TaskScheduler, SubmitReturnsValueAndException) {
    steplens::TaskScheduler scheduler(2);
    auto value = scheduler.submit([]() { return 7; });
    EXPECT_EQ(value.get(), 7);

    auto failed = scheduler.submit([]() -> int { throw std::runtime_error("submit failed"); });
    EXPECT_THROW(failed.get(), std::runtime_error);
}

TEST(TaskScheduler, ParallelForVisitsEachIndexOnce) {
    steplens::TaskScheduler scheduler(2);
    constexpr std::size_t kCount = 200;
    std::vector<std::atomic<int>> hits(kCount);
    scheduler.parallelFor(
        kCount, [&](std::size_t index) { hits[index].fetch_add(1); }, steplens::CancelToken{});
    for (const auto& hit : hits) {
        EXPECT_EQ(hit.load(), 1);
    }
}

TEST(TaskScheduler, ParallelForRethrowsAndPoolSurvives) {
    steplens::TaskScheduler scheduler(2);
    try {
        scheduler.parallelFor(
            32,
            [](std::size_t index) {
                if (index == 3) {
                    throw std::runtime_error("mesh failed");
                }
            },
            steplens::CancelToken{});
        FAIL() << "parallelFor should rethrow";
    } catch (const std::runtime_error& ex) {
        EXPECT_STREQ(ex.what(), "mesh failed");
    }

    std::atomic<int> sum{0};
    scheduler.parallelFor(
        10, [&](std::size_t index) { sum.fetch_add(static_cast<int>(index)); }, steplens::CancelToken{});
    EXPECT_EQ(sum.load(), 45);
}

TEST(TaskScheduler, ParallelForStopsWhenCancelled) {
    steplens::TaskScheduler scheduler(2);
    steplens::CancelSource source;
    std::atomic<bool> entered{false};
    std::thread canceller([&]() {
        while (!entered.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        source.requestStop();
    });

    EXPECT_THROW(scheduler.parallelFor(
                     100000,
                     [&](std::size_t) {
                         entered.store(true);
                         while (!source.stopRequested()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(1));
                         }
                     },
                     source.token()),
                 steplens::CancelledException);
    canceller.join();
}
