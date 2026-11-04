#include "core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace steplens {
namespace {

struct LogState {
    // 保护 stream、currentBytes、directory、ready。任意线程可调用 write。
    std::mutex mutex;
    std::filesystem::path directory;
    std::ofstream stream;
    std::uint64_t maxBytes = Log::kDefaultMaxBytes;
    unsigned keepFiles = Log::kDefaultKeepFiles;
    std::uint64_t currentBytes = 0;
    bool ready = false;
#ifdef NDEBUG
    std::atomic<LogLevel> minLevel{LogLevel::Info};
#else
    std::atomic<LogLevel> minLevel{LogLevel::Debug};
#endif
};

LogState& state() {
    static LogState value;
    return value;
}

std::filesystem::path archivePath(const std::filesystem::path& directory, unsigned index) {
    if (index == 0) {
        return directory / "steplens.log";
    }
    return directory / ("steplens." + std::to_string(index) + ".log");
}

std::string timestampUtc() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds);
    const auto dayPoint = std::chrono::floor<std::chrono::days>(seconds);
    const std::chrono::year_month_day date{dayPoint};
    const std::chrono::hh_mm_ss time{seconds - dayPoint};
    const auto year = static_cast<int>(date.year());
    const auto month = static_cast<unsigned>(date.month());
    const auto day = static_cast<unsigned>(date.day());
    const auto hour = static_cast<unsigned>(time.hours().count());
    const auto minute = static_cast<unsigned>(time.minutes().count());
    const auto second = static_cast<unsigned>(time.seconds().count());
    const auto milli = static_cast<int>(millis.count());

    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02u.%03dZ", year,
                                      month, day, hour, minute, second, milli);
    if (written <= 0) {
        return "1970-01-01T00:00:00.000Z";
    }
    return std::string(buffer);
}

std::string timestampLocal() {
    try {
        const auto now = std::chrono::system_clock::now();
        const auto* zone = std::chrono::current_zone();
        const auto local = zone->to_local(now);
        const auto info = zone->get_info(now);
        const auto dayPoint = std::chrono::floor<std::chrono::days>(local);
        const std::chrono::year_month_day date{dayPoint};
        const std::chrono::hh_mm_ss time{local - dayPoint};
        const auto offsetMinutes = std::chrono::duration_cast<std::chrono::minutes>(info.offset).count();
        const auto sign = offsetMinutes < 0 ? '-' : '+';
        const auto absolute = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
        char buffer[40];
        const int written = std::snprintf(
            buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02u%c%02d:%02d", static_cast<int>(date.year()),
            static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
            static_cast<unsigned>(time.hours().count()), static_cast<unsigned>(time.minutes().count()),
            static_cast<unsigned>(time.seconds().count()), sign, static_cast<int>(absolute / 60),
            static_cast<int>(absolute % 60));
        if (written <= 0) {
            return timestampUtc();
        }
        return std::string(buffer);
    } catch (...) {
        return timestampUtc();
    }
}

std::string_view levelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO";
}

void openActive(LogState& log) {
    const auto path = archivePath(log.directory, 0);
    log.stream.open(path, std::ios::app | std::ios::binary);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    log.currentBytes = ec ? 0 : static_cast<std::uint64_t>(size);
    log.ready = static_cast<bool>(log.stream);
}

void rotate(LogState& log) {
    log.stream.close();
    std::error_code ec;
    if (log.keepFiles > 1) {
        std::filesystem::remove(archivePath(log.directory, log.keepFiles - 1), ec);
        for (unsigned index = log.keepFiles - 1; index > 0; --index) {
            const auto source = archivePath(log.directory, index - 1);
            if (std::filesystem::exists(source, ec)) {
                std::filesystem::rename(source, archivePath(log.directory, index), ec);
            }
        }
    }
    openActive(log);
    log.currentBytes = 0;
}

} // namespace

bool Log::initialize(const std::filesystem::path& directory, std::uint64_t maxFileBytes, unsigned keepFiles) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return false;
    }
    auto& log = state();
    std::lock_guard lock(log.mutex);
    if (log.stream.is_open()) {
        log.stream.close();
    }
    log.directory = directory;
    log.maxBytes = maxFileBytes == 0 ? kDefaultMaxBytes : maxFileBytes;
    log.keepFiles = keepFiles == 0 ? 1 : keepFiles;
    openActive(log);
    return log.ready;
}

void Log::shutdown() {
    auto& log = state();
    std::lock_guard lock(log.mutex);
    if (log.stream.is_open()) {
        log.stream.flush();
        log.stream.close();
    }
    log.ready = false;
}

void Log::setMinLevel(LogLevel level) {
    state().minLevel.store(level, std::memory_order_relaxed);
}

void Log::write(LogLevel level, std::string_view message) {
    auto& log = state();
    if (static_cast<std::uint8_t>(level) <
        static_cast<std::uint8_t>(log.minLevel.load(std::memory_order_relaxed))) {
        return;
    }
    std::lock_guard lock(log.mutex);
    if (!log.ready || !log.stream) {
        return;
    }
    const std::string line =
        timestampLocal() + " " + std::string(levelName(level)) + " " + std::string(message);
    const auto bytes = static_cast<std::uint64_t>(line.size() + 1);
    if (log.currentBytes > 0 && log.currentBytes + bytes > log.maxBytes) {
        rotate(log);
        if (!log.ready) {
            return;
        }
    }
    log.stream << line << '\n';
    if (level == LogLevel::Warn || level == LogLevel::Error) {
        log.stream.flush();
    }
    log.currentBytes += bytes;
}

std::filesystem::path Log::directory() {
    auto& log = state();
    std::lock_guard lock(log.mutex);
    return log.directory;
}

bool Log::ready() {
    auto& log = state();
    std::lock_guard lock(log.mutex);
    return log.ready;
}

} // namespace steplens
