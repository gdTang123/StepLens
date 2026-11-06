#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/TaskScheduler.h"
#include "occtsupport/OcctBoundary.h"

#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// clang-format off
#include <Windows.h>
// clang-format on

namespace {

// 每个工作线程在自己的第一个任务开头记下钩子计数，然后停住，避免一个线程把队列吃完。
// useFactory 为真时走生产工厂；为假时用没有钩子的 TaskScheduler，给负对照。
std::vector<int> firstTaskArmCounts(unsigned workers, bool useFactory) {
    std::atomic<int> entered{0};
    std::atomic<bool> release{false};
    std::mutex mutex;
    std::vector<int> counts;
    counts.reserve(workers);

    std::unique_ptr<steplens::TaskScheduler> scheduler =
        useFactory ? steplens::occt::makeOcctTaskScheduler(workers)
                   : std::make_unique<steplens::TaskScheduler>(workers);
    std::vector<std::future<void>> jobs;
    jobs.reserve(workers);
    for (unsigned index = 0; index < workers; ++index) {
        jobs.push_back(scheduler->submit([&] {
            int arms = steplens::occt::OcctBoundary::armCountOnThisThread();
            {
                std::lock_guard lock(mutex);
                counts.push_back(arms);
            }
            entered.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }));
    }
    while (entered.load(std::memory_order_acquire) < static_cast<int>(workers)) {
        std::this_thread::yield();
    }
    release.store(true, std::memory_order_release);
    for (auto& job : jobs) {
        job.get();
    }
    return counts;
}

} // namespace

TEST(OcctBoundary, NullAccessOnWorkerBecomesInternalAndMarksProcess) {
    // 接不住的访问违例不要弹出 WER 对话框，否则 CI 会停在那里。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ASSERT_FALSE(steplens::occt::processContaminated());

    steplens::Result<int> result{0};
    {
        // 生产工厂在线程入口注入信号转换，再经 OcctBoundary 触发空指针访问。
        auto scheduler = steplens::occt::makeOcctTaskScheduler(1);
        auto future =
            scheduler->submit([] { return steplens::occt::OcctBoundary::probeNullPointerAccess(); });
        result = future.get();
    }

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(result.error().userMessage, std::string{steplens::occt::kProcessContaminatedUserMessage});
    EXPECT_TRUE(steplens::occt::processContaminated());
}

TEST(OcctBoundary, EntryHookRunsBeforeFirstTaskOnEveryWorker) {
    constexpr unsigned kWorkers = 2;
    const std::vector<int> counts = firstTaskArmCounts(kWorkers, true);
    ASSERT_EQ(counts.size(), kWorkers);
    for (int arms : counts) {
        // 工厂在线程入口注入的钩子。不调用 probe，避免它自己再 arm 把计数补上。
        EXPECT_GE(arms, 1);
    }
}

TEST(OcctBoundary, MissingEntryHookIsVisibleOnFirstTask) {
    // 负对照：入口不注入 armWorkerThread。同一条“钩子已跑过”断言必须失败。
    constexpr unsigned kWorkers = 2;
    const std::vector<int> counts = firstTaskArmCounts(kWorkers, false);
    ASSERT_EQ(counts.size(), kWorkers);
    for (int arms : counts) {
        EXPECT_EQ(arms, 0);
    }
}
