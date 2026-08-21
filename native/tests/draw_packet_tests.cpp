#include "apex/core/parse_error.hpp"
#include "apex/render/draw_packet.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::array<float, 16> identity(float x = 0.0F) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
}

apex::formats::Kn5Material material(std::string shader = "ksPerPixel") {
    apex::formats::Kn5Material result;
    result.name = "body";
    result.shader = std::move(shader);
    result.blendMode = 0;
    result.depthMode = 0;
    result.resources.push_back({"txDiffuse", 21, "body_d.dds"});
    return result;
}

apex::formats::Kn5Node static_mesh(std::string name, float x, std::uint32_t material_id = 0) {
    apex::formats::Kn5Node result;
    result.type = 2;
    result.kind = "mesh";
    result.name = std::move(name);
    result.transform = identity(x);
    result.active = true;
    result.visible = true;
    result.renderable = true;
    result.materialId = material_id;
    result.vertexStride = 11;
    result.vertices = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0,
                       1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0,
                       0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0};
    result.indices = {0, 1, 2};
    return result;
}

apex::scene::SceneSnapshot static_scene() {
    apex::scene::SceneSnapshot scene;
    (void)scene.add_material({"body", "ksPerPixel", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = scene.add_node(std::move(root));
    apex::scene::SceneNode mesh;
    mesh.name = "BODY";
    mesh.kind = apex::scene::NodeKind::mesh;
    mesh.material = 0;
    mesh.transform = identity(3);
    mesh.renderable = true;
    mesh.visible = true;
    mesh.active = true;
    (void)scene.add_node(std::move(mesh), root_id);
    return scene;
}

apex::formats::Kn5File static_model(std::string shader = "ksPerPixel") {
    apex::formats::Kn5File model;
    model.textures.resize(1);
    model.textures[0].active = true;
    model.textures[0].name = "body_d.dds";
    model.materials.push_back(material(std::move(shader)));
    model.root.type = 1;
    model.root.kind = "node";
    model.root.name = "ROOT";
    model.root.active = true;
    model.root.children.push_back(static_mesh("BODY", 3));
    return model;
}

apex::render::RenderPlan one_item_plan(bool transparent = false) {
    apex::render::RenderPlan plan;
    plan.items.push_back({1, 0, 0, 3.0F, transparent, true, {}, {}, {0, 1}});
    return plan;
}

void builds_static_packet_with_stock_state_and_ranges() {
    const auto model = static_model();
    const auto scene = static_scene();
    apex::render::DrawPacketOptions options;
    options.wireframe = true;
    options.selected_node = 1;
    const auto result = apex::render::build_draw_packets(model, scene, one_item_plan(), options);
    require(result.packets.size() == 1 && result.supported, "static packet builds");
    const auto& packet = result.packets.front();
    require(packet.primitive == apex::render::DrawPrimitiveKind::static_mesh &&
                packet.vertex_count == 3 && packet.index_count == 3 && packet.vertex_stride_floats == 11 &&
                packet.vertex_offset == 0 && packet.index_offset == 0,
            "static packet ranges and layout");
    require(packet.world_matrix[12] == 3.0F && packet.flags.wireframe && packet.flags.selected &&
                packet.flags.depth_test && packet.flags.depth_write && !packet.flags.transparent,
            "static packet matrices and state flags");
    require(packet.material_profile.stock != nullptr && packet.resources.size() == 1 &&
                packet.resources[0].bind_point == 21 && packet.resources[0].texture_index == 0 &&
                packet.resources[0].texture == "body_d.dds" &&
                packet.resources[0].slot == "txDiffuse",
            "stock material and resource slots");
    require(!result.unsupported_effects.empty() && !packet.shader_execution_supported,
            "staged shader execution is explicit");
}

void preserves_deterministic_transparent_order_and_unknown_shader_diagnostic() {
    auto model = static_model("unknownShader");
    model.root.children.push_back(static_mesh("GLASS_FAR", 20));
    auto scene = static_scene();
    scene.materials[0].shader = "unknownShader";
    apex::scene::SceneNode far_mesh;
    far_mesh.name = "GLASS_FAR";
    far_mesh.kind = apex::scene::NodeKind::mesh;
    far_mesh.material = 0;
    far_mesh.transform = identity(20);
    far_mesh.renderable = true;
    far_mesh.visible = true;
    far_mesh.active = true;
    (void)scene.add_node(std::move(far_mesh), 0);
    apex::render::RenderPlan plan;
    plan.items.push_back({2, 0, 0, 20.0F, true, true, {}, {}, {0, 2}});
    plan.items.push_back({1, 0, 0, 3.0F, false, true, {}, {}, {0, 1}});
    const auto result = apex::render::build_draw_packets(model, scene, plan);
    require(result.packets.size() == 2 && result.packets[0].node == 1 && result.packets[1].node == 2 &&
                result.packets[0].order == 0 && result.packets[1].order == 1,
            "opaque packets precede transparent packets deterministically");
    bool unknown = false;
    for (const auto& diagnostic : result.diagnostics) unknown = unknown || diagnostic.code == "UNKNOWN_SHADER";
    bool attributed = false;
    for (const auto& effect : result.unsupported_effects)
        attributed = attributed || (effect.code == "unknown_shader" && effect.node == 1 && effect.material == 0);
    require(unknown && attributed && !result.supported, "unknown stock shader is diagnosed and attributed");
}

void cpu_skinning_matches_reference_transform_and_validates_influences() {
    apex::formats::Kn5Node mesh;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19;
    mesh.bones.push_back({"Bone", identity(0)});
    mesh.vertices = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0,
                     1, 0, 0, 0, 0, 0, 0, 0};
    const std::map<std::string, apex::scene::Matrix4> bones = {{"Bone", identity(2)}};
    const auto skinned = apex::render::skin_vertices_reference(mesh, bones, identity(10));
    require(skinned.size() == 19 && std::abs(skinned[0] + 7.0F) < 1e-5F &&
                std::abs(skinned[1]) < 1e-5F && std::abs(skinned[3]) < 1e-5F &&
                std::abs(skinned[4] - 1.0F) < 1e-5F,
            "CPU skinning matches JS palette/world/local reference");

    mesh.vertices[15] = 2.0F;
    bool invalid_index = false;
    try {
        (void)apex::render::skin_vertices_reference(mesh, bones, identity());
    } catch (const apex::render::DrawPacketError& error) {
        invalid_index = error.code() == "INVALID_SKIN_INFLUENCE";
    }
    require(invalid_index, "invalid skin index is rejected");
}

void builds_validated_skinned_packet_with_bone_palette() {
    auto model = static_model("ksSkinnedMesh");
    auto& mesh = model.root.children.front();
    mesh.type = 3;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19;
    mesh.vertices = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0,
                     1, 0, 0, 0, 0, 0, 0, 0};
    mesh.indices = {0, 0, 0};
    mesh.bones.push_back({"BONE", identity()});
    apex::formats::Kn5Node bone;
    bone.type = 1;
    bone.kind = "node";
    bone.name = "BONE";
    bone.transform = identity();
    model.root.children.push_back(std::move(bone));
    apex::formats::Kn5Node duplicate_bone;
    duplicate_bone.type = 1;
    duplicate_bone.kind = "node";
    duplicate_bone.name = "BONE";
    duplicate_bone.transform = identity(1);
    model.root.children.push_back(std::move(duplicate_bone));

    auto scene = static_scene();
    scene.materials[0].shader = "ksSkinnedMesh";
    scene.nodes[1].kind = apex::scene::NodeKind::skinned_mesh;
    apex::scene::SceneNode scene_bone;
    scene_bone.name = "BONE";
    scene_bone.kind = apex::scene::NodeKind::node;
    scene_bone.transform = identity();
    scene_bone.active = true;
    scene_bone.visible = true;
    scene_bone.renderable = false;
    (void)scene.add_node(std::move(scene_bone), 0);
    apex::scene::SceneNode duplicate_scene_bone;
    duplicate_scene_bone.name = "BONE";
    duplicate_scene_bone.kind = apex::scene::NodeKind::node;
    duplicate_scene_bone.transform = identity(1);
    duplicate_scene_bone.active = true;
    duplicate_scene_bone.visible = true;
    duplicate_scene_bone.renderable = false;
    (void)scene.add_node(std::move(duplicate_scene_bone), 0);

    auto plan = one_item_plan();
    const auto result = apex::render::build_draw_packets(model, scene, plan);
    require(result.packets.size() == 1 && result.packets.front().primitive == apex::render::DrawPrimitiveKind::skinned_mesh &&
                result.packets.front().bone_palette.size() == 1,
            "skinned packet carries a validated bone palette");
    bool duplicate = false;
    for (const auto& diagnostic : result.diagnostics) duplicate = duplicate || diagnostic.code == "DUPLICATE_BONE_NAME";
    require(duplicate, "duplicate bone names are diagnosed while retaining first-name lookup");
}

