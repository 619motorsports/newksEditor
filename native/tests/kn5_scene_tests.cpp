#include <apex/scene/kn5_scene.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apex::formats::Kn5File;
using apex::formats::Kn5Material;
using apex::formats::Kn5Node;
using apex::scene::Kn5SceneError;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Kn5Node node(std::string name, std::array<float, 16> transform = {
                 1, 0, 0, 0, 0, 1, 0, 0,
                 0, 0, 1, 0, 0, 0, 0, 1}) {
    Kn5Node result;
    result.type = 1;
    result.kind = "node";
    result.name = std::move(name);
    result.transform = transform;
    result.active = true;
    return result;
}

Kn5Node mesh(std::string name, bool skinned = false) {
    Kn5Node result;
    result.type = skinned ? 3u : 2u;
    result.kind = skinned ? "skinnedMesh" : "mesh";
    result.name = std::move(name);
    result.active = true;
    result.visible = true;
    result.renderable = true;
    result.castShadows = true;
    result.transparent = true;
    result.materialId = 0;
    result.layer = 4;
    result.lodIn = 1;
    result.lodOut = 100;
    result.vertexStride = skinned ? 19 : 11;
    result.vertices.resize(result.vertexStride * 3, 0.0f);
    result.indices = {0, 1, 2};
    result.bounds = {1, 0, 0, 2};
    if (skinned) {
        apex::formats::Kn5Bone bone;
        bone.name = "root";
        bone.transform = {1, 0, 0, 0, 0, 1, 0, 0,
                          0, 0, 1, 0, 0, 0, 0, 1};
        result.bones.push_back(bone);
    }
    return result;
}

Kn5File fixture() {
    Kn5File model;
    model.source = "synthetic.kn5";
    Kn5Material material;
    material.name = "glass";
    material.shader = "ksPerPixel";
    material.blendMode = 1;
    model.materials.push_back(material);
    model.root = node("root", {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 10, 0, 0, 1});
    auto child = node("branch", {1, 0, 0, 0, 0, 1, 0, 0,
                                  0, 0, 1, 0, 0, 2, 0, 1});
    child.children.push_back(mesh("static"));
    child.children.push_back(mesh("skin", true));
    model.root.children.push_back(std::move(child));
    return model;
}

Kn5Node deepChain(std::size_t remaining) {
    Kn5Node result = node("deep");
    if (remaining != 0) result.children.push_back(deepChain(remaining - 1));
    return result;
}

void test_world_conversion_and_metadata() {
    const auto conversion = apex::scene::convertKn5Scene(fixture());
    const auto& snapshot = conversion.snapshot;
    require(snapshot.root == 0 && snapshot.nodes.size() == 4, "preorder node IDs");
    require(snapshot.nodes[0].children.size() == 1 && snapshot.nodes[1].children[0] == 2,
            "parent child links");
    require(snapshot.nodes[2].kind == apex::scene::NodeKind::mesh &&
                snapshot.nodes[3].kind == apex::scene::NodeKind::skinned_mesh,
            "static and skinned kinds");
    require(snapshot.nodes[2].transform[12] == 10 && snapshot.nodes[2].transform[13] == 2,
            "hierarchical world transform");
    require(snapshot.nodes[2].bounds_center[0] == 11 && snapshot.nodes[2].bounds_center[1] == 2,
            "world bounds center");
    require(snapshot.nodes[2].bounds_radius == 2, "world bounds radius");
    require(snapshot.nodes[2].local_bounds_center == apex::scene::Vector3{1.0F, 0.0F, 0.0F} &&
                snapshot.nodes[2].local_bounds_radius == 2.0F &&
                snapshot.nodes[2].local_bounds_source ==
                    apex::scene::LocalBoundsSource::kn5_serialized,
            "serialized mesh keeps native local bounds");
    require(snapshot.nodes[2].local_aabb_center ==
                std::optional<apex::scene::Vector3>{
                    apex::scene::Vector3{0.0F, 0.0F, 0.0F}},
            "static mesh keeps its exact vertex-AABB center separately");
    require(snapshot.nodes[3].local_bounds_center == apex::scene::Vector3{0.0F, 0.0F, 0.0F} &&
                snapshot.nodes[3].local_bounds_radius == 0.0F &&
                snapshot.nodes[3].local_bounds_source ==
                    apex::scene::LocalBoundsSource::kn5_vertex_mean,
            "skinned mesh keeps recovered vertex-mean bounds");
    require(snapshot.nodes[2].material == 0 && snapshot.nodes[2].transparent &&
                snapshot.nodes[2].cast_shadows && snapshot.nodes[2].layer == 4 &&
                snapshot.nodes[2].lod_in == 1 && snapshot.nodes[2].lod_out == 100,
            "mesh render metadata");
    require(snapshot.materials[0].blend_mode == apex::scene::BlendMode::alpha_blend,
            "material profile blend mode");
    require(conversion.geometry.size() == 2 && conversion.geometry[0].node == 2,
            "geometry metadata node IDs");
    require(conversion.geometry[0].vertex_count == 3 && conversion.geometry[0].vertex_stride == 11 &&
                !conversion.geometry[0].skinned && conversion.geometry[1].skinned &&
                conversion.geometry[1].bone_count == 1,
            "geometry metadata layout");
    require(snapshot.bounds_radius > 12.0f, "scene bounds extent");
}

