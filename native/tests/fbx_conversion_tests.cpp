#include "apex/formats/fbx_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using apex::formats::FbxArray;
using apex::formats::FbxConversionError;
using apex::formats::FbxDocument;
using apex::formats::FbxNode;
using apex::formats::FbxProperty;
using apex::formats::FbxValue;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

FbxNode node(std::string name, std::vector<FbxValue> values = {}, std::vector<FbxNode> children = {}) {
    FbxNode result;
    result.name = std::move(name);
    if (!values.empty()) result.properties.push_back({0, std::move(values)});
    result.children = std::move(children);
    return result;
}

FbxNode propertyNode(std::string name, std::vector<FbxValue> values) {
    return node(std::move(name), std::move(values));
}

FbxNode uvLayer(std::string_view mapping = "ByPolygonVertex",
                std::string_view reference = "IndexToDirect",
                FbxArray direct = FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 0.0, 1.0}},
                FbxArray indices = FbxArray{std::vector<std::int64_t>{0, 1, 2}}) {
    return node("LayerElementUV", {}, {
        propertyNode("MappingInformationType", {std::string(mapping)}),
        propertyNode("ReferenceInformationType", {std::string(reference)}),
        propertyNode("UV", {std::move(direct)}),
        propertyNode("UVIndex", {std::move(indices)})});
}

FbxDocument fixture() {
    const FbxNode geometry = node("Geometry", {
        std::int64_t(100), std::string("Geometry::Triangle"), std::string("Mesh")}, {
        propertyNode("Vertices", {FbxArray{std::vector<double>{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0}}}),
        propertyNode("PolygonVertexIndex", {FbxArray{std::vector<std::int64_t>{0, 1, -3}}}),
        propertyNode("LayerElementNormal", {}),
        uvLayer()});
    const FbxNode model = node("Model", {
        std::int64_t(200), std::string("Model::Triangle"), std::string("Mesh")}, {
        node("Properties70", {}, {
            propertyNode("P", {std::string("Lcl Translation"), std::string("Lcl Translation"), std::string(""), std::string("A"), 1.0, 2.0, 3.0}),
            propertyNode("P", {std::string("Lcl Rotation"), std::string("Lcl Rotation"), std::string(""), std::string("A"), 0.0, 0.0, 90.0})})});
    const FbxNode material = node("Material", {
        std::int64_t(300), std::string("Material::Paint"), std::string("Material")}, {
        propertyNode("ShadingModel", {std::string("Phong")})});
    const FbxNode objects = node("Objects", {}, {model, geometry, material});
    const FbxNode connections = node("Connections", {}, {
        node("C", {std::string("OO"), std::int64_t(100), std::int64_t(200)}),
        node("C", {std::string("OO"), std::int64_t(300), std::int64_t(200)})});
    FbxDocument document;
    document.roots = {objects, connections};
    document.header.version = 7400;
    return document;
}

FbxDocument seamFixture() {
    auto document = fixture();
    auto& geometry = document.roots[0].children[1];
    geometry.children[0].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<double>{0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                              1.0, 1.0, 0.0, 0.0, 1.0, 0.0}}};
    geometry.children[1].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<std::int64_t>{0, 1, -3, 0, 2, -4}}};
    geometry.children[3] = uvLayer(
        "ByPolygonVertex", "IndexToDirect",
        FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 1.0, 1.0,
                                     0.25, 0.5, 0.5, 1.0, 0.0, 1.0}},
        FbxArray{std::vector<std::int64_t>{0, 1, 2, 3, 4, 5}});
    return document;
}

std::string asciiTriangle() {
    return "FBXVersion: 7400\nObjects: {\n"
           " Model: 200, \"Model::Triangle\", \"Mesh\" { }\n"
           " Geometry: 100, \"Geometry::Triangle\", \"Mesh\" {\n"
           "  Vertices: *9 {\n"
           "   a: 0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0,0.0\n"
           "  }\n"
           "  PolygonVertexIndex: *3 {\n"
           "   a: 0,1,-3\n"
           "  }\n"
           "  LayerElementUV: 0 {\n"
           "   MappingInformationType: \"ByPolygonVertex\"\n"
           "   ReferenceInformationType: \"IndexToDirect\"\n"
           "   UV: *6 {\n"
           "    a: 0.0,0.0,1.0,0.0,0.0,1.0\n"
           "   }\n"
           "   UVIndex: *3 {\n"
           "    a: 0,1,2\n"
           "   }\n"
           "  }\n"
           " }\n"
           "}\nConnections: {\n C: \"OO\", 100, 200\n}\n";
}

