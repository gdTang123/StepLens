#include "io/LoadInternal.h"

#include "core/OcctLoadContract.h"

#include <UnitsMethods_LengthUnit.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>

#if defined(_WIN32) || defined(__GLIBC__)
#include <malloc.h>
#endif
#include <mutex>
#include <system_error>
#include <utility>

namespace steplens::io {

void releaseFreedHeap() noexcept {
#if defined(_WIN32)
    _heapmin();
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

} // namespace steplens::io

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
