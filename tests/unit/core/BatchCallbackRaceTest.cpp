#include "core/TaskScheduler.h"

#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

// 只是回调加锁模式示例，不守护 Tessellator 生产锁（生产锁由 windows-build 的 StepHeapRepro/ManySmallPrototypes 守护）。

constexpr int kWorkers = 4;
constexpr int kPrototypes = 200;

} // namespace

TEST(BatchCallback, LockedCallbackPatternExample) {
    steplens::TaskScheduler scheduler(static_cast<unsigned>(kWorkers));
    ASSERT_GE(scheduler.threadCount(), 2u);

    std::mutex gate;
    std::vector<int> delivered;
    std::set<std::thread::id> workers;
    delivered.reserve(static_cast<std::size_t>(kPrototypes));
    std::atomic<int> arrived{0};

    std::vector<std::future<void>> jobs;
    jobs.reserve(static_cast<std::size_t>(kWorkers));
    for (int worker = 0; worker < kWorkers; ++worker) {
        jobs.push_back(scheduler.submit([&, worker] {
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (arrived.load(std::memory_order_acquire) < kWorkers) {
                std::this_thread::yield();
            }
            for (int index = worker; index < kPrototypes; index += kWorkers) {
                std::lock_guard lock(gate);
                delivered.push_back(index);
                workers.insert(std::this_thread::get_id());
            }
        }));
    }
    for (std::future<void>& job : jobs) {
        job.get();
    }

    ASSERT_EQ(delivered.size(), static_cast<std::size_t>(kPrototypes));
    std::set<int> unique(delivered.begin(), delivered.end());
    EXPECT_EQ(unique.size(), static_cast<std::size_t>(kPrototypes));
    EXPECT_GE(workers.size(), 2u);
    std::cout << "tsan_callback prototypes=" << kPrototypes << " workers=" << workers.size() << "\n";
}
