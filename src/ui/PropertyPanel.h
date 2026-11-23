#pragma once

#include "model/DocumentModel.h"

#include <QDockWidget>
#include <QString>

#include <cstdint>

class QLabel;

namespace steplens::ui {

class PropertyPanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit PropertyPanel(QWidget* parent = nullptr);
    void showNode(const model::DocumentModel& document, model::NodeId id, const QString& name);
    void clearNode();

private:
    void publish();
    void finishMass(std::uint64_t generation, const QString& volume, const QString& area);

    QLabel* m_body = nullptr;
    QString m_name;
    QString m_box;
    QString m_volume;
    QString m_area;
    std::uint64_t m_generation = 0;
};

} // namespace steplens::ui
