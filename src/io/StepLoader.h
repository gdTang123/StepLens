#pragma once

#include "io/ILoader.h"

namespace steplens::io {

class StepLoader final : public ILoader {
public:
    [[nodiscard]] Result<LoadedDocument> load(const std::filesystem::path& file, const LoadOptions& options,
                                              ProgressSink& progress, const CancelToken& cancel) override;
};

} // namespace steplens::io
