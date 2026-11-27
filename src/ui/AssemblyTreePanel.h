#pragma once

#include "ui/AssemblyTreeModel.h"

#include <QDockWidget>

class QLineEdit;
class QTreeView;

namespace steplens::ui {

class AssemblyTreeFilterProxy;

class AssemblyTreePanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit AssemblyTreePanel(QWidget* parent = nullptr);
    void setDocument(std::shared_ptr<const model::DocumentModel> document);
    void selectNode(model::NodeId id);
    [[nodiscard]] AssemblyTreeModel* treeModel() const {
        return m_model;
    }
    void setNameFilter(const QString& text);
    [[nodiscard]] QString nameFilter() const;

signals:
    void nodeActivated(steplens::model::NodeId id);
    void nodeContextMenu(steplens::model::NodeId id, QPoint globalPos);
    void visibilityChanged(steplens::model::NodeId id, bool visible);

private:
    [[nodiscard]] model::NodeId nodeIdOf(const QModelIndex& proxyIndex) const;

    QLineEdit* m_search = nullptr;
    QTreeView* m_view = nullptr;
    AssemblyTreeModel* m_model = nullptr;
    AssemblyTreeFilterProxy* m_proxy = nullptr;
};

} // namespace steplens::ui
