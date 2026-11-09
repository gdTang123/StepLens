#pragma once

#include "io/ILoader.h"

namespace steplens::io {

// STEP 读取没有进程内的互斥锁。OCCT 的 STEP 读取器不能并发。
// 生产路径只走 LoadCoordinator：它同一时刻只跑一个 LoadSession。
// 不要在协调器外面再开线程同时调用 load()。
class StepLoader final : public ILoader {
public:
    [[nodiscard]] Result<LoadedDocument> load(const std::filesystem::path& file, const LoadOptions& options,
                                              ProgressSink& progress, const CancelToken& cancel) override;
};

} // namespace steplens::io