void test_ids_are_repeatable() {
    const auto first = apex::scene::convertKn5Scene(fixture());
    const auto second = apex::scene::convertKn5Scene(fixture());
    require(first.snapshot.nodes.size() == second.snapshot.nodes.size(), "stable node count");
    for (std::size_t index = 0; index < first.snapshot.nodes.size(); ++index) {
        require(first.snapshot.nodes[index].id == second.snapshot.nodes[index].id &&
                    first.snapshot.nodes[index].name == second.snapshot.nodes[index].name,
                "stable preorder IDs");
    }
}

void test_skinned_bounds_use_native_vertex_mean() {
    auto model = fixture();
    auto& skin = model.root.children[0].children[1];
    skin.vertices[0] = 0.0F;
    skin.vertices[skin.vertexStride] = 3.0F;
    skin.vertices[skin.vertexStride * 2U] = 3.0F;

    const auto conversion = apex::scene::convertKn5Scene(model);
    const auto& node = conversion.snapshot.nodes[3];
    require(node.local_bounds_center == apex::scene::Vector3{2.0F, 0.0F, 0.0F},
            "skinned local sphere center is arithmetic vertex mean");
    require(node.local_aabb_center ==
                std::optional<apex::scene::Vector3>{
                    apex::scene::Vector3{1.5F, 0.0F, 0.0F}},
            "skinned vertex-AABB center remains distinct from its native sphere");
    require(node.local_bounds_radius == 2.0F,
            "skinned local sphere radius is maximum distance from mean");
    require(node.bounds_center == apex::scene::Vector3{12.0F, 2.0F, 0.0F} &&
                node.bounds_radius == 2.0F,
            "skinned native sphere retains existing world-bounds contract");
}

