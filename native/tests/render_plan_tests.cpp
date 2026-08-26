#include "apex/render/render_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

apex::scene::SceneSnapshot fixture() {
    using namespace apex::scene;
    SceneSnapshot scene;
    scene.workspace_kind = "carLods";
    const MaterialId opaque = scene.add_material({"opaque", "ksPerPixel", BlendMode::opaque});
    const MaterialId alpha = scene.add_material({"alpha", "ksPerPixel", BlendMode::alpha_blend});
    SceneNode root_node;
    root_node.name = "ROOT";
    const NodeId root = scene.add_node(std::move(root_node));
    SceneNode environment_node;
    environment_node.name = "SHOWROOM";
    environment_node.workspace_auxiliary = "reflectionEnvironment";
    environment_node.workspace_file = "hangar.kn5";
    const NodeId environment = scene.add_node(std::move(environment_node), root);
    SceneNode floor;
    floor.name = "FLOOR";
    floor.kind = NodeKind::mesh;
    floor.material = opaque;
    floor.bounds_center = {0.0F, 0.0F, 8.0F};
    (void)scene.add_node(std::move(floor), environment);
    SceneNode glass;
    glass.name = "GLASS";
    glass.kind = NodeKind::mesh;
    glass.material = alpha;
    glass.layer = 2;
    glass.bounds_center = {0.0F, 0.0F, 4.0F};
    (void)scene.add_node(std::move(glass), environment);
    SceneNode hidden;
    hidden.name = "HIDDEN";
    hidden.active = false;
    const NodeId hidden_branch = scene.add_node(std::move(hidden), root);
    SceneNode hidden_mesh;
    hidden_mesh.name = "HIDDEN_MESH";
    hidden_mesh.kind = NodeKind::mesh;
    hidden_mesh.material = opaque;
    (void)scene.add_node(std::move(hidden_mesh), hidden_branch);
    SceneNode out_of_lod;
    out_of_lod.name = "OUT_OF_LOD";
    out_of_lod.kind = NodeKind::mesh;
    out_of_lod.material = opaque;
    out_of_lod.lod_in = 11.0F;
    out_of_lod.lod_out = 20.0F;
    out_of_lod.bounds_center = {0.0F, 0.0F, 10.0F};
    (void)scene.add_node(std::move(out_of_lod), root);
    SceneNode no_shadow_node;
    no_shadow_node.name = "NO_SHADOW";
    no_shadow_node.kind = NodeKind::mesh;
    no_shadow_node.material = opaque;
    no_shadow_node.cast_shadows = false;
    no_shadow_node.bounds_center = {0.0F, 0.0F, 2.0F};
    const NodeId no_shadow = scene.add_node(std::move(no_shadow_node), root);
    (void)no_shadow;
    return scene;
}

void visibility_lod_and_shadow() {
    const auto scene = fixture();
    apex::render::RenderPlanOptions options;
    options.camera_position = {0.0F, 0.0F, 0.0F};
    const auto plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 3U, "active visible meshes and LOD filtering");
    require(plan.shadow_casters.size() == 2U, "cast-shadow flag filters casters");
    require(plan.items[0].node != apex::scene::invalid_node_id, "stable node IDs");
    require(plan.unsupported_effects.size() == 1U, "unsupported effects are explicit");
    require(plan.unsupported_effects.front().code == "backend_effects", "unsupported effect code");
}

void lod_uses_each_item_world_distance_and_inclusive_out() {
    apex::scene::SceneSnapshot scene;
    const auto material = scene.add_material({"opaque", "ksPerPixel", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = scene.add_node(std::move(root));
    apex::scene::SceneNode mesh;
    mesh.name = "EDGE";
    mesh.kind = apex::scene::NodeKind::mesh;
    mesh.material = material;
    mesh.lod_in = 5.0F;
    mesh.lod_out = 5.0F;
    mesh.bounds_center = {0.0F, 0.0F, 5.0F};
    (void)scene.add_node(std::move(mesh), root_id);

    apex::render::RenderPlanOptions options;
    auto plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U, "LOD OUT includes an item at the authored boundary");
    options.camera_position = {0.0F, 0.0F, 1.0F};
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.empty(), "LOD filtering uses each item's camera distance");
}

