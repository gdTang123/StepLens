#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace steplens {

enum class LogLevel : std::uint8_t { Debug, Info, Warn, Error };

// 技术日志。m_mutex（在 cpp 的状态对象里）保护文件句柄和轮转，任意线程都可以写。
// 滚动保留 keepFiles 个文件，含当前的 steplens.log。默认 5 个、每个 1 MiB。
class Log {
public:
    static constexpr std::uint64_t kDefaultMaxBytes = 1024ull * 1024ull;
    static constexpr unsigned kDefaultKeepFiles = 5;

    // 目录不可创建或文件打不开时返回 false，之后 write 什么也不做。
    static bool initialize(const std::filesystem::path& directory,
                           std::uint64_t maxFileBytes = kDefaultMaxBytes,
                           unsigned keepFiles = kDefaultKeepFiles);
    static void write(LogLevel level, std::string_view message);
    static void debug(std::string_view message) {
        write(LogLevel::Debug, message);
    }
    static void info(std::string_view message) {
        write(LogLevel::Info, message);
    }
    static void warn(std::string_view message) {
        write(LogLevel::Warn, message);
    }
    static void error(std::string_view message) {
        write(LogLevel::Error, message);
    }

    [[nodiscard]] static std::filesystem::path directory();
    [[nodiscard]] static bool ready();
};

} // namespace steplens