void convertsParsedAsciiTriangle() {
    const auto text = asciiTriangle();
    const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    const auto document = apex::formats::parseFbx(bytes, "ascii-triangle.fbx");
    const auto result = apex::formats::convertFbxScene(document);
    require(result.meshes.size() == 1u && result.meshes[0].positions ==
                std::vector<float>{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F} &&
                result.meshes[0].uvs == std::vector<float>{0.0F, -0.0F, 1.0F, -0.0F, 0.0F, -1.0F} &&
                result.meshes[0].triangle_indices == std::vector<std::uint32_t>{0u, 1u, 2u},
            "parsed ASCII triangle converts to static geometry");
}

void convertsStaticGeometryTransformsAndMaterials() {
    const auto result = apex::formats::convertFbxScene(fixture());
    require(result.snapshot.root == 0u && result.snapshot.nodes.size() == 2u, "FBX synthetic root and model node");
    require(result.snapshot.nodes[1].kind == apex::scene::NodeKind::mesh && result.snapshot.nodes[1].material == 0u,
            "FBX mesh and material assignment");
    require(result.meshes.size() == 1u && result.meshes[0].positions.size() == 9u &&
                result.meshes[0].uvs == std::vector<float>{0.0F, -0.0F, 1.0F, -0.0F, 0.0F, -1.0F} &&
                result.meshes[0].triangle_indices == std::vector<std::uint32_t>{0u, 1u, 2u},
            "FBX static triangle geometry");
    require(result.transforms.size() == 1u && std::abs(result.transforms[0].local[12] - 1.0F) < 1e-6F &&
                std::abs(result.transforms[0].world[13] - 2.0F) < 1e-6F,
            "FBX local and world transforms");
    require(result.snapshot.nodes[1].bounds_radius > 0.7F && result.snapshot.nodes[1].bounds_center[2] > 2.9F && !result.complete,
            "FBX bounds and incomplete capability status");
    require(!result.diagnostics.empty() && result.diagnostics[0].code == "unsupported_layer_mapping",
            "FBX layer mapping diagnostic");
    require(result.snapshot.materials[0].name == "Paint" && result.snapshot.materials[0].shader == "Phong",
            "FBX material metadata");
}

void convertsUvSeamsAndFlipsV() {
    const auto result = apex::formats::convertFbxScene(seamFixture());
    require(result.meshes.size() == 1u && result.meshes[0].positions.size() == 18u &&
                result.meshes[0].uvs == std::vector<float>{0.0F, -0.0F, 1.0F, -0.0F, 1.0F, -1.0F,
                                                            0.25F, -0.5F, 0.5F, -1.0F, 0.0F, -1.0F} &&
                result.meshes[0].triangle_indices == std::vector<std::uint32_t>{0u, 1u, 2u, 3u, 4u, 5u},
            "FBX UV seams expand polygon corners and flip V");
    require(result.meshes[0].positions[0] == result.meshes[0].positions[9] &&
                result.meshes[0].positions[1] == result.meshes[0].positions[10] &&
                result.meshes[0].positions[2] == result.meshes[0].positions[11] &&
                result.meshes[0].uvs[0] != result.meshes[0].uvs[6],
            "FBX UV seam duplicates shared position with a distinct UV");
}

template <typename Function>
void expectsError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const FbxConversionError& error) {
        require(error.diagnostic().code == code,
                std::string("unexpected FBX conversion diagnostic: ") + error.diagnostic().code +
                    " (" + error.diagnostic().message + ")");
        return;
    }
    throw std::runtime_error("invalid FBX DOM was accepted");
}

