#include "occtsupport/OcctExceptions.h"

namespace steplens::occt {

// 让两个 OCCT 版本都实际编译这条模板，包括自定义 userMessage。返回值没有人调用，链接器可以丢掉。
int compileCatchOcct() {
    const auto plain = catchOcctAsResult([] { return 1; });
    const auto custom = catchOcctAsResult([] { return 2; }, ErrorCode::ParseFailed, "自定义文案");
    return (plain.ok() ? plain.value() : 0) + (custom.ok() ? custom.value() : 0);
}

} // namespace steplens::occt
