#include "apex/authoring/project_bake.hpp"
#include "apex/formats/kn5_bake.hpp"
#include "apex/formats/kn5_write.hpp"

#include <array>
#include <bit>
#include <cmath>
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
using apex::authoring::MaterialEdit;
using apex::authoring::MaterialResource;
using apex::authoring::MaterialVector;
using apex::authoring::NodeEdit;
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
    MaterialEdit material;
    material.scalars["shader"] = std::string("ksPerPixelNM");
    material.scalars["blendMode"] = std::string("0x1");
    material.scalars["depthMode"] = std::string("7");
    material.scalars["cullMode"] = std::string("NONE");
    material.scalars["ksDiffuse"] = 0.25F;
    material.scalars["useDetail"] = 1.0F;
    material.scalars["detailUVMultiplier"] = std::string("2.5");
    MaterialVector vector;
    vector.values = {2.0F, 3.0F, 0.0F, 0.0F};
    vector.components = 2u;
    material.vectors["detailScale"] = vector;
    material.resources["txDiffuse"].texture = "body.dds";
    material.resources["txNormal"].file = "textures/body_nm.dds";
    material.resources["txMaps"].color = std::array<float, 4>{1.0F, 0.5F, 0.25F, 1.0F};
    state.materials.emplace("Body", material);
    NodeEdit node;
    node.name = "CHASSIS";
    node.active = false;
    node.transform = apex::authoring::Matrix4{1, 0, 0, 0, 0, 1, 0, 0,
                                              0, 0, 1, 0, 4, 5, 6, 1};
    state.nodes.emplace("root", node);
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
    GeometryBaselines baselines;
    baselines.emplace("0", baseline());

    const auto result = buildKn5BakeProject(state, baselines);
    require(result.materials.size() == 1u && result.nodes.size() == 1u &&
                result.meshes.size() == 1u && result.geometry.size() == 1u &&
                result.baselines.size() == 1u && result.warnings.empty(),
            "project bake output counts");
    const auto& mappedMaterial = result.materials.at("Body");
    require(mappedMaterial.shader == "ksPerPixelNM" && mappedMaterial.blend_mode == 1u &&
                mappedMaterial.depth_mode == 7u && mappedMaterial.cull_mode == "NONE" &&
                mappedMaterial.properties.at("ksDiffuse") == std::vector<float>({0.25F}) &&
                mappedMaterial.properties.at("useDetail") == std::vector<float>({1.0F}) &&
                mappedMaterial.properties.at("detailUVMultiplier") ==
                    std::vector<float>({2.5F}) &&
                mappedMaterial.properties.at("detailScale") ==
                    std::vector<float>({2.0F, 3.0F}),
            "project bake preserves material state and property values");
    require(mappedMaterial.resources.at("txDiffuse").texture == "body.dds" &&
                mappedMaterial.resources.at("txNormal").file == "textures/body_nm.dds" &&
                mappedMaterial.resources.at("txMaps").color ==
                    std::array<float, 4>{1.0F, 0.5F, 0.25F, 1.0F},
            "project bake preserves embedded and CSP-only material resources");
    const auto& mappedNode = result.nodes.at("root");
    require(mappedNode.name == "CHASSIS" && mappedNode.active.has_value() &&
                !*mappedNode.active && mappedNode.transform->at(12) == 4.0F,
            "project bake preserves node edit optionals");
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
    state.materials.at("Body").scalars["shader"] = std::string("MUTATED");
    state.nodes.at("root").name = "MUTATED";
    baselines.at("0").vertices[0] = 99.0F;
    require(result.materials.at("Body").shader == "ksPerPixelNM" &&
                result.nodes.at("root").name == "CHASSIS" &&
                *result.meshes.at("BODY").layer == 0u &&
                result.baselines.at("0").vertices[0] == 0.0F,
            "project bake output does not alias caller state");
}

