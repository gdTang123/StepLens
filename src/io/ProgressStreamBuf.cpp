#include "io/ProgressStreamBuf.h"

namespace steplens::io {

ProgressStreamBuf::ProgressStreamBuf(const std::filesystem::path& file, std::uint64_t totalBytes,
                                     ProgressSink& sink, const CancelToken& cancel)
    : m_buffer(kChunkBytes), m_total(totalBytes), m_sink(sink), m_cancel(cancel) {
    m_in.open(file, std::ios::in | std::ios::binary);
}

ProgressStreamBuf::int_type ProgressStreamBuf::underflow() {
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    if (m_cancel.isCancelled()) {
        m_cancelled = true;
        return traits_type::eof();
    }
    if (!m_in.is_open()) {
        return traits_type::eof();
    }

    m_in.read(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
    const auto count = m_in.gcount();
    if (count <= 0) {
        if (!m_sawEof) {
            m_sawEof = true;
            m_sink.enterParsing();
        }
        return traits_type::eof();
    }

    const auto bytes = static_cast<std::uint64_t>(count);
    m_read += bytes;
    if (m_total > 0 && m_read > m_total) {
        m_read = m_total;
    }
    m_sink.setReadingBytes(m_read);
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data() + count);
    return traits_type::to_int_type(*gptr());
}

} // namespace steplens::io
