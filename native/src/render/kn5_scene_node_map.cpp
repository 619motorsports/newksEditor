#include "apex/render/kn5_scene_node_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace apex::render {
namespace {

using Node = apex::formats::Kn5Node;
using NodeId = apex::scene::NodeId;

[[nodiscard]] bool finite_matrix(const apex::formats::Kn5Matrix4& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

void fail(Kn5SceneNodeMapResult& result, const char* code, const char* message,
          NodeId node = apex::scene::invalid_node_id, bool limit_exceeded = false) {
    result.source_nodes.clear();
    result.diagnostic = {code, message, node, limit_exceeded};
}

[[nodiscard]] bool kind_matches(const Node& source,
                                apex::scene::NodeKind scene_kind) noexcept {
    switch (source.type) {
    case 1U: return source.kind == "node" && scene_kind == apex::scene::NodeKind::node;
    case 2U: return source.kind == "mesh" && scene_kind == apex::scene::NodeKind::mesh;
    case 3U: return source.kind == "skinnedMesh" && scene_kind == apex::scene::NodeKind::skinned_mesh;
    default: return false;
    }
}

}  // namespace

Kn5SceneNodeMapResult map_kn5_scene_nodes(
    const apex::formats::Kn5Node& root,
    const apex::scene::SceneSnapshot& scene,
    const Kn5SceneNodeMapLimits& limits) {
    Kn5SceneNodeMapResult result;
    if (limits.max_nodes == 0U) {
        fail(result, "NODE_LIMIT", "KN5 node input exceeds draw-packet limits", {}, true);
        return result;
    }
    if (limits.max_work_items == 0U) {
        fail(result, "WORK_LIMIT", "KN5 traversal work exceeds draw-packet limits", {}, true);
        return result;
    }
    if (scene.nodes.size() > limits.max_nodes ||
        scene.nodes.size() >= static_cast<std::size_t>(apex::scene::invalid_node_id)) {
        fail(result, "NODE_LIMIT", "scene node input exceeds draw-packet limits", {}, true);
        return result;
    }

    struct WorkItem {
        const Node* node = nullptr;
        std::size_t depth = 0;
    };
    std::vector<WorkItem> work;
    work.reserve(std::min<std::size_t>(limits.max_work_items, 1024U));
    work.push_back({&root, 0U});
    result.source_nodes.reserve(std::min(scene.nodes.size(), limits.max_nodes));

    while (!work.empty()) {
        const WorkItem item = work.back();
        work.pop_back();
        if (item.depth > limits.max_depth) {
            fail(result, "DEPTH_LIMIT", "KN5 node hierarchy exceeds draw-packet limits", {}, true);
            return result;
        }
        if (result.source_nodes.size() >= limits.max_nodes) {
            fail(result, "NODE_LIMIT", "KN5 node input exceeds draw-packet limits", {}, true);
            return result;
        }
        if (item.node == nullptr) {
            fail(result, "SCENE_MODEL_MISMATCH", "KN5 traversal encountered a null source node");
            return result;
        }
        if (!finite_matrix(item.node->transform) || item.node->name.size() > limits.max_name_bytes) {
            fail(result, !finite_matrix(item.node->transform) ? "NON_FINITE_MATRIX" : "STRING_LIMIT",
                 "KN5 node transform or name is invalid", {}, false);
            return result;
        }
        result.source_nodes.push_back(item.node);

        // The work bound covers visited nodes plus pending stack entries. Do
        // this check before pushing children so untrusted fan-out cannot grow
        // the traversal stack beyond the configured budget.
        if (result.source_nodes.size() > limits.max_work_items ||
            work.size() > limits.max_work_items - result.source_nodes.size() ||
            item.node->children.size() >
                limits.max_work_items - result.source_nodes.size() - work.size()) {
            fail(result, "WORK_LIMIT", "KN5 traversal work exceeds draw-packet limits", {}, true);
            return result;
        }
        if (!item.node->children.empty() && item.depth == limits.max_depth) {
            fail(result, "DEPTH_LIMIT", "KN5 node hierarchy exceeds draw-packet limits", {}, true);
            return result;
        }
        for (auto child = item.node->children.rbegin(); child != item.node->children.rend(); ++child)
            work.push_back({&*child, item.depth + 1U});
    }

    if (result.source_nodes.size() != scene.nodes.size()) {
        fail(result, "SCENE_MODEL_MISMATCH", "scene node IDs do not correspond to KN5 pre-order nodes");
        return result;
    }
    if (!scene.nodes.empty() && scene.root != 0U) {
        fail(result, "INVALID_NODE_ID", "scene root must be the first source-ordered node");
        return result;
    }
    for (std::size_t index = 0; index < scene.nodes.size(); ++index) {
        const auto& scene_node = scene.nodes[index];
        if (scene_node.id != static_cast<NodeId>(index)) {
            fail(result, "INVALID_NODE_ID", "scene node IDs must be dense and source ordered",
                 scene_node.id);
            return result;
        }
        const Node& source = *result.source_nodes[index];
        if (!kind_matches(source, scene_node.kind) || scene_node.name != source.name) {
            fail(result, "SCENE_MODEL_IDENTITY",
                 "scene node name or kind does not match the source KN5 node",
                 scene_node.id);
            return result;
        }
    }
    return result;
}

}  // namespace apex::render
