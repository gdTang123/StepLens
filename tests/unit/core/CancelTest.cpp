#include "core/CancelToken.h"
#include "core/LoadSession.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

TEST(CancelToken, ThrowIfCancelled) {
    steplens::CancelSource source;
    const steplens::CancelToken token = source.token();
    EXPECT_FALSE(token.isCancelled());
    token.throwIfCancelled();

    source.requestStop();
    EXPECT_TRUE(token.isCancelled());
    EXPECT_TRUE(source.token().isCancelled());
    EXPECT_THROW(token.throwIfCancelled(), steplens::CancelledException);
}

TEST(SoftCancel, UiReturnsBeforeBackgroundReleases) {
    steplens::SoftCancel cancel;
    std::atomic<int> phase{0};
    std::thread worker([&]() {
        phase.store(1);
        while (!cancel.stopRequested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        phase.store(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.noteBackgroundReleased();
        phase.store(3);
    });

    while (phase.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto started = std::chrono::steady_clock::now();
    cancel.requestFromUi();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_TRUE(cancel.stopRequested());
    EXPECT_LT(phase.load(), 3);
    EXPECT_FALSE(cancel.backgroundReleased());

    worker.join();
    EXPECT_TRUE(cancel.backgroundReleased());
    EXPECT_EQ(phase.load(), 3);
}

TEST(LoadSession, GuiCanDropWhileBackgroundStillReleases) {
    auto session = std::make_shared<steplens::LoadSession>(1024);
    auto background = session;
    session->progress().setReadingBytes(512);
    session->cancel();
    EXPECT_TRUE(session->token().isCancelled());
    EXPECT_TRUE(session->progress().cancelFrozen());
    EXPECT_FALSE(session->backgroundReleased());
    session.reset();
    background->progress().setTransfer(1.f, 10.0);
    EXPECT_TRUE(background->progress().cancelFrozen());
    background->noteBackgroundReleased();
    EXPECT_TRUE(background->backgroundReleased());
    background.reset();
}
