#include "TempDir.h"
#include "core/CrashDump.h"

#include <fstream>
#include <gtest/gtest.h>
#include <string>

TEST(CrashDump, InstallCreatesDirectory) {
    steplens::test::TempDir dir;
    const auto dumpDir = dir.path() / "dumps";
    steplens::CrashDump::install(dumpDir);
    EXPECT_TRUE(std::filesystem::is_directory(dumpDir));
}

TEST(CrashDump, SimulatedCrashWritesMinidumpOnWindows) {
    steplens::test::TempDir dir;
    const auto result = steplens::CrashDump::captureSimulatedCrash(dir.path());
#if defined(_WIN32)
    ASSERT_TRUE(result.ok()) << result.error().message;
    std::ifstream input(result.value(), std::ios::binary);
    char magic[4] = {};
    input.read(magic, 4);
    EXPECT_EQ(std::string(magic, 4), "MDMP");
#else
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::Internal);
    EXPECT_NE(result.error().userMessage.find("Windows"), std::string::npos);
#endif
}
