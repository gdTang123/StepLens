#include "core/MemoryMonitor.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/sysinfo.h>
#endif

namespace steplens {

MemoryMonitor::Sample MemoryMonitor::sample() const {
    Sample result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        result.processCommit = static_cast<std::uint64_t>(counters.PrivateUsage);
        result.processPeak = static_cast<std::uint64_t>(counters.PeakPagefileUsage);
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        result.systemAvailPhys = status.ullAvailPhys;
        result.systemTotalPhys = status.ullTotalPhys;
    }
#else
    // Linux: VmSize from /proc/self/status is the committed virtual address space; VmHWM is peak RSS.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t value = 0;
        if (!(fields >> key >> value))
            continue;
        if (key == "VmSize:")
            result.processCommit = value * 1024;
        if (key == "VmHWM:")
            result.processPeak = value * 1024;
    }
    // sysinfo reports physical RAM; mem_unit scales all fields.
    // clang-format off
    struct sysinfo info{};
    // clang-format on
    if (sysinfo(&info) == 0) {
        result.systemAvailPhys = static_cast<std::uint64_t>(info.freeram + info.bufferram) * info.mem_unit;
        result.systemTotalPhys = static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
    }
#endif
    return result;
}

std::uint64_t MemoryMonitor::estimatePeakBytes(std::uint64_t fileBytes, bool isStep) const {
    const double factor = isStep ? m_coefficients.step : m_coefficients.iges;
    const long double estimate = static_cast<long double>(fileBytes) * std::max(0.0, factor);
    return estimate >= static_cast<long double>(UINT64_MAX) ? UINT64_MAX
                                                            : static_cast<std::uint64_t>(estimate);
}

MemoryMonitor::Preflight MemoryMonitor::preflight(std::uint64_t estimatedPeak, std::uint64_t totalPhys,
                                                  double softLimitRatio, double hardLimitRatio) {
    if (totalPhys == 0 || hardLimitRatio <= 0.0 || softLimitRatio < 0.0)
        return Preflight::Open;
    // double，不是 long double：80/100 的 long double 比 double 字面量 0.80 略小，边界会判成 Confirm。
    const double ratio = static_cast<double>(estimatedPeak) / static_cast<double>(totalPhys);
    if (ratio >= hardLimitRatio)
        return Preflight::Block;
    if (ratio >= softLimitRatio)
        return Preflight::Confirm;
    return Preflight::Open;
}

} // namespace steplens