void test_preview_bounds_follow_transformed_authored_visibility() {
    Kn5File model;
    model.materials.push_back(fixture().materials.front());
    model.root = node("root", {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 10, 2, -3, 1});

    auto visible = mesh("visible");
    const std::array<std::array<float, 3>, 3> positions = {
        std::array<float, 3>{-2.0F, 1.0F, 0.0F},
        std::array<float, 3>{4.0F, 5.0F, 2.0F},
        std::array<float, 3>{0.0F, -1.0F, 6.0F}};
    for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
        for (std::size_t axis = 0U; axis < 3U; ++axis)
            visible.vertices[vertex * visible.vertexStride + axis] = positions[vertex][axis];
    }
    model.root.children.push_back(std::move(visible));

    auto hidden = mesh("hidden");
    hidden.visible = false;
    hidden.vertices[0] = -1000.0F;
    model.root.children.push_back(std::move(hidden));

    auto not_renderable = mesh("not-renderable");
    not_renderable.renderable = false;
    not_renderable.vertices[1] = 1000.0F;
    model.root.children.push_back(std::move(not_renderable));

    auto inactive = node("inactive");
    inactive.active = false;
    auto inactive_mesh = mesh("inactive-mesh");
    inactive_mesh.vertices[0] = 1000.0F;
    inactive.children.push_back(std::move(inactive_mesh));
    model.root.children.push_back(std::move(inactive));

    const auto conversion = apex::scene::convertKn5Scene(model);
    require(conversion.snapshot.nodes[1U].local_aabb_center ==
                std::optional<apex::scene::Vector3>{
                    apex::scene::Vector3{1.0F, 2.0F, 3.0F}},
            "per-mesh vertex-AABB center is retained for live transparent ordering");
    require(conversion.preview_bounds.has_value(), "visible preview bounds exist");
    const auto& bounds = *conversion.preview_bounds;
    require(bounds.minimum == apex::scene::Vector3({8.0F, 1.0F, -3.0F}) &&
                bounds.maximum == apex::scene::Vector3({14.0F, 7.0F, 3.0F}) &&
                bounds.center == apex::scene::Vector3({11.0F, 4.0F, 0.0F}),
            "preview bounds use transformed visible vertices only");
    require(std::abs(bounds.radius - std::sqrt(27.0F)) < 1e-5F,
            "preview radius is the transformed AABB half diagonal");

    model.root.active = false;
    const auto inactive_conversion = apex::scene::convertKn5Scene(model);
    require(!inactive_conversion.preview_bounds.has_value(),
            "inactive hierarchy has no preview bounds");
}

