#include "io/StepLoader.h"

#include "io/LoadInternal.h"
#include "io/OcctProgressBridge.h"
#include "io/ProgressStreamBuf.h"
#include "occtsupport/OcctBoundary.h"
#include "occtsupport/OcctExceptions.h"

#include <HeaderSection_FileSchema.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_HAsciiString.hxx>

#include <istream>
#include <utility>

namespace steplens::io {
namespace {

using detail::ReaderSlot;
using detail::XcafDocumentGuard;

void applyOptions(STEPCAFControl_Reader& reader, const LoadOptions& options) {
    reader.SetColorMode(options.readColors);
    reader.SetNameMode(options.readNames);
    reader.SetLayerMode(options.readLayers);
    reader.SetPropsMode(options.readProps);
    reader.SetGDTMode(options.readGdt);
    reader.SetMatMode(options.readMaterials);
    reader.SetViewMode(options.readViews);
    reader.SetSHUOMode(options.readShuo);
}

std::string readSchema(const STEPCAFControl_Reader& reader) {
    const auto model = reader.Reader().StepModel();
    if (model.IsNull() || !model->HasHeaderEntity(STANDARD_TYPE(HeaderSection_FileSchema))) {
        return {};
    }
    const auto schema = opencascade::handle<HeaderSection_FileSchema>::DownCast(
        model->HeaderEntity(STANDARD_TYPE(HeaderSection_FileSchema)));
    if (schema.IsNull() || schema->NbSchemaIdentifiers() < 1) {
        return {};
    }
    std::string joined;
    for (int index = 1; index <= schema->NbSchemaIdentifiers(); ++index) {
        const auto name = schema->SchemaIdentifiersValue(index);
        if (name.IsNull() || name->ToCString() == nullptr) {
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += name->ToCString();
    }
    return joined;
}

Result<LoadedDocument> readStep(const std::filesystem::path& file, const LoadOptions& options,
                                ProgressSink& progress, const CancelToken& cancel) {
    // 加载线程不是 TaskScheduler。进 OCCT 之前在这条线程上装信号转换。
    occt::OcctBoundary::armWorkerThread();

    const detail::InputFile input = detail::inspectInputFile(file);
    if (!input.ok) {
        return input.error;
    }
    const std::string& utf8 = input.utf8;
    const std::uint64_t fileBytes = input.bytes;

    ReaderSlot<STEPCAFControl_Reader> slot;
    slot.reader = std::make_unique<STEPCAFControl_Reader>();
    applyOptions(*slot.reader, options);

    ProgressStreamBuf buffer(file, fileBytes, progress, cancel);
    if (!buffer.isOpen()) {
        return detail::makeFailure(ErrorCode::FileNotFound, "open failed: " + utf8, "无法读取文件");
    }
    std::istream stream(&buffer);
    const IFSelect_ReturnStatus readStatus = slot.reader->ReadStream(utf8.c_str(), stream);
    if (cancel.isCancelled() || buffer.wasCancelled()) {
        return detail::cancelledError("ReadStream cancelled");
    }
    if (readStatus != IFSelect_RetDone) {
        return detail::makeFailure(ErrorCode::ParseFailed,
                                   "ReadStream failed: " + std::to_string(static_cast<int>(readStatus)),
                                   "文件损坏或不是有效的 STEP 文件");
    }
    if (progress.phase() == ProgressPhase::ReadingBytes) {
        progress.enterParsing();
    }
    if (cancel.isCancelled()) {
        return detail::cancelledError("cancelled after ReadStream");
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
            return detail::cancelledError("Transfer cancelled");
        }
        return detail::makeFailure(ErrorCode::TransferFailed, "Transfer failed", "转换几何时出错");
    }

    LoadedDocument loaded;
    loaded.schema = readSchema(*slot.reader);
    loaded.doc = document.release();
    loaded.lengthUnitToMm = detail::lengthUnitToMillimetre(loaded.doc);
    loaded.fileBytes = fileBytes;
    progress.setPresentFaceFraction(0.f);
    return loaded;
}

} // namespace

Result<LoadedDocument> StepLoader::load(const std::filesystem::path& file, const LoadOptions& options,
                                        ProgressSink& progress, const CancelToken& cancel) {
    // Interface_Static 只允许在 main() 里改。这里只设置这个 reader 自己的模式开关。
    auto result = occt::catchOcctAsResult(
        [&]() -> Result<LoadedDocument> { return readStep(file, options, progress, cancel); },
        ErrorCode::TransferFailed, "读取 STEP 文件时出错");
    return detail::preferCancel(std::move(result), cancel);
}

} // namespace steplens::io
