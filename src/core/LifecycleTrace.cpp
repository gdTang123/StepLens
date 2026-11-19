#include "core/LifecycleTrace.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace steplens {
namespace {
struct TraceState {
    std::atomic<unsigned long long> sequence{0};
    std::mutex mutex;
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mirror = INVALID_HANDLE_VALUE;
#else
    int file = -1;
    int mirror = -1;
#endif
};

TraceState& state() {
    static auto* value = new TraceState;
    return *value;
}

#if defined(_WIN32)
bool handleOk(HANDLE h) noexcept {
    return h != INVALID_HANDLE_VALUE && h != nullptr;
}

HANDLE openAppend(const char* path) noexcept {
    return CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void writeHandle(HANDLE file, const char* data, unsigned long bytes) noexcept {
    if (!handleOk(file) || data == nullptr || bytes == 0) {
        return;
    }
    const char* cursor = data;
    unsigned long remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (WriteFile(file, cursor, remaining, &written, nullptr) == 0 || written == 0) {
            return;
        }
        cursor += written;
        remaining -= written;
    }
}

std::string readEnvPath(const char* name) {
    const DWORD pathSize = GetEnvironmentVariableA(name, nullptr, 0);
    if (pathSize == 0) {
        return {};
    }
    std::string pathBuffer(static_cast<size_t>(pathSize) + 1, '\0');
    const DWORD pathLength =
        GetEnvironmentVariableA(name, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (pathLength == 0 || pathLength >= pathBuffer.size()) {
        return {};
    }
    pathBuffer.resize(static_cast<size_t>(pathLength));
    return pathBuffer;
}
#else
bool handleOk(int fd) noexcept {
    return fd >= 0;
}

int openAppend(const char* path) noexcept {
    return ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
}

void writeHandle(int file, const char* data, unsigned long bytes) noexcept {
    if (!handleOk(file) || data == nullptr || bytes == 0) {
        return;
    }
    const char* cursor = data;
    size_t remaining = static_cast<size_t>(bytes);
    while (remaining > 0) {
        const ssize_t written = ::write(file, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (written == 0) {
            return;
        }
        cursor += static_cast<size_t>(written);
        remaining -= static_cast<size_t>(written);
    }
}

std::string readEnvPath(const char* name) {
    const char* path = std::getenv(name);
    if (path == nullptr || *path == '\0') {
        return {};
    }
    return path;
}
#endif
} // namespace

void initializeLifecycleTrace() {
    const std::string primaryPath = readEnvPath("STEPLENS_LIFECYCLE_LOG");
    const std::string mirrorPath = readEnvPath("STEPLENS_DIAG_LOG");
    auto& trace = state();
    bool opened = false;
    {
        std::lock_guard lock(trace.mutex);
        if (!primaryPath.empty()) {
            trace.file = openAppend(primaryPath.c_str());
            opened = opened || handleOk(trace.file);
        }
        if (!mirrorPath.empty()) {
            trace.mirror = openAppend(mirrorPath.c_str());
            opened = opened || handleOk(trace.mirror);
        }
    }
    if (!opened) {
        return;
    }
    // 身份是本二进制编译期 STEPLENS_GIT_SHA，禁止回显环境注入值。
#ifndef STEPLENS_GIT_SHA
#define STEPLENS_GIT_SHA "unknown"
#endif
    lifecycleTrace("lifecycle-open", STEPLENS_GIT_SHA);
}

void lifecycleTrace(std::string_view event, std::string_view identity, int exitCode) noexcept {
    auto& trace = state();
    std::lock_guard lock(trace.mutex);
    if (!handleOk(trace.file) && !handleOk(trace.mirror)) {
        return;
    }
    char line[1024];
    const auto sequence = trace.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const int length =
        std::snprintf(line, sizeof(line), "%llu tid=%llu event=%.*s identity=%.*s exit=%d\n", sequence,
                      static_cast<unsigned long long>(thread), static_cast<int>(event.size()), event.data(),
                      static_cast<int>(identity.size()), identity.data(), exitCode);
    if (length <= 0) {
        return;
    }
    const auto bytes =
        static_cast<unsigned long>(length < static_cast<int>(sizeof(line)) ? length : sizeof(line) - 1);
    writeHandle(trace.file, line, bytes);
    writeHandle(trace.mirror, line, bytes);
}

} // namespace steplens
