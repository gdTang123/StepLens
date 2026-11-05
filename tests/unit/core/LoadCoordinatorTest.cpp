#include "core/LoadCoordinator.h"
#include "core/LoadSession.h"
#include "core/OcctLoadContract.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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
