#include "MemoryProbe.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
#include <psapi.h>
// clang-format on
#pragma comment(lib, "psapi.lib")
#else
#include <fstream>
#include <sys/resource.h>
#endif

namespace steplens::bench {
namespace {

constexpr std::chrono::milliseconds kSamplePeriod{20};
constexpr std::int64_t kBytesPerKb = 1024;

void raisePeak(std::atomic<std::int64_t>& slot, std::int64_t value) noexcept {
    if (value < 0) {
        return;
    }
    std::int64_t seen = slot.load(std::memory_order_relaxed);
    while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

std::int64_t kbToBytes(std::int64_t kilobytes) noexcept {
    if (kilobytes < 0) {
        return -1;
    }
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max() / kBytesPerKb;
    if (kilobytes > kMax) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return kilobytes * kBytesPerKb;
}

#if defined(_WIN32)
PROCESS_MEMORY_COUNTERS_EX counters() noexcept {
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),
                         sizeof(info));
    return info;
}

std::int64_t asBytes(SIZE_T value) noexcept {
    return static_cast<std::int64_t>(value);
}
#else
std::int64_t procKilobytes(const char* key) noexcept {
    std::ifstream file("/proc/self/status");
    const std::string prefix = std::string(key) + ":";
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const char* begin = line.c_str() + prefix.size();
        char* end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll(begin, &end, 10);
        if (end == begin || errno != 0) {
            return -1;
        }
        return static_cast<std::int64_t>(parsed);
    }
    return -1;
}

std::int64_t labeledKilobytes(std::string_view line) noexcept {
    const char* begin = line.data();
    const char* end = line.data() + line.size();
    while (begin < end && (*begin < '0' || *begin > '9')) {
        ++begin;
    }
    if (begin == end) {
        return -1;
    }
    char* parsedEnd = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(begin, &parsedEnd, 10);
    if (parsedEnd == begin || errno != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(parsed);
}

std::int64_t privateBytesNow() noexcept {
    std::ifstream file("/proc/self/smaps_rollup");
    if (!file) {
        return -1;
    }
    std::int64_t clean = -1;
    std::int64_t dirty = -1;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("Private_Clean:", 0) == 0) {
            clean = labeledKilobytes(line);
        } else if (line.rfind("Private_Dirty:", 0) == 0) {
            dirty = labeledKilobytes(line);
        }
    }
    const std::int64_t cleanBytes = kbToBytes(clean);
    const std::int64_t dirtyBytes = kbToBytes(dirty);
    if (cleanBytes < 0 || dirtyBytes < 0) {
        return -1;
    }
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    if (cleanBytes > kMax - dirtyBytes) {
        return kMax;
    }
    return cleanBytes + dirtyBytes;
}

std::int64_t maxRssBytes() noexcept {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return kbToBytes(static_cast<std::int64_t>(usage.ru_maxrss));
}
#endif

} // namespace

MemoryProbe::MemoryProbe() : m_sampler([this](std::stop_token stop) { loop(std::move(stop)); }) {
#if defined(_WIN32)
    m_platform = "windows";
    m_residentReset = true;
#else
    m_platform = "linux";
#endif
    resetRun();
}

MemoryProbe::~MemoryProbe() = default;

void MemoryProbe::loop(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        sampleOnce();
        std::this_thread::sleep_for(kSamplePeriod);
    }
}

void MemoryProbe::sampleOnce() noexcept {
#if defined(_WIN32)
    const PROCESS_MEMORY_COUNTERS_EX info = counters();
    raisePeak(m_peakResident, asBytes(info.WorkingSetSize));
    raisePeak(m_peakPrivate, asBytes(info.PrivateUsage));
#else
    raisePeak(m_peakResident, kbToBytes(procKilobytes("VmRSS")));
    const std::int64_t hwm = kbToBytes(procKilobytes("VmHWM"));
    raisePeak(m_peakResident, hwm);
    raisePeak(m_peakPrivate, privateBytesNow());
#endif
}

void MemoryProbe::resetRun() noexcept {
#if defined(_WIN32)
    const PROCESS_MEMORY_COUNTERS_EX info = counters();
    m_peakResident.store(asBytes(info.WorkingSetSize), std::memory_order_relaxed);
    m_peakPrivate.store(asBytes(info.PrivateUsage), std::memory_order_relaxed);
    m_residentReset = true;
#else
    std::ofstream clear("/proc/self/clear_refs");
    m_residentReset = static_cast<bool>(clear);
    if (m_residentReset) {
        clear << "5";
        m_residentReset = static_cast<bool>(clear);
    }
    const std::int64_t rss = kbToBytes(procKilobytes("VmRSS"));
    const std::int64_t priv = privateBytesNow();
    m_peakResident.store(rss < 0 ? 0 : rss, std::memory_order_relaxed);
    m_peakPrivate.store(priv < 0 ? 0 : priv, std::memory_order_relaxed);
#endif
}