void recovered_ksnet_mesh_lod_rule_is_bounded_and_inclusive() {
    apex::render::KsNetMeshLodRequest request;
    request.camera_fov_degrees = 80.0F;
    request.lod_in = 5.0F;
    request.lod_out = 4.0F;
    request.bounds_radius = 6.0F;
    request.bounds_center = {10.0F, 2.0F, -3.0F};
    request.camera_position = {10.0F, 2.0F, 2.0F};
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet near boundary is inclusive");
    request.camera_position[2] = 3.0F;
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet radius-floored far boundary is inclusive");
    request.camera_position[2] = 1.99F;
    require(!apex::render::ksnet_mesh_lod_visible(request),
            "ksNet distance below near is excluded");
    request.camera_position[2] = 3.01F;
    require(!apex::render::ksnet_mesh_lod_visible(request),
            "ksNet distance above effective far is excluded");

    request.camera_fov_degrees = 40.0F;
    request.camera_position[2] = 7.0F;
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet FOV factor scales squared camera distance");
    request.camera_fov_degrees = 160.0F;
    request.camera_position[2] = 3.0F;
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet FOV factor clamps to one");
    request.in_pvs = false;
    request.no_cull = true;
    require(!apex::render::ksnet_mesh_lod_visible(request),
            "ksNet does not restore an entry absent from PVS");
    request.in_pvs = true;
    request.camera_position[2] = std::numeric_limits<float>::quiet_NaN();
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet NO_CULL preserves an existing PVS entry");
    request.no_cull = false;
    require(!apex::render::ksnet_mesh_lod_visible(request),
            "bounded ksNet contract rejects non-finite distance");

    request.camera_position = {100.0F, 0.0F, 0.0F};
    request.bounds_center = {0.0F, 0.0F, 0.0F};
    request.lod_in = 0.0F;
    request.lod_out = 0.0F;
    request.bounds_radius = 0.0F;
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet skips distance culling when both LOD limits are zero");
    request.camera_position[0] = std::numeric_limits<float>::quiet_NaN();
    require(apex::render::ksnet_mesh_lod_visible(request),
            "ksNet zero-limit bypass does not read unused camera values");
}

void recovered_ksnet_mesh_lod_rule_reaches_opt_in_plan() {
    apex::scene::SceneSnapshot scene;
    const auto material = scene.add_material(
        {"opaque", "ksPerPixel", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = scene.add_node(std::move(root));
    apex::scene::SceneNode mesh;
    mesh.name = "LOD_MESH";
    mesh.kind = apex::scene::NodeKind::mesh;
    mesh.material = material;
    mesh.bounds_center = {0.0F, 0.0F, 6.0F};
    mesh.bounds_radius = 6.0F;
    mesh.lod_in = 5.0F;
    mesh.lod_out = 4.0F;
    const auto mesh_id = scene.add_node(std::move(mesh), root_id);

    apex::render::RenderPlanOptions options;
    options.ksnet_mesh_lod.emplace(80.0F);
    auto plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U && plan.shadow_casters.size() == 1U,
            "opt-in plan uses inclusive radius-floored ksNet LOD");

    options.ksnet_mesh_lod->camera_fov_degrees = 40.0F;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.empty() && plan.shadow_casters.empty(),
            "opt-in plan applies ksNet FOV scaling to every pass partition");

    const std::array<apex::render::KsNetMeshLodNodeState, 1U> absent = {{
        {mesh_id, false, true},
    }};
    options.ksnet_mesh_lod->camera_fov_degrees = 80.0F;
    options.ksnet_mesh_lod->node_states = absent;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.empty(),
            "an absent PVS entry remains hidden even when NO_CULL is set");

    const std::array<apex::render::KsNetMeshLodNodeState, 1U> no_cull = {{
        {mesh_id, true, true},
    }};
    options.camera_position = {0.0F, 0.0F,
                               -std::numeric_limits<float>::max()};
    options.ksnet_mesh_lod->node_states = no_cull;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U && std::isfinite(plan.items.front().distance),
            "explicit NO_CULL retains a finite-distance item outside its interval");

    const std::array<apex::render::NodeRenderStateOverride, 1U> override_lod = {{
        {mesh_id, std::nullopt, std::nullopt, 7.0, 7.0, std::nullopt},
    }};
    options.camera_position = {};
    options.ksnet_mesh_lod->node_states = {};
    options.node_state_overrides = override_lod;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.empty(),
            "CSP LOD state is resolved before the recovered predicate");

    const std::array<apex::render::NodeRenderStateOverride, 1U> zero_lod = {{
        {mesh_id, std::nullopt, std::nullopt, 0.0, 0.0, std::nullopt},
    }};
    options.camera_position = {0.0F, 0.0F,
                               -std::numeric_limits<float>::max()};
    options.node_state_overrides = zero_lod;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U && std::isfinite(plan.items.front().distance),
            "effective zero limits retain a finite-distance plan item");
}

