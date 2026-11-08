#include "io/ILoader.h"

#include "io/IgesLoader.h"
#include "io/StepLoader.h"

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace steplens::io {
namespace {

enum class CadFormat { Step, Iges, Unknown };

std::string lowerExtension(const std::filesystem::path& file) {
    std::string extension = file.extension().string();
    for (char& character : extension) {
        const unsigned char byte = static_cast<unsigned char>(character);
        character = static_cast<char>(std::tolower(byte));
    }
    return extension;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

CadFormat sniffHeader(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::in | std::ios::binary);
    if (!input) {
        return CadFormat::Unknown;
    }
    char bytes[128] = {};
    input.read(bytes, static_cast<std::streamsize>(sizeof(bytes)));
    const auto count = input.gcount();
    if (count <= 0) {
        return CadFormat::Unknown;
    }
    const std::string_view header(bytes, static_cast<std::size_t>(count));
    if (startsWith(header, "ISO-10303-21")) {
        return CadFormat::Step;
    }
    // IGES 起始段是 80 列定长记录，第 73 列是 'S'。
    if (header.size() >= 73 && header[72] == 'S') {
        return CadFormat::Iges;
    }
    return CadFormat::Unknown;
}

CadFormat detectFormat(const std::filesystem::path& file) {
    const std::string extension = lowerExtension(file);
    if (extension == ".stp" || extension == ".step") {
        return CadFormat::Step;
    }
    if (extension == ".igs" || extension == ".iges") {
        return CadFormat::Iges;
    }
    return sniffHeader(file);
}

class UnsupportedLoader final : public ILoader {
public:
    [[nodiscard]] Result<LoadedDocument> load(const std::filesystem::path&, const LoadOptions&, ProgressSink&,
                                              const CancelToken&) override {
        return Error{ErrorCode::UnsupportedFormat, "unsupported format", "不支持的文件格式"};
    }
};

} // namespace

std::unique_ptr<ILoader> makeLoader(const std::filesystem::path& file) {
    const CadFormat format = detectFormat(file);
    if (format == CadFormat::Step) {
        return std::make_unique<StepLoader>();
    }
    if (format == CadFormat::Iges) {
        return std::make_unique<IgesLoader>();
    }
    return std::make_unique<UnsupportedLoader>();
}

} // namespace steplens::io
