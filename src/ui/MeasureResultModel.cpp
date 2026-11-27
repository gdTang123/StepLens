#include "ui/MeasureResultModel.h"

#include <cmath>

namespace steplens::ui {

MeasureResultModel::MeasureResultModel(QObject* parent) : QAbstractListModel(parent) {}

int MeasureResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant MeasureResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
        return m_rows.at(index.row()).text;
    }
    return {};
}

QString MeasureResultModel::formatDistance(double millimeters, double unitToDisplay,
                                           const QString& unitLabel) {
    double scale = unitToDisplay;
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        scale = 1.0;
    }
    const double shown = millimeters * scale;
    if (!std::isfinite(shown)) {
        return QStringLiteral("无效距离");
    }
    return QStringLiteral("%1 %2").arg(shown, 0, 'f', 3).arg(unitLabel);
}

QString MeasureResultModel::formatAngle(double degrees) {
    if (!std::isfinite(degrees)) {
        return QStringLiteral("无效角度");
    }
    return QString::number(degrees, 'f', 2) + QStringLiteral("°");
}

void MeasureResultModel::addDistanceMillimeters(double millimeters, double unitToDisplay,
                                                const QString& unitLabel) {
    addEntry(MeasureKind::Distance, formatDistance(millimeters, unitToDisplay, unitLabel));
}

void MeasureResultModel::addAngleDegrees(double degrees) {
    addEntry(MeasureKind::Angle, formatAngle(degrees));
}

void MeasureResultModel::addEntry(MeasureKind kind, const QString& text) {
    const int row = m_rows.size();
    beginInsertRows({}, row, row);
    m_rows.push_back(MeasureResultEntry{kind, text});
    endInsertRows();
}

void MeasureResultModel::clearResults() {
    if (m_rows.isEmpty()) {
        return;
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

MeasureResultEntry MeasureResultModel::at(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return {};
    }
    return m_rows.at(row);
}

} // namespace steplens::ui
