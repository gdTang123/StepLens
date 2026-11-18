#include "../core/TestChildReap.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#include <QApplication>

#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <eh.h>
#include <float.h>
#include <xmmintrin.h>
#endif
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
constexpr unsigned kMxcsrStatus = 0x003Fu;
constexpr unsigned kMxcsrDaz = 0x0040u;
constexpr unsigned kMxcsrMask = 0x1F80u;
constexpr unsigned kMxcsrRc = 0x6000u;
constexpr unsigned kMxcsrFtz = 0x8000u;
constexpr unsigned kMxcsrPromised = kMxcsrStatus | kMxcsrDaz | kMxcsrMask | kMxcsrRc | kMxcsrFtz;

void sehSentinel(unsigned, struct _EXCEPTION_POINTERS*) {}
#endif

int runGuiFirstEntry() {
    int argc = 1;
    char arg0[] = "steplens_gui_first_entry";
    char* argv[] = {arg0, nullptr};
    QApplication app(argc, argv);
    if (QCoreApplication::instance() == nullptr || !qApp) {
        return 20;
    }
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    unsigned int ignored = 0;
    _controlfp_s(&ignored, _RC_DOWN, _MCW_RC);
    unsigned int mxcsr = _mm_getcsr();
    mxcsr |= kMxcsrDaz | kMxcsrFtz;
    mxcsr = (mxcsr & ~kMxcsrRc) | 0x2000u;
    mxcsr |= kMxcsrMask;
    mxcsr = (mxcsr & ~kMxcsrStatus) | 0x20u;
    _mm_setcsr(mxcsr);
    const unsigned int expectedControl = [&] {
        unsigned int value = 0;
        _controlfp_s(&value, 0, 0);
        return value;
    }();
    const unsigned int expectedMxcsr = _mm_getcsr();
    std::fprintf(stderr, "gui-first-before control=0x%08x mxcsr=0x%08x\n", expectedControl, expectedMxcsr);
    std::fflush(stderr);
    auto noop = [](unsigned, struct _EXCEPTION_POINTERS*) {};
    const auto previous = _set_se_translator(sehSentinel);
    static_cast<void>(previous);
    if (!steplens::model::waitForDocumentRelease(std::chrono::seconds(10))) {
        return 8;
    }
    if (steplens::occt::OcctBoundary::armCountOnThisThread() < 1) {
        return 9;
    }
    unsigned int afterControl = 0;
    _controlfp_s(&afterControl, 0, 0);
    const unsigned afterMxcsr = _mm_getcsr();
    std::fprintf(stderr, "gui-first-after control=0x%08x mxcsr=0x%08x\n", afterControl, afterMxcsr);
    std::fflush(stderr);
    const unsigned legal = _MCW_EM | _MCW_RC | _MCW_DN;
    if ((afterControl & legal) != (expectedControl & legal)) {
        return 11;
    }
    if ((afterMxcsr & kMxcsrPromised) != (expectedMxcsr & kMxcsrPromised)) {
        return 15;
    }
    const auto translator = _set_se_translator(noop);
    _set_se_translator(translator);
    if (translator != sehSentinel) {
        return 16;
    }
#else
    static_cast<void>(fesetround(FE_DOWNWARD));
    std::fprintf(stderr, "gui-first-before round=%d\n", fegetround());
    std::fflush(stderr);
    if (!steplens::model::waitForDocumentRelease(std::chrono::seconds(10))) {
        return 8;
    }
    if (steplens::occt::OcctBoundary::armCountOnThisThread() < 1) {
        return 9;
    }
    const int afterRound = fegetround();
    std::fprintf(stderr, "gui-first-after round=%d\n", afterRound);
    std::fflush(stderr);
    if (afterRound != FE_DOWNWARD) {
        return 11;
    }
#endif
    return 0;
}