void validates_layout_resource_identity_and_metadata_limits() {
    auto layout_model = static_model("ksSkinnedMesh");
    auto layout_scene = static_scene();
    layout_scene.materials[0].shader = "ksSkinnedMesh";
    const auto layout = apex::render::build_draw_packets(layout_model, layout_scene, one_item_plan());
    require(layout.packets.empty() && !layout.supported, "incompatible stock vertex layout is rejected");
    bool layout_diagnostic = false;
    for (const auto& diagnostic : layout.diagnostics) layout_diagnostic = layout_diagnostic || diagnostic.code == "UNSUPPORTED_LAYOUT";
    require(layout_diagnostic, "unsupported stock layout has a diagnostic");

    auto mismatched_resource_model = static_model();
    mismatched_resource_model.materials[0].resources[0].texture = "other.dds";
    const auto mismatched_resource = apex::render::build_draw_packets(mismatched_resource_model, static_scene(), one_item_plan());
    require(mismatched_resource.packets.empty() && !mismatched_resource.supported, "unresolved resource texture name is rejected");

    auto identity_scene = static_scene();
    identity_scene.nodes[1].name = "RENAMED";
    const auto identity = apex::render::build_draw_packets(static_model(), identity_scene, one_item_plan());
    require(identity.packets.empty() && !identity.supported, "scene and KN5 node identity mismatch is rejected");

    const auto bounded_model = static_model();
    const auto& bounded_mesh = bounded_model.root.children.front();
    apex::render::DrawPacketLimits limits;
    limits.max_packet_bytes = bounded_mesh.vertices.size() * sizeof(float) + bounded_mesh.indices.size() * sizeof(std::uint16_t);
    const auto metadata_limited = apex::render::build_draw_packets(bounded_model, static_scene(), one_item_plan(), {}, limits);
    require(metadata_limited.packets.empty() && metadata_limited.limit_exceeded, "packet metadata is included in byte limits");
}