void rejectsMalformedStateBeforeCopy() {
    ProjectState state;
    state.materials["Body"] = {};
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_edit");

    state = {};
    state.materials["Body"].scalars["shader"] = 1.0F;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "field_type");

    state = {};
    state.materials["Body"].scalars["useDetail"] = true;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "field_type");

    state = {};
    state.materials["Body"].vectors["bad"].components = 1u;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "vector_size");

    state = {};
    state.materials["Body"].scalars["same"] = 1.0F;
    state.materials["Body"].vectors["SAME"].components = 2u;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "ambiguous_key");

    state = {};
    state.materials["Body"].resources["txDiffuse"] = MaterialResource{};
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "resource_value");

    state = {};
    state.materials["Body"].resources["txDiffuse"].texture = "body.dds";
    state.materials["Body"].resources["txDiffuse"].file = "body.png";
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "resource_value");

    state = {};
    state.materials["Body"].resources["txDiffuse"].clear = true;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "resource_clear");

    state = {};
    state.nodes["root"] = {};
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_edit");

    state = {};
    state.nodes["01"].active = true;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "invalid_path");

    state = {};
    state.nodes["root"].name = "";
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_string");

    state = {};
    state.nodes["root"].transform = apex::authoring::Matrix4{};
    (*state.nodes["root"].transform)[0] = std::numeric_limits<float>::quiet_NaN();
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "non_finite");

    state = {};
    state.meshes["BODY"].lodIn = std::numeric_limits<float>::quiet_NaN();
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "non_finite");

    state = {};
    state.meshes["BODY"] = {};
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_edit");

    state = {};
    state.meshes["BODY"].layer = 1u;
    state.meshes["body"].layer = 2u;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "ambiguous_key");

    state = {};
    state.meshes[""].layer = 1u;
    expectsError([&] { (void)buildKn5BakeProject(state, {}); }, "empty_string");

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
    limits.maxMaterialEdits = 0u;
    ProjectState materialState;
    materialState.materials["Body"].scalars["shader"] = std::string("ksPerPixel");
    expectsError([&] { (void)buildKn5BakeProject(materialState, {}, limits); },
                 "material_limit");
    limits = {};
    limits.maxMaterialFields = 0u;
    expectsError([&] { (void)buildKn5BakeProject(materialState, {}, limits); },
                 "output_limit");
    limits = {};
    limits.maxMaterialResources = 0u;
    ProjectState resourceState;
    resourceState.materials["Body"].resources["txDiffuse"].texture = "body.dds";
    expectsError([&] { (void)buildKn5BakeProject(resourceState, {}, limits); },
                 "output_limit");
    limits = {};
    limits.maxNodeEdits = 0u;
    ProjectState nodeState;
    nodeState.nodes["root"].active = false;
    expectsError([&] { (void)buildKn5BakeProject(nodeState, {}, limits); }, "node_limit");
    limits = {};
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
    material.resources.push_back({"txDiffuse", 21u, "body.dds"});
    file.materials.push_back(std::move(material));
    apex::formats::Kn5Texture texture;
    texture.active = true;
    texture.name = "body.dds";
    texture.size = 3u;
    texture.data = {1u, 2u, 3u};
    file.textures.push_back(std::move(texture));
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
    state.materials["body"].scalars["shader"] = std::string("ksPerPixelNM");
    state.materials["body"].scalars["blendMode"] = std::string("1");
    state.materials["body"].scalars["depthMode"] = std::string("7");
    state.materials["body"].scalars["ksDiffuse"] = 0.25F;
    MaterialVector detailScale;
    detailScale.values = {2.0F, 3.0F, 0.0F, 0.0F};
    detailScale.components = 2u;
    state.materials["body"].vectors["detailScale"] = detailScale;
    state.materials["body"].resources["txDiffuse"].texture = "BODY.DDS";
    state.nodes["root"].name = "CHASSIS";
    state.nodes["root"].active = false;
    state.nodes["root"].transform = apex::authoring::Matrix4{1, 0, 0, 0, 0, 1, 0, 0,
                                                             0, 0, 1, 0, 2, 3, 4, 1};
    state.meshes["body"].transparent = true;
    state.meshes["body"].layer = 7u;
    state.geometry["0"].reverse_winding = true;
    GeometryBaselines baselines;
    baselines["0"] = baseline();

    const auto project = buildKn5BakeProject(state, baselines);
    const auto baked = apex::formats::bakeKn5(bakeFixture(), project);
    require(baked.applied.materials == 1u && baked.applied.properties == 2u &&
                baked.applied.resources == 1u && baked.applied.nodes == 1u &&
                baked.applied.meshes == 1u &&
                baked.applied.geometry == 1u && !baked.model.root.active &&
                baked.model.materials[0].shader == "ksPerPixelNM" &&
                baked.model.materials[0].blendMode == 1u &&
                baked.model.materials[0].depthMode == 7u &&
                baked.model.materials[0].resources[0].texture == "body.dds" &&
                baked.model.materials[0].resources[0].textureId == 21u &&
                baked.model.root.name == "CHASSIS" && baked.model.root.transform[12] == 2.0F &&
                baked.model.root.children[0].transparent &&
                baked.model.root.children[0].layer == 7u &&
                baked.model.root.children[0].indices == std::vector<std::uint16_t>({0u, 0u, 1u}),
            "project-state adapter feeds material, node, mesh, and geometry edits into the production KN5 bake");

    const auto reparsed = apex::formats::parseKn5(apex::formats::serializeKn5(baked.model));
    require(reparsed.materials[0].shader == "ksPerPixelNM" &&
                reparsed.materials[0].blendMode == 1u &&
                reparsed.materials[0].depthMode == 7u &&
                reparsed.materials[0].properties.size() == 2u &&
                reparsed.materials[0].properties[0].value2 ==
                    std::array<float, 2>{2.0F, 3.0F} &&
                reparsed.materials[0].properties[1].value == 0.25F &&
                reparsed.materials[0].resources[0].texture == "body.dds" &&
                reparsed.materials[0].resources[0].textureId == 21u &&
                reparsed.root.name == "CHASSIS" && !reparsed.root.active &&
                reparsed.root.transform[12] == 2.0F &&
                reparsed.root.children[0].transparent &&
                reparsed.root.children[0].layer == 7u,
            "project-state material and node edits survive KN5 serialization");
}

