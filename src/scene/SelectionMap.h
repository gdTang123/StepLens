#pragma once

// T08 节点和 AIS 对象的双向映射。点选得到的是实例对象，不是原型。
// T10 装配树和 T12 测量从这里取 NodeId，不在这里做树或测量。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "model/DocumentModel.h"

#include <AIS_InteractiveObject.hxx>

#include <optional>
#include <unordered_map>

namespace steplens::scene {

class SelectionMap {
public:
    void clear();
    void bind(model::NodeId id, const opencascade::handle<AIS_InteractiveObject>& object);
    [[nodiscard]] std::optional<model::NodeId>
    nodeOf(const opencascade::handle<AIS_InteractiveObject>& object) const;
    [[nodiscard]] opencascade::handle<AIS_InteractiveObject> objectOf(model::NodeId id) const;

private:
    // 键是 AIS 对象地址。handle 的引用计数是原子的，对象本身只在 GUI 线程使用。
    std::unordered_map<const AIS_InteractiveObject*, model::NodeId> m_objectToNode;
    std::unordered_map<model::NodeId, opencascade::handle<AIS_InteractiveObject>> m_nodeToObject;
};

} // namespace steplens::scene
