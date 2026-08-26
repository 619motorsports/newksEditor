#include "apex/render/render_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace apex::render {
namespace {

struct WalkState {
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    bool parent_active = true;
    bool excluded = false;
    std::string workspace_auxiliary;
    std::string workspace_file;
    std::vector<apex::scene::NodeId> ancestors;
};

[[nodiscard]] float safe_distance(const apex::scene::Vector3& point,
                                  const apex::scene::Vector3& camera) noexcept {
    const float dx = point[0] - camera[0];
    const float dy = point[1] - camera[1];
    const float dz = point[2] - camera[2];
    const float squared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(squared)) return 0.0F;
    return std::sqrt(std::max(0.0F, squared));
}

[[nodiscard]] bool lod_visible(double lod_in, double lod_out,
                               float distance) noexcept {
    // A finite authored LOD interval is required. A non-positive OUT means
    // open-ended, matching the current JS viewport condition.
    lod_in = std::isfinite(lod_in) ? lod_in : 0.0;
    lod_out = std::isfinite(lod_out) ? lod_out : 0.0;
    if (!std::isfinite(distance)) return false;
    return distance >= lod_in && (lod_out <= 0.0F || distance <= lod_out);
}

[[nodiscard]] bool item_less(const RenderItem& first,
                             const RenderItem& second) noexcept {
    if (first.transparent != second.transparent) return !first.transparent;
    if (first.layer != second.layer) return first.layer < second.layer;
    if (first.transparent && first.distance != second.distance) {
        return first.distance > second.distance;
    }
    return false;
}

void sort_capture_items(std::vector<RenderItem>& items) {
    std::stable_sort(items.begin(), items.end(), item_less);
}

[[nodiscard]] std::string effective_workspace_kind(
    const apex::scene::SceneSnapshot& scene,
    const RenderPlanOptions& options) {
    return options.workspace_kind.empty() ? scene.workspace_kind : options.workspace_kind;
}

[[nodiscard]] float effective_bounds_radius(
    const apex::scene::SceneSnapshot& scene,
    const RenderPlanOptions& options) noexcept {
    return options.bounds_radius > 0.0F ? options.bounds_radius : scene.bounds_radius;
}

[[nodiscard]] bool contains(const std::vector<apex::scene::NodeId>& ids,
                            apex::scene::NodeId value) noexcept {
    return std::find(ids.begin(), ids.end(), value) != ids.end();
}

}  // namespace

const char* reflection_selection_mode_name(ReflectionSelectionMode mode) noexcept {
    switch (mode) {
        case ReflectionSelectionMode::disabled:
            return "disabled";
        case ReflectionSelectionMode::explicit_subtree:
            return "explicit";
        case ReflectionSelectionMode::environment:
            return "environment";
        case ReflectionSelectionMode::scene:
            return "scene";
        case ReflectionSelectionMode::fallback:
            return "fallback";
    }
    return "fallback";
}

bool ksnet_mesh_lod_visible(const KsNetMeshLodRequest& request) noexcept {
    if (!request.in_pvs) return false;
    if (request.no_cull) return true;
    if (request.lod_in == 0.0F && request.lod_out == 0.0F) return true;
    if (!std::isfinite(request.camera_fov_degrees) ||
        !std::isfinite(request.lod_in) || !std::isfinite(request.lod_out) ||
        !std::isfinite(request.bounds_radius) || request.bounds_radius < 0.0F) {
        return false;
    }
    for (const float value : request.camera_position)
        if (!std::isfinite(value)) return false;
    for (const float value : request.bounds_center)
        if (!std::isfinite(value)) return false;

    const float scale = std::clamp(request.camera_fov_degrees * 0.0125F,
                                   0.0F, 1.0F);
    const float dx = request.camera_position[0] - request.bounds_center[0];
    const float dy = request.camera_position[1] - request.bounds_center[1];
    const float dz = request.camera_position[2] - request.bounds_center[2];
    const float distance_squared = dx * dx + dy * dy + dz * dz;
    const float scale_squared = scale * scale;
    const float scaled_distance_squared = distance_squared * scale_squared;
    const float near_squared = request.lod_in * request.lod_in;
    const float far = std::max(request.lod_out, request.bounds_radius);
    const float far_squared = far * far;
    if (!std::isfinite(scaled_distance_squared) ||
        !std::isfinite(near_squared) || !std::isfinite(far_squared)) {
        return false;
    }
    return !(scaled_distance_squared < near_squared ||
             far_squared < scaled_distance_squared);
}