void rejectsInvalidReferencesIndicesAndNonFiniteValues() {
    auto invalidIndex = fixture();
    auto& polygon = invalidIndex.roots[0].children[1].children[1].properties[0].values[0];
    polygon = FbxValue{FbxArray{std::vector<std::int64_t>{0, 1, -4}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(invalidIndex); }, "invalid_index");

    auto unterminated = fixture();
    auto& polygonValues = unterminated.roots[0].children[1].children[1].properties[0].values[0];
    polygonValues = FbxValue{FbxArray{std::vector<std::int64_t>{0, 1, 2}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(unterminated); }, "invalid_geometry");

    auto nonFinite = fixture();
    auto& vertices = nonFinite.roots[0].children[1].children[0].properties[0].values[0];
    vertices = FbxValue{FbxArray{std::vector<double>{0.0, 0.0, 0.0, 1.0, 0.0, std::numeric_limits<double>::infinity(), 0.0, 1.0, 0.0}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(nonFinite); }, "non_finite");

    auto badConnection = fixture();
    badConnection.roots[1].children[0].properties[0].values[2] = std::int64_t(9999);
    expectsError([&] { (void)apex::formats::convertFbxScene(badConnection); }, "invalid_reference");

    auto duplicateId = fixture();
    duplicateId.roots[0].children[2].properties[0].values[0] = std::int64_t(200);
    expectsError([&] { (void)apex::formats::convertFbxScene(duplicateId); }, "duplicate_id");

    auto zeroId = fixture();
    zeroId.roots[0].children[0].properties[0].values[0] = std::int64_t(0);
    expectsError([&] { (void)apex::formats::convertFbxScene(zeroId); }, "invalid_id");
    auto roundedId = fixture();
    roundedId.roots[0].children[0].properties[0].values[0] = 9223372036854775808.0;
    expectsError([&] { (void)apex::formats::convertFbxScene(roundedId); }, "invalid_id");

    auto oversizedPolygon = fixture();
    oversizedPolygon.roots[0].children[1].children[1].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<std::int64_t>(32u, 0)}};
    oversizedPolygon.roots[0].children[1].children[3].children[3].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<std::int64_t>(32u, 0)}};
    auto indexLimits = apex::formats::FbxConversionLimits{};
    indexLimits.max_indices = 3u;
    expectsError([&] { (void)apex::formats::convertFbxScene(oversizedPolygon, indexLimits); }, "index_limit");

    auto extreme = fixture();
    extreme.roots[0].children[1].children[0].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<float>{std::numeric_limits<float>::max(), 0.0F, 0.0F,
                                              -std::numeric_limits<float>::max(), 1.0F, 0.0F,
                                              0.0F, 0.0F, std::numeric_limits<float>::max()}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(extreme); }, "non_finite");
}

void rejectsMalformedAndUnsupportedUvLayers() {
    auto oddShape = fixture();
    auto& oddDirect = oddShape.roots[0].children[1].children[3].children[2].properties[0].values[0];
    oddDirect = FbxValue{FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 0.5}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(oddShape); }, "invalid_uv");

    auto outOfRange = fixture();
    auto& uvIndices = outOfRange.roots[0].children[1].children[3].children[3].properties[0].values[0];
    uvIndices = FbxValue{FbxArray{std::vector<std::int64_t>{0, 1, 3}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(outOfRange); }, "invalid_uv");

    auto countMismatch = fixture();
    auto& shortUvIndices = countMismatch.roots[0].children[1].children[3].children[3].properties[0].values[0];
    shortUvIndices = FbxValue{FbxArray{std::vector<std::int64_t>{0, 1}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(countMismatch); }, "invalid_uv");

    auto missingIndex = fixture();
    missingIndex.roots[0].children[1].children[3].children.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(missingIndex); }, "invalid_uv");

    auto nonFiniteUv = fixture();
    auto& finiteUv = nonFiniteUv.roots[0].children[1].children[3].children[2].properties[0].values[0];
    finiteUv = FbxValue{FbxArray{std::vector<double>{0.0, 0.0, std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(nonFiniteUv); }, "non_finite");

    auto unsupportedMapping = fixture();
    unsupportedMapping.roots[0].children[1].children[3].children[0].properties[0].values[0] =
        std::string("ByVertice");
    const auto mappingResult = apex::formats::convertFbxScene(unsupportedMapping);
    require(mappingResult.meshes.size() == 1u && mappingResult.meshes[0].uvs.empty() && !mappingResult.complete,
            "unsupported FBX UV mapping remains explicit and does not alter static geometry");
    require(std::any_of(mappingResult.diagnostics.begin(), mappingResult.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "unsupported_layer_mapping"; }),
            "unsupported FBX UV mapping diagnostic");

    auto unsupportedReference = fixture();
    unsupportedReference.roots[0].children[1].children[3].children[1].properties[0].values[0] =
        std::string("Direct");
    const auto referenceResult = apex::formats::convertFbxScene(unsupportedReference);
    require(referenceResult.meshes.size() == 1u && referenceResult.meshes[0].uvs.empty() && !referenceResult.complete,
            "unsupported FBX UV reference remains explicit");
    require(std::any_of(referenceResult.diagnostics.begin(), referenceResult.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code == "unsupported_layer_mapping"; }),
            "unsupported FBX UV reference diagnostic");
}

