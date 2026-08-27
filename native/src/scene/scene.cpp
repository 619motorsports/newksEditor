#include "apex/scene/scene.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace apex::scene {

const SceneNode* SceneSnapshot::find_node(NodeId id) const noexcept {
    if (id == invalid_node_id || static_cast<std::size_t>(id) >= nodes.size()) return nullptr;
    const SceneNode& node = nodes[static_cast<std::size_t>(id)];
    return node.id == id ? &node : nullptr;
}

const SceneMaterial* SceneSnapshot::find_material(MaterialId id) const noexcept {
    if (id == invalid_material_id || static_cast<std::size_t>(id) >= materials.size()) return nullptr;
    return &materials[static_cast<std::size_t>(id)];
}

NodeId SceneSnapshot::add_node(SceneNode node, NodeId parent) {
    const NodeId id = static_cast<NodeId>(nodes.size());
    if (static_cast<std::size_t>(id) != nodes.size()) {
        throw std::overflow_error("scene node count exceeds NodeId range");
    }
    if (parent != invalid_node_id && find_node(parent) == nullptr) {
        throw std::invalid_argument("scene node parent does not exist");
    }
    if (parent == invalid_node_id && root != invalid_node_id) {
        throw std::invalid_argument("scene snapshot already has a root");
    }
    node.id = id;
    node.parent = parent;
    nodes.push_back(std::move(node));
    if (parent == invalid_node_id) {
        root = id;
    } else {
        nodes[static_cast<std::size_t>(parent)].children.push_back(id);
    }
    return id;
}

MaterialId SceneSnapshot::add_material(SceneMaterial material) {
    const MaterialId id = static_cast<MaterialId>(materials.size());
    if (static_cast<std::size_t>(id) != materials.size()) {
        throw std::overflow_error("scene material count exceeds MaterialId range");
    }
    materials.push_back(std::move(material));
    return id;
}

}  // namespace apex::scene
