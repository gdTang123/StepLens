#include "io/IgesLoader.h"

#include "io/LoadInternal.h"
#include "io/OcctProgressBridge.h"
#include "occtsupport/OcctBoundary.h"
#include "occtsupport/OcctExceptions.h"

#include <IFSelect_ReturnStatus.hxx>
#include <IGESCAFControl_Reader.hxx>

#include <mutex>
#include <utility>

namespace steplens::io {
namespace {

using detail::ReaderSlot;
using detail::XcafDocumentGuard;

std::mutex& igesLoadMutex() {
    static std::mutex mutex;
    return mutex;
}

void applyOptions(IGESCAFControl_Reader& reader, const LoadOptions& options) {
    reader.SetColorMode(options.readColors);
    reader.SetNameMode(options.readNames);
    reader.SetLayerMode(options.readLayers);
}

Result<LoadedDocument> readIges(const std::filesystem::path& file, const LoadOptions& options,
                                ProgressSink& progress, const CancelToken& cancel) {
    occt::OcctBoundary::armWorkerThread();

    const detail::InputFile input = detail::inspectInputFile(file);
    if (!input.ok) {
        return input.error;
    }
    const std::string& utf8 = input.utf8;
    const std::uint64_t fileBytes = input.bytes;
    if (cancel.isCancelled()) {
        return detail::cancelledError("cancelled before IGES ReadFile");
    }

    // ReadFile 没有字节回调。先进入解析段，这段时间靠 ProgressSink 的时间估算前进。
    progress.enterParsing();

    ReaderSlot<IGESCAFControl_Reader> slot;
    slot.reader = std::make_unique<IGESCAFControl_Reader>();
    applyOptions(*slot.reader, options);
    const IFSelect_ReturnStatus readStatus = slot.reader->ReadFile(utf8.c_str());
    if (cancel.isCancelled()) {
        return detail::cancelledError("IGES ReadFile cancelled");
    }
    // 损坏的 IGES 有时仍返回 RetDone，但一个根都没有。那是解析失败，不是转换失败。
    const int roots =
        readStatus == IFSelect_RetDone ? static_cast<int>(slot.reader->NbRootsForTransfer()) : 0;
    if (readStatus != IFSelect_RetDone || roots <= 0) {
        return detail::makeFailure(ErrorCode::ParseFailed,
                                   "ReadFile failed: " + std::to_string(static_cast<int>(readStatus)) +
                                       " roots=" + std::to_string(roots),
                                   "文件损坏或不是有效的 IGES 文件");
    }

    XcafDocumentGuard document;
    document.create();
    if (document.isNull()) {
        return detail::makeFailure(ErrorCode::Internal, "NewDocument failed", "无法创建文档");
    }

    progress.enterTransfer();
    opencascade::handle<OcctProgressBridge> bridge = new OcctProgressBridge(progress, cancel);
    const bool transferred = static_cast<bool>(slot.reader->Transfer(document.get(), bridge->Start()));
    if (!transferred || cancel.isCancelled()) {
        if (cancel.isCancelled()) {
            return detail::cancelledError("IGES Transfer cancelled");
        }
        return detail::makeFailure(ErrorCode::TransferFailed, "Transfer failed", "转换几何时出错");
    }

    LoadedDocument loaded;
    loaded.schema = "IGES";
    loaded.doc = document.release();
    loaded.lengthUnitToMm = detail::lengthUnitToMillimetre(loaded.doc);
    loaded.fileBytes = fileBytes;
    progress.setPresentFaceFraction(0.f);
    return loaded;
}

} // namespace

Result<LoadedDocument> IgesLoader::load(const std::filesystem::path& file, const LoadOptions& options,
                                        ProgressSink& progress, const CancelToken& cancel) {
    // 先拿 IGES 锁，NewDocument/Close 再拿 xcafDocumentMutex()。
    std::lock_guard lock(igesLoadMutex());
    auto result = occt::catchOcctAsResult(
        [&]() -> Result<LoadedDocument> { return readIges(file, options, progress, cancel); },
        ErrorCode::TransferFailed, "读取 IGES 文件时出错");
    return detail::preferCancel(std::move(result), cancel);
}

} // namespace steplens::io
