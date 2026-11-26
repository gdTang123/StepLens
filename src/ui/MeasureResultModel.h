#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

namespace steplens::ui {

enum class MeasureKind : quint8 { Distance, Angle };

struct MeasureResultEntry {
    MeasureKind kind = MeasureKind::Distance;
    QString text;
};

// 只保存已经算完的展示文本，不再算几何。默认距离 mm 三位、角度两位。
class MeasureResultModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit MeasureResultModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void addDistanceMillimeters(double millimeters, double unitToDisplay = 1.0,
                                const QString& unitLabel = QStringLiteral("mm"));
    void addAngleDegrees(double degrees);
    void addEntry(MeasureKind kind, const QString& text);
    void clearResults();
    [[nodiscard]] int count() const {
        return m_rows.size();
    }
    [[nodiscard]] MeasureResultEntry at(int row) const;

    [[nodiscard]] static QString formatDistance(double millimeters, double unitToDisplay = 1.0,
                                                const QString& unitLabel = QStringLiteral("mm"));
    [[nodiscard]] static QString formatAngle(double degrees);

private:
    QVector<MeasureResultEntry> m_rows;
};

} // namespace steplens::ui
