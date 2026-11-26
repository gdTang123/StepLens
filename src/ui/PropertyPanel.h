#pragma once

#include "model/DocumentModel.h"
#include "ui/MeasureResultModel.h"

#include <QDockWidget>
#include <QString>

#include <cstdint>

class QLabel;
class QListView;

namespace steplens::ui {

class PropertyPanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit PropertyPanel(QWidget* parent = nullptr);
    void showNode(const model::DocumentModel& document, model::NodeId id, const QString& name);
    void clearNode();
    [[nodiscard]] MeasureResultModel* measureModel() const {
        return m_measures;
    }
    void setDisplayLengthUnit(double fromMillimeters, const QString& label);
    void addDistanceResult(double millimeters);
    void addAngleResult(double degrees);
    void clearMeasurements();

private:
    void publish();
    void finishMass(std::uint64_t generation, const QString& volume, const QString& area);

    QLabel* m_body = nullptr;
    QListView* m_measureView = nullptr;
    MeasureResultModel* m_measures = nullptr;
    QString m_name;
    QString m_box;
    QString m_volume;
    QString m_area;
    double m_fromMillimeters = 1.0;
    QString m_lengthLabel = QStringLiteral("mm");
    std::uint64_t m_generation = 0;
};

} // namespace steplens::ui
