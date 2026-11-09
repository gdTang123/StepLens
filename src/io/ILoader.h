#pragma once

// T05 的加载结果只到 XCAF 文档。装配树、网格、场景、测量都不在这里（T06 及以后）。

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "core/Result.h"

#include <TDocStd_Document.hxx>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace steplens::io {

struct LoadOptions {
    bool readColors = true;
    bool readNames = true;
    bool readLayers = false;
    bool readProps = false;
    bool readGdt = false;
    bool readMaterials = false;
    bool readViews = false;
    bool readShuo = true;
};

// 加载线程的产出。调用方拥有 doc，关闭时必须走 xcafDocumentMutex()。
struct LoadedDocument {
    opencascade::handle<TDocStd_Document> doc;
    double lengthUnitToMm = 1.0;
    std::uint64_t fileBytes = 0;
    std::string schema;
};

class ILoader {
public:
    virtual ~ILoader() = default;

    // progress 必须已经 reset 成这次文件的大小（LoadSession 构造时会做）。
    // 这里不调用 ProgressSink::reset，避免把同一次加载的单调钳位清掉。
    [[nodiscard]] virtual Result<LoadedDocument> load(const std::filesystem::path& file,
                                                      const LoadOptions& options, ProgressSink& progress,
                                                      const CancelToken& cancel) = 0;
};

// .stp/.step → STEP，.igs/.iges → IGES。没有扩展名时看文件头。
// 认不出的格式仍返回一个加载器，load() 给出中文“不支持的文件格式”。不会返回空指针。
[[nodiscard]] std::unique_ptr<ILoader> makeLoader(const std::filesystem::path& file);

} // namespace steplens::io
