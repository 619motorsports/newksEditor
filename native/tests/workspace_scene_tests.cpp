#include "apex/render/render_plan.hpp"
#include "apex/workspace/workspace_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using apex::scene::NodeId;
using apex::workspace::WorkspaceError;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void expects_error(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const WorkspaceError& value) {
        require(value.format() == "WORKSPACE", "workspace error attribution");
        require(value.code() == code, "unexpected workspace scene error code");
        return;
    }
    throw std::runtime_error("workspace scene adapter accepted malformed input");
}

struct Fixture {
    apex::scene::SceneSnapshot scene;
    apex::workspace::WorkspaceMetadata workspace;
    NodeId lod0_root = apex::scene::invalid_node_id;
    NodeId lod0_mesh = apex::scene::invalid_node_id;
    NodeId lod1_root = apex::scene::invalid_node_id;
    NodeId lod1_mesh = apex::scene::invalid_node_id;
    NodeId auxiliary_root = apex::scene::invalid_node_id;
    NodeId auxiliary_mesh = apex::scene::invalid_node_id;
};

Fixture fixture() {
    using namespace apex::scene;
    Fixture value;
    const auto material = value.scene.add_material(
        {"body", "ksPerPixel", BlendMode::opaque});
    SceneNode root;
    root.name = "ROOT";
    const NodeId root_id = value.scene.add_node(std::move(root));

    const auto add_file = [&](std::string name, bool visible,
                              std::optional<apex::workspace::CarLodManifest> lod,
                              std::string auxiliary, NodeId& wrapper_id,
                              NodeId& mesh_id) {
        SceneNode wrapper;
        wrapper.name = name;
        wrapper_id = value.scene.add_node(std::move(wrapper), root_id);
        SceneNode mesh;
        mesh.name = name + "_MESH";
        mesh.kind = NodeKind::mesh;
        mesh.material = material;
        mesh.active = visible;
        mesh.visible = visible;
        mesh.renderable = visible;
        mesh.bounds_center = {0.0F, 0.0F, 15.0F};
        mesh.lod_in = 5.0F;
        mesh.lod_out = 5.0F;
        mesh_id = value.scene.add_node(std::move(mesh), wrapper_id);

        apex::workspace::WorkspaceFile file;
        file.name = std::move(name);
        file.lod = std::move(lod);
        file.auxiliary = std::move(auxiliary);
        value.workspace.files.push_back(std::move(file));
    };

    apex::workspace::CarLodManifest lod0;
    lod0.index = 0U;
    lod0.file = "lod0.kn5";
    lod0.inDistance = 0.0F;
    lod0.outDistance = 15.0F;
    apex::workspace::CarLodManifest lod1;
    lod1.index = 1U;
    lod1.file = "lod1.kn5";
    lod1.inDistance = 15.0F;
    lod1.outDistance = 45.0F;
    add_file("lod0.kn5", false, lod0, {}, value.lod0_root, value.lod0_mesh);
    add_file("lod1.kn5", true, lod1, {}, value.lod1_root, value.lod1_mesh);
    add_file("driver.kn5", true, std::nullopt, "driver", value.auxiliary_root,
             value.auxiliary_mesh);
    value.workspace.kind = "carLods";
    return value;
}

apex::workspace::WorkspaceLodResolutionRequest request_for(
    const Fixture& value, const apex::workspace::WorkspaceSceneBinding& binding) {
    apex::workspace::WorkspaceLodResolutionRequest request;
    request.workspace = &value.workspace;
    request.scene = &value.scene;
    request.file_root_nodes = binding.file_root_nodes;
    request.bounds_center = {0.0F, 0.0F, 0.0F};
    request.camera_position = {0.0F, 0.0F, 20.0F};
    return request;
}

