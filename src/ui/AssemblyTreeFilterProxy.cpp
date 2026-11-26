#include "ui/AssemblyTreeFilterProxy.h"

namespace steplens::ui {

AssemblyTreeFilterProxy::AssemblyTreeFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setRecursiveFilteringEnabled(false);
}

void AssemblyTreeFilterProxy::setNameFilter(const QString& text) {
    if (m_filter == text) {
        return;
    }
    beginFilterChange();
    m_filter = text;
    endFilterChange();
}

bool AssemblyTreeFilterProxy::selfOrDescendantMatches(const QModelIndex& sourceIndex) const {
    if (!sourceIndex.isValid() || sourceModel() == nullptr) {
        return false;
    }
    if (m_filter.isEmpty()) {
        return true;
    }
    const QString name = sourceModel()->data(sourceIndex, Qt::DisplayRole).toString();
    if (name.contains(m_filter, Qt::CaseInsensitive)) {
        return true;
    }
    const int rows = sourceModel()->rowCount(sourceIndex);
    for (int row = 0; row < rows; ++row) {
        if (selfOrDescendantMatches(sourceModel()->index(row, 0, sourceIndex))) {
            return true;
        }
    }
    return false;
}

bool AssemblyTreeFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (sourceModel() == nullptr) {
        return false;
    }
    if (m_filter.isEmpty()) {
        return true;
    }
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    return selfOrDescendantMatches(index);
}

} // namespace steplens::ui
