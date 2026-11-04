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
#include <ShlObj.h>
#include <string>
#else
#include <unistd.h>
#include <vector>
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
    // 不用 getenv：MSVC /WX 会把 C4996 当成错误，窄字符路径也会碰到中文用户名。
    PWSTR wide = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide)) && wide != nullptr) {
        const std::filesystem::path path{wide};
        CoTaskMemFree(wide);
        return path;
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
    DWORD capacity = MAX_PATH;
    std::wstring buffer(capacity, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (length == 0) {
            return {};
        }
        if (length < capacity) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        capacity *= 2;
        buffer.assign(capacity, L'\0');
    }
#else
    std::vector<char> buffer(256);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size() - 1) {
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
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
