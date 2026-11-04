#pragma once

// 唯一允许出现 OCC_VERSION_HEX 的源文件。CI 只在 *.h/*.cpp 里 grep 这一处。
// 7.9.3：Standard_Failure 派生自 Standard_Transient，消息用 GetMessageString()。
// 8.0.1：改为派生自 std::exception，消息用 what()。GetMessageString() 已弃用，/WX 下不能调用。
//        没有 DynamicType()，类型名用 typeid。

#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>

#include <typeinfo>

namespace steplens::occt {

constexpr bool kIsOcct8 = OCC_VERSION_HEX >= 0x080000;

inline const char* versionString() noexcept {
    return OCC_VERSION_COMPLETE;
}

inline const char* failureMessage(const Standard_Failure& failure) {
    const char* text = nullptr;
#if OCC_VERSION_HEX >= 0x080000
    text = failure.what();
#else
    text = failure.GetMessageString();
#endif
    if (text == nullptr || text[0] == '\0') {
        return "Standard_Failure";
    }
    return text;
}

inline const char* failureTypeName(const Standard_Failure& failure) {
    return typeid(failure).name();
}

} // namespace steplens::occt
