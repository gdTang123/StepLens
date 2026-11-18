#include "core/LifecycleTrace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#else
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
#else
    int file = -1;
#endif
};

TraceState& state() {
    static auto* value = new TraceState;
    return *value;
}
} // namespace

void initializeLifecycleTrace() {
    const char* path = std::getenv("STEPLENS_LIFECYCLE_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    auto& trace = state();
    std::lock_guard lock(trace.mutex);
#if defined(_WIN32)
    trace.file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    trace.file = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
#endif
}

void lifecycleTrace(std::string_view event, std::string_view identity, int exitCode) noexcept {
    auto& trace = state();
    std::lock_guard lock(trace.mutex);
#if defined(_WIN32)
    if (trace.file == INVALID_HANDLE_VALUE)
        return;
#else
    if (trace.file < 0)
        return;
#endif
    char line[1024];
    const auto sequence = trace.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const int length =
        std::snprintf(line, sizeof(line), "%llu tid=%llu event=%.*s identity=%.*s exit=%d\n", sequence,
                      static_cast<unsigned long long>(thread), static_cast<int>(event.size()), event.data(),
                      static_cast<int>(identity.size()), identity.data(), exitCode);
    if (length <= 0)
        return;
    const auto bytes =
        static_cast<unsigned long>(length < static_cast<int>(sizeof(line)) ? length : sizeof(line) - 1);
#if defined(_WIN32)
    DWORD written = 0;
    WriteFile(trace.file, line, bytes, &written, nullptr);
#else
    static_cast<void>(::write(trace.file, line, bytes));
#endif
}

} // namespace steplens