void matchesJavaScriptNumberConversion() {
    ProjectState state;
    state.materials["Body"].scalars["blendMode"] = std::string("0x");
    state.materials["Body"].scalars["depthMode"] = std::string("0b");
    state.materials["Body"].scalars["asciiSpace"] = std::string(" ");
    state.materials["Body"].scalars["empty"] = std::string("");
    state.materials["Body"].scalars["unicodeSpace"] = std::string("\xc2\xa0");
    state.materials["Body"].scalars["badOctal"] = std::string("0o");
    state.materials["Body"].scalars["badHexDigit"] = std::string("0xg");
    state.materials["Body"].scalars["underflow"] = std::string("1e-324");
    state.materials["Body"].scalars["negativeUnderflow"] = std::string("-1e-324");
    state.materials["Body"].scalars["overflow"] = std::string("1e309");

    const auto project = buildKn5BakeProject(state, {});
    const auto& material = project.materials.at("Body");
    require(project.warnings.size() == 2u && !material.blend_mode &&
                !material.depth_mode &&
                material.properties.at("asciiSpace") == std::vector<float>({0.0F}) &&
                material.properties.at("empty") == std::vector<float>({0.0F}) &&
                material.properties.at("unicodeSpace") == std::vector<float>({0.0F}) &&
                material.properties.at("badOctal").empty(),
            "project bake follows finite JavaScript Number conversion");
    require(material.properties.at("badHexDigit").empty() &&
                material.properties.at("underflow") == std::vector<float>({0.0F}) &&
                material.properties.at("negativeUnderflow").size() == 1u &&
                std::signbit(material.properties.at("negativeUnderflow")[0]) &&
                material.properties.at("overflow").empty(),
            "project bake distinguishes decimal underflow from overflow");
}

void preservesMaterialLossDiagnostics() {
    ProjectState state;
    state.materials["Body"].scalars["blendMode"] = std::string("ALPHA_BLEND");
    state.materials["Body"].scalars["depthMode"] = std::string("READ_ONLY");
    state.materials["Body"].scalars["cullMode"] = std::string("NONE");
    state.materials["Body"].scalars["ksDiffuse"] = std::string("ORIGINAL");
    state.materials["Body"].resources["txNormal"].file = "textures/body_nm.dds";

    const auto project = buildKn5BakeProject(state, {});
    require(project.warnings.size() == 2u &&
                project.materials.at("Body").properties.at("ksDiffuse").empty(),
            "adapter retains unsupported mode and property diagnostics");
    const auto baked = apex::formats::bakeKn5(bakeFixture(), project);
    require(baked.warnings.size() == 5u && baked.applied.materials == 0u,
            "production bake returns adapter and CSP-only material warnings");
}

} // namespace

int main() {
    try {
        mapsOnlySupportedStateExactly();
        rejectsMalformedStateBeforeCopy();
        enforcesCopyLimits();
        adapterFeedsProductionBakeAuthority();
        matchesJavaScriptNumberConversion();
        preservesMaterialLossDiagnostics();
        std::cout << "project bake tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project bake tests failed: " << error.what() << '\n';
        return 1;
    }
}
