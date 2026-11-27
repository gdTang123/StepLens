#include "ui/AssemblyTreePanel.h"

#include "ui/AssemblyTreeFilterProxy.h"

#include <QItemSelectionModel>
#include <QLineEdit>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace steplens::ui {

AssemblyTreePanel::AssemblyTreePanel(QWidget* parent) : QDockWidget(tr("装配树"), parent) {
    m_model = new AssemblyTreeModel(this);
    m_proxy = new AssemblyTreeFilterProxy(this);
    m_proxy->setSourceModel(m_model);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_search = new QLineEdit(root);
    m_search->setObjectName(QStringLiteral("assemblyTreeSearch"));
    m_search->setPlaceholderText(tr("按名称搜索"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(tr("装配树名称搜索"));
    m_search->setToolTip(tr("按名称过滤装配树；命中节点会保留祖先路径"));
    layout->addWidget(m_search);

    m_view = new QTreeView(root);
    m_view->setObjectName(QStringLiteral("assemblyTreeView"));
    m_view->setModel(m_proxy);
    m_view->setHeaderHidden(true);
    m_view->setUniformRowHeights(true);
    m_view->setAccessibleName(tr("装配树"));
    layout->addWidget(m_view, 1);
    setWidget(root);
    setObjectName(QStringLiteral("assemblyTreePanel"));
    setMinimumWidth(220);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_search, &QLineEdit::textChanged, this, &AssemblyTreePanel::setNameFilter);
    connect(m_view, &QTreeView::clicked, this,
            [this](const QModelIndex& proxyIndex) { emit nodeActivated(nodeIdOf(proxyIndex)); });
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex proxyIndex = m_view->indexAt(pos);
        if (!proxyIndex.isValid()) {
            return;
        }
        emit nodeContextMenu(nodeIdOf(proxyIndex), m_view->viewport()->mapToGlobal(pos));
    });
    connect(m_model, &AssemblyTreeModel::visibilityChanged, this, &AssemblyTreePanel::visibilityChanged);
}

model::NodeId AssemblyTreePanel::nodeIdOf(const QModelIndex& proxyIndex) const {
    const QModelIndex source = m_proxy->mapToSource(proxyIndex);
    return static_cast<model::NodeId>(source.internalId());
}

void AssemblyTreePanel::setNameFilter(const QString& text) {
    m_proxy->setNameFilter(text);
    if (text.isEmpty()) {
        m_view->collapseAll();
        if (m_proxy->rowCount() > 0) {
            m_view->expand(m_proxy->index(0, 0));
        }
        return;
    }
    m_view->expandAll();
}

QString AssemblyTreePanel::nameFilter() const {
    return m_proxy->nameFilter();
}

void AssemblyTreePanel::setDocument(std::shared_ptr<const model::DocumentModel> document) {
    m_search->clear();
    m_proxy->setNameFilter({});
    m_model->setDocument(std::move(document));
    m_view->collapseAll();
    if (m_proxy->rowCount() > 0) {
        m_view->expand(m_proxy->index(0, 0));
    }
}

void AssemblyTreePanel::selectNode(model::NodeId id) {
    const QModelIndex source = m_model->indexOf(id);
    if (!source.isValid()) {
        return;
    }
    const QModelIndex item = m_proxy->mapFromSource(source);
    if (!item.isValid()) {
        return;
    }
    QModelIndex ancestor = item.parent();
    while (ancestor.isValid()) {
        m_view->expand(ancestor);
        ancestor = ancestor.parent();
    }
    m_view->selectionModel()->setCurrentIndex(item, QItemSelectionModel::ClearAndSelect);
    m_view->scrollTo(item, QAbstractItemView::PositionAtCenter);
}

} // namespace steplens::ui