template <typename Function>
void requireError(Function&& function, std::string_view context) {
    try {
        function();
    } catch (const Kn5SceneError&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(std::string(context) + ": invalid scene was accepted");
}

void test_invalid_references_and_boundaries() {
    auto badMaterial = fixture();
    badMaterial.root.children[0].children[0].materialId = 3;
    requireError([&] { (void)apex::scene::convertKn5Scene(badMaterial); }, "material reference");

    auto badIndex = fixture();
    badIndex.root.children[0].children[0].indices[1] = 99;
    requireError([&] { (void)apex::scene::convertKn5Scene(badIndex); }, "index reference");

    auto badStride = fixture();
    badStride.root.children[0].children[0].vertexStride = 10;
    requireError([&] { (void)apex::scene::convertKn5Scene(badStride); }, "vertex stride");

    auto shortPositionStride = fixture();
    shortPositionStride.root.children[0].children[0].vertexStride = 2;
    shortPositionStride.root.children[0].children[0].vertices.resize(6U);
    requireError(
        [&] { (void)apex::scene::convertKn5Scene(shortPositionStride); },
        "vertex stride shorter than a position");

    auto badMatrix = fixture();
    badMatrix.root.transform[0] = std::numeric_limits<float>::quiet_NaN();
    requireError([&] { (void)apex::scene::convertKn5Scene(badMatrix); }, "non-finite transform");

    auto badBounds = fixture();
    badBounds.root.children[0].children[0].bounds[3] = -1;
    requireError([&] { (void)apex::scene::convertKn5Scene(badBounds); }, "negative bounds");

    auto tooDeep = Kn5File{};
    tooDeep.materials.push_back(fixture().materials[0]);
    tooDeep.root = deepChain(1026);
    requireError([&] { (void)apex::scene::convertKn5Scene(tooDeep); }, "hierarchy depth");
}

void test_conversion_limits_before_snapshot_allocation() {
    auto model = fixture();
    model.materials.push_back(model.materials.front());
    apex::scene::Kn5SceneLimits material_limits;
    material_limits.max_materials = 1;
    requireError([&] { (void)apex::scene::convertKn5Scene(model, material_limits); },
                 "material conversion limit");

    apex::scene::Kn5SceneLimits node_limits;
    node_limits.max_nodes = 3;
    requireError([&] { (void)apex::scene::convertKn5Scene(fixture(), node_limits); },
                 "node conversion limit");

    apex::scene::Kn5SceneLimits depth_limits;
    depth_limits.max_depth = 1;
    requireError([&] { (void)apex::scene::convertKn5Scene(fixture(), depth_limits); },
                 "depth conversion limit");
}

void test_conversion_aggregate_budget_and_string_limits() {
    auto valid = fixture();
    apex::scene::Kn5SceneLimits bounded;
    bounded.max_native_object_bytes = 1U << 20U;
    bounded.max_string_bytes = 64U;
    const auto converted = apex::scene::convertKn5Scene(valid, bounded);
    require(converted.snapshot.nodes.size() == 4U, "bounded valid scene remains supported");

    auto zero_budget = fixture();
    apex::scene::Kn5SceneLimits zero;
    zero.max_native_object_bytes = 0U;
    requireError([&] { (void)apex::scene::convertKn5Scene(zero_budget, zero); },
                 "zero scene conversion budget");

    auto oversized = fixture();
    oversized.materials[0].name.assign(65U, 'm');
    requireError([&] { (void)apex::scene::convertKn5Scene(oversized, bounded); },
                 "oversized scene string");

    auto many = Kn5File{};
    many.materials.push_back(fixture().materials.front());
    many.root = node("root");
    for (std::size_t index = 0U; index < 32U; ++index)
        many.root.children.push_back(node("n"));
    apex::scene::Kn5SceneLimits low_budget;
    low_budget.max_native_object_bytes = 4096U;
    low_budget.max_string_bytes = 64U;
    requireError([&] { (void)apex::scene::convertKn5Scene(many, low_budget); },
                 "many-small-node aggregate budget");

    apex::scene::Kn5SceneLimits record_budget;
    record_budget.max_native_object_bytes = sizeof(apex::scene::SceneNode) - 1U;
    requireError([&] { (void)apex::scene::convertKn5Scene(fixture(), record_budget); },
                 "checked record budget boundary");
}

void test_malformed_input_is_rejected_during_preflight() {
    auto malformed = fixture();
    malformed.root.children[0].children[0].vertexStride = 10U;
    requireError([&] { (void)apex::scene::convertKn5Scene(malformed); },
                 "malformed geometry preflight");

    auto invalid_child = fixture();
    Kn5Node bad_child;
    bad_child.type = 99U;
    invalid_child.root.children[0].children[0].children.push_back(std::move(bad_child));
    requireError([&] { (void)apex::scene::convertKn5Scene(invalid_child); },
                 "malformed mesh child preflight");
}

void test_truncated_kn5_is_rejected_before_scene_conversion() {
    const std::vector<std::uint8_t> truncated = {'s', 'c', '6', '9', '6', '9'};
    try {
        const auto parsed = apex::formats::parseKn5(
            std::span<const std::uint8_t>(truncated.data(), truncated.size()),
            "truncated-scene.kn5");
        (void)apex::scene::convertKn5Scene(parsed);
    } catch (const apex::formats::Kn5Error&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("truncated KN5: wrong exception: ") + error.what());
    }
    throw std::runtime_error("truncated KN5 was accepted");
}

}  // namespace

int main() {
    try {
        test_world_conversion_and_metadata();
        test_ids_are_repeatable();
        test_skinned_bounds_use_native_vertex_mean();
        test_preview_bounds_follow_transformed_authored_visibility();
        test_invalid_references_and_boundaries();
        test_conversion_limits_before_snapshot_allocation();
        test_conversion_aggregate_budget_and_string_limits();
        test_malformed_input_is_rejected_during_preflight();
        test_truncated_kn5_is_rejected_before_scene_conversion();
        std::cout << "KN5 scene tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KN5 scene tests failed: " << error.what() << '\n';
        return 1;
    }
}
