#pragma once

// 只给已经链接 OCCT 的翻译单元包含。core 不包含这个头。
// 7.9.3 的 Standard_Failure 不继承 std::exception，8.0.1 继承。GetMessageString() 两个版本都有。
// 必须写在 STEPLENS_RESULT_CATCHES 之前，否则 8.0.1 会被收成笼统的 "内部错误"。

#include "core/Result.h"
#include "io/occt_compat.h"

#include <string>
#include <utility>

namespace steplens::io {

template <class F>
auto catchOcctAsResult(F&& fn, ErrorCode code = ErrorCode::Internal) ->
    typename AsResult<std::remove_cvref_t<std::invoke_result_t<F&>>>::type {
    using Raw = std::remove_cvref_t<std::invoke_result_t<F&>>;
    using R = typename AsResult<Raw>::type;
    try {
        if constexpr (IsResult<Raw>::value) {
            return std::invoke(std::forward<F>(fn));
        } else {
            return R{std::invoke(std::forward<F>(fn))};
        }
    } catch (const Standard_Failure& failure) {
        return Error{code, occt::failureMessage(failure), "读取或计算时出错"};
    }
    STEPLENS_RESULT_CATCHES
}

} // namespace steplens::io