std::vector<MemoryMetric> MemoryProbe::runMetrics() {
    sampleOnce();
    std::vector<MemoryMetric> metrics;
#if defined(_WIN32)
    MemoryMetric working;
    working.name = "WorkingSetSize";
    working.role = "peak_working_set_sampled";
    working.source = "GetProcessMemoryInfo WorkingSetSize, sampled";
    working.correspondsTo = "PeakWorkingSetSize";
    working.aggregation = "median";
    working.bytes = m_peakResident.load(std::memory_order_relaxed);
    metrics.push_back(std::move(working));

    MemoryMetric privateBytes;
    privateBytes.name = "PrivateUsage";
    privateBytes.role = "commit_private";
    privateBytes.source = "GetProcessMemoryInfo PrivateUsage, sampled peak";
    privateBytes.correspondsTo = "PrivateUsage";
    privateBytes.aggregation = "median";
    privateBytes.bytes = m_peakPrivate.load(std::memory_order_relaxed);
    metrics.push_back(std::move(privateBytes));
#else
    const std::int64_t hwm = kbToBytes(procKilobytes("VmHWM"));
    const std::int64_t sampled = m_peakResident.load(std::memory_order_relaxed);
    MemoryMetric resident;
    resident.name = "VmHWM";
    resident.role = "peak_resident_set";
    resident.source = m_residentReset
                          ? "/proc/self/status VmHWM after clear_refs 5, max with sampled VmRSS"
                          : "/proc/self/status VmHWM (clear_refs unavailable), max with sampled VmRSS";
    resident.correspondsTo = "PeakWorkingSetSize";
    resident.aggregation = "median";
    if (hwm < 0 && sampled < 0) {
        resident.available = false;
        resident.bytes = 0;
    } else {
        resident.bytes = hwm > sampled ? hwm : sampled;
    }
    metrics.push_back(std::move(resident));

    MemoryMetric privateBytes;
    privateBytes.name = "private_bytes";
    privateBytes.role = "commit_private";
    privateBytes.source = "smaps_rollup Private_Clean+Private_Dirty";
    privateBytes.correspondsTo = "PrivateUsage";
    privateBytes.aggregation = "median";
    const std::int64_t sampledPrivate = m_peakPrivate.load(std::memory_order_relaxed);
    const std::int64_t now = privateBytesNow();
    const std::int64_t peak = now > sampledPrivate ? now : sampledPrivate;
    if (peak < 0) {
        privateBytes.available = false;
        privateBytes.bytes = 0;
    } else {
        privateBytes.bytes = peak;
    }
    metrics.push_back(std::move(privateBytes));
#endif
    return metrics;
}

std::vector<MemoryMetric> MemoryProbe::heldMetrics() const {
    std::vector<MemoryMetric> metrics;
#if defined(_WIN32)
    const PROCESS_MEMORY_COUNTERS_EX info = counters();
    MemoryMetric working;
    working.name = "WorkingSetSizeHeld";
    working.role = "held_working_set";
    working.source = "GetProcessMemoryInfo WorkingSetSize after stages, model still resident";
    working.correspondsTo = "WorkingSetSize";
    working.aggregation = "median";
    working.bytes = asBytes(info.WorkingSetSize);
    metrics.push_back(std::move(working));

    MemoryMetric privateBytes;
    privateBytes.name = "PrivateUsageHeld";
    privateBytes.role = "held_commit_private";
    privateBytes.source = "GetProcessMemoryInfo PrivateUsage after stages, model still resident";
    privateBytes.correspondsTo = "PrivateUsage";
    privateBytes.aggregation = "median";
    privateBytes.bytes = asBytes(info.PrivateUsage);
    metrics.push_back(std::move(privateBytes));
#else
    MemoryMetric resident;
    resident.name = "VmRSSHeld";
    resident.role = "held_resident_set";
    resident.source = "/proc/self/status VmRSS after stages, model still resident";
    resident.correspondsTo = "WorkingSetSize";
    resident.aggregation = "median";
    const std::int64_t rss = kbToBytes(procKilobytes("VmRSS"));
    if (rss < 0) {
        resident.available = false;
        resident.bytes = 0;
    } else {
        resident.bytes = rss;
    }
    metrics.push_back(std::move(resident));

    MemoryMetric privateBytes;
    privateBytes.name = "private_bytes_held";
    privateBytes.role = "held_commit_private";
    privateBytes.source = "smaps_rollup Private_Clean+Private_Dirty after stages, model still resident";
    privateBytes.correspondsTo = "PrivateUsage";
    privateBytes.aggregation = "median";
    const std::int64_t now = privateBytesNow();
    if (now < 0) {
        privateBytes.available = false;
        privateBytes.bytes = 0;
    } else {
        privateBytes.bytes = now;
    }
    metrics.push_back(std::move(privateBytes));
#endif
    return metrics;
}

std::vector<MemoryMetric> MemoryProbe::lifetimeMetrics() const {
    std::vector<MemoryMetric> metrics;
#if defined(_WIN32)
    const PROCESS_MEMORY_COUNTERS_EX info = counters();
    MemoryMetric working;
    working.name = "PeakWorkingSetSize";
    working.role = "peak_working_set";
    working.source = "PROCESS_MEMORY_COUNTERS_EX::PeakWorkingSetSize";
    working.correspondsTo = "PeakWorkingSetSize";
    working.aggregation = "process_lifetime";
    working.bytes = asBytes(info.PeakWorkingSetSize);
    metrics.push_back(std::move(working));

    MemoryMetric commit;
    commit.name = "PeakPagefileUsage";
    commit.role = "peak_commit";
    commit.source = "PROCESS_MEMORY_COUNTERS_EX::PeakPagefileUsage";
    commit.correspondsTo = "PeakPagefileUsage";
    commit.aggregation = "process_lifetime";
    commit.bytes = asBytes(info.PeakPagefileUsage);
    metrics.push_back(std::move(commit));
#else
    MemoryMetric resident;
    resident.name = "ru_maxrss";
    resident.role = "process_lifetime_peak_resident_set";
    resident.source = "getrusage RUSAGE_SELF ru_maxrss";
    resident.correspondsTo = "PeakWorkingSetSize";
    resident.aggregation = "process_lifetime";
    const std::int64_t bytes = maxRssBytes();
    if (bytes < 0) {
        resident.available = false;
    } else {
        resident.bytes = bytes;
    }
    metrics.push_back(std::move(resident));
#endif
    return metrics;
}

} // namespace steplens::bench