bool contains(std::span<const NodeId> values, NodeId wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

void binds_metadata_and_resolves_half_open_ranges() {
    auto value = fixture();
    const auto binding = apex::workspace::bindWorkspaceScene(value.scene, value.workspace);
    require(binding.file_root_nodes.size() == 3U, "workspace root binding count");
    require(value.scene.workspace_kind == "carLods", "workspace kind binding");
    require(value.scene.nodes[value.lod0_root].workspace_file == "lod0.kn5",
            "workspace file label binding");
    require(value.scene.nodes[value.auxiliary_root].workspace_auxiliary == "driver",
            "workspace auxiliary binding");

    const auto resolved = apex::workspace::resolveWorkspaceLod(request_for(value, binding));
    require(std::abs(resolved.camera_distance - 20.0F) < 1e-5F,
            "workspace camera distance");
    require(std::abs(resolved.effective_distance - 15.0F) < 1e-5F,
            "workspace fixed-FOV distance");
    require(resolved.active_indices.size() == 1U && resolved.active_indices[0] == 1U,
            "workspace OUT is exclusive and next IN is inclusive");
    require(contains(resolved.excluded_root_nodes, value.lod0_root) &&
                !contains(resolved.excluded_root_nodes, value.auxiliary_root),
            "inactive LOD root is excluded and auxiliary remains active");

    apex::render::RenderPlanOptions options;
    options.camera_position = {0.0F, 0.0F, 20.0F};
    options.excluded_subtree_roots = resolved.excluded_root_nodes;
    const auto plan = apex::render::build_render_plan(value.scene, options);
    require(plan.items.size() == 2U, "resolved LOD and auxiliary reach render plan");
    require(plan.items[0].workspace_file == "lod1.kn5" &&
                plan.items[1].workspace_file == "driver.kn5",
            "workspace file identity is inherited in stable source order");
    require(plan.items[0].distance == 5.0F,
            "mesh LOD keeps its distinct inclusive upper boundary");
}

void forced_overlap_gap_track_camera_and_isolation() {
    auto value = fixture();
    value.scene.nodes[value.lod0_mesh].active = false;
    value.scene.nodes[value.lod0_mesh].visible = false;
    value.scene.nodes[value.lod0_mesh].renderable = false;
    const auto binding = apex::workspace::bindWorkspaceScene(value.scene, value.workspace);

    auto request = request_for(value, binding);
    request.selected_index = 0U;
    auto resolved = apex::workspace::resolveWorkspaceLod(request);
    require(resolved.active_indices == std::vector<std::uint32_t>{0U},
            "forced workspace LOD ignores camera distance");
    require(contains(resolved.excluded_root_nodes, value.lod1_root),
            "forced workspace LOD excludes other LOD roots");

    apex::render::RenderPlanOptions options;
    options.camera_position = request.camera_position;
    options.excluded_subtree_roots = resolved.excluded_root_nodes;
    options.isolated = true;
    options.isolated_node = value.lod0_mesh;
    const auto isolated = apex::render::build_render_plan(value.scene, options);
    require(isolated.items.size() == 1U && isolated.items[0].node == value.lod0_mesh,
            "isolation bypasses authored visibility and workspace LOD selection");
    require(isolated.reflection.mode == apex::render::ReflectionSelectionMode::disabled,
            "isolation disables reflection capture");

    value.workspace.files[0].lod->outDistance = 30.0F;
    request = request_for(value, binding);
    resolved = apex::workspace::resolveWorkspaceLod(request);
    require(resolved.active_indices == std::vector<std::uint32_t>({0U, 1U}),
            "overlapping workspace ranges remain simultaneously active");

    value.workspace.files[0].lod->outDistance = 10.0F;
    value.workspace.files[1].lod->inDistance = 20.0F;
    resolved = apex::workspace::resolveWorkspaceLod(request);
    require(resolved.active_indices.empty() && resolved.excluded_root_nodes.size() == 2U,
            "workspace LOD gaps can exclude every LOD root");

    request.camera_position = {0.0F, 0.0F, 200.0F};
    request.track_camera = true;
    resolved = apex::workspace::resolveWorkspaceLod(request);
    require(std::abs(resolved.effective_distance - 15.0F) < 1e-5F,
            "track-camera LOD divisor is multiplied by ten");
}

void resolves_preview_state_and_visibility_precedence() {
    using namespace apex::scene;
    SceneSnapshot scene;
    const MaterialId material = scene.add_material({"body", "ksPerPixel", BlendMode::opaque});
    SceneNode root;
    root.name = "ROOT";
    const NodeId root_id = scene.add_node(std::move(root));

    const auto add_branch_mesh = [&](std::string name, bool active, NodeId parent) {
        SceneNode branch;
        branch.name = std::move(name);
        branch.active = active;
        const NodeId branch_id = scene.add_node(std::move(branch), parent);
        SceneNode mesh;
        mesh.name = scene.nodes[branch_id].name + "_MESH";
        mesh.kind = NodeKind::mesh;
        mesh.material = material;
        const NodeId mesh_id = scene.add_node(std::move(mesh), branch_id);
        return std::pair{branch_id, mesh_id};
    };

    const auto first_high = add_branch_mesh("COCKPIT_HR", true, root_id);
    const auto duplicate_high = add_branch_mesh("COCKPIT_HR", false, root_id);
    const auto low = add_branch_mesh("COCKPIT_LR", false, root_id);

    SceneNode inactive;
    inactive.name = "INACTIVE_PARENT";
    inactive.active = false;
    const NodeId inactive_id = scene.add_node(std::move(inactive), root_id);
    const auto regular_rf = add_branch_mesh("RIM_RF", true, inactive_id);
    const auto blurred_rf = add_branch_mesh("RIM_BLUR_RF", false, inactive_id);
    const auto regular_lf = add_branch_mesh("RIM_LF", true, root_id);
    const auto blurred_lf = add_branch_mesh("RIM_BLUR_LF", false, root_id);

    SceneNode driver;
    driver.name = "driver.kn5";
    driver.workspace_auxiliary = "driver";
    const NodeId driver_id = scene.add_node(std::move(driver), root_id);
    SceneNode driver_helmet;
    driver_helmet.name = "Helmet";
    const NodeId driver_helmet_id = scene.add_node(std::move(driver_helmet), driver_id);
    SceneNode visor;
    visor.name = "VISOR";
    const NodeId visor_id = scene.add_node(std::move(visor), driver_helmet_id);
    SceneNode driver_mesh;
    driver_mesh.name = "DRIVER_MESH";
    driver_mesh.kind = NodeKind::mesh;
    driver_mesh.material = material;
    const NodeId driver_mesh_id = scene.add_node(std::move(driver_mesh), visor_id);

    SceneNode car;
    car.name = "car.kn5";
    const NodeId car_id = scene.add_node(std::move(car), root_id);
    const auto car_helmet = add_branch_mesh("helmet", true, car_id);
    SceneNode out_of_lod;
    out_of_lod.name = "OUT_OF_LOD";
    out_of_lod.kind = NodeKind::mesh;
    out_of_lod.material = material;
    out_of_lod.lod_in = 100.0F;
    const NodeId out_of_lod_id = scene.add_node(std::move(out_of_lod), root_id);

    const std::vector<std::string> hidden_names = {" Helmet ", "HELMET", "visor", "missing"};
    apex::workspace::WorkspacePreviewResolutionRequest request;
    request.scene = &scene;
    request.cockpit_high_visible = false;
    request.blurred_rims_visible = true;
    request.driver_cockpit = true;
    request.driver_hidden_names = hidden_names;
    const auto resolved = apex::workspace::resolveWorkspacePreview(request);

    require(resolved.cockpit_available && resolved.cockpit_high_root == first_high.first &&
                resolved.cockpit_low_root == low.first && resolved.cockpit_high_nodes == 2U &&
                resolved.cockpit_low_nodes == 1U,
            "cockpit preview uses the first exact high/low pair");
    require(resolved.rim_blur_available && resolved.first_regular_rim == regular_lf.first &&
                resolved.regular_rim_nodes == 2U && resolved.blurred_rim_nodes == 2U,
            "rim preview uses canonical corner order and all exact roots");
    require(resolved.driver_hidden_requested == 3U && resolved.driver_hidden_matched == 2U &&
                resolved.suppressed_root_nodes == std::vector<NodeId>{driver_helmet_id},
            "driver names are trimmed, deduplicated, inherited, and reduced to "
            "one subtree root");
    require(scene.nodes[first_high.first].active && !scene.nodes[duplicate_high.first].active &&
                !scene.nodes[low.first].active,
            "preview resolution does not mutate authored activity");

    apex::render::RenderPlanOptions options;
    options.activity_overrides = resolved.activity_overrides;
    options.suppressed_subtree_roots = resolved.suppressed_root_nodes;
    auto plan = apex::render::build_render_plan(scene, options);
    const auto has_item = [&](NodeId wanted) {
        return std::any_of(plan.items.begin(), plan.items.end(),
                           [&](const auto& item) { return item.node == wanted; });
    };
    require(plan.items.size() == 3U && has_item(low.second) && has_item(blurred_lf.second) &&
                has_item(car_helmet.second),
            "cockpit, rim, and driver preview state reaches the render planner");
    require(!has_item(first_high.second) && !has_item(duplicate_high.second) &&
                !has_item(regular_lf.second) && !has_item(regular_rf.second) &&
                !has_item(blurred_rf.second) && !has_item(driver_mesh_id),
            "preview overrides keep duplicates, ancestors, and driver "
            "suppression exact");

    options.show_hidden = true;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 8U, "show-hidden reveals authored and preview-hidden meshes only");
    require(std::none_of(plan.items.begin(), plan.items.end(),
                         [&](const auto& item) {
                             return item.node == driver_mesh_id || item.node == out_of_lod_id;
                         }),
            "show-hidden keeps driver suppression and per-mesh LOD");

    options.show_hidden = false;
    options.isolated = true;
    options.isolated_node = driver_mesh_id;
    plan = apex::render::build_render_plan(scene, options);
    require(plan.items.size() == 1U && plan.items.front().node == driver_mesh_id,
            "isolation bypasses driver suppression for the exact mesh");
}

