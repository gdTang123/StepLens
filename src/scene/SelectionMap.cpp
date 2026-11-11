#include "scene/SelectionMap.h"

namespace steplens::scene {

void SelectionMap::clear() {
    m_objectToNode.clear();
    m_nodeToObject.clear();
}

void SelectionMap::bind(model::NodeId id, const opencascade::handle<AIS_InteractiveObject>& object) {
    if (object.IsNull()) {
        return;
    }
    const auto previous = m_nodeToObject.find(id);
    if (previous != m_nodeToObject.end() && !previous->second.IsNull()) {
        m_objectToNode.erase(previous->second.get());
    }
    m_nodeToObject[id] = object;
    m_objectToNode[object.get()] = id;
}

std::optional<model::NodeId>
SelectionMap::nodeOf(const opencascade::handle<AIS_InteractiveObject>& object) const {
    if (object.IsNull()) {
        return std::nullopt;
    }
    const auto found = m_objectToNode.find(object.get());
    if (found == m_objectToNode.end()) {
        return std::nullopt;
    }
    return found->second;
}

opencascade::handle<AIS_InteractiveObject> SelectionMap::objectOf(model::NodeId id) const {
    const auto found = m_nodeToObject.find(id);
    if (found == m_nodeToObject.end()) {
        return {};
    }
    return found->second;
}

} // namespace steplens::scene