void enforcesUvExpansionBudgets() {
    auto indexLimited = fixture();
    auto indexLimits = apex::formats::FbxConversionLimits{};
    indexLimits.max_indices = 2u;
    expectsError([&] { (void)apex::formats::convertFbxScene(indexLimited, indexLimits); }, "index_limit");

    auto vertexLimited = seamFixture();
    auto vertexLimits = apex::formats::FbxConversionLimits{};
    vertexLimits.max_vertices = 4u;
    expectsError([&] { (void)apex::formats::convertFbxScene(vertexLimited, vertexLimits); }, "vertex_limit");

    auto outputLimited = fixture();
    auto outputLimits = apex::formats::FbxConversionLimits{};
    outputLimits.max_output_bytes = 1u;
    expectsError([&] { (void)apex::formats::convertFbxScene(outputLimited, outputLimits); }, "output_limit");
}

void enforcesLimitsAndUnsupportedCapability() {
    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_output_bytes = 1u;
    expectsError([&] { (void)apex::formats::convertFbxScene(fixture(), limits); }, "output_limit");

    auto unsupported = fixture();
    unsupported.roots.push_back(node("AnimationStack", {std::int64_t(500), std::string("AnimationStack::A"), std::string("AnimationStack")}));
    unsupported.roots.push_back(node("Texture", {std::int64_t(600), std::string("Texture::A"), std::string("Texture")}));
    const auto result = apex::formats::convertFbxScene(unsupported);
    require(!result.complete, "unsupported FBX features are not marked complete");
    bool animation = false, image = false;
    for (const auto& diagnostic : result.diagnostics) {
        animation = animation || diagnostic.code == "unsupported_animation";
        image = image || diagnostic.code == "unsupported_images";
    }
    require(animation && image, "unsupported FBX feature diagnostics");

    auto unsupportedTransform = fixture();
    unsupportedTransform.roots[0].children[0].children[0].children.push_back(
        propertyNode("P", {std::string("RotationOrder"), std::string("RotationOrder"), std::string(""), std::string("A"), std::int64_t(0)}));
    const auto transformResult = apex::formats::convertFbxScene(unsupportedTransform);
    bool transform = false;
    for (const auto& diagnostic : transformResult.diagnostics)
        transform = transform || diagnostic.code == "unsupported_transform_property";
    require(transform && !transformResult.complete, "unsupported FBX transform diagnostic");

    auto multiple = fixture();
    auto extraGeometry = multiple.roots[0].children[1];
    extraGeometry.properties[0].values[0] = std::int64_t(101);
    auto extraMaterial = multiple.roots[0].children[2];
    extraMaterial.properties[0].values[0] = std::int64_t(301);
    multiple.roots[0].children.push_back(extraGeometry);
    multiple.roots[0].children.push_back(extraMaterial);
    multiple.roots[1].children.push_back(node("C", {std::string("OO"), std::int64_t(101), std::int64_t(200)}));
    multiple.roots[1].children.push_back(node("C", {std::string("OO"), std::int64_t(301), std::int64_t(200)}));
    const auto multipleResult = apex::formats::convertFbxScene(multiple);
    bool multipleGeometry = false, multipleMaterial = false;
    for (const auto& diagnostic : multipleResult.diagnostics) {
        multipleGeometry = multipleGeometry || diagnostic.code == "multiple_geometry";
        multipleMaterial = multipleMaterial || diagnostic.code == "multiple_materials";
    }
    require(multipleGeometry && multipleMaterial && !multipleResult.complete && multipleResult.meshes.size() == 2u,
            "multiple FBX assignments are explicitly first-wins");

    auto diagnosticLimited = unsupported;
    auto diagnosticLimits = apex::formats::FbxConversionLimits{};
    diagnosticLimits.max_diagnostics = 1u;
    diagnosticLimits.max_diagnostic_bytes = 256u;
    const auto diagnosticResult = apex::formats::convertFbxScene(diagnosticLimited, diagnosticLimits);
    require(diagnosticResult.diagnostics.size() <= 1u && !diagnosticResult.complete,
            "FBX diagnostics are bounded");
    auto pathLimits = apex::formats::FbxConversionLimits{};
    pathLimits.max_diagnostic_path_bytes = 4u;
    expectsError([&] { (void)apex::formats::convertFbxScene(unsupported, pathLimits); }, "diagnostic_limit");

    auto nodeLimited = apex::formats::FbxConversionLimits{};
    nodeLimited.max_nodes = 1u;
    expectsError([&] { (void)apex::formats::convertFbxScene(fixture(), nodeLimited); }, "node_limit");
    auto zeroConnections = apex::formats::FbxConversionLimits{};
    zeroConnections.max_connections = 0u;
    expectsError([&] { (void)apex::formats::convertFbxScene(fixture(), zeroConnections); }, "connection_limit");
    auto zeroIndices = apex::formats::FbxConversionLimits{};
    zeroIndices.max_indices = 0u;
    expectsError([&] { (void)apex::formats::convertFbxScene(fixture(), zeroIndices); }, "index_limit");
    auto propertyLimited = apex::formats::FbxConversionLimits{};
    propertyLimited.max_property_values = 2u;
    expectsError([&] { (void)apex::formats::convertFbxScene(fixture(), propertyLimited); }, "property_limit");
    auto deep = fixture();
    FbxNode* cursor = &deep.roots[0];
    for (int index = 0; index < 4; ++index) {
        cursor->children.push_back(node("Nested"));
        cursor = &cursor->children.back();
    }
    auto depthLimited = apex::formats::FbxConversionLimits{};
    depthLimited.max_depth = 2u;
    expectsError([&] { (void)apex::formats::convertFbxScene(deep, depthLimited); }, "depth_limit");

    const auto capability = apex::formats::fbxSceneConversionCapability();
    require(capability.static_geometry && capability.node_transforms && capability.material_assignment &&
                !capability.skinning && !capability.animation && !capability.images && !capability.layer_mappings,
            "FBX conversion capability detail");
}

