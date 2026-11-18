#include "scene/SelectionMap.h"

#include <algorithm>

namespace steplens::scene {

void SelectionMap::clear() {
    m_objectToNode.clear();
    m_nodeToObjects.clear();
}

void SelectionMap::bind(model::NodeId id, const opencascade::handle<AIS_InteractiveObject>& object) {
    if (object.IsNull()) {
        return;
    }
    auto& objects = m_nodeToObjects[id];
    const auto existing = m_objectToNode.find(object.get());
    if (existing != m_objectToNode.end() && existing->second == id) {
        return;
    }
    if (existing != m_objectToNode.end()) {
        auto previous = m_nodeToObjects.find(existing->second);
        if (previous != m_nodeToObjects.end()) {
            std::erase_if(previous->second,
                          [&](const auto& current) { return current.get() == object.get(); });
        }
    }
    if (std::find_if(objects.begin(), objects.end(),
                     [&](const auto& current) { return current.get() == object.get(); }) == objects.end()) {
        objects.push_back(object);
    }
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
    const auto found = m_nodeToObjects.find(id);
    if (found == m_nodeToObjects.end() || found->second.empty()) {
        return {};
    }
    return found->second.front();
}

std::vector<opencascade::handle<AIS_InteractiveObject>> SelectionMap::objectsOf(model::NodeId id) const {
    const auto found = m_nodeToObjects.find(id);
    return found == m_nodeToObjects.end() ? std::vector<opencascade::handle<AIS_InteractiveObject>>{}
                                          : found->second;
}

} // namespace steplens::scene
