#pragma once

#include "core/CancelToken.h"
#include "core/ProgressSink.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <streambuf>
#include <vector>

namespace steplens::io {

// ReadFile 没有进度回调。ReadStream 吃 std::istream，所以用这个缓冲统计已读字节。
// 分段比例在 ProgressSink（6.5）：这里只调用 setReadingBytes。流真正读完时 enterParsing，
// 后面没有 underflow 的解析段就靠读端时间估算往前走。
// 取消时 underflow 直接返回 EOF，让解析提前结束。调用方再丢掉结果。
// 只在加载线程上使用。
class ProgressStreamBuf final : public std::streambuf {
public:
    static constexpr std::size_t kChunkBytes = 1u << 20;

    ProgressStreamBuf(const std::filesystem::path& file, std::uint64_t totalBytes, ProgressSink& sink,
                      const CancelToken& cancel);

    [[nodiscard]] bool isOpen() const noexcept {
        return m_in.is_open();
    }

    // underflow 因为取消返回了 EOF。自然读到文件末尾不会置上。
    [[nodiscard]] bool wasCancelled() const noexcept {
        return m_cancelled;
    }

protected:
    int_type underflow() override;

private:
    std::ifstream m_in;
    std::vector<char> m_buffer;
    std::uint64_t m_read = 0;
    std::uint64_t m_total = 0;
    ProgressSink& m_sink;
    CancelToken m_cancel;
    bool m_sawEof = false;
    bool m_cancelled = false;
};

} // namespace steplens::io
