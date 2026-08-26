#include "apex/render/stock_scene_execution.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

[[nodiscard]] StockSceneExecutionResult fail(StaticSceneResourceStatus status,
                                              std::string code,
                                              std::string message) {
    StockSceneExecutionResult result;
    result.status = status;
    result.diagnostic = {std::move(code), std::move(message)};
    return result;
}

bool add_bytes(std::uint64_t amount, std::uint64_t& total,
               std::uint64_t limit) noexcept {
    if (amount > limit - std::min(total, limit)) return false;
    total += amount;
    return true;
}

bool add_count(std::size_t count, std::size_t element_size,
               std::uint64_t& total, std::uint64_t limit) noexcept {
    if (element_size != 0U &&
        count > std::numeric_limits<std::uint64_t>::max() / element_size)
        return false;
    return add_bytes(static_cast<std::uint64_t>(count) * element_size, total, limit);
}

bool add_product(std::initializer_list<std::size_t> factors, std::size_t element_size,
                 std::uint64_t& total, std::uint64_t limit) noexcept {
    std::uint64_t product = element_size;
    for (const std::size_t factor : factors) {
        if (factor != 0U && product > std::numeric_limits<std::uint64_t>::max() / factor)
            return false;
        product *= factor;
    }
    return add_bytes(product, total, limit);
}

bool merge_damage_activity_overrides(
    const apex::scene::SceneSnapshot& scene,
    std::span<const apex::scene::NodeActivityOverride> caller,
    std::span<const apex::scene::NodeActivityOverride> damage,
    std::vector<apex::scene::NodeActivityOverride>& merged,
    Diagnostic& diagnostic) {
    const std::size_t node_count = scene.nodes.size();
    if (caller.size() > node_count || damage.size() > node_count ||
        caller.size() > node_count - std::min(damage.size(), node_count)) {
        diagnostic = {"stock_scene_damage_activity_limit",
                      "Merged damage activity overrides exceed the scene node limit"};
        return false;
    }

    // Keep one bounded origin byte per scene node. This makes conflicts
    // deterministic and avoids an unbounded associative container for input
    // that originated in a parsed model or caller-provided preview state.
    std::vector<std::uint8_t> origins(node_count, 0U);
    merged.reserve(caller.size() + damage.size());
    const auto append = [&](std::span<const apex::scene::NodeActivityOverride> values,
                            bool from_damage) {
        for (const apex::scene::NodeActivityOverride& value : values) {
            if (value.node == apex::scene::invalid_node_id ||
                static_cast<std::size_t>(value.node) >= node_count) {
                diagnostic = {from_damage ? "stock_scene_damage_activity_invalid"
                                          : "stock_scene_activity_override_invalid",
                              from_damage
                                  ? "Damage preview activity references an unknown scene node"
                                  : "An activity override references an unknown scene node"};
                return false;
            }
            const std::size_t index = static_cast<std::size_t>(value.node);
            if (origins[index] != 0U) {
                if (from_damage && origins[index] == 1U) {
                    diagnostic = {"stock_scene_damage_activity_conflict",
                                  "Caller activity state conflicts with native damage activity"};
                } else if (from_damage) {
                    diagnostic = {"stock_scene_damage_activity_duplicate",
                                  "Damage preview produced duplicate activity overrides"};
                } else {
                    diagnostic = {"stock_scene_activity_override_duplicate",
                                  "Activity overrides contain a duplicate scene node"};
                }
                return false;
            }
            origins[index] = from_damage ? 2U : 1U;
            merged.push_back(value);
        }
        return true;
    };
    return append(caller, false) && append(damage, true);
}

