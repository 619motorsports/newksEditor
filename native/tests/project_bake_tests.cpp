#include "apex/authoring/project_bake.hpp"
#include "apex/formats/kn5_bake.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using apex::authoring::GeometryBaseline;
using apex::authoring::GeometryEdit;
using apex::authoring::GeometryBaselines;
using apex::authoring::MeshEdit;
using apex::authoring::ProjectBakeError;
using apex::authoring::ProjectBakeLimits;
using apex::authoring::ProjectState;
using apex::authoring::buildKn5BakeProject;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void expectsError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const ProjectBakeError& error) {
        require(error.code() == code, "unexpected project bake error code");
        return;
    }
    throw std::runtime_error("expected project bake error");
}

GeometryBaseline baseline() {
    GeometryBaseline result;
    result.vertices.resize(22u, 0.0F);
    result.vertices[5] = 1.0F;
    result.vertices[8] = std::bit_cast<float>(0xff0080ffu);
    result.vertices[16] = 1.0F;
    result.vertices[19] = std::bit_cast<float>(0xff0080ffu);
    result.indices = {0u, 1u, 0u};
    result.bounds = std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F};
    return result;
}

void mapsOnlySupportedStateExactly() {
    ProjectState state;
    MeshEdit mesh;
    mesh.transparent = false;
    mesh.castShadows = false;
    mesh.layer = 0u;
    mesh.lodIn = 0.0F;
    mesh.lodOut = 25.0F;
    state.meshes.emplace("BODY", mesh);
    GeometryEdit geometry;
    geometry.remove_degenerate = true;
    geometry.reverse_winding = false;
    geometry.recalculate_normals = true;
    geometry.transform = apex::authoring::Matrix4{1, 0, 0, 0, 0, 1, 0, 0,
                                                  0, 0, 1, 0, 2, 3, 4, 1};
    state.geometry.emplace("0", geometry);
    state.nodes.emplace("ignored", apex::authoring::NodeEdit{});
    GeometryBaselines baselines;
    baselines.emplace("0", baseline());

    const auto result = buildKn5BakeProject(state, baselines);
    require(result.meshes.size() == 1u && result.geometry.size() == 1u &&
                result.baselines.size() == 1u, "project bake output counts");
    const auto& mappedMesh = result.meshes.at("BODY");
    require(mappedMesh.transparent.has_value() && !*mappedMesh.transparent &&
                mappedMesh.cast_shadows.has_value() && !*mappedMesh.cast_shadows &&
                mappedMesh.layer.has_value() && *mappedMesh.layer == 0u &&
                mappedMesh.lod_in.has_value() && *mappedMesh.lod_in == 0.0F &&
                mappedMesh.lod_out.has_value() && *mappedMesh.lod_out == 25.0F,
            "project bake preserves false and zero optionals");
    const auto& mappedGeometry = result.geometry.at("0");
    require(mappedGeometry.remove_degenerate && !mappedGeometry.reverse_winding &&
                mappedGeometry.recalculate_normals && mappedGeometry.transform->at(12) == 2.0F,
            "project bake preserves geometry edit schema");
    require(result.baselines.at("0").vertices == baselines.at("0").vertices &&
                result.baselines.at("0").indices == baselines.at("0").indices,
            "project bake copies caller-owned baselines");

    state.meshes.at("BODY").layer = 99u;
    baselines.at("0").vertices[0] = 99.0F;
    require(*result.meshes.at("BODY").layer == 0u && result.baselines.at("0").vertices[0] == 0.0F,
            "project bake output does not alias caller state");
}

