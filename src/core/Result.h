#pragma once

#include "core/CancelToken.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <new>
#include <string>
#include <string_view>
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

template <class T> struct IsResult : std::false_type {};

template <class T> struct IsResult<Result<T>> : std::true_type {};

template <class T> struct AsResult {
    using type = Result<std::remove_cvref_t<T>>;
};

template <class T> struct AsResult<Result<T>> {
    using type = Result<T>;
};

// 两个 OCCT 版本共用。7.9.3 的 Standard_Failure 不继承 std::exception，必须在这条宏之前单独捕获。
#define STEPLENS_RESULT_CATCHES                                                                              \
    catch (const ::steplens::CancelledException&) {                                                          \
        return ::steplens::Error{::steplens::ErrorCode::Cancelled, "cancelled", "已取消"};                   \
    }                                                                                                        \
    catch (const std::bad_alloc&) {                                                                          \
        return ::steplens::Error{::steplens::ErrorCode::OutOfMemory, "bad_alloc", "内存不足"};               \
    }                                                                                                        \
    catch (const std::exception& ex) {                                                                       \
        const char* what = ex.what();                                                                        \
        if (what == nullptr || std::string_view{what}.empty()) {                                             \
            what = "unknown";                                                                                \
        }                                                                                                    \
        return ::steplens::Error{::steplens::ErrorCode::Internal, what, "内部错误"};                         \
    }                                                                                                        \
    catch (...) {                                                                                            \
        return ::steplens::Error{::steplens::ErrorCode::Internal, "unknown exception", "内部错误"};          \
    }

// fn 若已经返回 Result<U>，这里也返回 Result<U>，不再套一层。任何异常都不会穿出。
template <class F> auto catchAsResult(F&& fn) -> typename AsResult<std::invoke_result_t<F&>>::type {
    using Raw = std::remove_cvref_t<std::invoke_result_t<F&>>;
    using R = typename AsResult<Raw>::type;
    try {
        if constexpr (IsResult<Raw>::value) {
            return std::invoke(std::forward<F>(fn));
        } else {
            return R{std::invoke(std::forward<F>(fn))};
        }
    }
    STEPLENS_RESULT_CATCHES
}

} // namespace steplens
