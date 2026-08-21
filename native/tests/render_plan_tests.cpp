#include "apex/render/render_plan.hpp"

#include <cmath>
#include <iostream>
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
        color_order_matches_viewport();
        reflection_selection_matches_js_contract();
        malformed_tree_is_bounded();
        std::cout << "render plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "render plan tests failed: " << error.what() << '\n';
        return 1;
    }
}
