#include "core/CrashDump.h"

#include "core/AppPaths.h"

#include <cstdio>
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

void appendText(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE || text == nullptr) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
}

void writeStackFile(const wchar_t* path, EXCEPTION_POINTERS* info) {
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[512];
#ifndef STEPLENS_GIT_SHA
#define STEPLENS_GIT_SHA "unknown"
#endif
    appendText(file, "git_sha=" STEPLENS_GIT_SHA "\r\n");
    wchar_t exe[MAX_PATH];
    const DWORD exeLen = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        char narrow[MAX_PATH * 2];
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, exe, -1, narrow, sizeof(narrow), nullptr, nullptr);
        if (bytes > 0) {
            appendText(file, "exe=");
            appendText(file, narrow);
            appendText(file, "\r\n");
        }
        wchar_t pdb[MAX_PATH];
        wcsncpy_s(pdb, exe, _TRUNCATE);
        wchar_t* dot = wcsrchr(pdb, L'.');
        if (dot != nullptr) {
            wcscpy_s(dot, static_cast<size_t>(pdb + MAX_PATH - dot), L".pdb");
            const DWORD attrs = GetFileAttributesW(pdb);
            WIN32_FILE_ATTRIBUTE_DATA data{};
            const BOOL sized = GetFileAttributesExW(pdb, GetFileExInfoStandard, &data);
            const int pdbBytes =
                WideCharToMultiByte(CP_UTF8, 0, pdb, -1, narrow, sizeof(narrow), nullptr, nullptr);
            appendText(file, "pdb=");
            if (pdbBytes > 0) {
                appendText(file, narrow);
            }
            appendText(file, "\r\n");
            _snprintf_s(line, sizeof(line), _TRUNCATE, "pdb_exists=%d pdb_bytes=%lu\r\n",
                        attrs != INVALID_FILE_ATTRIBUTES ? 1 : 0, sized ? data.nFileSizeLow : 0UL);
            appendText(file, line);
        }
    }
    HANDLE identity = CreateFileA("build\\msvc\\steplens-build-identity.txt", GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    appendText(file, "identity_file=");
    if (identity == INVALID_HANDLE_VALUE) {
        appendText(file, "missing\r\n");
    } else {
        char bytes[256]{};
        DWORD read = 0;
        ReadFile(identity, bytes, sizeof(bytes) - 1, &read, nullptr);
        CloseHandle(identity);
        bytes[read] = '\0';
        for (DWORD index = 0; index < read; ++index) {
            if (bytes[index] == '\r' || bytes[index] == '\n') {
                bytes[index] = ' ';
            }
        }
        appendText(file, bytes);
        appendText(file, "\r\n");
    }
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        appendText(file, "exception=none\r\n");
        CloseHandle(file);
        return;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE, "exception_code=0x%08lX address=%p\r\n",
                info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    appendText(file, line);
#if defined(_M_X64)
    if (info->ContextRecord != nullptr) {
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int index = 0; index < 48; ++index) {
            if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                            SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {
                break;
            }
            if (frame.AddrPC.Offset == 0) {
                break;
            }
            const DWORD64 base = SymGetModuleBase64(process, frame.AddrPC.Offset);
            _snprintf_s(line, sizeof(line), _TRUNCATE, "frame=%d pc=0x%llX module=0x%llX\r\n", index,
                        static_cast<unsigned long long>(frame.AddrPC.Offset),
                        static_cast<unsigned long long>(base));
            appendText(file, line);
        }
        SymCleanup(process);
    }
#else
    appendText(file, "stackwalk=not-x64\r\n");
#endif
    CloseHandle(file);
}

void writeNamedDump(const wchar_t* directory, EXCEPTION_POINTERS* info) {
    if (directory == nullptr || directory[0] == L'\0') {
        return;
    }
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t stem[1200];
    swprintf_s(stem, L"%s\\steplens-%04u%02u%02u-%02u%02u%02u-%lu", directory, time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId());
    wchar_t dump[1300];
    wchar_t stack[1300];
    swprintf_s(dump, L"%s.dmp", stem);
    swprintf_s(stack, L"%s.stack.txt", stem);
    writeDumpFile(dump, info);
    writeStackFile(stack, info);
}

void writeAutoDump(EXCEPTION_POINTERS* info) {
    writeNamedDump(g_dumpDirectory, info);
    char lifeNarrow[1024];
    const DWORD lifeLen = GetEnvironmentVariableA("STEPLENS_LIFECYCLE_LOG", lifeNarrow, sizeof(lifeNarrow));
    if (lifeLen == 0 || lifeLen >= sizeof(lifeNarrow)) {
        return;
    }
    wchar_t lifeWide[1024];
    if (MultiByteToWideChar(CP_ACP, 0, lifeNarrow, -1, lifeWide, 1024) <= 0) {
        return;
    }
    wchar_t* slash = wcsrchr(lifeWide, L'\\');
    if (slash == nullptr) {
        slash = wcsrchr(lifeWide, L'/');
    }
    if (slash == nullptr) {
        return;
    }
    *slash = L'\0';
    writeNamedDump(lifeWide, info);
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