RenderPlan build_render_plan(const apex::scene::SceneSnapshot& scene,
                             const RenderPlanOptions& options) {
    RenderPlan plan;
    plan.unsupported_effects.push_back({
        "backend_effects",
        "Shader execution, texture sampling, lighting, fog, and post-processing are not represented; this plan makes no pixel-fidelity claim.",
    });

    if (scene.root == apex::scene::invalid_node_id || scene.find_node(scene.root) == nullptr) {
        plan.reflection.mode = options.include_reflections ? ReflectionSelectionMode::fallback
                                                            : ReflectionSelectionMode::disabled;
        plan.reflection.reason = options.include_reflections ? "Scene has no visible geometry" : "Reflections disabled";
        return plan;
    }

    const bool isolated = scene.isolated || options.isolated;
    std::vector<WalkState> stack;
    std::vector<bool> excluded_roots(scene.nodes.size(), false);
    std::vector<bool> suppressed_roots(scene.nodes.size(), false);
    std::vector<std::int8_t> activity_overrides(scene.nodes.size(), -1);
    std::vector<const NodeRenderStateOverride*> node_state_overrides(scene.nodes.size(), nullptr);
    for (const apex::scene::NodeId id : options.excluded_subtree_roots) {
        if (id != apex::scene::invalid_node_id &&
            static_cast<std::size_t>(id) < excluded_roots.size()) {
            excluded_roots[static_cast<std::size_t>(id)] = true;
        }
    }
    for (const apex::scene::NodeId id : options.suppressed_subtree_roots) {
        if (id != apex::scene::invalid_node_id &&
            static_cast<std::size_t>(id) < suppressed_roots.size()) {
            suppressed_roots[static_cast<std::size_t>(id)] = true;
        }
    }
    for (const apex::scene::NodeActivityOverride& override_value : options.activity_overrides) {
        if (override_value.node != apex::scene::invalid_node_id &&
            static_cast<std::size_t>(override_value.node) < activity_overrides.size()) {
            activity_overrides[static_cast<std::size_t>(override_value.node)] =
                override_value.active ? 1 : 0;
        }
    }
    for (const NodeRenderStateOverride& override_value : options.node_state_overrides) {
        if (override_value.node != apex::scene::invalid_node_id &&
            static_cast<std::size_t>(override_value.node) < node_state_overrides.size()) {
            node_state_overrides[static_cast<std::size_t>(override_value.node)] = &override_value;
        }
    }
    stack.push_back({scene.root, true, false, {}, {}, {}});
    std::vector<bool> visited(scene.nodes.size(), false);
    while (!stack.empty()) {
        WalkState state = std::move(stack.back());
        stack.pop_back();
        const apex::scene::SceneNode* node = scene.find_node(state.node);
        if (node == nullptr) continue;
        const std::size_t node_index = static_cast<std::size_t>(node->id);
        if (node_index >= visited.size() || visited[node_index]) continue;
        visited[node_index] = true;

        const bool effective_active =
            activity_overrides[node_index] < 0 ? node->active : activity_overrides[node_index] != 0;
        const bool branch_active = state.parent_active && (options.show_hidden || effective_active);
        const bool excluded =
            state.excluded || excluded_roots[node_index] || suppressed_roots[node_index];
        const std::string workspace_auxiliary = node->workspace_auxiliary.empty()
                                                    ? state.workspace_auxiliary
                                                    : node->workspace_auxiliary;
        const std::string workspace_file = node->workspace_file.empty() ? state.workspace_file : node->workspace_file;
        std::vector<apex::scene::NodeId> ancestors = state.ancestors;
        ancestors.push_back(node->id);

        const bool geometry = node->kind == apex::scene::NodeKind::mesh ||
                              node->kind == apex::scene::NodeKind::skinned_mesh;
        const bool isolated_item = isolated && node->id == options.isolated_node;
        const float distance = safe_distance(node->bounds_center, options.camera_position);
        const NodeRenderStateOverride* state_override = node_state_overrides[node_index];
        const double lod_in = state_override != nullptr && state_override->lod_in.has_value()
                                  ? *state_override->lod_in
                                  : static_cast<double>(node->lod_in);
        const double lod_out = state_override != nullptr && state_override->lod_out.has_value()
                                   ? *state_override->lod_out
                                   : static_cast<double>(node->lod_out);
        const bool normally_visible =
            !isolated && !excluded && branch_active &&
            (options.show_hidden || (node->visible && node->renderable)) &&
            lod_visible(lod_in, lod_out, distance);
        const bool isolated_visible = isolated_item && lod_visible(lod_in, lod_out, distance);
        if (geometry && (isolated_visible || normally_visible)) {
            const apex::scene::SceneMaterial* material = scene.find_material(node->material);
            const bool material_alpha_blend = material != nullptr &&
                                              material->blend_mode == apex::scene::BlendMode::alpha_blend;
            const bool transparency_overridden =
                state_override != nullptr && state_override->is_transparent.has_value();
            const bool transparent = transparency_overridden
                                         ? *state_override->is_transparent
                                         : node->transparent || material_alpha_blend;
            RenderItem item{
                node->id,
                node->material,
                state_override != nullptr && state_override->layer.has_value()
                    ? *state_override->layer
                    : static_cast<double>(node->layer),
                distance,
                transparent,
                transparency_overridden,
                state_override != nullptr && state_override->cast_shadows.has_value()
                    ? *state_override->cast_shadows
                    : node->cast_shadows,
                workspace_auxiliary,
                workspace_file,
                ancestors,
            };
            plan.items.push_back(item);
            if (item.casts_shadows && options.include_shadows) plan.shadow_casters.push_back(item);
        }

        // Reverse push preserves the source child order with a LIFO walk and
        // avoids recursive stack growth for deeply nested untrusted scenes.
        for (auto child = node->children.rbegin(); child != node->children.rend(); ++child) {
            if (scene.find_node(*child) == nullptr) continue;
            stack.push_back({*child, branch_active, excluded, workspace_auxiliary,
                             workspace_file, ancestors});
        }
    }

    sort_capture_items(plan.items);
    for (const RenderItem& item : plan.items) {
        if (item.transparent) {
            plan.transparent_items.push_back(item);
        } else {
            plan.opaque_items.push_back(item);
        }
    }

    if (!options.include_reflections || isolated) {
        plan.reflection.mode = ReflectionSelectionMode::disabled;
        plan.reflection.reason = isolated ? "Isolated mesh preview" : "Reflections disabled";
        return plan;
    }

    const apex::scene::NodeId explicit_root = options.explicit_reflection_root;
    if (explicit_root != apex::scene::invalid_node_id) {
        plan.reflection.mode = ReflectionSelectionMode::explicit_subtree;
        plan.reflection.root = explicit_root;
        if (const auto* root = scene.find_node(explicit_root); root != nullptr) {
            plan.reflection.root_name = root->name.empty() ? "Selected subtree" : root->name;
        } else {
            plan.reflection.root_name = "Selected subtree";
        }
        for (const RenderItem& item : plan.items) {
            if (contains(item.reflection_ancestors, explicit_root)) plan.reflection.items.push_back(item);
        }
        sort_capture_items(plan.reflection.items);
        if (plan.reflection.items.empty()) {
            plan.reflection.reason = "Selected reflection subtree has no visible geometry";
        }
        return plan;
    }

    for (const RenderItem& item : plan.items) {
        if (item.workspace_auxiliary == "reflectionEnvironment") plan.reflection.items.push_back(item);
    }
    if (!plan.reflection.items.empty()) {
        plan.reflection.mode = ReflectionSelectionMode::environment;
        plan.reflection.root_name = plan.reflection.items.front().workspace_file.empty()
                                        ? "Reflection environment"
                                        : plan.reflection.items.front().workspace_file;
        sort_capture_items(plan.reflection.items);
        return plan;
    }

    if (effective_workspace_kind(scene, options) == "track" || effective_bounds_radius(scene, options) > 20.0F) {
        plan.reflection.mode = ReflectionSelectionMode::scene;
        plan.reflection.root_name = "Whole scene";
        plan.reflection.items = plan.items;
        if (plan.reflection.items.empty()) plan.reflection.reason = "Scene has no visible geometry";
        return plan;
    }

    plan.reflection.mode = ReflectionSelectionMode::fallback;
    plan.reflection.reason = "No separate environment geometry";
    return plan;
}

}  // namespace apex::render
