#include "core/AppPaths.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace steplens {
namespace {

bool canWrite(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return false;
    }
    const auto probe = directory / ".write_probe";
    {
        std::ofstream output(probe, std::ios::binary);
        if (!output) {
            return false;
        }
    }
    std::filesystem::remove(probe, ec);
    return true;
}

std::filesystem::path userDataRoot() {
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0') {
        return std::filesystem::path(local);
    }
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && profile[0] != '\0') {
        return std::filesystem::path(profile) / "AppData" / "Local";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "share";
    }
#endif
    return std::filesystem::temp_directory_path() / "StepLens";
}

} // namespace

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    return std::filesystem::path(buffer).parent_path();
#endif
}

std::filesystem::path defaultWritableDirectory(std::string_view folderName) {
    const auto exeDir = executableDirectory();
    if (!exeDir.empty()) {
        const auto beside = exeDir / folderName;
        if (canWrite(beside)) {
            return beside;
        }
    }
    const auto fallback = userDataRoot() / "StepLens" / folderName;
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    return fallback;
}

} // namespace steplens