void expectGuiHangChildFailsClosed(const char* tag, const char* hangEnv,
                                   std::chrono::milliseconds runBudget) {
#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    TestEnvRestore mark("STEPLENS_B2_GUI_CHILD", "1");
    TestEnvRestore hang(hangEnv, "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    mark.restore();
    hang.restore();
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result =
        waitOrKillWindowsProcess(guard.info.hProcess, runBudget, std::chrono::milliseconds{2000});
    logTestChildReap(tag, static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.killed);
    EXPECT_EQ(result.killStatus, 1);
    EXPECT_TRUE(result.reaped) << "kill 后应在有限时间内回收";
    EXPECT_NE(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap((std::string(tag) + "-close").c_str(), static_cast<long>(process.dwProcessId), cleanup);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    TestEnvRestore mark("STEPLENS_B2_GUI_CHILD", "1");
    TestEnvRestore platform("QT_QPA_PLATFORM", "offscreen");
    TestEnvRestore hang(hangEnv, "1");
    const pid_t child = testFork();
    if (child != 0) {
        mark.restore();
        platform.restore();
        hang.restore();
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    PosixProcessGuard guard(child);
    const auto result = waitOrKillPidOwned(guard, runBudget, std::chrono::milliseconds{2000});
    logTestChildReap(tag, static_cast<long>(child), result);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.killed);
    EXPECT_EQ(result.killStatus, 1);
    EXPECT_TRUE(result.reaped) << "kill 后应在有限时间内回收";
    EXPECT_NE(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap((std::string(tag) + "-close").c_str(), static_cast<long>(child), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry) << "已消费后不得再持有 kill 权";
    EXPECT_FALSE(cleanup.killed) << "close 不得再杀已注销身份";
#endif
}

TEST(DocumentRelease, GuiQApplicationFirstEntryRestoresCallerFpPolicy) {
    const TestEnvValue childMark = readTestEnvironment("STEPLENS_B2_GUI_CHILD");
    if (childMark.present && !childMark.value.empty()) {
        if (readTestEnvironment("STEPLENS_B2_HANG_BEFORE_ENTRY").present) {
            testChildHangForever();
        }
        const int code = runGuiFirstEntry();
        if (readTestEnvironment("STEPLENS_B2_HANG_AFTER_ENTRY").present) {
            testChildHangForever();
        }
        std::exit(code);
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    TestEnvRestore mark("STEPLENS_B2_GUI_CHILD", "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    mark.restore();
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result = waitOrKillWindowsProcess(guard.info.hProcess, std::chrono::milliseconds{20000},
                                                 std::chrono::milliseconds{2000});
    logTestChildReap("gui-first", static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0) << "QApplication first-entry failed, code=" << result.exitCode;
    const auto cleanup = guard.close();
    logTestChildReap("gui-first-close", static_cast<long>(process.dwProcessId), cleanup);
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    TestEnvRestore mark("STEPLENS_B2_GUI_CHILD", "1");
    TestEnvRestore platform("QT_QPA_PLATFORM", "offscreen");
    const pid_t child = testFork();
    if (child != 0) {
        mark.restore();
        platform.restore();
    }
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    PosixProcessGuard guard(child);
    const auto result =
        waitOrKillPidOwned(guard, std::chrono::milliseconds{20000}, std::chrono::milliseconds{2000});
    logTestChildReap("gui-first", static_cast<long>(child), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    const auto cleanup = guard.close();
    logTestChildReap("gui-first-close", static_cast<long>(child), cleanup);
    EXPECT_FALSE(cleanup.ownedAtEntry) << "已消费后不得再持有 kill 权";
    EXPECT_FALSE(cleanup.killed) << "close 不得再杀已注销身份";
#endif
}

TEST(DocumentRelease, HangBeforeGuiFirstEntryFailsClosed) {
    expectGuiHangChildFailsClosed("hang-before-gui", "STEPLENS_B2_HANG_BEFORE_ENTRY",
                                  std::chrono::milliseconds{2000});
}

TEST(DocumentRelease, HangAfterGuiFirstEntryCleanupFailsClosed) {
    expectGuiHangChildFailsClosed("hang-after-gui", "STEPLENS_B2_HANG_AFTER_ENTRY",
                                  std::chrono::milliseconds{15000});
}