void rejects_malformed_preview_state() {
    apex::workspace::WorkspacePreviewResolutionRequest request;
    expects_error([&] { (void)apex::workspace::resolveWorkspacePreview(request); },
                  "INVALID_REQUEST");

    auto value = fixture();
    request.scene = &value.scene;
    auto repeated = value.scene;
    repeated.nodes[repeated.root].children.push_back(value.lod0_root);
    request.scene = &repeated;
    expects_error([&] { (void)apex::workspace::resolveWorkspacePreview(request); },
                  "INVALID_SCENE");

    request.scene = &value.scene;
    apex::workspace::WorkspaceSceneLimits limits;
    limits.max_scene_nodes = value.scene.nodes.size() - 1U;
    expects_error([&] { (void)apex::workspace::resolveWorkspacePreview(request, limits); },
                  "COUNT_LIMIT");

    const std::vector<std::string> names = {"one", "two"};
    request.driver_hidden_names = names;
    limits = {};
    limits.max_files = 1U;
    expects_error([&] { (void)apex::workspace::resolveWorkspacePreview(request, limits); },
                  "COUNT_LIMIT");
    limits = {};
    limits.max_string_bytes = 2U;
    expects_error([&] { (void)apex::workspace::resolveWorkspacePreview(request, limits); },
                  "STRING_LIMIT");
    limits = {};
    limits.max_aggregate_bytes = 1U;
    expects_error([&] { (void)apex::workspace::resolveWorkspacePreview(request, limits); },
                  "AGGREGATE_LIMIT");

    apex::scene::SceneSnapshot cockpit_only;
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const NodeId root_id = cockpit_only.add_node(std::move(root));
    apex::scene::SceneNode high;
    high.name = "COCKPIT_HR";
    (void)cockpit_only.add_node(std::move(high), root_id);
    request = {};
    request.scene = &cockpit_only;
    request.cockpit_high_visible = true;
    const auto unavailable = apex::workspace::resolveWorkspacePreview(request);
    require(!unavailable.cockpit_available && unavailable.activity_overrides.empty(),
            "a missing cockpit pair keeps authored state without a partial override");
}

