#include "apex/core/parse_error.hpp"
#include "apex/domain/animation_preview.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/kn5_scene_node_map.hpp"
#include "apex/scene/kn5_scene.hpp"

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

void validates_bounded_kn5_scene_node_mapping_directly() {
    apex::formats::Kn5Node source_root;
    source_root.type = 1;
    source_root.kind = "node";
    source_root.name = "ROOT";

    apex::scene::SceneSnapshot scene;
    apex::scene::SceneNode scene_root;
    scene_root.name = "ROOT";
    const auto root_id = scene.add_node(std::move(scene_root));
    auto* source_parent = &source_root;
    auto scene_parent = root_id;
    constexpr std::size_t depth = 8U;
    for (std::size_t index = 0; index < depth; ++index) {
        apex::formats::Kn5Node child;
        child.type = index + 1U == depth ? 2U : 1U;
        child.kind = index + 1U == depth ? "mesh" : "node";
        child.name = "NODE_" + std::to_string(index);
        source_parent->children.push_back(std::move(child));
        source_parent = &source_parent->children.back();

        apex::scene::SceneNode scene_child;
        scene_child.name = source_parent->name;
        scene_child.kind = index + 1U == depth ? apex::scene::NodeKind::mesh
                                                : apex::scene::NodeKind::node;
        const auto child_id = scene.add_node(std::move(scene_child), scene_parent);
        scene_parent = child_id;
    }

    apex::render::Kn5SceneNodeMapLimits limits;
    limits.max_depth = depth;
    limits.max_nodes = depth + 1U;
    limits.max_work_items = depth + 1U;
    const auto valid = apex::render::map_kn5_scene_nodes(source_root, scene, limits);
    require(valid.ok() && valid.source_nodes.size() == scene.nodes.size() &&
                valid.source_nodes.back()->name == "NODE_7",
            "direct source/scene pre-order mapping succeeds");

    auto deep_limits = limits;
    deep_limits.max_depth = 2U;
    const auto deep = apex::render::map_kn5_scene_nodes(source_root, scene, deep_limits);
    require(!deep.ok() && deep.diagnostic.code == "DEPTH_LIMIT" && deep.diagnostic.limit_exceeded,
            "deep source hierarchy is bounded before recursion or allocation");

    auto work_limits = limits;
    work_limits.max_work_items = 4U;
    const auto work = apex::render::map_kn5_scene_nodes(source_root, scene, work_limits);
    require(!work.ok() && work.diagnostic.code == "WORK_LIMIT" && work.diagnostic.limit_exceeded,
            "source traversal work count is bounded");

    auto node_limits = limits;
    node_limits.max_nodes = 4U;
    const auto node_count = apex::render::map_kn5_scene_nodes(source_root, scene, node_limits);
    require(!node_count.ok() && node_count.diagnostic.code == "NODE_LIMIT" &&
                node_count.diagnostic.limit_exceeded,
            "source and scene node counts obey the node limit");

    auto count_scene = scene;
    count_scene.nodes.pop_back();
    const auto count = apex::render::map_kn5_scene_nodes(source_root, count_scene, limits);
    require(!count.ok() && count.diagnostic.code == "SCENE_MODEL_MISMATCH",
            "source and scene node counts must match");

    auto name_scene = scene;
    name_scene.nodes[2].name = "RENAMED";
    const auto name = apex::render::map_kn5_scene_nodes(source_root, name_scene, limits);
    require(!name.ok() && name.diagnostic.code == "SCENE_MODEL_IDENTITY" && name.diagnostic.node == 2U,
            "scene node names must match source pre-order");

    auto kind_scene = scene;
    kind_scene.nodes.back().kind = apex::scene::NodeKind::node;
    const auto kind = apex::render::map_kn5_scene_nodes(source_root, kind_scene, limits);
    require(!kind.ok() && kind.diagnostic.code == "SCENE_MODEL_IDENTITY" &&
                kind.diagnostic.node == static_cast<apex::scene::NodeId>(scene.nodes.size() - 1U),
            "scene node kinds must match source pre-order");

    auto dense_scene = scene;
    dense_scene.nodes[3].id = 99U;
    const auto dense = apex::render::map_kn5_scene_nodes(source_root, dense_scene, limits);
    require(!dense.ok() && dense.diagnostic.code == "INVALID_NODE_ID" && dense.diagnostic.node == 99U,
            "scene node IDs must be dense");

    auto root_scene = scene;
    root_scene.root = 1U;
    const auto invalid_root = apex::render::map_kn5_scene_nodes(source_root, root_scene, limits);
    require(!invalid_root.ok() && invalid_root.diagnostic.code == "INVALID_NODE_ID",
            "scene root must be the first source node");
}

