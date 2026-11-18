#pragma once

// 仅供新增父包装测试：成功读真实退出码；超时/WAIT_FAILED 保留失败并
// kill + 有限回收后再关句柄。不用阻塞 waitpid(...,0)。ECHILD 未知状态非 0。
// 不是生产进程服务。

#include <chrono>
#include <cstdio>
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
};

inline void logTestChildReap(const char* tag, long pid, const TestChildReap& result) {
    std::fprintf(stderr,
                 "%s pid=%ld exit=%d reaped=%d timedOut=%d waitFailed=%d killed=%d killStatus=%d "
                 "reapWait=%ld\n",
                 tag, pid, result.exitCode, static_cast<int>(result.reaped),
                 static_cast<int>(result.timedOut), static_cast<int>(result.waitFailed),
                 static_cast<int>(result.killed), result.killStatus, result.reapWaitCode);
    std::fflush(stderr);
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

#if defined(_WIN32)
inline TestChildReap waitOrKillWindowsProcess(HANDLE process, std::chrono::milliseconds runBudget,
                                              std::chrono::milliseconds reapBudget) {
    TestChildReap result;
    const DWORD runMs = runBudget.count() < 0 ? 0 : static_cast<DWORD>(runBudget.count());
    const DWORD waited = WaitForSingleObject(process, runMs);
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
    result.killStatus = TerminateProcess(process, 1) ? 1 : 0;
    const DWORD reapMs = reapBudget.count() < 0 ? 0 : static_cast<DWORD>(reapBudget.count());
    const DWORD reap = WaitForSingleObject(process, reapMs);
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

    WindowsProcessGuard() = default;
    WindowsProcessGuard(const WindowsProcessGuard&) = delete;
    WindowsProcessGuard& operator=(const WindowsProcessGuard&) = delete;

    ~WindowsProcessGuard() {
        close();
    }

    void attach(const PROCESS_INFORMATION& process) {
        info = process;
        valid = true;
    }

    void close() {
        if (!valid) {
            return;
        }
        if (info.hProcess != nullptr && info.hProcess != INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(info.hProcess, 0) != WAIT_OBJECT_0) {
                TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 2000);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread != nullptr && info.hThread != INVALID_HANDLE_VALUE) {
            CloseHandle(info.hThread);
        }
        valid = false;
        info = {};
    }
};
#else
inline TestChildReap waitOrKillPid(pid_t pid, std::chrono::milliseconds runBudget,
                                   std::chrono::milliseconds reapBudget) {
    TestChildReap result;
    if (pid <= 0) {
        result.waitFailed = true;
        result.exitCode = 125;
        result.reaped = false;
        return result;
    }
    const auto runDeadline = std::chrono::steady_clock::now() + runBudget;
    const auto reapDeadline = runDeadline + reapBudget;
    int status = 0;
    auto poll = [&]() -> pid_t {
        const pid_t done = waitpid(pid, &status, WNOHANG);
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
    result.killStatus = (kill(pid, SIGKILL) == 0) ? 1 : 0;
    for (;;) {
        const pid_t done = poll();
        if (done < 0) {
            result.reaped = false;
            result.exitCode = 125;
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

    PosixProcessGuard() = default;
    PosixProcessGuard(const PosixProcessGuard&) = delete;
    PosixProcessGuard& operator=(const PosixProcessGuard&) = delete;

    explicit PosixProcessGuard(pid_t child) : pid(child), valid(child > 0) {}

    ~PosixProcessGuard() {
        close();
    }

    void close() {
        if (!valid || pid <= 0) {
            return;
        }
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done != pid && !(done < 0 && errno == ECHILD)) {
            static_cast<void>(kill(pid, SIGKILL));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
            while (std::chrono::steady_clock::now() <= deadline) {
                const pid_t reaped = waitpid(pid, &status, WNOHANG);
                if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        valid = false;
        pid = -1;
    }
};
#endif
