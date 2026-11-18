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
    SetEnvironmentVariableA("STEPLENS_B2_GUI_CHILD", "1");
    SetEnvironmentVariableA(hangEnv, "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    SetEnvironmentVariableA("STEPLENS_B2_GUI_CHILD", nullptr);
    SetEnvironmentVariableA(hangEnv, nullptr);
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result =
        waitOrKillWindowsProcess(guard.info.hProcess, runBudget, std::chrono::milliseconds{2000});
    logTestChildReap(tag, static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.killed);
    EXPECT_TRUE(result.reaped) << "kill 后应在有限时间内回收";
    EXPECT_NE(result.exitCode, 0);
    guard.close();
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_B2_GUI_CHILD", "1", 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    setenv(hangEnv, "1", 1);
    const pid_t child = fork();
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    unsetenv("STEPLENS_B2_GUI_CHILD");
    unsetenv(hangEnv);
    PosixProcessGuard guard(child);
    const auto result = waitOrKillPid(child, runBudget, std::chrono::milliseconds{2000});
    logTestChildReap(tag, static_cast<long>(child), result);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.killed);
    EXPECT_TRUE(result.reaped) << "kill 后应在有限时间内回收";
    EXPECT_NE(result.exitCode, 0);
    guard.close();
#endif
}

TEST(DocumentRelease, GuiQApplicationFirstEntryRestoresCallerFpPolicy) {
    const char* childMark = std::getenv("STEPLENS_B2_GUI_CHILD");
    if (childMark != nullptr && *childMark != '\0') {
        if (std::getenv("STEPLENS_B2_HANG_BEFORE_ENTRY") != nullptr) {
            testChildHangForever();
        }
        const int code = runGuiFirstEntry();
        if (std::getenv("STEPLENS_B2_HANG_AFTER_ENTRY") != nullptr) {
            testChildHangForever();
        }
        std::exit(code);
    }

#if defined(_WIN32)
    char module[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameA(nullptr, module, MAX_PATH), 0u);
    SetEnvironmentVariableA("STEPLENS_B2_GUI_CHILD", "1");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + module +
                          "\" --gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy";
    WindowsProcessGuard guard;
    const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    SetEnvironmentVariableA("STEPLENS_B2_GUI_CHILD", nullptr);
    ASSERT_TRUE(created) << "启动失败不得记为退出码 0";
    guard.attach(process);
    const auto result = waitOrKillWindowsProcess(guard.info.hProcess, std::chrono::milliseconds{20000},
                                                 std::chrono::milliseconds{2000});
    logTestChildReap("gui-first", static_cast<long>(GetProcessId(guard.info.hProcess)), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0) << "QApplication first-entry failed, code=" << result.exitCode;
    guard.close();
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_B2_GUI_CHILD", "1", 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    const pid_t child = fork();
    ASSERT_GE(child, 0) << "启动失败不得记为退出码 0";
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    unsetenv("STEPLENS_B2_GUI_CHILD");
    PosixProcessGuard guard(child);
    const auto result =
        waitOrKillPid(child, std::chrono::milliseconds{20000}, std::chrono::milliseconds{2000});
    logTestChildReap("gui-first", static_cast<long>(child), result);
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.waitFailed);
    EXPECT_TRUE(result.reaped);
    EXPECT_EQ(result.exitCode, 0);
    guard.close();
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
