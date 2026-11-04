#include "io/OcctExceptions.h"

namespace steplens::io {

// 让两个 OCCT 版本都实际编译这条模板。返回值没有人调用，链接器可以丢掉。
int compileCatchOcct() {
    const auto result = catchOcctAsResult([] { return 1; });
    return result.ok() ? result.value() : 0;
}

} // namespace steplens::io
