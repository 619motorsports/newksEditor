#pragma once

#include "apex/scene/scene.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

enum class ReflectionSelectionMode {
    disabled,
    explicit_subtree,
    environment,
    scene,
    fallback,
};

struct RenderPlanOptions {
    // Camera position is used for each item's LOD interval and transparent order.
    // They do not imply a projection or a graphics API coordinate convention.
    apex::scene::Vector3 camera_position = {0.0F, 0.0F, 0.0F};
    bool include_shadows = true;
    bool include_reflections = true;
    bool isolated = false;
    apex::scene::NodeId isolated_node = apex::scene::invalid_node_id;
    // A pre-resolution layer can exclude whole subtrees without changing the
    // immutable scene. Isolation bypasses these exclusions, as it does in the
    // production WebGL preview.
    std::span<const apex::scene::NodeId> excluded_subtree_roots{};
    apex::scene::NodeId explicit_reflection_root = apex::scene::invalid_node_id;
    std::string workspace_kind;
    float bounds_radius = 0.0F;
};

struct RenderItem {
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
    std::uint32_t layer = 0;
    float distance = 0.0F;
    bool transparent = false;
    bool casts_shadows = false;
    std::string workspace_auxiliary;
    std::string workspace_file;
    // Includes the item node and its ancestors, in root-to-leaf order.
    std::vector<apex::scene::NodeId> reflection_ancestors;
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