bool preplan_within_limit(const apex::scene::SceneSnapshot& scene,
                          bool include_ksnet_node_states,
                          bool include_camera_filter_catalog,
                          std::uint64_t limit,
                          Diagnostic& diagnostic) {
    if (limit == 0U) {
        diagnostic = {"stock_scene_plan_preflight_limit",
                      "The stock-scene plan preparation budget must be nonzero"};
        return false;
    }
    std::uint64_t bytes = sizeof(RenderPlan);
    if (bytes > limit) {
        diagnostic = {"stock_scene_plan_preflight_limit",
                      "The render-plan record exceeds the plan preparation budget"};
        return false;
    }
    std::size_t max_depth = 1U;
    std::size_t max_workspace_bytes = 0U;
    std::size_t child_edges = 0U;
    for (const apex::scene::SceneNode& node : scene.nodes) {
        if (node.workspace_auxiliary.size() >
            std::numeric_limits<std::size_t>::max() - node.workspace_file.size()) {
            diagnostic = {"stock_scene_plan_preflight_limit",
                          "Scene workspace metadata size overflows the plan budget"};
            return false;
        }
        max_workspace_bytes = std::max(
            max_workspace_bytes, node.workspace_auxiliary.size() + node.workspace_file.size());
        if (node.children.size() > std::numeric_limits<std::size_t>::max() - child_edges) {
            diagnostic = {"stock_scene_plan_preflight_limit",
                          "Scene child-edge count overflows the plan budget"};
            return false;
        }
        child_edges += node.children.size();

    }

    const std::size_t node_count = scene.nodes.size();
    if (node_count >= static_cast<std::size_t>(apex::scene::invalid_node_id)) {
        diagnostic = {"stock_scene_topology_invalid",
                      "Scene node count exceeds the bounded node ID range"};
        return false;
    }
    if ((node_count == 0U && scene.root != apex::scene::invalid_node_id) ||
        (node_count != 0U && scene.find_node(scene.root) == nullptr)) {
        diagnostic = {"stock_scene_topology_invalid",
                      "Scene root does not reference a valid node"};
        return false;
    }
    for (std::size_t index = 0U; index < node_count; ++index) {
        const apex::scene::SceneNode& node = scene.nodes[index];
        if (node.id != static_cast<apex::scene::NodeId>(index)) {
            diagnostic = {"stock_scene_topology_invalid",
                          "Scene node IDs must match their bounded snapshot indices"};
            return false;
        }
        if (node.parent != apex::scene::invalid_node_id &&
            scene.find_node(node.parent) == nullptr) {
            diagnostic = {"stock_scene_topology_invalid",
                          "Scene node parent references an unknown node"};
            return false;
        }
        for (const apex::scene::NodeId child_id : node.children) {
            const apex::scene::SceneNode* child = scene.find_node(child_id);
            if (child == nullptr || child->parent != node.id) {
                diagnostic = {"stock_scene_topology_invalid",
                              "Scene child edges are malformed or inconsistent with parent IDs"};
                return false;
            }
        }
    }

    // These vectors provide an O(nodes + edges) Kahn traversal. Charge their
    // storage before allocating them so a deep hostile chain is rejected
    // without first materializing unbounded topology state.
    if (!add_count(node_count, sizeof(std::uint32_t) + sizeof(std::size_t) +
                               sizeof(apex::scene::NodeId), bytes, limit)) {
        diagnostic = {"stock_scene_plan_preflight_limit",
                      "Scene topology-depth state exceeds the plan preparation budget"};
        return false;
    }
    std::vector<std::uint32_t> indegree(node_count, 0U);
    std::vector<std::size_t> depth(node_count, 1U);
    std::vector<apex::scene::NodeId> queue;
    queue.reserve(node_count);
    for (const apex::scene::SceneNode& node : scene.nodes) {
        for (const apex::scene::NodeId child_id : node.children) {
            const std::size_t child_index = static_cast<std::size_t>(child_id);
            if (indegree[child_index] == std::numeric_limits<std::uint32_t>::max()) {
                diagnostic = {"stock_scene_topology_invalid",
                              "Scene child indegree exceeds the bounded topology range"};
                return false;
            }
            ++indegree[child_index];
            if (indegree[child_index] > 1U) {
                diagnostic = {"stock_scene_topology_invalid",
                              "Scene topology contains a node with multiple parent edges"};
                return false;
            }
        }
    }
    for (std::size_t index = 0U; index < node_count; ++index) {
        const apex::scene::SceneNode& node = scene.nodes[index];
        const bool is_root = node.id == scene.root;
        if ((is_root && (node.parent != apex::scene::invalid_node_id ||
                         indegree[index] != 0U)) ||
            (!is_root && (node.parent == apex::scene::invalid_node_id ||
                          indegree[index] != 1U))) {
            diagnostic = {"stock_scene_topology_invalid",
                          "Scene parent IDs are inconsistent with child edges"};
            return false;
        }
    }
    for (std::size_t index = 0U; index < node_count; ++index)
        if (indegree[index] == 0U)
            queue.push_back(static_cast<apex::scene::NodeId>(index));
    std::size_t processed = 0U;
    for (std::size_t read = 0U; read < queue.size(); ++read) {
        const std::size_t index = static_cast<std::size_t>(queue[read]);
        ++processed;
        max_depth = std::max(max_depth, depth[index]);
        for (const apex::scene::NodeId child_id : scene.nodes[index].children) {
            const std::size_t child_index = static_cast<std::size_t>(child_id);
            if (depth[index] == std::numeric_limits<std::size_t>::max()) {
                diagnostic = {"stock_scene_plan_preflight_limit",
                              "Scene topology depth overflows the plan budget"};
                return false;
            }
            depth[child_index] = std::max(depth[child_index], depth[index] + 1U);
            --indegree[child_index];
            if (indegree[child_index] == 0U)
                queue.push_back(static_cast<apex::scene::NodeId>(child_index));
        }
    }
    if (processed != node_count) {
        diagnostic = {"stock_scene_topology_invalid",
                      "Scene topology contains a parent cycle"};
        return false;
    }

    // build_render_plan allocates visited, hard-exclusion, suppression, and
    // activity-override arrays. It also allocates node-state pointer arrays,
    // including the opt-in ksNet array, plus a LIFO WalkState stack. The stack capacity
    // can approach the node count on a wide tree. Ancestor vectors are charged
    // separately with the authored maximum depth.
    const std::size_t state_pointer_bytes =
        sizeof(const NodeRenderStateOverride*) +
        (include_ksnet_node_states ? sizeof(const KsNetMeshLodNodeState*) : 0U);
    if (!add_count(node_count, 3U * sizeof(bool) + sizeof(std::int8_t) +
                                   state_pointer_bytes, bytes,
                   limit) ||
        !add_count(node_count, sizeof(std::uint32_t) + sizeof(bool) +
                               2U * sizeof(std::string) + sizeof(std::vector<std::uint32_t>),
                   bytes, limit) ||
        !add_count(child_edges, sizeof(std::uint32_t), bytes, limit) ||
        !add_product({node_count, max_depth}, sizeof(std::uint32_t), bytes, limit) ||
        !add_product({node_count, max_depth}, sizeof(std::uint32_t), bytes, limit)) {
        diagnostic = {"stock_scene_plan_preflight_limit",
                      "Scene traversal state exceeds the stock-scene plan preparation budget"};
        return false;
    }

    // A render item may be retained in items, one opaque/transparent list,
    // shadow-only items, shadow casters, and the reflection selection. Charge six copies,
    // including their ancestor IDs and inherited workspace strings.
    if (!add_product({node_count, 6U}, sizeof(RenderItem), bytes, limit) ||
        !add_product({node_count, max_depth, 6U}, sizeof(apex::scene::NodeId), bytes, limit) ||
        !add_product({node_count, max_workspace_bytes, 6U}, 1U, bytes, limit) ||
        (include_camera_filter_catalog &&
         !add_count(node_count,
                    sizeof(std::optional<CameraMeshRenderable>) +
                        2U * sizeof(std::uint8_t),
                    bytes, limit)) ||
        !add_count(5U, sizeof(UnsupportedEffect), bytes, limit) ||
        !add_bytes(15U * 1024U, bytes, limit)) {
        diagnostic = {"stock_scene_plan_preflight_limit",
                      "Retained render-plan items or staged evidence exceed the preparation budget"};
        return false;
    }
    return true;
}

