#include <apex/scene/kn5_scene.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

}  // namespace

int main() {
    try {
        test_world_conversion_and_metadata();
        test_ids_are_repeatable();
        test_invalid_references_and_boundaries();
        test_conversion_limits_before_snapshot_allocation();
        std::cout << "KN5 scene tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KN5 scene tests failed: " << error.what() << '\n';
        return 1;
    }
}