void rejects_malformed_state_without_partial_binding() {
    auto value = fixture();
    value.scene.workspace_kind = "before";
    value.scene.nodes[value.lod0_root].workspace_file = "unchanged";

    auto mismatched = value.workspace;
    mismatched.files[0].name = "wrong.kn5";
    expects_error([&] { (void)apex::workspace::bindWorkspaceScene(value.scene, mismatched); },
                  "INVALID_SCENE");
    require(value.scene.workspace_kind == "before" &&
                value.scene.nodes[value.lod0_root].workspace_file == "unchanged",
            "failed binding leaves the scene unchanged");

    auto invalid_lod = value.workspace;
    invalid_lod.files[0].lod->outDistance =
        std::numeric_limits<float>::quiet_NaN();
    expects_error([&] { (void)apex::workspace::bindWorkspaceScene(value.scene, invalid_lod); },
                  "INVALID_LOD");

    apex::workspace::WorkspaceSceneLimits limits;
    limits.max_string_bytes = 1U;
    expects_error([&] { (void)apex::workspace::bindWorkspaceScene(value.scene, value.workspace, limits); },
                  "STRING_LIMIT");
    limits = {};
    limits.max_aggregate_bytes = 1U;
    expects_error([&] { (void)apex::workspace::bindWorkspaceScene(value.scene, value.workspace, limits); },
                  "AGGREGATE_LIMIT");

    const auto binding = apex::workspace::bindWorkspaceScene(value.scene, value.workspace);
    auto request = request_for(value, binding);
    request.camera_position[0] = std::numeric_limits<float>::infinity();
    expects_error([&] { (void)apex::workspace::resolveWorkspaceLod(request); },
                  "NON_FINITE_CAMERA");
    request = request_for(value, binding);
    auto duplicate_roots = binding.file_root_nodes;
    duplicate_roots[1] = duplicate_roots[0];
    request.file_root_nodes = duplicate_roots;
    expects_error([&] { (void)apex::workspace::resolveWorkspaceLod(request); },
                  "INVALID_SCENE");
    request.workspace = nullptr;
    expects_error([&] { (void)apex::workspace::resolveWorkspaceLod(request); },
                  "INVALID_REQUEST");
}

}  // namespace

int main() {
    try {
        binds_metadata_and_resolves_half_open_ranges();
        forced_overlap_gap_track_camera_and_isolation();
        resolves_preview_state_and_visibility_precedence();
        rejects_malformed_preview_state();
        rejects_malformed_state_without_partial_binding();
        std::cout << "workspace scene tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace scene tests failed: " << error.what() << '\n';
        return 1;
    }
}
