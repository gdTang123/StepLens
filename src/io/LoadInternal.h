#pragma once

// 只给 io 的翻译单元包含。公开头是 ILoader.h。

#include "core/CancelToken.h"
#include "core/Log.h"
#include "core/Result.h"
#include "io/ILoader.h"

#include <TDocStd_Document.hxx>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace steplens::io::detail {

// path::u8string() 在 C++20 里是 UTF-8。STEP 走我们自己的 ifstream（宽路径），
// IGES 的 ReadFile(const char*) 由 OCCT 按 UTF-8 打开（OSD_OpenFile）。
// 不调用 WideCharToMultiByte，因此也不包含 stringapiset.h。
[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path);

struct InputFile {
    bool ok = false;
    Error error{ErrorCode::FileNotFound, "file not found", "找不到文件"};
    std::uint64_t bytes = 0;
    std::string utf8;
};

// 不存在、不是普通文件、或拿不到大小时 ok 为 false，error.userMessage 是中文。
[[nodiscard]] InputFile inspectInputFile(const std::filesystem::path& file);

void releaseFreedHeap() noexcept;

// 复现实验用。测试在调用回收前装上，用来打印当时还有几份文档在后台 Close。
// 不改变默认行为。不要合并到 main。
using HeapPendingProbe = int (*)() noexcept;
void setHeapPendingProbe(HeapPendingProbe probe) noexcept;

[[nodiscard]] opencascade::handle<TDocStd_Document> newXcafDocument();

// NewDocument / Close 只走 xcafDocumentMutex()。doc 在返回前会被清空。
void closeXcafDocument(opencascade::handle<TDocStd_Document>& doc);

// 文档单位换到毫米。没有单位属性时返回 1。
[[nodiscard]] double lengthUnitToMillimetre(const opencascade::handle<TDocStd_Document>& doc);

[[nodiscard]] inline Error makeFailure(ErrorCode code, std::string message, std::string userMessage) {
    Log::error(message);
    return Error{code, std::move(message), std::move(userMessage)};
}

[[nodiscard]] inline Error cancelledError(std::string message = "cancelled") {
    return Error{ErrorCode::Cancelled, std::move(message), "已取消"};
}

// 取消已经提出时，解析/转换失败都算取消。内存不足仍按内存不足返回。
template <class T> [[nodiscard]] Result<T> preferCancel(Result<T> result, const CancelToken& cancel) {
    if (result.ok() || !cancel.isCancelled()) {
        return result;
    }
    if (result.error().code == ErrorCode::Cancelled || result.error().code == ErrorCode::OutOfMemory) {
        return result;
    }
    return cancelledError(result.error().message);
}

class XcafDocumentGuard {
public:
    XcafDocumentGuard() = default;
    ~XcafDocumentGuard() {
        reset();
    }

    XcafDocumentGuard(const XcafDocumentGuard&) = delete;
    XcafDocumentGuard& operator=(const XcafDocumentGuard&) = delete;

    XcafDocumentGuard(XcafDocumentGuard&& other) noexcept : m_doc(other.m_doc) {
        other.m_doc.Nullify();
    }

    XcafDocumentGuard& operator=(XcafDocumentGuard&&) = delete;

    void create() {
        m_doc = newXcafDocument();
    }

    [[nodiscard]] bool isNull() const {
        return m_doc.IsNull();
    }

    [[nodiscard]] const opencascade::handle<TDocStd_Document>& get() const {
        return m_doc;
    }

    [[nodiscard]] opencascade::handle<TDocStd_Document> release() {
        opencascade::handle<TDocStd_Document> doc = m_doc;
        m_doc.Nullify();
        return doc;
    }

    void reset() {
        if (!m_doc.IsNull()) {
            closeXcafDocument(m_doc);
        }
    }

private:
    opencascade::handle<TDocStd_Document> m_doc;
};

// 析构时丢掉读取器并把空闲堆还给系统。成功和失败都走这里，避免漏释放。
template <class Reader> class ReaderSlot {
public:
    std::unique_ptr<Reader> reader;

    ReaderSlot() = default;
    ~ReaderSlot() {
        reader.reset();
        releaseFreedHeap();
    }

    ReaderSlot(const ReaderSlot&) = delete;
    ReaderSlot& operator=(const ReaderSlot&) = delete;
};

} // namespace steplens::io::detail
