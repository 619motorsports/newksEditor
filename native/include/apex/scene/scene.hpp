#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace apex::scene {

using NodeId = std::uint32_t;
using MaterialId = std::uint32_t;

inline constexpr NodeId invalid_node_id = std::numeric_limits<NodeId>::max();
inline constexpr MaterialId invalid_material_id = std::numeric_limits<MaterialId>::max();

using Matrix4 = std::array<float, 16>;
using Vector3 = std::array<float, 3>;

inline constexpr Matrix4 identity_matrix = {
    1.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 0.0F, 1.0F,
};

/** Scene kinds that can be submitted by the backend-neutral render planner. */
enum class NodeKind {
    node,
    mesh,
    skinned_mesh,
};

/** The serialized KN5 blend modes needed to classify transparent geometry. */
enum class BlendMode : std::uint8_t {
    opaque = 0,
    alpha_blend = 1,
    alpha_to_coverage = 2,
};

struct SceneMaterial {
    std::string name;
    std::string shader;
    BlendMode blend_mode = BlendMode::opaque;
};

/**
 * A renderer-independent scene node.
 *
 * bounds_center is already in world space. The native planner intentionally
 * does not interpret vertex buffers or shader data; the loader that creates a
 * snapshot is responsible for transforming its bounds. Children are ordered
 * and are visited in that order, matching the WebGL scene walk.
 */
struct SceneNode {
    NodeId id = invalid_node_id;
    NodeId parent = invalid_node_id;
    std::string name;
    NodeKind kind = NodeKind::node;
    bool active = true;
    bool visible = true;
    bool renderable = true;
    bool transparent = false;
    bool cast_shadows = true;
    std::uint32_t layer = 0;
    float lod_in = 0.0F;
    float lod_out = 0.0F;
    MaterialId material = invalid_material_id;
    Vector3 bounds_center = {0.0F, 0.0F, 0.0F};
    float bounds_radius = 0.0F;
    Matrix4 transform = identity_matrix;
    // Workspace metadata is inherited by descendants when empty, just as
    // workspaceAuxiliary/workspaceFile are inherited by the JS scene walk.
    std::string workspace_auxiliary;
    std::string workspace_file;
    std::vector<NodeId> children;
};

/**
 * Immutable-at-render-time scene data shared by all graphics backends.
 *
 * The vectors are public to make adapters from existing parsed models simple;
 * add_node/add_material should be preferred when constructing a valid tree.
 */
struct SceneSnapshot {
    NodeId root = invalid_node_id;
    std::vector<SceneNode> nodes;
    std::vector<SceneMaterial> materials;
    std::string workspace_kind;
    float bounds_radius = 0.0F;
    bool isolated = false;

    [[nodiscard]] const SceneNode* find_node(NodeId id) const noexcept;
    [[nodiscard]] const SceneMaterial* find_material(MaterialId id) const noexcept;

    /** Add a node and connect it to parent, preserving child insertion order. */
    [[nodiscard]] NodeId add_node(SceneNode node,
                                   NodeId parent = invalid_node_id);
    [[nodiscard]] MaterialId add_material(SceneMaterial material);
};

}  // namespace apex::scene
