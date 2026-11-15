#include "ui/AssemblyTreePanel.h"

#include <QItemSelectionModel>
#include <QTreeView>

namespace steplens::ui {

AssemblyTreePanel::AssemblyTreePanel(QWidget* parent) : QDockWidget(tr("装配树"), parent) {
    m_model = new AssemblyTreeModel(this);
    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setHeaderHidden(true);
    m_view->setUniformRowHeights(true);
    setWidget(m_view);
    setObjectName(QStringLiteral("assemblyTreePanel"));
    setMinimumWidth(220);
    connect(m_view, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        emit nodeActivated(static_cast<model::NodeId>(index.internalId()));
    });
    connect(m_model, &AssemblyTreeModel::visibilityChanged, this, &AssemblyTreePanel::visibilityChanged);
}

void AssemblyTreePanel::setDocument(std::shared_ptr<const model::DocumentModel> document) {
    m_model->setDocument(std::move(document));
    m_view->collapseAll();
    if (m_model->rowCount() > 0)
        m_view->expand(m_model->index(0, 0));
}

void AssemblyTreePanel::selectNode(model::NodeId id) {
    const QModelIndex item = m_model->indexOf(id);
    if (!item.isValid())
        return;
    QModelIndex ancestor = item.parent();
    while (ancestor.isValid()) {
        m_view->expand(ancestor);
        ancestor = ancestor.parent();
    }
    m_view->selectionModel()->setCurrentIndex(item, QItemSelectionModel::ClearAndSelect);
    m_view->scrollTo(item, QAbstractItemView::PositionAtCenter);
}

} // namespace steplens::ui
