#pragma once

#include "core/CancelToken.h"

#include <exception>
#include <functional>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace steplens {

enum class ErrorCode : std::uint8_t {
    Ok,
    FileNotFound,
    UnsupportedFormat,
    ParseFailed,
    TransferFailed,
    Cancelled,
    OutOfMemory,
    MeshFailed,
    Internal
};

// message 是英文技术信息，写日志。userMessage 是给界面的中文（UTF-8）。
struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
    std::string userMessage;
};

// MSVC 的 /std:c++20 没有 std::expected。调用方式和将来的 std::expected 保持一致。
template <class T> class Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> 有歧义");
    static_assert(!std::is_void_v<T>, "Result 不能是 void");

public:
    Result(T value) : m_v(std::move(value)) {}
    Result(Error err) : m_v(std::move(err)) {}

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(m_v);
    }

    [[nodiscard]] T& value() {
        return std::get<T>(m_v);
    }
    [[nodiscard]] const T& value() const {
        return std::get<T>(m_v);
    }
    [[nodiscard]] const Error& error() const {
        return std::get<Error>(m_v);
    }

private:
    std::variant<T, Error> m_v;
};

// 模块边界用：把异常收成 Result。7.9.3 的 Standard_Failure 不是 std::exception，
// 那个类型在 io 里单独捕获（core 不链接 OCCT）。
template <class F> using CallResult = std::remove_cvref_t<std::invoke_result_t<F>>;

template <class F> auto catchAsResult(F&& fn) -> Result<CallResult<F>> {
    try {
        return std::invoke(std::forward<F>(fn));
    } catch (const CancelledException&) {
        return Error{ErrorCode::Cancelled, "cancelled", "已取消"};
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::OutOfMemory, "bad_alloc", "内存不足，无法打开该文件"};
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        if (what == nullptr || what[0] == '\0') {
            what = "unknown";
        }
        return Error{ErrorCode::Internal, what, "内部错误"};
    }
}

} // namespace steplens
