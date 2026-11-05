#pragma once

#include <exception>
#include <stop_token>
#include <utility>

namespace steplens {

// 软取消抛出的异常。parallelFor 和加载线程把它重新抛给调用者，再收成 Result。
class CancelledException : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "cancelled";
    }
};

// 可复制的观察端。复制后的 token 看到同一次 request_stop。
class CancelToken {
public:
    CancelToken() = default;
    explicit CancelToken(std::stop_token token) : m_st(std::move(token)) {}

    [[nodiscard]] bool isCancelled() const noexcept {
        return m_st.stop_requested();
    }

    void throwIfCancelled() const {
        if (isCancelled()) {
            throw CancelledException{};
        }
    }

    [[nodiscard]] const std::stop_token& stopToken() const noexcept {
        return m_st;
    }

private:
    std::stop_token m_st;
};

// 拥有停止状态的一端。requestStop 只改标志，不等待后台。
class CancelSource {
public:
    CancelSource() = default;
    CancelSource(const CancelSource&) = delete;
    CancelSource& operator=(const CancelSource&) = delete;
    CancelSource(CancelSource&&) noexcept = default;
    CancelSource& operator=(CancelSource&&) noexcept = default;

    [[nodiscard]] CancelToken token() const noexcept {
        return CancelToken{m_source.get_token()};
    }

    void requestStop() noexcept {
        m_source.request_stop();
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return m_source.stop_requested();
    }

private:
    std::stop_source m_source;
};

} // namespace steplens