bool validate_render_options(const apex::scene::SceneSnapshot& scene,
                             const RenderPlanOptions& options, Diagnostic& diagnostic) {
    const std::size_t node_count = scene.nodes.size();
    const bool isolated = scene.isolated || options.isolated;
    if (isolated && (options.isolated_node == apex::scene::invalid_node_id ||
                     static_cast<std::size_t>(options.isolated_node) >= node_count)) {
        diagnostic = {"stock_scene_isolation_invalid",
                      "The isolated node does not reference a scene node"};
        return false;
    }

    std::vector<bool> seen(node_count, false);
    const auto reset_seen = [&]() { std::fill(seen.begin(), seen.end(), false); };
    const auto validate_roots = [&](std::span<const apex::scene::NodeId> roots,
                                    const char* invalid_code, const char* duplicate_code,
                                    const char* label) {
        if (roots.size() > node_count) {
            diagnostic = {"stock_scene_render_option_limit",
                          std::string(label) + " count exceeds the scene node count"};
            return false;
        }
        reset_seen();
        for (const apex::scene::NodeId id : roots) {
            if (id == apex::scene::invalid_node_id || static_cast<std::size_t>(id) >= node_count) {
                diagnostic = {invalid_code,
                              std::string(label) + " references an unknown scene node"};
                return false;
            }
            const std::size_t index = static_cast<std::size_t>(id);
            if (seen[index]) {
                diagnostic = {duplicate_code,
                              std::string(label) + " contains a duplicate scene node"};
                return false;
            }
            seen[index] = true;
        }
        return true;
    };

    if (options.activity_overrides.size() > node_count) {
        diagnostic = {"stock_scene_render_option_limit",
                      "Activity-override count exceeds the scene node count"};
        return false;
    }
    reset_seen();
    for (const apex::scene::NodeActivityOverride& override_value : options.activity_overrides) {
        if (override_value.node == apex::scene::invalid_node_id ||
            static_cast<std::size_t>(override_value.node) >= node_count) {
            diagnostic = {"stock_scene_activity_override_invalid",
                          "An activity override references an unknown scene node"};
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(override_value.node);
        if (seen[index]) {
            diagnostic = {"stock_scene_activity_override_duplicate",
                          "Activity overrides contain a duplicate scene node"};
            return false;
        }
        seen[index] = true;
    }
    if (options.node_state_overrides.size() > node_count) {
        diagnostic = {"stock_scene_render_option_limit",
                      "Node-state override count exceeds the scene node count"};
        return false;
    }
    reset_seen();
    for (const NodeRenderStateOverride& override_value : options.node_state_overrides) {
        if (override_value.node == apex::scene::invalid_node_id ||
            static_cast<std::size_t>(override_value.node) >= node_count) {
            diagnostic = {"stock_scene_node_state_override_invalid",
                          "A node-state override references an unknown scene node"};
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(override_value.node);
        if (seen[index]) {
            diagnostic = {"stock_scene_node_state_override_duplicate",
                          "Node-state overrides contain a duplicate scene node"};
            return false;
        }
        if ((override_value.layer.has_value() && !std::isfinite(*override_value.layer)) ||
            (override_value.lod_in.has_value() && !std::isfinite(*override_value.lod_in)) ||
            (override_value.lod_out.has_value() && !std::isfinite(*override_value.lod_out))) {
            diagnostic = {"stock_scene_node_state_override_non_finite",
                          "Node-state override numeric values must be finite"};
            return false;
        }
        seen[index] = true;
    }
    if (options.defer_camera_mesh_filter) {
        if (options.ksnet_mesh_lod.has_value()) {
            diagnostic = {
                "stock_scene_camera_mesh_filter_mode_conflict",
                "Deferred camera mesh filtering cannot use the PVS-array LOD mode"};
            return false;
        }
        for (const apex::scene::SceneNode& node : scene.nodes) {
            if ((node.kind != apex::scene::NodeKind::mesh &&
                 node.kind != apex::scene::NodeKind::skinned_mesh) ||
                node.local_bounds_source ==
                    apex::scene::LocalBoundsSource::unavailable) {
                continue;
            }
            if (!std::isfinite(node.local_bounds_radius) ||
                node.local_bounds_radius < 0.0F ||
                std::any_of(node.local_bounds_center.begin(),
                            node.local_bounds_center.end(),
                            [](float value) { return !std::isfinite(value); })) {
                diagnostic = {
                    "stock_scene_camera_mesh_bounds_invalid",
                    "Deferred camera mesh filtering requires finite KN5 local bounds"};
                return false;
            }
        }
        const double max_float =
            static_cast<double>(std::numeric_limits<float>::max());
        for (const NodeRenderStateOverride& override_value :
             options.node_state_overrides) {
            if ((override_value.lod_in.has_value() &&
                 std::abs(*override_value.lod_in) > max_float) ||
                (override_value.lod_out.has_value() &&
                 std::abs(*override_value.lod_out) > max_float)) {
                diagnostic = {
                    "stock_scene_camera_mesh_lod_out_of_range",
                    "Deferred camera mesh LOD overrides must fit in finite float values"};
                return false;
            }
        }
    }
    if (options.ksnet_mesh_lod.has_value()) {
        const KsNetMeshLodOptions& lod = *options.ksnet_mesh_lod;
        if (!std::isfinite(lod.camera_fov_degrees)) {
            diagnostic = {"stock_scene_ksnet_lod_fov_non_finite",
                          "The ksNet mesh-LOD camera FOV must be finite"};
            return false;
        }
        for (const float coordinate : options.camera_position) {
            if (!std::isfinite(coordinate)) {
                diagnostic = {"stock_scene_ksnet_lod_camera_non_finite",
                              "The ksNet mesh-LOD camera position must be finite"};
                return false;
            }
        }
        if (lod.node_states.size() > node_count) {
            diagnostic = {"stock_scene_render_option_limit",
                          "ksNet mesh-LOD node-state count exceeds the scene node count"};
            return false;
        }
        reset_seen();
        for (const KsNetMeshLodNodeState& state : lod.node_states) {
            const apex::scene::SceneNode* node = scene.find_node(state.node);
            if (node == nullptr ||
                (node->kind != apex::scene::NodeKind::mesh &&
                 node->kind != apex::scene::NodeKind::skinned_mesh)) {
                diagnostic = {"stock_scene_ksnet_lod_state_invalid",
                              "A ksNet mesh-LOD state references an unknown or non-mesh scene node"};
                return false;
            }
            const std::size_t index = static_cast<std::size_t>(state.node);
            if (seen[index]) {
                diagnostic = {"stock_scene_ksnet_lod_state_duplicate",
                              "ksNet mesh-LOD states contain a duplicate scene node"};
                return false;
            }
            seen[index] = true;
        }
        for (const apex::scene::SceneNode& node : scene.nodes) {
            if (node.kind != apex::scene::NodeKind::mesh &&
                node.kind != apex::scene::NodeKind::skinned_mesh)
                continue;
            if (!std::isfinite(node.lod_in) || !std::isfinite(node.lod_out) ||
                !std::isfinite(node.bounds_radius) || node.bounds_radius < 0.0F ||
                std::any_of(node.bounds_center.begin(), node.bounds_center.end(),
                            [](float value) { return !std::isfinite(value); })) {
                diagnostic = {"stock_scene_ksnet_lod_mesh_non_finite",
                              "ksNet mesh-LOD inputs require finite mesh bounds and LOD limits"};
                return false;
            }
        }
        const double max_float = static_cast<double>(std::numeric_limits<float>::max());
        for (const NodeRenderStateOverride& override_value : options.node_state_overrides) {
            if ((override_value.lod_in.has_value() &&
                 std::abs(*override_value.lod_in) > max_float) ||
                (override_value.lod_out.has_value() &&
                 std::abs(*override_value.lod_out) > max_float)) {
                diagnostic = {"stock_scene_ksnet_lod_override_out_of_range",
                              "ksNet mesh-LOD overrides must fit in finite float values"};
                return false;
            }
        }
    }
    if (!validate_roots(options.excluded_subtree_roots, "stock_scene_exclusion_invalid",
                        "stock_scene_exclusion_duplicate", "Subtree exclusions")) {
        return false;
    }
    return validate_roots(options.suppressed_subtree_roots, "stock_scene_suppression_invalid",
                          "stock_scene_suppression_duplicate", "Subtree suppressions");
}

bool plan_within_limit(const RenderPlan& plan, std::uint64_t limit) noexcept {
    std::uint64_t bytes = sizeof(RenderPlan);
    if (bytes > limit) return false;
    const auto count_bytes = [](std::size_t count, std::size_t element_size,
                                std::uint64_t& output) noexcept {
        if (element_size != 0U &&
            count > std::numeric_limits<std::uint64_t>::max() / element_size)
            return false;
        output = static_cast<std::uint64_t>(count) * element_size;
        return true;
    };
    const auto add_item = [&](const RenderItem& item) {
        std::uint64_t ancestor_bytes = 0U;
        if (!count_bytes(item.reflection_ancestors.size(), sizeof(apex::scene::NodeId),
                         ancestor_bytes))
            return false;
        if (item.workspace_auxiliary.size() >
            std::numeric_limits<std::uint64_t>::max() - item.workspace_file.size())
            return false;
        const std::uint64_t string_bytes =
            static_cast<std::uint64_t>(item.workspace_auxiliary.size()) +
            static_cast<std::uint64_t>(item.workspace_file.size());
        if (!add_bytes(sizeof(RenderItem), bytes, limit) ||
            !add_bytes(ancestor_bytes, bytes, limit) ||
            !add_bytes(string_bytes, bytes, limit))
            return false;
        return true;
    };
    for (const RenderItem& item : plan.items)
        if (!add_item(item)) return false;
    for (const RenderItem& item : plan.opaque_items)
        if (!add_item(item)) return false;
    for (const RenderItem& item : plan.transparent_items)
        if (!add_item(item)) return false;
    for (const RenderItem& item : plan.shadow_only_items)
        if (!add_item(item)) return false;
    for (const RenderItem& item : plan.shadow_casters)
        if (!add_item(item)) return false;
    for (const RenderItem& item : plan.reflection.items)
        if (!add_item(item)) return false;
    if (!add_bytes(plan.reflection.root_name.size(), bytes, limit) ||
        !add_bytes(plan.reflection.reason.size(), bytes, limit))
        return false;
    for (const UnsupportedEffect& effect : plan.unsupported_effects)
        if (!add_bytes(effect.code.size(), bytes, limit) ||
            !add_bytes(effect.description.size(), bytes, limit))
            return false;
    return true;
}

}  // namespace

StockSceneExecutionResult prepare_stock_scene_execution(
    Device& device, const StockSceneExecutionRequest& request) {
    try {
        if (request.model == nullptr || request.scene == nullptr)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_scene_request_missing",
                        "A KN5 model and scene snapshot are required");
        if (request.scene->nodes.size() > request.limits.max_scene_nodes)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_scene_node_limit",
                        "Scene node count exceeds the stock-scene preparation limit");
        if (request.scene->materials.size() > request.limits.max_scene_materials)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_scene_material_limit",
                        "Scene material count exceeds the stock-scene preparation limit");
        if (request.model->materials.size() > request.limits.max_model_materials)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_scene_model_material_limit",
                        "KN5 material count exceeds the stock-scene preparation limit");
        if (request.model->textures.size() > request.limits.max_model_textures)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_scene_texture_limit",
                        "KN5 texture count exceeds the stock-scene preparation limit");
        if (request.packets.wireframe != request.wireframe)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_scene_wireframe_mismatch",
                        "Draw-packet and pipeline wireframe options must agree");

        StockSceneExecutionResult result;
        std::vector<apex::scene::NodeActivityOverride> merged_damage_activity;
        RenderPlanOptions render_options = request.render;
        std::span<const MaterialBindingOverrides> material_overrides =
            request.overrides_by_material;
        if (request.evaluate_damage_preview) {
            DamagePreviewRequest damage_request;
            damage_request.model = request.model;
            damage_request.scene = request.scene;
            damage_request.broken_visible = request.damage_broken_visible;
            damage_request.base_material_overrides = request.overrides_by_material;
            DamagePreviewResult damage_result =
                resolve_damage_preview(damage_request, request.limits.damage);
            result.damage_preview = std::move(damage_result);
            if (!result.damage_preview->ok()) {
                const DamagePreviewDiagnostic* error = nullptr;
                for (const DamagePreviewDiagnostic& diagnostic :
                     result.damage_preview->diagnostics) {
                    if (diagnostic.severity == DamagePreviewDiagnosticSeverity::error) {
                        error = &diagnostic;
                        break;
                    }
                }
                result.status = result.damage_preview->limit_exceeded
                                    ? StaticSceneResourceStatus::invalid_request
                                    : StaticSceneResourceStatus::unsupported;
                result.diagnostic = error != nullptr
                                        ? Diagnostic{error->code, error->message}
                                        : Diagnostic{
                                              result.damage_preview->limit_exceeded
                                                  ? "stock_scene_damage_preview_limit"
                                                  : "stock_scene_damage_preview_unsupported",
                                              result.damage_preview->limit_exceeded
                                                  ? "Damage-preview preparation exceeded its configured limit"
                                                  : "Damage-preview resolution is unsupported"};
                return result;
            }

            Diagnostic activity_diagnostic;
            if (!merge_damage_activity_overrides(
                    *request.scene, request.render.activity_overrides,
                    result.damage_preview->activity_overrides, merged_damage_activity,
                    activity_diagnostic)) {
                result.status = StaticSceneResourceStatus::invalid_request;
                result.diagnostic = std::move(activity_diagnostic);
                return result;
            }
            render_options.activity_overrides = merged_damage_activity;
            material_overrides = result.damage_preview->material_overrides;
        }

        Diagnostic preflight_diagnostic;
        if (!preplan_within_limit(*request.scene,
                                  render_options.ksnet_mesh_lod.has_value(),
                                  render_options.defer_camera_mesh_filter,
                                  request.limits.max_plan_bytes,
                                  preflight_diagnostic)) {
            result.status = StaticSceneResourceStatus::invalid_request;
            result.diagnostic = std::move(preflight_diagnostic);
            return result;
        }
        if (!validate_render_options(*request.scene, render_options, preflight_diagnostic)) {
            result.status = StaticSceneResourceStatus::invalid_request;
            result.diagnostic = std::move(preflight_diagnostic);
            return result;
        }
        result.render_plan = build_render_plan(*request.scene, render_options);
        result.render_plan.unsupported_effects.push_back({
            "stock_scene_snapshot_staged",
            "Workspace LOD/FOV, unresolved CSP shader and resource changes, and surface overlays remain outside this main-color handoff.",
        });
        if (result.render_plan.items.size() > request.limits.max_plan_items ||
            result.render_plan.shadow_only_items.size() >
                request.limits.max_plan_items - result.render_plan.items.size() ||
            !plan_within_limit(result.render_plan, request.limits.max_plan_bytes)) {
            result.status = StaticSceneResourceStatus::invalid_request;
            result.diagnostic = {"stock_scene_plan_limit",
                                 "Render-plan items or retained evidence exceed the stock-scene preparation limit"};
            return result;
        }

        DrawPacketBuildResult packet_result = build_draw_packets(
            *request.model, *request.scene, result.render_plan, request.packets,
            request.limits.packets);
        result.packet_diagnostics = std::move(packet_result.diagnostics);
        result.packet_unsupported_effects = std::move(packet_result.unsupported_effects);
        if (!packet_result.supported) {
            result.status = StaticSceneResourceStatus::invalid_request;
            if (!result.packet_diagnostics.empty()) {
                result.diagnostic = {result.packet_diagnostics.front().code,
                                     result.packet_diagnostics.front().message};
            } else {
                result.diagnostic = {"stock_scene_packets_unsupported",
                                     "Draw-packet construction rejected the scene"};
            }
            return result;
        }
        result.shadow_packet_order =
            std::move(packet_result.shadow_packet_order);

        StockMaterialExecutionRequest material_request;
        material_request.model = request.model;
        material_request.scene = request.scene;
        material_request.packets = packet_result.packets;
        material_request.shader_modules = request.shader_modules;
        material_request.overrides_by_material = material_overrides;
        material_request.targets = request.targets;
        material_request.wireframe = request.wireframe;
        material_request.directional_shadow_receiver =
            request.directional_shadow_receiver;
        material_request.texture_authority = request.texture_authority;
        material_request.limits = request.limits.material;
        StockMaterialExecutionResult material_result =
            prepare_stock_material_execution(device, material_request);
        result.status = material_result.status;
        result.diagnostic = std::move(material_result.diagnostic);
        result.resources = std::move(material_result.resources);
        return result;
    } catch (const std::bad_alloc&) {
        return fail(StaticSceneResourceStatus::allocation_failed,
                    "stock_scene_allocation_failed",
                    "Bounded stock-scene preparation could not allocate its plan or packet evidence");
    }
}

}  // namespace apex::render
