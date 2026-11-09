#pragma once

#include "io/ILoader.h"

namespace steplens::io {

// IGES 读取不保证并发安全。load() 用进程内的一把锁把 ReadFile 和 Transfer 串起来。
// 锁顺序：先这把锁，再 xcafDocumentMutex()。不要反过来。
// ReadFile 阻塞期间没有取消检查点。软取消只在这次调用之前和返回之后生效，
// 调用进行中的取消要等 ReadFile 结束才看得到。
class IgesLoader final : public ILoader {
public:
    [[nodiscard]] Result<LoadedDocument> load(const std::filesystem::path& file, const LoadOptions& options,
                                              ProgressSink& progress, const CancelToken& cancel) override;
};

} // namespace steplens::io
