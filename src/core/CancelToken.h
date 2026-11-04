#pragma once

#include <atomic>
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

// 架构 6.6：界面调用 requestFromUi 后立刻返回（目标 ≤ 1 秒，这里本身不等待）。
// 后台线程观察 token，释放完再 noteBackgroundReleased。退出进程时不必等待释放。
class SoftCancel {
public:
    void requestFromUi() noexcept {
        m_source.requestStop();
    }

    [[nodiscard]] CancelToken token() const noexcept {
        return m_source.token();
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return m_source.stopRequested();
    }

    // 由后台线程在丢弃结果并释放内存之后调用。界面不调用它。
    void noteBackgroundReleased() noexcept {
        m_released.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool backgroundReleased() const noexcept {
        return m_released.load(std::memory_order_acquire);
    }

private:
    CancelSource m_source;
    // 由后台线程写，界面线程读。
    std::atomic<bool> m_released{false};
};

} // namespace steplens
