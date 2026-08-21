#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/scene/scene.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace apex::render {

// Limits for the bounded source-node to SceneSnapshot identity adapter. The
// adapter is deliberately independent of the KN5 parser limits because final
// workspace models can be assembled in memory after parsing.
struct Kn5SceneNodeMapLimits {
    std::size_t max_depth = 1'000'000;
    std::size_t max_nodes = 1'000'000;
    // Bounds the number of visited nodes plus nodes waiting on the work stack.
    std::size_t max_work_items = 1'000'000;
    std::size_t max_name_bytes = 1U << 20;
};

struct Kn5SceneNodeMapDiagnostic {
    std::string code;
    std::string message;
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    bool limit_exceeded = false;
};

struct Kn5SceneNodeMapResult {
    // Pointers retain source pre-order. The source Kn5Node tree must remain
    // unchanged and alive while callers use this vector.
    std::vector<const apex::formats::Kn5Node*> source_nodes;
    Kn5SceneNodeMapDiagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return diagnostic.code.empty(); }
};

// Iteratively maps a final (possibly workspace-merged) KN5 tree to the dense
// pre-order SceneSnapshot IDs. It validates source depth/count/name/kind and
// does not rely on a recursive traversal helper.
[[nodiscard]] Kn5SceneNodeMapResult map_kn5_scene_nodes(
    const apex::formats::Kn5Node& root,
    const apex::scene::SceneSnapshot& scene,
    const Kn5SceneNodeMapLimits& limits = {});

}  // namespace apex::render