void malformed_generic_lod_inputs_fail_closed() {
    auto scene = fixture();
    apex::render::RenderPlanOptions options;
    options.camera_position = {-std::numeric_limits<float>::max(), 0.0F, 0.0F};
    scene.nodes[2U].bounds_center = {
        std::numeric_limits<float>::max(), 0.0F, 0.0F};
    auto plan = apex::render::build_render_plan(scene, options);
    const auto extreme_item = std::find_if(
        plan.items.begin(), plan.items.end(), [&](const auto& item) {
            return item.node == scene.nodes[2U].id;
        });
    require(extreme_item != plan.items.end() &&
                extreme_item->distance == std::numeric_limits<float>::max(),
            "finite coordinate overflow saturates instead of producing a non-finite item");

    options.camera_position = {};
    scene.nodes[2U].bounds_center = {0.0F, 0.0F, 8.0F};
    scene.nodes[2U].lod_in = std::numeric_limits<float>::quiet_NaN();
    plan = apex::render::build_render_plan(scene, options);
    require(std::none_of(plan.items.begin(), plan.items.end(),
                         [&](const auto& item) {
                             return item.node == scene.nodes[2U].id;
                         }),
            "non-finite generic LOD limits fail closed");
}

void deferred_camera_filter_retains_exact_kn5_descriptor() {
    auto scene = fixture();
    auto& mesh = scene.nodes[6U];
    mesh.local_bounds_center = {1.0F, 2.0F, 3.0F};
    mesh.local_bounds_radius = 4.0F;
    mesh.local_bounds_source = apex::scene::LocalBoundsSource::kn5_serialized;

    apex::render::RenderPlanOptions options;
    options.defer_camera_mesh_filter = true;
    const auto plan = apex::render::build_render_plan(scene, options);
    const auto item = std::find_if(
        plan.items.begin(), plan.items.end(), [&](const auto& candidate) {
            return candidate.node == mesh.id;
        });
    require(item != plan.items.end(),
            "deferred camera filter retains an initially out-of-LOD mesh");
    require(item->camera_mesh_filter.has_value() &&
                item->camera_mesh_filter->bounding_sphere.center ==
                    mesh.local_bounds_center &&
                item->camera_mesh_filter->bounding_sphere.radius == 4.0F &&
                item->camera_mesh_filter->lod_in == mesh.lod_in &&
                item->camera_mesh_filter->lod_out == mesh.lod_out &&
                !item->camera_mesh_filter->no_cull &&
                !item->camera_mesh_filter->is_static,
            "deferred item copies the recovered ordinary KN5 filter state");
    require(scene.nodes[2U].local_bounds_source ==
                apex::scene::LocalBoundsSource::unavailable &&
                std::none_of(plan.items.begin(), plan.items.end(),
                             [&](const auto& candidate) {
                                 return candidate.node == scene.nodes[2U].id &&
                                        candidate.camera_mesh_filter.has_value();
                             }),
            "unrecovered bounds keep an explicit conservative fallback");
}

void color_order_matches_viewport() {
    const auto scene = fixture();
    apex::render::RenderPlanOptions options;
    const auto plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 3U, "order fixture item count");
    require(!plan.items[0].transparent && !plan.items[1].transparent, "opaque items precede transparent items");
    require(plan.items[2].transparent, "alpha material is transparent");
    require(plan.items[0].node < plan.items[1].node, "opaque layer tie is stable traversal order");
    require(plan.items[2].layer == 2U, "layer is retained in render item");
    require(plan.opaque_items.size() == 2U && plan.transparent_items.size() == 1U, "pass partitions");
}

void preview_visibility_precedence_is_immutable() {
    auto scene = fixture();
    const auto environment = scene.nodes[1U].id;
    const auto hidden_branch = scene.nodes[4U].id;
    const auto hidden_mesh = scene.nodes[5U].id;
    const auto out_of_lod = scene.nodes[6U].id;
    const bool authored_environment_active = scene.nodes[1U].active;
    const bool authored_hidden_active = scene.nodes[4U].active;

    const std::array<apex::scene::NodeActivityOverride, 1U> overrides = {
        apex::scene::NodeActivityOverride{environment, false}};
    apex::render::RenderPlanOptions options;
    options.activity_overrides = overrides;
    auto plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U, "activity overrides replace authored branch state");

    const std::array<apex::scene::NodeId, 1U> suppressed = {hidden_branch};
    options.show_hidden = true;
    options.suppressed_subtree_roots = suppressed;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 3U, "show-hidden bypasses authored and preview "
                                     "state but keeps suppression and mesh LOD");
    require(std::none_of(plan.items.begin(), plan.items.end(),
                         [&](const auto& item) {
                             return item.node == hidden_mesh || item.node == out_of_lod;
                         }),
            "suppressed and out-of-LOD meshes stay hidden");

    options.isolated = true;
    options.isolated_node = hidden_mesh;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U && plan.items.front().node == hidden_mesh,
            "isolation bypasses authored state, preview state, and subtree "
            "suppression");

    options.isolated_node = out_of_lod;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.empty(), "isolation still obeys the selected mesh LOD interval");
    require(scene.nodes[1U].active == authored_environment_active &&
                scene.nodes[4U].active == authored_hidden_active,
            "preview planning does not mutate authored scene state");
}

