#include "apex/render/render_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
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
    double squared = 0.0;
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        if (!std::isfinite(point[axis]) || !std::isfinite(camera[axis]))
            return std::numeric_limits<float>::infinity();
        const double delta = static_cast<double>(point[axis]) -
                             static_cast<double>(camera[axis]);
        squared += delta * delta;
    }
    const double distance = std::sqrt(squared);
    if (!std::isfinite(distance))
        return std::numeric_limits<float>::infinity();
    if (distance > static_cast<double>(std::numeric_limits<float>::max()))
        return std::numeric_limits<float>::max();
    return static_cast<float>(distance);
}

[[nodiscard]] std::optional<apex::scene::Vector3> safe_transform_point(
    const apex::scene::Matrix4& matrix,
    const apex::scene::Vector3& point) noexcept {
    apex::scene::Vector3 output{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        const double value =
            static_cast<double>(matrix[row]) * point[0] +
            static_cast<double>(matrix[4U + row]) * point[1] +
            static_cast<double>(matrix[8U + row]) * point[2] +
            static_cast<double>(matrix[12U + row]);
        if (!std::isfinite(value) ||
            value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
            return std::nullopt;
        }
        output[row] = static_cast<float>(value);
    }
    return output;
}

[[nodiscard]] bool lod_visible(double lod_in, double lod_out,
                               float distance) noexcept {
    // A finite authored LOD interval is required. A non-positive OUT means
    // open-ended, matching the current JS viewport condition.
    if (!std::isfinite(lod_in) || !std::isfinite(lod_out) ||
        !std::isfinite(distance))
        return false;
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
    std::vector<const KsNetMeshLodNodeState*> ksnet_node_states;
    if (options.ksnet_mesh_lod.has_value()) {
        ksnet_node_states.resize(scene.nodes.size(), nullptr);
        for (const KsNetMeshLodNodeState& state : options.ksnet_mesh_lod->node_states) {
            if (state.node != apex::scene::invalid_node_id &&
                static_cast<std::size_t>(state.node) < ksnet_node_states.size()) {
                ksnet_node_states[static_cast<std::size_t>(state.node)] = &state;
            }
        }
    }
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
        const std::optional<apex::scene::Vector3> aabb_center =
            node->local_aabb_center.has_value()
                ? safe_transform_point(node->transform,
                                       *node->local_aabb_center)
                : std::nullopt;
        const float distance = safe_distance(
            aabb_center.value_or(node->bounds_center),
            options.camera_position);
        const NodeRenderStateOverride* state_override = node_state_overrides[node_index];
        const double lod_in = state_override != nullptr && state_override->lod_in.has_value()
                                  ? *state_override->lod_in
                                  : static_cast<double>(node->lod_in);
        const double lod_out = state_override != nullptr && state_override->lod_out.has_value()
                                   ? *state_override->lod_out
                                   : static_cast<double>(node->lod_out);
        const bool lod_is_visible = [&]() noexcept {
            if (!std::isfinite(distance)) return false;
            if (options.defer_camera_mesh_filter) return true;
            if (!options.ksnet_mesh_lod.has_value())
                return lod_visible(lod_in, lod_out, distance);
            const KsNetMeshLodNodeState* lod_state = ksnet_node_states[node_index];
            KsNetMeshLodRequest request;
            request.camera_position = options.camera_position;
            request.bounds_center = node->bounds_center;
            request.camera_fov_degrees = options.ksnet_mesh_lod->camera_fov_degrees;
            request.lod_in = static_cast<float>(lod_in);
            request.lod_out = static_cast<float>(lod_out);
            request.bounds_radius = node->bounds_radius;
            request.in_pvs = lod_state != nullptr
                                 ? lod_state->in_pvs
                                 : options.ksnet_mesh_lod->default_in_pvs;
            request.no_cull = lod_state != nullptr
                                  ? lod_state->no_cull
                                  : options.ksnet_mesh_lod->default_no_cull;
            return ksnet_mesh_lod_visible(request);
        }();
        const bool normally_visible =
            !isolated && !excluded && branch_active &&
            (options.show_hidden || (node->visible && node->renderable)) &&
            lod_is_visible;
        const bool isolated_visible = isolated_item && lod_is_visible;
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
                std::nullopt,
            };
            if (options.defer_camera_mesh_filter &&
                node->local_bounds_source != apex::scene::LocalBoundsSource::unavailable &&
                lod_in >= -static_cast<double>(std::numeric_limits<float>::max()) &&
                lod_in <= static_cast<double>(std::numeric_limits<float>::max()) &&
                lod_out >= -static_cast<double>(std::numeric_limits<float>::max()) &&
                lod_out <= static_cast<double>(std::numeric_limits<float>::max()) &&
                item.layer >= 0.0 &&
                item.layer <= static_cast<double>(std::numeric_limits<std::uint32_t>::max()) &&
                std::floor(item.layer) == item.layer) {
                CameraMeshRenderable filter;
                filter.bounding_sphere = {
                    node->local_bounds_center, node->local_bounds_radius};
                filter.lod_in = static_cast<float>(lod_in);
                filter.lod_out = static_cast<float>(lod_out);
                filter.layer = static_cast<std::uint32_t>(item.layer);
                filter.cast_shadows = item.casts_shadows;
                filter.visible = true;
                filter.transparent = item.transparent;
                // Ordinary KN5 Renderable construction sets both fields to
                // false. Runtime mutation is not inferred from KN5 data.
                filter.no_cull = false;
                filter.is_static = false;
                item.camera_mesh_filter = filter;
            }
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
