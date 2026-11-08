#pragma once

#include "core/CancelToken.h"
#include "core/ProgressSink.h"

#include <Message_ProgressIndicator.hxx>

namespace steplens::io {

// 把 OCCT 的进度和 UserBreak 接到 ProgressSink / CancelToken。
// 20%–90% 的区段由 ProgressSink::setTransfer 负责，这里只传 OCCT 的 0–1 位置。
// 取消之后 OCCT 会把位置跳到 1。token 已经取消时不再写入，避免把进度条顶满。
class OcctProgressBridge final : public Message_ProgressIndicator {
public:
    OcctProgressBridge(ProgressSink& sink, const CancelToken& cancel);

    bool UserBreak() override;

protected:
    void Show(const Message_ProgressScope& scope, const bool force) override;

private:
    ProgressSink& m_sink;
    CancelToken m_cancel;
};

} // namespace steplens::io
