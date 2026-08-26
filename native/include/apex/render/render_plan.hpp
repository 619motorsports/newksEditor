#pragma once

#include "apex/render/camera_mesh_filter.hpp"
#include "apex/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

inline constexpr std::size_t invalid_render_item_source_order =
    std::numeric_limits<std::size_t>::max();

// A render-time projection of one CSP MESH_ADJUSTMENT. Optional fields retain
// the difference between an explicit false/zero and an absent override.
struct NodeRenderStateOverride {
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    std::optional<bool> is_transparent;
    std::optional<double> layer;
    std::optional<double> lod_in;
    std::optional<double> lod_out;
    std::optional<bool> cast_shadows;
};

enum class ReflectionSelectionMode {
    disabled,
    explicit_subtree,
    environment,
    scene,
    fallback,
};

/** Inputs for the recovered ksNet PvsProcessor per-mesh LOD rule.
 *
 * The loader has not recovered runtime MESH_FLAG_NO_CULL population. Callers
 * must supply that state explicitly and must not infer it from KN5 fields.
 */
struct KsNetMeshLodRequest {
    apex::scene::Vector3 camera_position{};
    apex::scene::Vector3 bounds_center{};
    float camera_fov_degrees = 45.0F;
    float lod_in = 0.0F;
    float lod_out = 0.0F;
    float bounds_radius = 0.0F;
    bool in_pvs = true;
    bool no_cull = false;
};

/**
 * Apply the recovered inclusive FOV-scaled ksNet per-mesh LOD predicate.
 * Native NO_CULL and zero-limit bypasses do not read the remaining inputs.
 * Callers that require finite retained values must validate those inputs.
 */
[[nodiscard]] bool ksnet_mesh_lod_visible(
    const KsNetMeshLodRequest& request) noexcept;

/** Explicit per-node inputs for the recovered ksNet PVS-array LOD rule. */
struct KsNetMeshLodNodeState {
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    bool in_pvs = true;
    bool no_cull = false;
};

/**
 * Opt-in render-plan inputs for the recovered ksNet PVS-array LOD rule.
 *
 * The defaults describe ordinary loaded KN5 meshes. They are explicit here
 * because PVS and NO_CULL are not serialized in the SceneNode snapshot.
 * Per-node entries replace both defaults for their referenced mesh.
 * Direct planner callers must supply valid, unique mesh IDs. The stock-scene
 * facade validates this requirement before it builds a plan.
 */
struct KsNetMeshLodOptions {
    constexpr explicit KsNetMeshLodOptions(float camera_fov) noexcept
        : camera_fov_degrees(camera_fov) {}

    // Required explicitly because the installed Camera default is 60 degrees,
    // while the port's workspace cameras commonly start at 45 degrees.
    float camera_fov_degrees;
    bool default_in_pvs = true;
    bool default_no_cull = false;
    std::span<const KsNetMeshLodNodeState> node_states{};
};

struct RenderPlanOptions {
    // Camera position is used for each item's LOD interval and transparent order.
    // They do not imply a projection or a graphics API coordinate convention.
    apex::scene::Vector3 camera_position = {0.0F, 0.0F, 0.0F};
    bool include_shadows = true;
    bool include_reflections = true;
    bool isolated = false;
    apex::scene::NodeId isolated_node = apex::scene::invalid_node_id;
    // Show-hidden bypasses authored active/visible/renderable state and
    // preview activity overrides. It does not bypass hard subtree exclusions
    // or mesh LOD intervals. Isolation takes precedence over show-hidden.
    bool show_hidden = false;
    // Cockpit/rim preview resolvers can replace selected active flags without
    // mutating the source scene. The stock-scene facade rejects invalid or
    // duplicate IDs before it calls the backend-neutral planner.
    std::span<const apex::scene::NodeActivityOverride> activity_overrides{};
    // CSP mesh-state overrides are applied per node after authored KN5 state.
    // The stock-scene facade rejects invalid, duplicate, or non-finite values.
    std::span<const NodeRenderStateOverride> node_state_overrides{};
    // A pre-resolution layer can exclude whole subtrees without changing the
    // immutable scene. Workspace LOD uses this hard exclusion. Show-hidden
    // does not bypass it; isolation does.
    std::span<const apex::scene::NodeId> excluded_subtree_roots{};
    // Driver cockpit-hidden roots are suppressed after show-hidden, matching
    // the production preview. Isolation still selects the exact mesh.
    std::span<const apex::scene::NodeId> suppressed_subtree_roots{};
    apex::scene::NodeId explicit_reflection_root = apex::scene::invalid_node_id;
    std::string workspace_kind;
    float bounds_radius = 0.0F;
    // Absent preserves the existing WebGL-compatible center-distance rule.
    // Present applies the recovered ksNet PVS-array distance/LOD predicate.
    // This does not claim full CameraMeshFilter pass or frustum parity.
    std::optional<KsNetMeshLodOptions> ksnet_mesh_lod;
    // Retain meshes across their authored LOD interval. This mode also keeps
    // active, renderable Shadowgen candidates that isVisible excludes from
    // color. A live caller must evaluate each pass descriptor every frame.
    bool defer_camera_mesh_filter = false;
};

struct RenderItem {
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
    double layer = 0.0;
    float distance = 0.0F;
    bool transparent = false;
    bool transparency_overridden = false;
    bool casts_shadows = false;
    std::string workspace_auxiliary;
    std::string workspace_file;
    // Includes the item node and its ancestors, in root-to-leaf order.
    std::vector<apex::scene::NodeId> reflection_ancestors;
    // Present only when defer_camera_mesh_filter requested an exact KN5 local
    // sphere. An absent value requires a conservative visible fallback.
    std::optional<CameraMeshRenderable> camera_mesh_filter;
    // Stable depth-first scene occurrence before any color-pass sort. The
    // packet bridge uses this token to recover the native shadow traversal.
    std::size_t source_order = invalid_render_item_source_order;
};

struct UnsupportedEffect {
    std::string code;
    std::string description;
};

struct ReflectionSelection {
    ReflectionSelectionMode mode = ReflectionSelectionMode::fallback;
    std::vector<RenderItem> items;
    apex::scene::NodeId root = apex::scene::invalid_node_id;
    std::string root_name;
    std::string reason;
};

struct RenderPlan {
    // items is sorted exactly like the current viewport draw list:
    // opaque first, then transparent; layer ascending; transparent ties are
    // back-to-front. stable ordering retains scene traversal order for ties.
    std::vector<RenderItem> items;
    std::vector<RenderItem> opaque_items;
    std::vector<RenderItem> transparent_items;
    // CameraMeshFilter mode retains active, renderable shadow casters that
    // the color pass excludes only because isVisible is false. These items
    // prepare geometry and resources but never enter color submission.
    std::vector<RenderItem> shadow_only_items;
    // Shadow submission intentionally retains scene traversal order. The
    // shadow pass does not use the color-pass transparency ordering.
    std::vector<RenderItem> shadow_casters;
    ReflectionSelection reflection;
    // This is an explicit contract boundary: this plan contains no shader,
    // texture, lighting, or post-processing implementation and makes no
    // pixel-fidelity claim.
    std::vector<UnsupportedEffect> unsupported_effects;
};

[[nodiscard]] RenderPlan build_render_plan(
    const apex::scene::SceneSnapshot& scene,
    const RenderPlanOptions& options = {});

[[nodiscard]] const char* reflection_selection_mode_name(
    ReflectionSelectionMode mode) noexcept;

}  // namespace apex::render
