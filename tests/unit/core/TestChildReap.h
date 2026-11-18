#pragma once

// 仅供新增父包装测试：成功读真实退出码；超时/WAIT_FAILED 保留失败并
// kill + 有限回收后再关句柄。不用阻塞 waitpid(...,0)。ECHILD 未知状态非 0。
// 不是生产进程服务。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

struct TestChildReap {
    int exitCode = 125;
    bool reaped = false;
    bool timedOut = false;
    bool waitFailed = false;
    bool killed = false;
    int killStatus = -1;
    long reapWaitCode = -1;
    bool lostChild = false;
    bool ownedAtEntry = false;
};

inline void logTestChildReap(const char* tag, long pid, const TestChildReap& result) {
    std::fprintf(stderr,
                 "%s pid=%ld exit=%d reaped=%d timedOut=%d waitFailed=%d killed=%d killStatus=%d "
                 "reapWait=%ld lostChild=%d ownedAtEntry=%d\n",
                 tag, pid, result.exitCode, static_cast<int>(result.reaped),
                 static_cast<int>(result.timedOut), static_cast<int>(result.waitFailed),
                 static_cast<int>(result.killed), result.killStatus, result.reapWaitCode,
                 static_cast<int>(result.lostChild), static_cast<int>(result.ownedAtEntry));
    std::fflush(stderr);
}

inline std::function<bool(const char*, long, const TestChildReap&)> testUnconfirmedCleanupHook;
inline std::function<void(const char*, long, const TestChildReap&)> testCloseHook;

inline void notifyClose(const char* tag, long pid, const TestChildReap& result) noexcept {
    if (!testCloseHook) {
        return;
    }
    try {
        testCloseHook(tag, pid, result);
    } catch (...) {
    }
}

inline void reportUnconfirmedCleanup(const char* tag, long pid, const TestChildReap& result) noexcept {
    logTestChildReap(tag, pid, result);
    bool handled = false;
    if (testUnconfirmedCleanupHook) {
        try {
            handled = testUnconfirmedCleanupHook(tag, pid, result);
        } catch (...) {
            handled = true;
        }
    }
    if (!handled) {
        try {
            ADD_FAILURE() << tag << " pid=" << pid << " unconfirmed reap exit=" << result.exitCode
                          << " killed=" << result.killed << " killStatus=" << result.killStatus
                          << " reapWait=" << result.reapWaitCode;
        } catch (...) {
        }
    }
}

inline void testChildHangForever() {
#if defined(_WIN32)
    Sleep(INFINITE);
#else
    for (;;) {
        pause();
    }
#endif
}

class TestEnvRestore {
public:
    TestEnvRestore() = default;
    TestEnvRestore(const char* name, const char* value) {
        assign(name, value);
    }
    TestEnvRestore(const TestEnvRestore&) = delete;
    TestEnvRestore& operator=(const TestEnvRestore&) = delete;
    ~TestEnvRestore() {
        restore();
    }

    void assign(const char* name, const char* value) {
        restore();
        name_ = name;
#if defined(_WIN32)
        char buf[32767];
        const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0 && n < sizeof(buf)) {
            had_ = true;
            previous_ = buf;
        } else {
            had_ = false;
            previous_.clear();
        }
        SetEnvironmentVariableA(name, value);
#else
        if (const char* prev = std::getenv(name)) {
            had_ = true;
            previous_ = prev;
        } else {
            had_ = false;
            previous_.clear();
        }
        setenv(name, value, 1);
#endif
        active_ = true;
    }

    void restore() noexcept {
        if (!active_) {
            return;
        }
        active_ = false;
#if defined(_WIN32)
        if (had_) {
            SetEnvironmentVariableA(name_.c_str(), previous_.c_str());
        } else {
            SetEnvironmentVariableA(name_.c_str(), nullptr);
        }
#else
        if (had_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    std::string previous_;
    bool had_ = false;
    bool active_ = false;
};

#if defined(_WIN32)
inline std::function<BOOL(HANDLE, UINT)> testTerminateProcessHook;
inline std::function<DWORD(HANDLE, DWORD)> testWaitForSingleObjectHook;

inline BOOL testTerminateProcess(HANDLE process, UINT code) {
    if (testTerminateProcessHook) {
        return testTerminateProcessHook(process, code);
    }
    return TerminateProcess(process, code);
}

inline DWORD testWaitForSingleObject(HANDLE process, DWORD ms) {
    if (testWaitForSingleObjectHook) {
        return testWaitForSingleObjectHook(process, ms);
    }
    return WaitForSingleObject(process, ms);
}

inline TestChildReap waitOrKillWindowsProcess(HANDLE process, std::chrono::milliseconds runBudget,
                                              std::chrono::milliseconds reapBudget) {
    TestChildReap result;
    const DWORD runMs = runBudget.count() < 0 ? 0 : static_cast<DWORD>(runBudget.count());
    const DWORD waited = testWaitForSingleObject(process, runMs);
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (GetExitCodeProcess(process, &code) == 0 || code == STILL_ACTIVE) {
            result.waitFailed = true;
            result.exitCode = 125;
            return result;
        }
        result.reaped = true;
        result.exitCode = static_cast<int>(code);
        return result;
    }
    if (waited == WAIT_TIMEOUT) {
        result.timedOut = true;
    } else {
        result.waitFailed = true;
    }
    result.killed = true;
    result.killStatus = testTerminateProcess(process, 1) ? 1 : 0;
    const DWORD reapMs = reapBudget.count() < 0 ? 0 : static_cast<DWORD>(reapBudget.count());
    const DWORD reap = testWaitForSingleObject(process, reapMs);
    result.reapWaitCode = static_cast<long>(reap);
    if (reap == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        result.reaped = true;
        result.exitCode = 125;
        return result;
    }
    result.reaped = false;
    result.exitCode = 125;
    return result;
}