void cpu_skinning_matches_rotation_nonuniform_scale_and_inverse_bind() {
    apex::formats::Kn5Node mesh;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19;
    mesh.bones.push_back({"Bone", identity(-1)});
    mesh.vertices = {1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
                     1, 0, 0, 0, 0, 0, 0, 0};
    const apex::scene::Matrix4 rotated_scaled = {0, 2, 0, 0, -3, 0, 0, 0, 0, 0, 1, 0, 3, 4, 0, 1};
    const auto skinned = apex::render::skin_vertices_reference(mesh, {{"Bone", rotated_scaled}}, identity());
    require(std::abs(skinned[0] - 3.0F) < 1e-5F && std::abs(skinned[1] - 4.0F) < 1e-5F &&
                std::abs(skinned[3]) < 1e-5F && std::abs(skinned[4] - 1.0F) < 1e-5F,
            "CPU skinning preserves rotation and inverse-bind translation");
}

void rejects_invalid_geometry_and_limits_before_packet_growth() {
    auto model = static_model();
    model.root.children.front().indices[0] = 9;
    const auto scene = static_scene();
    const auto invalid = apex::render::build_draw_packets(model, scene, one_item_plan());
    require(invalid.packets.empty() && !invalid.supported, "invalid index is diagnosed before packet creation");

    auto valid_model = static_model();
    apex::render::DrawPacketLimits limits;
    limits.max_packet_bytes = 1;
    const auto limited = apex::render::build_draw_packets(valid_model, scene, one_item_plan(), {}, limits);
    require(limited.packets.empty() && limited.limit_exceeded, "packet byte limit is enforced before growth");
}

