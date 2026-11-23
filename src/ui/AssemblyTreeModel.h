#pragma once

#include "model/DocumentModel.h"

#include <QAbstractItemModel>

#include <memory>
#include <vector>

namespace steplens::ui {

class AssemblyTreeModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit AssemblyTreeModel(QObject* parent = nullptr);
    void setDocument(std::shared_ptr<const model::DocumentModel> document);
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& = {}) const override {
        return 1;
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QModelIndex indexOf(model::NodeId id) const;
    [[nodiscard]] QString displayName(model::NodeId id) const;
    void showOnly(model::NodeId id);
    void showAll();
    [[nodiscard]] std::shared_ptr<const model::DocumentModel> document() const {
        return m_model;
    }

signals:
    void visibilityChanged(steplens::model::NodeId id, bool visible);

private:
    [[nodiscard]] bool validIndex(const QModelIndex& index) const;
    [[nodiscard]] bool effectiveVisible(model::NodeId id) const;
    void setSubtreeVisible(model::NodeId id, bool visible);
    std::shared_ptr<const model::DocumentModel> m_model;
    std::vector<std::uint8_t> m_visible;
    std::vector<int> m_rows;
};

} // namespace steplens::ui