apex::render::RenderPlan one_item_plan(bool transparent = false) {
    apex::render::RenderPlan plan;
    plan.items.push_back({1, 0, 0, 3.0F, transparent, false, true, {}, {}, {0, 1}, {}});
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

void retains_shadow_only_packets_without_color_authority() {
    auto model = static_model();
    model.root.children.push_back(static_mesh("HIDDEN_CASTER", 6));
    auto scene = static_scene();
    apex::scene::SceneNode hidden;
    hidden.name = "HIDDEN_CASTER";
    hidden.kind = apex::scene::NodeKind::mesh;
    hidden.material = 0U;
    hidden.transform = identity(6);
    hidden.renderable = true;
    hidden.visible = false;
    hidden.active = true;
    const auto hidden_id = scene.add_node(std::move(hidden), scene.root);

    auto plan = one_item_plan();
    plan.items.front().source_order = 1U;
    plan.shadow_only_items.push_back(
        {hidden_id, 0U, 0.0, 6.0F, false, false, true, {}, {},
         {scene.root, hidden_id}, {}});
    plan.shadow_only_items.back().source_order = 0U;
    const auto result = apex::render::build_draw_packets(model, scene, plan);
    require(result.supported && result.packets.size() == 2U &&
                !result.packets[0U].shadow_only &&
                result.packets[1U].node == hidden_id &&
                result.packets[1U].shadow_only &&
                result.packets[1U].flags.cast_shadows &&
                result.shadow_packet_order ==
                    std::vector<std::uint32_t>({1U, 0U}),
            "shadow-only geometry retains source order without color authority");

    apex::render::DrawPacketLimits one_packet_limit;
    one_packet_limit.max_packets = 1U;
    const auto limited = apex::render::build_draw_packets(
        model, scene, plan, {}, one_packet_limit);
    require(!limited.supported && limited.limit_exceeded &&
                limited.packets.empty() && !limited.diagnostics.empty() &&
                limited.diagnostics.front().code == "INPUT_LIMIT",
            "shadow-only candidates consume the packet limit before allocation");

    auto duplicate_plan = one_item_plan();
    duplicate_plan.shadow_only_items.push_back(duplicate_plan.items.front());
    const auto duplicate = apex::render::build_draw_packets(
        static_model(), static_scene(), duplicate_plan);
    require(!duplicate.supported && duplicate.packets.empty() &&
                !duplicate.diagnostics.empty() &&
                duplicate.diagnostics.front().code ==
                    "INVALID_SHADOW_ONLY_RENDER_ITEM",
            "duplicate shadow-only packet identity fails before packet creation");

    auto duplicate_order_plan = plan;
    duplicate_order_plan.shadow_only_items.front().source_order = 1U;
    const auto duplicate_order = apex::render::build_draw_packets(
        model, scene, duplicate_order_plan);
    require(!duplicate_order.supported &&
                duplicate_order.shadow_packet_order.empty() &&
                !duplicate_order.diagnostics.empty() &&
                duplicate_order.diagnostics.back().code ==
                    "INVALID_SOURCE_ORDER",
            "duplicate packet source order fails before shadow handoff");

    auto partial_order_plan = plan;
    partial_order_plan.shadow_only_items.front().source_order =
        apex::render::invalid_render_item_source_order;
    const auto partial_order = apex::render::build_draw_packets(
        model, scene, partial_order_plan);
    require(!partial_order.supported &&
                partial_order.shadow_packet_order.empty() &&
                !partial_order.diagnostics.empty() &&
                partial_order.diagnostics.back().code ==
                    "INVALID_SOURCE_ORDER",
            "partial packet source order fails before shadow handoff");
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
    plan.items.push_back({2, 0, 0, 20.0F, true, false, true, {}, {}, {0, 2}, {}});
    plan.items.push_back({1, 0, 0, 3.0F, false, false, true, {}, {}, {0, 1}, {}});
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

void palette_skinning_overload_matches_kn5_helper_and_rejects_malformed_streams() {
    apex::formats::Kn5Node mesh;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19;
    mesh.bones.push_back({"Bone", identity(0)});
    mesh.vertices = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0,
                     1, 0, 0, 0, 0, 0, 0, 0};
    const std::map<std::string, apex::scene::Matrix4> bones = {{"Bone", identity(2)}};
    const auto expected = apex::render::skin_vertices_reference(mesh, bones, identity(10));
    const std::array<apex::scene::Matrix4, 1> palette = {identity(2)};
    const auto actual = apex::render::skin_vertices_reference(mesh.vertices, palette, identity(10));
    require(actual.size() == expected.size(), "palette overload preserves vertex stream size");
    for (std::size_t index = 0; index < actual.size(); ++index)
        require(std::abs(actual[index] - expected[index]) < 1e-6F, "palette overload matches KN5 helper");

    const std::vector<float> truncated(mesh.vertices.begin(), mesh.vertices.end() - 1);
    bool truncated_rejected = false;
    try {
        (void)apex::render::skin_vertices_reference(truncated, palette, identity());
    } catch (const apex::render::DrawPacketError& error) {
        truncated_rejected = error.code() == "INVALID_SKIN_LAYOUT";
    }
    require(truncated_rejected, "truncated-like skin stream is rejected");

    auto nonfinite_palette = palette;
    nonfinite_palette[0][0] = std::numeric_limits<float>::quiet_NaN();
    bool nonfinite_rejected = false;
    try {
        (void)apex::render::skin_vertices_reference(mesh.vertices, nonfinite_palette, identity());
    } catch (const apex::render::DrawPacketError& error) {
        nonfinite_rejected = error.code() == "NON_FINITE_MATRIX";
    }
    require(nonfinite_rejected, "non-finite palette is rejected");

    apex::render::DrawPacketLimits byte_limits;
    byte_limits.max_cpu_skin_bytes = 18U * sizeof(float);
    bool byte_limited = false;
    try {
        (void)apex::render::skin_vertices_reference(mesh.vertices, palette, identity(), byte_limits);
    } catch (const apex::render::DrawPacketError& error) {
        byte_limited = error.code() == "SKIN_BYTE_LIMIT";
    }
    require(byte_limited, "palette overload enforces bounded output bytes");
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

void carries_a_fixed_animation_pose_into_cpu_skinning() {
    auto model = static_model("ksSkinnedMesh");
    model.root.transform = identity();
    auto& mesh = model.root.children.front();
    mesh.type = 3U;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19U;
    mesh.transform = identity(3.0F);
    mesh.vertices = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0,
                     1, 0, 0, 0, 0, 0, 0, 0};
    mesh.indices = {0, 0, 0};
    mesh.bones.push_back({"BONE", identity()});

    apex::formats::Kn5Node bone;
    bone.type = 1U;
    bone.kind = "node";
    bone.name = "BONE";
    bone.active = true;
    bone.transform = identity();
    model.root.children.push_back(std::move(bone));

    apex::formats::KsAnimation animation;
    animation.source = "pose.ksanim";
    animation.version = 2U;
    animation.frameCount = 2U;
    apex::formats::KsAnimationTrack bone_track;
    bone_track.name = "BONE";
    bone_track.animated = true;
    bone_track.frames = {
        {{0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}},
        {{0, 0, 0, 1}, {4, 0, 0}, {1, 1, 1}},
    };
    animation.tracks.push_back(std::move(bone_track));

    const auto animation_bytes = apex::formats::serializeKsAnimation(animation);
    const auto parsed_animation = apex::formats::parseKsAnimation(
        animation_bytes, "pose.ksanim");
    const auto applied = apex::domain::apply_animation_preview(
        model, parsed_animation, 0.25F);
    require(applied.position == 0.25F && applied.matched_nodes == 1U &&
                applied.skinning_required,
            "fixed animation pose requests CPU skinning");

    const auto converted = apex::scene::convertKn5Scene(model);
    const auto packets = apex::render::build_draw_packets(
        model, converted.snapshot, one_item_plan());
    require(packets.packets.size() == 1U &&
                packets.packets[0].bone_palette.size() == 1U &&
                std::abs(packets.packets[0].bone_palette[0][12] - 2.0F) < 1.0e-6F,
            "fixed animation pose reaches the draw packet bone palette");

    const auto& animated_mesh = model.root.children.front();
    const auto skinned = apex::render::skin_vertices_reference(
        animated_mesh.vertices, packets.packets[0].bone_palette,
        packets.packets[0].world_matrix);
    require(skinned.size() == 19U &&
                std::abs(skinned[0] - 2.0F) < 1.0e-6F,
            "fixed animation pose changes the CPU-skinned vertex stream");
}

void keeps_selected_body_packet_on_animated_parent_handoff() {
    auto model = static_model();
    model.root.transform = identity();
    auto body = std::move(model.root.children.front());
    body.transform = identity();
    const auto source_body_name = body.name;
    const auto source_body_material = body.materialId;
    const auto source_body_vertices = body.vertices;
    model.root.children.clear();

    apex::formats::Kn5Node animated_parent;
    animated_parent.type = 1U;
    animated_parent.kind = "node";
    animated_parent.name = "AnimatedParent";
    animated_parent.active = true;
    animated_parent.visible = true;
    animated_parent.transform = identity();
    animated_parent.children.push_back(std::move(body));
    model.root.children.push_back(std::move(animated_parent));

    apex::formats::KsAnimation animation;
    animation.source = "parent.ksanim";
    animation.version = 2U;
    animation.frameCount = 3U;
    apex::formats::KsAnimationTrack parent_track;
    parent_track.name = "AnimatedParent";
    parent_track.animated = true;
    parent_track.frames = {
        {{0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}},
        {{0, 0, 0, 1}, {4, 0, 0}, {1, 1, 1}},
        {{0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}},
    };
    animation.tracks.push_back(std::move(parent_track));

    const auto animation_bytes = apex::formats::serializeKsAnimation(animation);
    const auto parsed_animation = apex::formats::parseKsAnimation(
        animation_bytes, "parent.ksanim");
    require(parsed_animation.version == 2U && parsed_animation.frameCount == 3U &&
                parsed_animation.tracks.size() == 1U &&
                parsed_animation.tracks.front().name == "AnimatedParent",
            "animated parent fixture survives KSANIM v2 serialization and parsing");

    const auto applied = apex::domain::apply_animation_preview(
        model, parsed_animation, 0.5F);
    require(applied.position == 0.5F && applied.matched_nodes == 1U &&
                applied.matched_tracks == 1U,
            "animated parent pose is sampled and applied at the midpoint");
    require(model.root.children.front().transform[12] == 2.0F,
            "midpoint animation updates only the animated parent transform");
    require(model.root.children.front().children.front().name == source_body_name &&
                model.root.children.front().children.front().materialId == source_body_material &&
                model.root.children.front().children.front().vertices == source_body_vertices,
            "animation handoff preserves Body mesh identity and source geometry");

    const auto converted = apex::scene::convertKn5Scene(model);
    apex::scene::NodeId selected_body_id = apex::scene::invalid_node_id;
    const apex::scene::SceneNode* selected_body = nullptr;
    for (const auto& node : converted.snapshot.nodes) {
        if (node.name == "BODY") {
            selected_body_id = node.id;
            selected_body = &node;
            break;
        }
    }
    require(selected_body != nullptr && selected_body_id != apex::scene::invalid_node_id,
            "Body selection resolves to the converted scene node");

    apex::render::RenderPlan plan;
    plan.items.push_back({selected_body_id, selected_body->material, 0, 2.0F,
                          false, false, true, {}, {}, {0, 1, 2}, {}});
    apex::render::DrawPacketOptions options;
    options.selected_node = selected_body_id;
    const auto packets = apex::render::build_draw_packets(
        model, converted.snapshot, plan, options);
    require(packets.supported && packets.packets.size() == 1U,
            "selected Body survives animated-parent packet construction");

    const auto& packet = packets.packets.front();
    require(packet.node == selected_body_id && packet.flags.selected &&
                std::abs(packet.world_matrix[12] - 2.0F) < 1.0e-6F,
            "selected Body packet carries the animated parent world translation");
    require(packet.material == selected_body->material &&
                packet.material_profile.shader == model.materials[packet.material].shader &&
                packet.vertex_count == source_body_vertices.size() / 11U &&
                packet.vertex_stride_floats == 11U,
            "selected Body packet retains stable material and mesh identity");
    require(packet.resources.size() == 1U && packet.resources.front().slot == "txDiffuse" &&
                packet.resources.front().bind_point == 21U &&
                packet.resources.front().texture_index == 0U &&
                packet.resources.front().texture == "body_d.dds",
            "selected Body packet retains stable texture resource identity");
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
        validates_bounded_kn5_scene_node_mapping_directly();
        builds_static_packet_with_stock_state_and_ranges();
        retains_shadow_only_packets_without_color_authority();
        preserves_deterministic_transparent_order_and_unknown_shader_diagnostic();
        cpu_skinning_matches_reference_transform_and_validates_influences();
        palette_skinning_overload_matches_kn5_helper_and_rejects_malformed_streams();
        builds_validated_skinned_packet_with_bone_palette();
        carries_a_fixed_animation_pose_into_cpu_skinning();
        keeps_selected_body_packet_on_animated_parent_handoff();
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