void rejects_nonfinite_scene_and_ambiguous_resource_inputs() {
    auto model = static_model();
    auto scene = static_scene();
    scene.nodes[1].transform[0] = std::numeric_limits<float>::quiet_NaN();
    const auto nonfinite = apex::render::build_draw_packets(model, scene, one_item_plan());
    require(nonfinite.packets.empty() && !nonfinite.supported, "non-finite scene matrix is rejected");

    auto duplicate_model = static_model();
    duplicate_model.materials[0].resources.push_back({"txDiffuse", 7, "other.dds"});
    const auto duplicate = apex::render::build_draw_packets(duplicate_model, static_scene(), one_item_plan());
    require(duplicate.packets.empty() && !duplicate.supported, "duplicate resource slots are rejected");

    auto ambiguous_model = static_model();
    ambiguous_model.textures.push_back(ambiguous_model.textures.front());
    ambiguous_model.textures.back().name = " BODY_D.DDS ";
    const auto ambiguous = apex::render::build_draw_packets(ambiguous_model, static_scene(), one_item_plan());
    require(ambiguous.packets.empty() && !ambiguous.supported, "ambiguous texture names are rejected");

    auto scoped_model = static_model();
    scoped_model.textures[0].workspaceFileIndex = 0U;
    scoped_model.textures.push_back(scoped_model.textures.front());
    scoped_model.textures[1].workspaceFileIndex = 1U;
    scoped_model.materials[0].workspaceFileIndex = 1U;
    const auto scoped = apex::render::build_draw_packets(scoped_model, static_scene(), one_item_plan());
    require(scoped.packets.size() == 1 && scoped.packets.front().resources.size() == 1 &&
                scoped.packets.front().resources.front().texture_index == 1U,
            "workspace scope disambiguates equal texture names");

    auto empty_model = static_model();
    empty_model.materials[0].resources[0].texture.clear();
    const auto empty = apex::render::build_draw_packets(empty_model, static_scene(), one_item_plan());
    bool missing_texture_warning = false;
    for (const auto& diagnostic : empty.diagnostics)
        missing_texture_warning = missing_texture_warning || diagnostic.code == "MISSING_TEXTURE";
    require(empty.packets.size() == 1 && empty.packets.front().resources.size() == 1 &&
                empty.packets.front().resources.front().texture_index == apex::render::invalid_draw_texture_index &&
                missing_texture_warning, "empty texture names remain staged with a warning");

    auto invalid_plan = one_item_plan(true);
    invalid_plan.items[0].distance = std::numeric_limits<float>::quiet_NaN();
    const auto invalid_order = apex::render::build_draw_packets(static_model(), static_scene(), invalid_plan);
    require(invalid_order.packets.empty() && !invalid_order.supported, "non-finite ordering distance is rejected");
}

}  // namespace

int main() {
    try {
        builds_static_packet_with_stock_state_and_ranges();
        preserves_deterministic_transparent_order_and_unknown_shader_diagnostic();
        cpu_skinning_matches_reference_transform_and_validates_influences();
        builds_validated_skinned_packet_with_bone_palette();
        validates_layout_resource_identity_and_metadata_limits();
        cpu_skinning_matches_rotation_nonuniform_scale_and_inverse_bind();
        rejects_invalid_geometry_and_limits_before_packet_growth();
        rejects_nonfinite_scene_and_ambiguous_resource_inputs();
        std::cout << "draw packet tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "draw packet tests failed: " << error.what() << '\n';
        return 1;
    }
}