void csp_node_state_controls_order_lod_transparency_and_shadows() {
    const auto scene = fixture();
    const auto floor = scene.nodes[2U].id;
    const auto glass = scene.nodes[3U].id;
    const auto out_of_lod = scene.nodes[6U].id;
    const std::array<apex::render::NodeRenderStateOverride, 3U> overrides = {{
        {floor, true, 0.5, std::nullopt, std::nullopt, false},
        {glass, false, 4.25, std::nullopt, std::nullopt, std::nullopt},
        {out_of_lod, std::nullopt, std::nullopt, 0.0, 0.0, false},
    }};
    apex::render::RenderPlanOptions options;
    options.node_state_overrides = overrides;
    const auto plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 4U, "CSP LOD override includes an authored out-of-LOD mesh");
    const auto* floor_item = [&]() -> const apex::render::RenderItem* {
        const auto found = std::find_if(plan.items.begin(), plan.items.end(),
                                        [&](const auto& item) { return item.node == floor; });
        return found == plan.items.end() ? nullptr : &*found;
    }();
    const auto* glass_item = [&]() -> const apex::render::RenderItem* {
        const auto found = std::find_if(plan.items.begin(), plan.items.end(),
                                        [&](const auto& item) { return item.node == glass; });
        return found == plan.items.end() ? nullptr : &*found;
    }();
    require(floor_item != nullptr && floor_item->transparent &&
                floor_item->transparency_overridden && floor_item->layer == 0.5,
            "CSP transparency and fractional layer override authored state");
    require(glass_item != nullptr && !glass_item->transparent &&
                glass_item->transparency_overridden && glass_item->layer == 4.25,
            "explicit false transparency overrides an alpha material default");
    require(plan.items.back().node == floor,
            "CSP transparent classification controls final pass ordering");
    require(plan.shadow_casters.size() == 1U &&
                plan.shadow_casters.front().node == glass,
            "CSP cast-shadow state controls shadow submission");
}

void reflection_selection_matches_js_contract() {
    auto scene = fixture();
    apex::render::RenderPlanOptions options;
    auto plan = apex::render::build_render_plan(scene, options);
    require(plan.reflection.mode == apex::render::ReflectionSelectionMode::environment, "tagged environment wins");
    require(plan.reflection.items.size() == 2U, "environment subtree selection");
    require(plan.reflection.root_name == "hangar.kn5", "environment file label");

    options.explicit_reflection_root = scene.nodes[1].id;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.reflection.mode == apex::render::ReflectionSelectionMode::explicit_subtree, "explicit subtree mode");
    require(plan.reflection.items.size() == 2U, "explicit subtree includes descendants");

    options.explicit_reflection_root = apex::scene::invalid_node_id;
    options.bounds_radius = 3.0F;
    scene.nodes[1].workspace_auxiliary.clear();
    scene.nodes[1].workspace_file.clear();
    plan = apex::render::build_render_plan(scene, options);
    require(plan.reflection.mode == apex::render::ReflectionSelectionMode::fallback, "small isolated car fallback");
    options.workspace_kind = "track";
    plan = apex::render::build_render_plan(scene, options);
    require(plan.reflection.mode == apex::render::ReflectionSelectionMode::scene, "track captures whole scene");

    options.isolated = true;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.reflection.mode == apex::render::ReflectionSelectionMode::disabled, "isolated preview disables reflection");
    require(plan.reflection.reason == "Isolated mesh preview", "isolated reflection reason");
}

void malformed_tree_is_bounded() {
    apex::scene::SceneSnapshot scene;
    apex::scene::SceneNode root;
    root.id = 0U;
    root.name = "ROOT";
    root.children = {0U, 55U};
    scene.nodes.push_back(std::move(root));
    scene.root = 0U;
    const auto plan = apex::render::build_render_plan(scene);
    require(plan.items.empty(), "cycle and invalid child cannot create geometry");
}

}  // namespace

int main() {
    try {
        visibility_lod_and_shadow();
        lod_uses_each_item_world_distance_and_inclusive_out();
        recovered_ksnet_mesh_lod_rule_is_bounded_and_inclusive();
        recovered_ksnet_mesh_lod_rule_reaches_opt_in_plan();
        malformed_generic_lod_inputs_fail_closed();
        deferred_camera_filter_retains_exact_kn5_descriptor();
        color_order_matches_viewport();
        preview_visibility_precedence_is_immutable();
        csp_node_state_controls_order_lod_transparency_and_shadows();
        reflection_selection_matches_js_contract();
        malformed_tree_is_bounded();
        std::cout << "render plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "render plan tests failed: " << error.what() << '\n';
        return 1;
    }
}
