#pragma once

#include <QSortFilterProxyModel>

namespace steplens::ui {

// 名称过滤：自身或任一子孙命中时保留祖先路径；清空过滤词后恢复全树。
// 源模型必须是唯一的 AssemblyTreeModel，不复制节点。
class AssemblyTreeFilterProxy final : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit AssemblyTreeFilterProxy(QObject* parent = nullptr);

    void setNameFilter(const QString& text);
    [[nodiscard]] QString nameFilter() const {
        return m_filter;
    }

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    [[nodiscard]] bool selfOrDescendantMatches(const QModelIndex& sourceIndex) const;
    QString m_filter;
};

} // namespace steplens::ui
