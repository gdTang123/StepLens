#include "ui/AssemblyTreeModel.h"

#include "model/DocumentRelease.h"

#include <QString>

#include <algorithm>
#include <limits>

namespace steplens::ui {

AssemblyTreeModel::AssemblyTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void AssemblyTreeModel::setDocument(std::shared_ptr<const model::DocumentModel> document) {
    beginResetModel();
    model::handoffDocument(std::move(m_model));
    m_model = std::move(document);
    m_visible.assign(m_model ? m_model->nodeCount() : 0, 1);
    m_rows.assign(m_model ? m_model->nodeCount() : 0, 0);
    if (m_model) {
        for (std::size_t parentId = 0; parentId < m_model->nodeCount(); ++parentId) {
            const auto& children = m_model->node(static_cast<model::NodeId>(parentId)).children;
            for (std::size_t row = 0; row < children.size(); ++row)
                m_rows[children[row]] = static_cast<int>(row);
        }
    }
    endResetModel();
}

bool AssemblyTreeModel::validIndex(const QModelIndex& item) const {
    return m_model && item.isValid() && item.column() == 0 && item.internalId() < m_model->nodeCount();
}

QModelIndex AssemblyTreeModel::index(int row, int column, const QModelIndex& parentItem) const {
    if (!m_model || row < 0 || column != 0 || columnCount() <= column) {
        return {};
    }
    const std::vector<model::NodeId>* children = nullptr;
    if (!parentItem.isValid()) {
        if (m_model->nodeCount() == 0 || row != 0)
            return {};
        return createIndex(0, 0, static_cast<quintptr>(0));
    }
    if (!validIndex(parentItem))
        return {};
    children = &m_model->node(static_cast<model::NodeId>(parentItem.internalId())).children;
    if (static_cast<std::size_t>(row) >= children->size())
        return {};
    const model::NodeId id = (*children)[static_cast<std::size_t>(row)];
    return createIndex(row, 0, static_cast<quintptr>(id));
}

QModelIndex AssemblyTreeModel::parent(const QModelIndex& child) const {
    if (!validIndex(child))
        return {};
    const auto id = static_cast<model::NodeId>(child.internalId());
    if (id == 0)
        return {};
    const auto parentId = m_model->node(id).parent;
    return createIndex(m_rows[parentId], 0, static_cast<quintptr>(parentId));
}

int AssemblyTreeModel::rowCount(const QModelIndex& parentItem) const {
    if (!m_model)
        return 0;
    if (!parentItem.isValid())
        return m_model->nodeCount() == 0 ? 0 : 1;
    if (!validIndex(parentItem) || parentItem.column() != 0)
        return 0;
    return static_cast<int>(
        m_model->node(static_cast<model::NodeId>(parentItem.internalId())).children.size());
}

QVariant AssemblyTreeModel::data(const QModelIndex& item, int role) const {
    if (!validIndex(item))
        return {};
    const auto id = static_cast<model::NodeId>(item.internalId());
    const auto& node = m_model->node(id);
    if (role == Qt::DisplayRole) {
        if (!node.name.empty())
            return QString::fromStdU16String(node.name);
        return tr("未命名-%1").arg(static_cast<qulonglong>(id) + 1);
    }
    if (role == Qt::CheckStateRole)
        return m_visible[id] ? Qt::Checked : Qt::Unchecked;
    return {};
}

bool AssemblyTreeModel::setData(const QModelIndex& item, const QVariant& value, int role) {
    if (!validIndex(item) || role != Qt::CheckStateRole)
        return false;
    const auto state = static_cast<Qt::CheckState>(value.toInt());
    if (state != Qt::Checked && state != Qt::Unchecked)
        return false;
    const auto id = static_cast<model::NodeId>(item.internalId());
    const bool visible = state == Qt::Checked;
    if (m_visible[id] == static_cast<std::uint8_t>(visible))
        return true;
    setSubtreeVisible(id, visible);
    std::vector<model::NodeId> pending{id};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        emit visibilityChanged(current, effectiveVisible(current));
        const auto& children = m_model->node(current).children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return true;
}

void AssemblyTreeModel::setSubtreeVisible(model::NodeId id, bool visible) {
    m_visible[id] = static_cast<std::uint8_t>(visible);
    const auto& node = m_model->node(id);
    emit dataChanged(indexOf(id), indexOf(id), {Qt::CheckStateRole});
    for (const auto child : node.children)
        setSubtreeVisible(child, visible);
}

bool AssemblyTreeModel::effectiveVisible(model::NodeId id) const {
    while (true) {
        if (!m_visible[id])
            return false;
        if (id == 0)
            return true;
        id = m_model->node(id).parent;
    }
}

Qt::ItemFlags AssemblyTreeModel::flags(const QModelIndex& item) const {
    if (!validIndex(item))
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
}

QModelIndex AssemblyTreeModel::indexOf(model::NodeId id) const {
    if (!m_model || id >= m_model->nodeCount())
        return {};
    if (id == 0)
        return createIndex(0, 0, static_cast<quintptr>(id));
    return createIndex(m_rows[id], 0, static_cast<quintptr>(id));
}

} // namespace steplens::ui
