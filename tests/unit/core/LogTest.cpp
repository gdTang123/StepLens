#include "TempDir.h"
#include "core/Log.h"

#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int countLogs(const std::filesystem::path& directory) {
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("steplens", 0) == 0 && entry.path().extension() == ".log") {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST(Log, WritesAndRotatesToFiveFiles) {
    steplens::test::TempDir dir;
    constexpr std::uint64_t kMaxBytes = 80;
    ASSERT_TRUE(steplens::Log::initialize(dir.path(), kMaxBytes, steplens::Log::kDefaultKeepFiles));

    constexpr int kLines = 30;
    for (int line = 0; line < kLines; ++line) {
        steplens::Log::info("line " + std::to_string(line) + " pad-pad-pad");
    }

    steplens::Log::shutdown();
    EXPECT_LE(countLogs(dir.path()), static_cast<int>(steplens::Log::kDefaultKeepFiles));
    EXPECT_GE(countLogs(dir.path()), 2);
    const auto active = readFile(dir.path() / "steplens.log");
    EXPECT_NE(active.find("line " + std::to_string(kLines - 1)), std::string::npos);
}

TEST(Log, ConcurrentWritesStayIntact) {
    steplens::test::TempDir dir;
    ASSERT_TRUE(steplens::Log::initialize(dir.path(), steplens::Log::kDefaultMaxBytes, 5));
    constexpr int kThreads = 4;
    constexpr int kEach = 40;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int threadIndex = 0; threadIndex < kThreads; ++threadIndex) {
        threads.emplace_back([threadIndex]() {
            for (int line = 0; line < kEach; ++line) {
                steplens::Log::info("t" + std::to_string(threadIndex) + "-" + std::to_string(line));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    steplens::Log::shutdown();
    const auto text = readFile(dir.path() / "steplens.log");
    int found = 0;
    for (int threadIndex = 0; threadIndex < kThreads; ++threadIndex) {
        for (int line = 0; line < kEach; ++line) {
            if (text.find("t" + std::to_string(threadIndex) + "-" + std::to_string(line)) !=
                std::string::npos) {
                ++found;
            }
        }
    }
    EXPECT_EQ(found, kThreads * kEach);
}
