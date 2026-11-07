#include "core/LoadCoordinator.h"
#include "core/LoadSession.h"
#include "core/OcctLoadContract.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

// 死锁时后台线程不会返回。超时后摘掉线程并让进程以非 0 退出，避免 gtest 在 join 上一直挂着。
template <class F> void requireFinishes(std::chrono::milliseconds budget, const char* name, F&& body) {
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([finished, body = std::forward<F>(body)]() mutable {
        body();
        finished->store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!finished->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!finished->load(std::memory_order_acquire)) {
        worker.detach();
        std::fprintf(stderr, "DEADLOCK %s exceeded %lld ms\n", name, static_cast<long long>(budget.count()));
        std::_Exit(2);
    }
    worker.join();
}

struct ReplaceProbe {
    steplens::LoadCoordinator coordinator;
    std::atomic<bool> started{false};
    std::atomic<int> callbackSubmits{0};
    std::atomic<bool> ranSuperseded{false};
    std::atomic<bool> ranLatest{false};
};

} // namespace

TEST(LoadSession, NoteWakesWaiter) {
    auto session = std::make_shared<steplens::LoadSession>(8);
    std::atomic<bool> entered{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        entered.store(true);
        EXPECT_TRUE(session->waitForBackgroundReleased(std::chrono::seconds(2)));
    });
    while (!entered.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(session->backgroundReleased());
    session->noteBackgroundReleased();
    worker.join();
    EXPECT_TRUE(session->backgroundReleased());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}