struct WindowsProcessGuard {
    PROCESS_INFORMATION info{};
    bool valid = false;
    TestChildReap lastCleanup{};

    WindowsProcessGuard() = default;
    WindowsProcessGuard(const WindowsProcessGuard&) = delete;
    WindowsProcessGuard& operator=(const WindowsProcessGuard&) = delete;

    ~WindowsProcessGuard() noexcept {
        static_cast<void>(close("guard-dtor"));
    }

    void attach(const PROCESS_INFORMATION& process) {
        info = process;
        valid = true;
    }

    long pidValue() const {
        if (info.dwProcessId != 0) {
            return static_cast<long>(info.dwProcessId);
        }
        if (info.hProcess == nullptr || info.hProcess == INVALID_HANDLE_VALUE) {
            return 0;
        }
        return static_cast<long>(GetProcessId(info.hProcess));
    }

    TestChildReap close(const char* tag = "guard-close") noexcept {
        TestChildReap result;
        result.ownedAtEntry = valid;
        const long pid = pidValue();
        if (!valid) {
            lastCleanup = result;
            return result;
        }
        if (info.hProcess != nullptr && info.hProcess != INVALID_HANDLE_VALUE) {
            const DWORD firstWait = testWaitForSingleObject(info.hProcess, 0);
            if (firstWait == WAIT_OBJECT_0) {
                DWORD code = 1;
                if (GetExitCodeProcess(info.hProcess, &code) != 0 && code != STILL_ACTIVE) {
                    result.reaped = true;
                    result.exitCode = static_cast<int>(code);
                } else {
                    result.waitFailed = true;
                    result.exitCode = 125;
                }
            } else {
                result.killed = true;
                result.killStatus = testTerminateProcess(info.hProcess, 1) ? 1 : 0;
                const DWORD reap = testWaitForSingleObject(info.hProcess, 2000);
                result.reapWaitCode = static_cast<long>(reap);
                if (reap == WAIT_OBJECT_0) {
                    result.reaped = true;
                    result.exitCode = 125;
                } else {
                    result.reaped = false;
                    result.exitCode = 125;
                }
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread != nullptr && info.hThread != INVALID_HANDLE_VALUE) {
            CloseHandle(info.hThread);
        }
        const bool hadProcess = true;
        static_cast<void>(hadProcess);
        valid = false;
        info = {};
        lastCleanup = result;
        logTestChildReap(tag, pid, result);
        notifyClose(tag, pid, result);
        if (result.ownedAtEntry && !result.reaped) {
            reportUnconfirmedCleanup(tag, pid, result);
        }
        return result;
    }
};
#else
inline std::function<pid_t(pid_t, int*, int)> testWaitpidHook;
inline std::function<int(pid_t, int)> testKillHook;
inline std::function<pid_t()> testForkHook;
inline int testForkFailRemaining = 0;

inline void armTestForkFailures(int count) {
    testForkFailRemaining = count;
}

inline pid_t testWaitpid(pid_t pid, int* status, int flags) {
    if (testWaitpidHook) {
        return testWaitpidHook(pid, status, flags);
    }
    return ::waitpid(pid, status, flags);
}

inline int testKill(pid_t pid, int sig) {
    if (testKillHook) {
        return testKillHook(pid, sig);
    }
    return ::kill(pid, sig);
}

inline pid_t testFork() {
    static const int envFails = [] {
        const char* value = std::getenv("STEPLENS_TEST_FAIL_FIRST_FORK");
        return (value != nullptr && value[0] != '\0' && value[0] != '0') ? 1 : 0;
    }();
    static int envLeft = envFails;
    if (testForkHook) {
        return testForkHook();
    }
    if (testForkFailRemaining > 0) {
        --testForkFailRemaining;
        errno = EAGAIN;
        return -1;
    }
    if (envLeft > 0) {
        --envLeft;
        errno = EAGAIN;
        return -1;
    }
    return ::fork();
}

inline TestChildReap waitOrKillPid(pid_t pid, std::chrono::milliseconds runBudget,
                                   std::chrono::milliseconds reapBudget) {
    TestChildReap result;
    if (pid <= 0) {
        result.waitFailed = true;
        result.lostChild = true;
        result.exitCode = 125;
        result.reaped = false;
        return result;
    }
    const auto runDeadline = std::chrono::steady_clock::now() + runBudget;
    const auto reapDeadline = runDeadline + reapBudget;
    int status = 0;
    auto poll = [&]() -> pid_t {
        const pid_t done = testWaitpid(pid, &status, WNOHANG);
        if (done < 0 && errno == EINTR) {
            return 0;
        }
        return done;
    };
    auto takeStatus = [&]() {
        result.reaped = true;
        if (WIFEXITED(status)) {
            result.exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exitCode = 128 + WTERMSIG(status);
        } else {
            result.exitCode = 125;
        }
    };
    for (;;) {
        const pid_t done = poll();
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.reaped = false;
            result.waitFailed = true;
            result.lostChild = true;
            result.exitCode = 125;
            return result;
        }
        if (done == pid) {
            takeStatus();
            return result;
        }
        if (std::chrono::steady_clock::now() >= runDeadline) {
            result.timedOut = true;
            break;
        }
        const auto left = runDeadline - std::chrono::steady_clock::now();
        const auto slice = left > std::chrono::milliseconds{20} ? std::chrono::milliseconds{20}
                                                                : std::chrono::milliseconds{1};
        std::this_thread::sleep_for(slice);
    }
    result.killed = true;
    result.killStatus = (testKill(pid, SIGKILL) == 0) ? 1 : 0;
    for (;;) {
        const pid_t done = poll();
        if (done < 0) {
            result.reaped = false;
            result.lostChild = true;
            result.exitCode = 125;
            if (errno != ECHILD) {
                result.waitFailed = true;
            }
            return result;
        }
        if (done == pid) {
            result.reaped = true;
            result.exitCode = 125;
            return result;
        }
        if (std::chrono::steady_clock::now() >= reapDeadline) {
            result.reaped = false;
            result.exitCode = 125;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

struct PosixProcessGuard {
    pid_t pid = -1;
    bool valid = false;
    TestChildReap lastCleanup{};

    PosixProcessGuard() = default;
    PosixProcessGuard(const PosixProcessGuard&) = delete;
    PosixProcessGuard& operator=(const PosixProcessGuard&) = delete;

    explicit PosixProcessGuard(pid_t child) : pid(child), valid(child > 0) {}

    ~PosixProcessGuard() noexcept {
        static_cast<void>(close("guard-dtor"));
    }

    bool ownsKill() const {
        return valid && pid > 0;
    }

    void releaseKillOwnership() {
        valid = false;
    }

    TestChildReap close(const char* tag = "guard-close") noexcept {
        TestChildReap result;
        result.ownedAtEntry = ownsKill();
        const pid_t watched = pid;
        if (!result.ownedAtEntry) {
            lastCleanup = result;
            return result;
        }
        int status = 0;
        const pid_t done = testWaitpid(pid, &status, WNOHANG);
        if (done == pid) {
            result.reaped = true;
            if (WIFEXITED(status)) {
                result.exitCode = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exitCode = 128 + WTERMSIG(status);
            } else {
                result.exitCode = 125;
            }
        } else if (done < 0 && errno != EINTR) {
            result.lostChild = true;
            result.waitFailed = true;
            result.exitCode = 125;
        } else {
            result.killed = true;
            result.killStatus = (testKill(pid, SIGKILL) == 0) ? 1 : 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
            for (;;) {
                const pid_t reaped = testWaitpid(pid, &status, WNOHANG);
                if (reaped == pid) {
                    result.reaped = true;
                    result.exitCode = 125;
                    break;
                }
                if (reaped < 0 && errno != EINTR) {
                    result.lostChild = true;
                    result.exitCode = 125;
                    if (errno != ECHILD) {
                        result.waitFailed = true;
                    }
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    result.reaped = false;
                    result.exitCode = 125;
                    result.reapWaitCode = 1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        valid = false;
        lastCleanup = result;
        logTestChildReap(tag, static_cast<long>(watched), result);
        notifyClose(tag, static_cast<long>(watched), result);
        if (!result.reaped && !result.lostChild) {
            reportUnconfirmedCleanup(tag, static_cast<long>(watched), result);
        }
        return result;
    }
};

inline bool shouldReleaseKillOwnership(const TestChildReap& result) {
    return result.reaped || result.lostChild;
}

inline TestChildReap waitOrKillPidOwned(PosixProcessGuard& guard, std::chrono::milliseconds runBudget,
                                        std::chrono::milliseconds reapBudget) {
    const auto result = waitOrKillPid(guard.pid, runBudget, reapBudget);
    if (shouldReleaseKillOwnership(result)) {
        guard.releaseKillOwnership();
    }
    return result;
}
#endif
