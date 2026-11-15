#pragma once

#include "ui/AssemblyTreeModel.h"

#include <QDockWidget>

class QTreeView;

namespace steplens::ui {

class AssemblyTreePanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit AssemblyTreePanel(QWidget* parent = nullptr);
    void setDocument(std::shared_ptr<const model::DocumentModel> document);
    void selectNode(model::NodeId id);
    [[nodiscard]] AssemblyTreeModel* treeModel() const {
        return m_model;
    }

signals:
    void nodeActivated(steplens::model::NodeId id);
    void visibilityChanged(steplens::model::NodeId id, bool visible);

private:
    QTreeView* m_view = nullptr;
    AssemblyTreeModel* m_model = nullptr;
};

} // namespace steplens::ui
