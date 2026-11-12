#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "io/LoadInternal.h"

#include "core/OcctLoadContract.h"

#include <Standard.hxx>
#include <UnitsMethods_LengthUnit.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>

#include <malloc.h>
#if defined(_WIN32)
#include <Windows.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace steplens::io::detail {

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

InputFile inspectInputFile(const std::filesystem::path& file) {
    InputFile input;
    input.utf8 = pathUtf8(file);
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::status(file, statusError);
    if (statusError || !std::filesystem::exists(status)) {
        input.error = makeFailure(ErrorCode::FileNotFound, "file not found: " + input.utf8, "找不到文件");
        return input;
    }
    if (!std::filesystem::is_regular_file(status)) {
        input.error =
            makeFailure(ErrorCode::FileNotFound, "not a regular file: " + input.utf8, "无法读取文件");
        return input;
    }
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(file, sizeError);
    if (sizeError) {
        input.error = makeFailure(ErrorCode::FileNotFound, "file size failed: " + input.utf8, "无法读取文件");
        return input;
    }
    input.bytes = static_cast<std::uint64_t>(bytes);
    input.ok = true;
    return input;
}

namespace {

std::atomic<HeapPendingProbe> g_pendingProbe{nullptr};

bool envFlag(const char* name) noexcept {
#if defined(_WIN32)
    char buffer[8] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    return length == 1 && buffer[0] == '1';
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
#endif
}

unsigned long threadToken() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentThreadId());
#else
    return static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

} // namespace

void setHeapPendingProbe(HeapPendingProbe probe) noexcept {
    g_pendingProbe.store(probe, std::memory_order_release);
}

void releaseFreedHeap() noexcept {
    int pending = -1;
    if (const HeapPendingProbe probe = g_pendingProbe.load(std::memory_order_acquire)) {
        pending = probe();
    }
    const bool noTrim = envFlag("STEPLENS_REPRO_NO_TRIM");
    const bool skipPending = envFlag("STEPLENS_REPRO_SKIP_TRIM_IF_PENDING") && pending > 0;
    const bool purgeFirst = envFlag("STEPLENS_REPRO_PURGE_BEFORE_TRIM");
    // 对照实验的观测点：pending>0 表示后台 Close 还没结束，这时再 _heapmin 就是并发。
    std::fprintf(stderr, "heap_reclaim pending=%d no_trim=%d skip_pending=%d purge_first=%d thread=%lu\n",
                 pending, noTrim ? 1 : 0, skipPending ? 1 : 0, purgeFirst ? 1 : 0, threadToken());
    std::fflush(stderr);
    if (noTrim || skipPending) {
        return;
    }
    if (purgeFirst) {
        Standard::Purge();
    }
#if defined(_WIN32)
    _heapmin();
#else
    malloc_trim(0);
#endif
}

opencascade::handle<TDocStd_Document> newXcafDocument() {
    // XCAFApp_Application 的 NewDocument / Close 不是线程安全的。全进程只用这一把锁。
    std::lock_guard lock(xcafDocumentMutex());
    opencascade::handle<TDocStd_Document> doc;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
    return doc;
}

void closeXcafDocument(opencascade::handle<TDocStd_Document>& doc) {
    if (doc.IsNull()) {
        return;
    }
    try {
        std::lock_guard lock(xcafDocumentMutex());
        if (!doc.IsNull()) {
            XCAFApp_Application::GetApplication()->Close(doc);
        }
    } catch (...) {
        // 析构路径上不能再抛。句柄先放下，避免下次再 Close 一次。
    }
    doc.Nullify();
}

double lengthUnitToMillimetre(const opencascade::handle<TDocStd_Document>& doc) {
    if (doc.IsNull()) {
        return 1.0;
    }
    double millimetres = 0.0;
    if (!XCAFDoc_DocumentTool::GetLengthUnit(doc, millimetres, UnitsMethods_LengthUnit_Millimeter)) {
        return 1.0;
    }
    if (!(millimetres > 0.0)) {
        return 1.0;
    }
    return millimetres;
}

} // namespace steplens::io::detail