void rejectsMalformedStateBeforeCopy() {
    ProjectState state;
    state.meshes["BODY"].lodIn = std::numeric_limits<float>::quiet_NaN();
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "non_finite");

    state = {};
    state.meshes["BODY"] = {};
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_edit");

    state = {};
    GeometryEdit edit;
    edit.transform = apex::authoring::Matrix4{};
    (*edit.transform)[0] = std::numeric_limits<float>::infinity();
    state.geometry["0"] = edit;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "non_finite");

    state = {};
    state.geometry["0"] = {};
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_edit");

    state = {};
    GeometryBaselines bad;
    bad["0"].vertices = {1.0F};
    expectsError([&] { (void)buildKn5BakeProject(state, bad); }, "vertex_stride");
    bad = {};
    bad["0"] = baseline();
    bad["0"].indices = {0u, 1u, 4u};
    expectsError([&] { (void)buildKn5BakeProject(state, bad); }, "invalid_index");
}

void enforcesCopyLimits() {
    ProjectState state;
    state.meshes["BODY"].layer = 1u;
    ProjectBakeLimits limits;
    limits.maxMeshEdits = 0u;
    expectsError([&] { (void)buildKn5BakeProject(state, {}, limits); }, "mesh_limit");
    limits = {};
    limits.maxMeshEdits = 0u;
    require(buildKn5BakeProject({}, {}, limits).meshes.empty(),
            "zero category limit accepts an empty category");
    limits = {};
    limits.maxBaselineBytes = 8u;
    GeometryBaselines baselines;
    baselines["0"] = baseline();
    expectsError([&] { (void)buildKn5BakeProject({}, baselines, limits); }, "output_limit");
    limits = {};
    limits.maxStringBytes = 3u;
    expectsError([&] { (void)buildKn5BakeProject(state, {}, limits); }, "string_limit");
    limits = {};
    limits.maxTotalStringBytes = 5u;
    ProjectState twoNames;
    twoNames.meshes["one"].layer = 1u;
    twoNames.meshes["two"].layer = 2u;
    expectsError([&] { (void)buildKn5BakeProject(twoNames, {}, limits); }, "output_limit");
}

apex::formats::Kn5File bakeFixture() {
    apex::formats::Kn5File file;
    file.magic = "sc6969";
    file.version = 6u;
    apex::formats::Kn5Material material;
    material.name = "Body";
    material.shader = "ksPerPixel";
    file.materials.push_back(std::move(material));
    file.root.type = 1u;
    file.root.kind = "node";
    file.root.name = "root";
    file.root.active = true;
    file.root.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    apex::formats::Kn5Node mesh;
    mesh.type = 2u;
    mesh.kind = "mesh";
    mesh.name = "BODY";
    mesh.vertexStride = 11u;
    mesh.vertices = baseline().vertices;
    mesh.indices = baseline().indices;
    mesh.bounds = {0.0F, 0.0F, 1.0F, 1.0F};
    mesh.materialId = 0u;
    mesh.renderable = true;
    file.root.children.push_back(std::move(mesh));
    return file;
}

void adapterFeedsProductionBakeAuthority() {
    ProjectState state;
    state.meshes["body"].transparent = true;
    state.meshes["body"].layer = 7u;
    state.geometry["0"].reverse_winding = true;
    GeometryBaselines baselines;
    baselines["0"] = baseline();

    const auto project = buildKn5BakeProject(state, baselines);
    const auto baked = apex::formats::bakeKn5(bakeFixture(), project);
    require(baked.applied.meshes == 1u && baked.applied.geometry == 1u &&
                baked.model.root.children[0].transparent &&
                baked.model.root.children[0].layer == 7u &&
                baked.model.root.children[0].indices == std::vector<std::uint16_t>({0u, 0u, 1u}),
            "project-state adapter feeds mesh and geometry edits into the production KN5 bake");
}

} // namespace

int main() {
    try {
        mapsOnlySupportedStateExactly();
        rejectsMalformedStateBeforeCopy();
        enforcesCopyLimits();
        adapterFeedsProductionBakeAuthority();
        std::cout << "project bake tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project bake tests failed: " << error.what() << '\n';
        return 1;
    }
}
