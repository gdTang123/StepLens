#include "io/OcctProgressBridge.h"

#include <Message_ProgressScope.hxx>

namespace steplens::io {

OcctProgressBridge::OcctProgressBridge(ProgressSink& sink, const CancelToken& cancel)
    : m_sink(sink), m_cancel(cancel) {}

bool OcctProgressBridge::UserBreak() {
    return m_cancel.isCancelled();
}

void OcctProgressBridge::Show(const Message_ProgressScope&, const bool) {
    if (m_cancel.isCancelled()) {
        return;
    }
    // 时间部分由 enterTransfer 记下的阶段时钟在 fraction() 里计算。这里只报 OCCT 位置。
    m_sink.setTransfer(static_cast<float>(GetPosition()), 0.0);
}

} // namespace steplens::io
