#pragma once

// 基准进程的峰值内存。Windows 用 PROCESS_MEMORY_COUNTERS_EX。
// Linux 用 /proc/self/status 的 VmHWM（对应 PeakWorkingSetSize）和
// smaps_rollup 的 Private_Clean + Private_Dirty（对应 PrivateUsage / 提交内存）。
// 名字写进 JSON，不把 Linux 的计数伪装成 Windows API 的字段名。

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace steplens::bench {

struct MemoryMetric {
    std::string name;
    std::string role;
    std::string source;
    std::string correspondsTo;
    std::string aggregation;
    std::int64_t bytes = 0;
    bool available = true;
};

class MemoryProbe {
public:
    MemoryProbe();
    ~MemoryProbe();

    MemoryProbe(const MemoryProbe&) = delete;
    MemoryProbe& operator=(const MemoryProbe&) = delete;
    MemoryProbe(MemoryProbe&&) = delete;
    MemoryProbe& operator=(MemoryProbe&&) = delete;

    // 下一次 snapshot 只统计这次重置之后的峰值。进程级计数器清不掉。
    void resetRun() noexcept;
    [[nodiscard]] std::vector<MemoryMetric> runMetrics();
    [[nodiscard]] std::vector<MemoryMetric> lifetimeMetrics() const;
    [[nodiscard]] const std::string& platform() const noexcept {
        return m_platform;
    }
    [[nodiscard]] bool residentResetSupported() const noexcept {
        return m_residentReset;
    }

private:
    void sampleOnce() noexcept;
    void loop(std::stop_token stop) noexcept;

    std::string m_platform;
    bool m_residentReset = false;
    std::atomic<std::int64_t> m_peakResident{0};
    std::atomic<std::int64_t> m_peakPrivate{0};
    std::jthread m_sampler;
};

} // namespace steplens::bench
