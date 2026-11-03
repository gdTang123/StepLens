#include "core/Version.h"

namespace steplens {

// 让版本字符串进入静态库，避免这个翻译单元被链接器当成空文件丢掉。
extern const std::string_view kLinkedVersion = kVersion;

} // namespace steplens
