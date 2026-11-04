#pragma once

// 7.9.3 与 8.0.1 的差异只放在这里。core 不包含本头。
// 8.0.1：Standard_Failure 继承 std::exception，消息用 what()。
//        GetMessageString() 标了弃用（"Use what() instead"），/WX 下不能调用。
//        没有 DynamicType()，这里也不用它。
// 7.9.3：Standard_Failure 仍继承 Standard_Transient，消息用 GetMessageString()。

#include <Standard_Failure.hxx>

#include <exception>
#include <string_view>
#include <type_traits>

namespace steplens::occt {

template <class Failure> const char* failureMessage(const Failure& failure) {
    const char* text = nullptr;
    if constexpr (std::is_base_of_v<std::exception, Failure>) {
        text = failure.what();
    } else {
        text = failure.GetMessageString();
    }
    if (text == nullptr || std::string_view{text}.empty()) {
        return "Standard_Failure";
    }
    return text;
}

} // namespace steplens::occt