TEST(LoadCoordinator, LastRequestWinsRunsOnlyLatestAfterPreviousRelease) {
    steplens::LoadCoordinator coordinator;
    std::mutex gate;
    std::condition_variable startedCv;
    bool aStarted = false;
    std::shared_ptr<steplens::LoadSession> sessionA;
    std::vector<char> ran;
    std::atomic<int> maxLive{0};
    std::atomic<bool> sample{true};

    std::thread sampler([&]() {
        while (sample.load()) {
            const int live = coordinator.liveBackgroundSessions();
            int previous = maxLive.load();
            while (live > previous && !maxLive.compare_exchange_weak(previous, live)) {
            }
            std::this_thread::yield();
        }
    });

    auto runner = [&](char id) {
        return [&, id](const std::shared_ptr<steplens::LoadSession>& session) {
            {
                std::lock_guard lock(gate);
                ran.push_back(id);
                if (id == 'A') {
                    sessionA = session;
                    aStarted = true;
                    startedCv.notify_all();
                }
            }
            if (id == 'A') {
                while (!session->token().isCancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                session->noteBackgroundReleased();
                return;
            }
            std::shared_ptr<steplens::LoadSession> previous;
            {
                std::lock_guard lock(gate);
                previous = sessionA;
            }
            EXPECT_TRUE(previous);
            EXPECT_TRUE(previous->backgroundReleased());
            session->noteBackgroundReleased();
        };
    };

    coordinator.submit(steplens::LoadCoordinator::LoadRequest{128, {}, runner('A')});
    bool started = false;
    {
        std::unique_lock lock(gate);
        started = startedCv.wait_for(lock, std::chrono::seconds(2), [&] { return aStarted; });
    }
    const int liveAfterStart = coordinator.liveBackgroundSessions();
    int previousMax = maxLive.load();
    while (liveAfterStart > previousMax && !maxLive.compare_exchange_weak(previousMax, liveAfterStart)) {
    }
    if (!started) {
        sample.store(false);
        sampler.join();
        FAIL() << "A did not start";
    }
    EXPECT_EQ(liveAfterStart, 1);

    coordinator.submit(steplens::LoadCoordinator::LoadRequest{64, {}, runner('B')});
    coordinator.submit(steplens::LoadCoordinator::LoadRequest{32, {}, runner('C')});
    coordinator.waitUntilIdle();

    sample.store(false);
    sampler.join();

    ASSERT_EQ(ran.size(), 2u);
    EXPECT_EQ(ran[0], 'A');
    EXPECT_EQ(ran[1], 'C');
    EXPECT_LE(maxLive.load(), 1);
    EXPECT_GE(maxLive.load(), 1);
}

TEST(LoadCoordinator, StopCallbackReenterSubmitDoesNotDeadlock) {
    constexpr auto kBudget = std::chrono::seconds(4);
    auto probe = std::make_shared<ReplaceProbe>();
    requireFinishes(kBudget, "reenter-submit", [probe] {
        probe->coordinator.submit(steplens::LoadCoordinator::LoadRequest{
            32, {}, [probe](const std::shared_ptr<steplens::LoadSession>& session) {
                std::stop_callback callback(session->token().stopToken(), [probe] {
                    probe->callbackSubmits.fetch_add(1);
                    probe->coordinator.submit(steplens::LoadCoordinator::LoadRequest{
                        8, {}, [probe](const std::shared_ptr<steplens::LoadSession>& next) {
                            probe->ranLatest.store(true);
                            next->noteBackgroundReleased();
                        }});
                });
                probe->started.store(true);
                while (!session->token().isCancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                session->noteBackgroundReleased();
            }});
        for (int i = 0; i < 400 && !probe->started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        probe->coordinator.submit(steplens::LoadCoordinator::LoadRequest{
            16, {}, [probe](const std::shared_ptr<steplens::LoadSession>& session) {
                probe->ranSuperseded.store(true);
                session->noteBackgroundReleased();
            }});
        probe->coordinator.waitUntilIdle();
    });
    EXPECT_EQ(probe->callbackSubmits.load(), 1);
    EXPECT_FALSE(probe->ranSuperseded.load());
    EXPECT_TRUE(probe->ranLatest.load());
}

TEST(LoadCoordinator, DestructorCancelReenterSubmitDoesNotDeadlock) {
    constexpr auto kBudget = std::chrono::seconds(4);
    auto probe = std::make_shared<ReplaceProbe>();
    requireFinishes(kBudget, "reenter-destructor", [probe] {
        auto coordinator = std::make_unique<steplens::LoadCoordinator>();
        coordinator->submit(steplens::LoadCoordinator::LoadRequest{
            32, {}, [probe, raw = coordinator.get()](const std::shared_ptr<steplens::LoadSession>& session) {
                std::stop_callback callback(session->token().stopToken(), [probe, raw] {
                    probe->callbackSubmits.fetch_add(1);
                    raw->submit(steplens::LoadCoordinator::LoadRequest{
                        8, {}, [](const std::shared_ptr<steplens::LoadSession>& next) {
                            next->noteBackgroundReleased();
                        }});
                });
                probe->started.store(true);
                while (!session->token().isCancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                session->noteBackgroundReleased();
            }});
        for (int i = 0; i < 400 && !probe->started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        coordinator.reset();
    });
    EXPECT_EQ(probe->callbackSubmits.load(), 1);
    EXPECT_TRUE(probe->started.load());
}

TEST(LoadCoordinator, RapidSupersedeKeepsOnlyLatest) {
    constexpr auto kBudget = std::chrono::seconds(4);
    auto probe = std::make_shared<ReplaceProbe>();
    requireFinishes(kBudget, "rapid-bc", [probe] {
        probe->coordinator.submit(steplens::LoadCoordinator::LoadRequest{
            128, {}, [probe](const std::shared_ptr<steplens::LoadSession>& session) {
                probe->started.store(true);
                while (!session->token().isCancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                session->noteBackgroundReleased();
            }});
        for (int i = 0; i < 400 && !probe->started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        probe->coordinator.submit(steplens::LoadCoordinator::LoadRequest{
            64, {}, [probe](const std::shared_ptr<steplens::LoadSession>& session) {
                probe->ranSuperseded.store(true);
                session->noteBackgroundReleased();
            }});
        probe->coordinator.submit(steplens::LoadCoordinator::LoadRequest{
            32, {}, [probe](const std::shared_ptr<steplens::LoadSession>& session) {
                probe->ranLatest.store(true);
                session->noteBackgroundReleased();
            }});
        probe->coordinator.waitUntilIdle();
    });
    EXPECT_TRUE(probe->started.load());
    EXPECT_FALSE(probe->ranSuperseded.load());
    EXPECT_TRUE(probe->ranLatest.load());
}

TEST(OcctLoadContract, InterfaceStaticInitializesOnce) {
    steplens::initializeInterfaceStaticOnMainThread();
    EXPECT_THROW(steplens::initializeInterfaceStaticOnMainThread(), std::logic_error);

    std::atomic<bool> threw{false};
    std::thread worker([&]() {
        try {
            steplens::initializeInterfaceStaticOnMainThread();
        } catch (const std::logic_error&) {
            threw.store(true);
        }
    });
    worker.join();
    EXPECT_TRUE(threw.load());
}

TEST(OcctLoadContract, XcafDocumentMutexIsProcessWide) {
    EXPECT_EQ(&steplens::xcafDocumentMutex(), &steplens::xcafDocumentMutex());
    steplens::xcafDocumentMutex().lock();
    bool blocked = false;
    std::thread worker([&]() {
        blocked = !steplens::xcafDocumentMutex().try_lock();
        if (!blocked) {
            steplens::xcafDocumentMutex().unlock();
        }
    });
    worker.join();
    steplens::xcafDocumentMutex().unlock();
    EXPECT_TRUE(blocked);
}
