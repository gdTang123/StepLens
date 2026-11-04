#include "core/CrashDump.h"

#include "core/AppPaths.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
// DbgHelp.h 依赖 Windows.h 里的 DWORD/HANDLE。SortIncludes 会把 DbgHelp.h 排到前面。
#include <Windows.h>
#include <DbgHelp.h>
// clang-format on
#include <cstdlib>
#endif

namespace steplens {
namespace {

#if defined(_WIN32)

constexpr DWORD kSimulatedCrashCode = 0xE0000001;
constexpr MINIDUMP_TYPE kDumpType =
    static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
wchar_t g_dumpDirectory[1024] = {};

bool writeDumpFile(const wchar_t* path, EXCEPTION_POINTERS* info) {
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    MINIDUMP_EXCEPTION_INFORMATION* exceptionPtr = nullptr;
    if (info != nullptr) {
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = info;
        exceptionInfo.ClientPointers = FALSE;
        exceptionPtr = &exceptionInfo;
    }
    const BOOL wrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType,
                                         exceptionPtr, nullptr, nullptr);
    CloseHandle(file);
    if (wrote == FALSE) {
        DeleteFileW(path);
        return false;
    }
    return true;
}

int handleSimulated(EXCEPTION_POINTERS* info, const wchar_t* path, bool* wrote) {
    if (wrote != nullptr) {
        *wrote = writeDumpFile(path, info);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void raiseAndCapture(const wchar_t* path, bool* wrote) {
    __try {
        RaiseException(kSimulatedCrashCode, 0, 0, nullptr);
    } __except (handleSimulated(GetExceptionInformation(), path, wrote)) {
    }
}

void writeAutoDump(EXCEPTION_POINTERS* info) {
    if (g_dumpDirectory[0] == L'\0') {
        return;
    }
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t file[1200];
    swprintf_s(file, L"%s\\steplens-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_dumpDirectory, time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId());
    writeDumpFile(file, info);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* info) {
    writeAutoDump(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    writeAutoDump(nullptr);
    std::abort();
}

void onPureCall() {
    writeAutoDump(nullptr);
    std::abort();
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    writeAutoDump(nullptr);
    std::abort();
}

#endif

} // namespace

void CrashDump::install(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
#if defined(_WIN32)
    const std::wstring native = directory.wstring();
    if (native.size() < 1024) {
        wcsncpy_s(g_dumpDirectory, native.c_str(), _TRUNCATE);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        // 若以后调用 OSD::SetSignal，它也可能改未处理异常过滤器。install 要排在那之后。
        SetUnhandledExceptionFilter(onUnhandled);
        std::set_terminate(onTerminate);
        _set_purecall_handler(onPureCall);
        _set_invalid_parameter_handler(onInvalidParameter);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#else
    (void)directory;
#endif
}

void CrashDump::installDefault() {
    install(defaultWritableDirectory("dumps"));
}

Result<std::filesystem::path> CrashDump::captureSimulatedCrash(const std::filesystem::path& directory) {
#if !defined(_WIN32)
    (void)directory;
    return Error{ErrorCode::Internal, "minidump is windows-only", "崩溃转储只在 Windows 上生成"};
#else
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return Error{ErrorCode::Internal, ec.message(), "无法创建转储目录"};
    }
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t name[64];
    swprintf_s(name, L"steplens-%04u%02u%02u-%02u%02u%02u-%lu.dmp", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId());
    const auto path = directory / name;
    const std::wstring native = path.wstring();
    if (native.size() >= 1024) {
        return Error{ErrorCode::Internal, "path too long", "转储路径过长"};
    }
    wchar_t wide[1024];
    wcsncpy_s(wide, native.c_str(), _TRUNCATE);
    bool wrote = false;
    raiseAndCapture(wide, &wrote);
    if (!wrote) {
        return Error{ErrorCode::Internal, "MiniDumpWriteDump failed", "写入崩溃转储失败"};
    }
    return path;
#endif
}

void CrashDump::crashForDump() {
#if defined(_WIN32)
    RaiseException(kSimulatedCrashCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
    std::abort();
}

} // namespace steplens