void boundsTemporaryContainersBeforeConversion() {
    auto many = fixture();
    auto& objects = many.roots[0];
    auto& connections = many.roots[1];
    constexpr std::size_t extraCount = 64u;
    for (std::size_t index = 0u; index < extraCount; ++index) {
        const auto id = static_cast<std::int64_t>(1000u + index);
        objects.children.push_back(node("Material", {
            id, std::string("Material::Extra") + std::to_string(index), std::string("Material")}, {
            propertyNode("ShadingModel", {std::string("Phong")})}));
        connections.children.push_back(node("C", {std::string("OO"), id, std::int64_t(200)}));
    }
    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_materials = extraCount + 1u;
    limits.max_connections = extraCount + 2u;
    limits.max_output_bytes = 16u * 1024u;
    expectsError([&] { (void)apex::formats::convertFbxScene(many, limits); }, "output_limit");
}

}  // namespace

int main() {
    try {
        convertsParsedAsciiTriangle();
        convertsStaticGeometryTransformsAndMaterials();
        convertsUvSeamsAndFlipsV();
        rejectsInvalidReferencesIndicesAndNonFiniteValues();
        rejectsMalformedAndUnsupportedUvLayers();
        enforcesUvExpansionBudgets();
        enforcesLimitsAndUnsupportedCapability();
        boundsTemporaryContainersBeforeConversion();
        std::cout << "fbx conversion tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fbx conversion tests failed: " << error.what() << '\n';
        return 1;
    }
}
