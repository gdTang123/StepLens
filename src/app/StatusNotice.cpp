#include "app/StatusNotice.h"

#include <QStringList>

namespace steplens {

void StatusNotice::setMemoryConditions(bool configInvalid, bool samplingFailed) {
    QStringList details;
    if (configInvalid)
        details.append(QStringLiteral("内存比例配置无效，已使用默认值"));
    if (samplingFailed)
        details.append(QStringLiteral("内存预检不可用，已跳过"));
    m_message = details.join(QStringLiteral("；"));
    m_finished = false;
}

void StatusNotice::finish() {
    if (!m_message.isEmpty() && !m_finished) {
        m_clock.start();
        m_finished = true;
    }
}

void StatusNotice::clear() {
    m_message.clear();
    m_finished = false;
}

QString StatusNotice::message() const {
    if (m_message.isEmpty() || (m_finished && m_clock.elapsed() >= 5000))
        return {};
    return m_message;
}

QString StatusNotice::combine(const QString& stage) const {
    const QString notice = message();
    if (notice.isEmpty())
        return stage;
    return stage.isEmpty() ? notice : stage + QStringLiteral(" — ") + notice;
}

} // namespace steplens
