#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#include <QApplication>

#include <cfenv>
#include <chrono>
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
    mxcsr &= ~kMxcsrStatus;
    _mm_setcsr(mxcsr);
    const unsigned int expectedControl = [&] {
        unsigned int value = 0;
        _controlfp_s(&value, 0, 0);
        return value;
    }();
    const unsigned int expectedMxcsr = _mm_getcsr();
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
    const unsigned legal = _MCW_EM | _MCW_RC | _MCW_DN;
    if ((afterControl & legal) != (expectedControl & legal)) {
        return 11;
    }
    if ((_mm_getcsr() & kMxcsrPromised) != (expectedMxcsr & kMxcsrPromised)) {
        return 15;
    }
    const auto translator = _set_se_translator(noop);
    _set_se_translator(translator);
    if (translator != sehSentinel) {
        return 16;
    }
#else
    static_cast<void>(fesetround(FE_DOWNWARD));
    if (!steplens::model::waitForDocumentRelease(std::chrono::seconds(10))) {
        return 8;
    }
    if (steplens::occt::OcctBoundary::armCountOnThisThread() < 1) {
        return 9;
    }
    if (fegetround() != FE_DOWNWARD) {
        return 11;
    }
#endif
    return 0;
}

TEST(DocumentRelease, GuiQApplicationFirstEntryRestoresCallerFpPolicy) {
    const char* childMark = std::getenv("STEPLENS_B2_GUI_CHILD");
    if (childMark != nullptr && *childMark != '\0') {
        std::exit(runGuiFirstEntry());
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
    ASSERT_TRUE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process));
    SetEnvironmentVariableA("STEPLENS_B2_GUI_CHILD", nullptr);
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = 9;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    EXPECT_EQ(waitResult, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u) << "QApplication first-entry failed, code=" << exitCode;
#else
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe");
    const std::string exe = exePath.string();
    setenv("STEPLENS_B2_GUI_CHILD", "1", 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(),
              "--gtest_filter=DocumentRelease.GuiQApplicationFirstEntryRestoresCallerFpPolicy", nullptr);
        _exit(127);
    }
    unsetenv("STEPLENS_B2_GUI_CHILD");
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}
