#pragma once

#include <QElapsedTimer>
#include <QString>

namespace steplens {

// Shared timed notice state used by MainWindow and the Qt Widgets evidence harness.
class StatusNotice final {
public:
    void setMemoryConditions(bool configInvalid, bool samplingFailed);
    void finish();
    void clear();
    [[nodiscard]] QString message() const;
    [[nodiscard]] QString combine(const QString& stage) const;

private:
    QString m_message;
    QElapsedTimer m_clock;
    bool m_finished = false;
};

} // namespace steplens
