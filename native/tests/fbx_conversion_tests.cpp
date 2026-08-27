#include "apex/formats/fbx_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
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

template <typename Function>
void expectsError(Function&& function, std::string_view code);

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

FbxNode normalLayer(
    std::string_view mapping = "ByPolygonVertex",
    std::string_view reference = "Direct",
    FbxArray direct = FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0,
                                                   0.0, 1.0, 0.0, 0.0,
                                                   1.0}},
    std::optional<FbxArray> indices = std::nullopt) {
    std::vector<FbxNode> children = {
        propertyNode("MappingInformationType", {std::string(mapping)}),
        propertyNode("ReferenceInformationType", {std::string(reference)}),
        propertyNode("Normals", {std::move(direct)})};
    if (indices.has_value())
        children.push_back(
            propertyNode("NormalsIndex", {std::move(*indices)}));
    return node("LayerElementNormal", {}, std::move(children));
}

FbxNode materialLayer(
    std::string_view mapping = "AllSame",
    std::string_view reference = "IndexToDirect",
    FbxArray slots = FbxArray{std::vector<std::int64_t>{0}}) {
    return node("LayerElementMaterial", {}, {
        propertyNode("MappingInformationType", {std::string(mapping)}),
        propertyNode("ReferenceInformationType", {std::string(reference)}),
        propertyNode("Materials", {std::move(slots)})});
}

FbxDocument fixture() {
    const FbxNode geometry = node("Geometry", {
        std::int64_t(100), std::string("Geometry::Triangle"), std::string("Mesh")}, {
        propertyNode("Vertices", {FbxArray{std::vector<double>{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0}}}),
        propertyNode("PolygonVertexIndex", {FbxArray{std::vector<std::int64_t>{0, 1, -3}}}),
        normalLayer(),
        uvLayer(),
        materialLayer()});
    const FbxNode model = node("Model", {
        std::int64_t(200), std::string("Model::Triangle"), std::string("Mesh")}, {
        node("Properties70", {}, {
            propertyNode("P", {std::string("Lcl Translation"), std::string("Lcl Translation"), std::string(""), std::string("A"), 1.0, 2.0, 3.0}),
            propertyNode("P", {std::string("Lcl Rotation"), std::string("Lcl Rotation"), std::string(""), std::string("A"), 0.0, 0.0, 90.0})})});
    const FbxNode material = node("Material", {
        std::int64_t(300), std::string("Material::Paint"), std::string("Material")}, {
        propertyNode("ShadingModel", {std::string("Phong")}),
        node("Properties70", {}, {
            propertyNode("P", {std::string("AmbientColor"), std::string("ColorRGB"), std::string("Color"), std::string(""), 0.25, 0.5, 0.75}),
            propertyNode("P", {std::string("DiffuseColor"), std::string("ColorRGB"), std::string("Color"), std::string(""), 0.6, 0.7, 0.8}),
            propertyNode("P", {std::string("SpecularColor"), std::string("ColorRGB"), std::string("Color"), std::string(""), 0.9, 0.4, 0.2}),
            propertyNode("P", {std::string("Shininess"), std::string("double"), std::string("Number"), std::string(""), 0.5})})});
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
    geometry.children[2] = normalLayer(
        "ByControlPoint", "Direct",
        FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 0.0, 1.0,
                                     0.0, 0.0, 1.0, 0.0, 0.0, 1.0}});
    geometry.children[3] = uvLayer(
        "ByPolygonVertex", "IndexToDirect",
        FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 1.0, 1.0,
                                     0.25, 0.5, 0.5, 1.0, 0.0, 1.0}},
        FbxArray{std::vector<std::int64_t>{0, 1, 2, 3, 4, 5}});
    return document;
}

FbxNode skinCluster(std::int64_t id, std::string name,
                    std::vector<std::int64_t> indexes,
                    std::vector<double> weights,
                    std::vector<double> transform_link = {
                        1.0, 0.0, 0.0, 0.0,
                        0.0, 1.0, 0.0, 0.0,
                        0.0, 0.0, 1.0, 0.0,
                        2.0, 0.0, 0.0, 1.0}) {
    return node(
        "Deformer",
        {id, std::string("SubDeformer::") + name, std::string("Cluster")},
        {propertyNode("Indexes", {FbxArray{std::move(indexes)}}),
         propertyNode("Weights", {FbxArray{std::move(weights)}}),
         propertyNode("TransformLink",
                      {FbxArray{std::move(transform_link)}})});
}

FbxDocument skinFixture(std::size_t cluster_count = 1u) {
    auto document = seamFixture();
    auto& objects = document.roots[0].children;
    auto& connections = document.roots[1].children;
    objects.push_back(node(
        "Deformer",
        {std::int64_t(400), std::string("Deformer::Skin"),
         std::string("Skin")}));
    connections.push_back(node(
        "C", {std::string("OO"), std::int64_t(400), std::int64_t(100)}));
    for (std::size_t index = 0u; index < cluster_count; ++index) {
        const auto cluster_id = static_cast<std::int64_t>(500u + index);
        const auto bone_id = static_cast<std::int64_t>(600u + index);
        objects.push_back(skinCluster(
            cluster_id, "Cluster" + std::to_string(index),
            index == 0u ? std::vector<std::int64_t>{0, 1, 2, 3}
                        : std::vector<std::int64_t>{0},
            index == 0u ? std::vector<double>{0.25, 0.5, 0.75, 1.0}
                        : std::vector<double>{0.1 * static_cast<double>(index + 1u)}));
        objects.push_back(node(
            "Model",
            {bone_id, std::string("Model::Bone") + std::to_string(index),
             std::string("LimbNode")}));
        connections.push_back(node(
            "C", {std::string("OO"), cluster_id, std::int64_t(400)}));
        connections.push_back(node(
            "C", {std::string("OO"), bone_id, cluster_id}));
    }
    return document;
}

FbxDocument fileTextureFixture(std::string fileName =
                                   "C:\\cars\\example\\texture\\paint.png") {
    auto document = fixture();
    document.roots[0].children.push_back(node(
        "Texture",
        {std::int64_t(400), std::string("Texture::Paint"),
         std::string("TextureVideoClip")},
        {propertyNode("FileName", {std::move(fileName)})}));
    document.roots[1].children.push_back(node(
        "C", {std::string("OP"), std::int64_t(400), std::int64_t(300),
              std::string("DiffuseColor")}));
    return document;
}

FbxDocument embeddedTextureFixture(
    std::vector<std::uint8_t> content = {0x89U, 0x50U, 0x4eU, 0x47U}) {
    auto document = fileTextureFixture();
    document.roots[0].children.push_back(node(
        "Video",
        {std::int64_t(500), std::string("Video::Paint"),
         std::string("Clip")},
        {propertyNode("RelativeFilename",
                      {std::string("textures/embedded.png")}),
         propertyNode("FileName", {std::string("fallback.png")}),
         propertyNode("Content", {std::move(content)})}));
    document.roots[1].children.push_back(node(
        "C", {std::string("OO"), std::int64_t(500),
              std::int64_t(400)}));
    return document;
}

FbxDocument geometricTransformFixture() {
    auto document = fixture();
    auto& properties = document.roots[0].children[0].children[0];
    properties.children.push_back(propertyNode("P", {
        std::string("GeometricTranslation"), std::string("GeometricTranslation"),
        std::string(""), std::string("A"), 10.0, 0.0, 0.0}));
    properties.children.push_back(propertyNode("P", {
        std::string("GeometricScaling"), std::string("GeometricScaling"),
        std::string(""), std::string("A"), 2.0, 2.0, 2.0}));
    return document;
}

FbxDocument animationFixture() {
    auto document = fixture();
    auto& objects = document.roots[0];
    auto& connections = document.roots[1];
    objects.children.push_back(node("AnimationStack", {
        std::int64_t(500), std::string("AnimationStack::Take 001"), std::string("AnimationStack")}, {
        propertyNode("LocalStart", {std::int64_t(0)}),
        propertyNode("LocalStop", {std::int64_t(100)})}));
    objects.children.push_back(node("AnimationLayer", {
        std::int64_t(501), std::string("AnimationLayer::BaseLayer"), std::string("AnimationLayer")}));
    objects.children.push_back(node("AnimationCurveNode", {
        std::int64_t(502), std::string("AnimationCurveNode::T"), std::string("AnimationCurveNode")}));
    objects.children.push_back(node("AnimationCurve", {
        std::int64_t(503), std::string("AnimationCurve::TX"), std::string("AnimationCurve")}, {
        propertyNode("KeyTime", {FbxArray{std::vector<std::int64_t>{0, 100}}}),
        propertyNode("KeyValueFloat", {FbxArray{std::vector<float>{0.0F, 10.0F}}}),
        propertyNode("KeyAttrFlags", {FbxArray{std::vector<std::int64_t>{4, 4}}})}));
    connections.children.push_back(node("C", {std::string("OO"), std::int64_t(501), std::int64_t(500)}));
    connections.children.push_back(node("C", {std::string("OO"), std::int64_t(502), std::int64_t(501)}));
    connections.children.push_back(node("C", {std::string("OP"), std::int64_t(502), std::int64_t(200), std::string("Lcl Translation")}));
    connections.children.push_back(node("C", {std::string("OP"), std::int64_t(503), std::int64_t(502), std::string("d|X")}));
    return document;
}

FbxDocument animationSelectionFixture() {
    auto document = animationFixture();
    auto& objects = document.roots[0];
    auto& connections = document.roots[1];
    // Keep the dynamic Triangle first, then put two static eligible Models
    // under it. The Camera is a Model record, but native loadAnimationNode
    // does not emit a track for eCamera attributes.
    objects.children.push_back(node("Model", {
        std::int64_t(201), std::string("Model::StaticNull"), std::string("Null")}));
    objects.children.push_back(node("Model", {
        std::int64_t(202), std::string("Model::Camera"), std::string("Camera")}));
    objects.children.push_back(node("Model", {
        std::int64_t(203), std::string("Model::StaticLimb"), std::string("LimbNode")}));
    connections.children.push_back(node("C", {std::string("OO"), std::int64_t(200), std::int64_t(0)}));
    connections.children.push_back(node("C", {std::string("OO"), std::int64_t(201), std::int64_t(200)}));
    connections.children.push_back(node("C", {std::string("OO"), std::int64_t(203), std::int64_t(201)}));
    connections.children.push_back(node("C", {std::string("OO"), std::int64_t(202), std::int64_t(0)}));
    return document;
}

FbxDocument duplicateNameAnimationFixture() {
    auto document = animationSelectionFixture();
    auto& objects = document.roots[0];
    // Model 201 is static but shares the first Model's native animation-set
    // name.  Model 203 remains a distinct eligible track; the Camera remains
    // excluded from both cases.
    objects.children[7].properties[0].values[1] = std::string("Model::Triangle");
    return document;
}

FbxDocument staticAnimationFixture() {
    auto document = fixture();
    document.roots[0].children.push_back(node("AnimationStack", {
        std::int64_t(500), std::string("AnimationStack::Static Take"), std::string("AnimationStack")}, {
        propertyNode("LocalStart", {std::int64_t(0)}),
        propertyNode("LocalStop", {std::int64_t(100)})}));
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
                result.meshes[0].normals ==
                    std::vector<float>{0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                       1.0F, 0.0F, 0.0F, 1.0F} &&
                result.meshes[0].triangle_indices ==
                    std::vector<std::uint32_t>{0u, 1u, 2u} &&
                result.meshes[0].triangle_material_slots ==
                    std::vector<std::int32_t>{0},
            "FBX static triangle geometry");
    require(result.transforms.size() == 1u && std::abs(result.transforms[0].local[12] - 1.0F) < 1e-6F &&
                std::abs(result.transforms[0].world[13] - 2.0F) < 1e-6F,
            "FBX local and world transforms");
    require(result.snapshot.nodes[1].bounds_radius > 0.7F &&
                result.snapshot.nodes[1].bounds_center[2] > 2.9F &&
                result.complete,
            "FBX bounds and supported layer capability status");
    require(result.snapshot.nodes[1].local_aabb_center.has_value() &&
                *result.snapshot.nodes[1].local_aabb_center ==
                    apex::scene::Vector3{0.5F, 0.5F, 0.0F},
            "FBX conversion retains the exact local vertex-AABB center");
    require(result.diagnostics.empty(), "supported FBX layers need no warning");
    require(result.node_geometry.size() == 1u &&
                result.node_geometry[0].materials ==
                    std::vector<apex::scene::MaterialId>{0u},
            "FBX node retains its material-slot connection order");
    require(result.snapshot.materials[0].name == "Paint" &&
                result.snapshot.materials[0].shader == "ksPerPixel",
            "FBX material uses the recovered native shader");
    require(result.material_parameters.size() == 1u &&
                result.material_parameters[0].recognized_surface &&
                result.material_parameters[0].ambient_color ==
                    std::array<float, 3u>{0.25F, 0.5F, 0.75F} &&
                result.material_parameters[0].diffuse_color ==
                    std::array<float, 3u>{0.6F, 0.7F, 0.8F} &&
                result.material_parameters[0].specular_color ==
                    std::array<float, 3u>{0.9F, 0.4F, 0.2F} &&
                result.material_parameters[0].shininess == 0.5F,
            "FBX material retains bounded native scalar sources");
}

void convertsBoundedNativeSkinning() {
    const auto result = apex::formats::convertFbxScene(skinFixture());
    require(result.complete && result.meshes.size() == 1u &&
                result.meshes[0].skin.has_value() &&
                result.node_geometry.size() == 1u &&
                result.snapshot.nodes[1].kind ==
                    apex::scene::NodeKind::skinned_mesh,
            "FBX Skin and Cluster records produce a mapped skinned mesh");
    const auto& skin = *result.meshes[0].skin;
    require(skin.skin_object_id == 400 && skin.bones.size() == 1u &&
                skin.bones[0].model_object_id == 600 &&
                skin.bones[0].name == "Bone0" &&
                std::abs(skin.bones[0].inverse_bind[12] + 2.0F) < 1.0e-6F,
            "FBX TransformLink is inverted without an added mesh transform");
    require(skin.vertex_influences.size() == 6u &&
                skin.vertex_influences[0].weights[0] == 0.25F &&
                skin.vertex_influences[3].weights[0] == 0.25F &&
                skin.vertex_influences[1].weights[0] == 0.5F &&
                skin.vertex_influences[2].weights[0] == 0.75F &&
                skin.vertex_influences[5].weights[0] == 1.0F,
            "UV-seam expansion retains source control-point influences");

    auto transformed_document = skinFixture();
    transformed_document.roots[0].children[0].children[0].children.push_back(
        propertyNode(
            "P", {std::string("GeometricTranslation"),
                  std::string("GeometricTranslation"), std::string(""),
                  std::string("A"), 3.0, 0.0, 0.0}));
    const auto transformed =
        apex::formats::convertFbxScene(transformed_document);
    require(transformed.node_geometry.size() == 1u &&
                transformed.node_geometry[0].geometric[12] == 3.0F &&
                transformed.snapshot.nodes[1].renderable,
            "FBX skinned meshes retain geometric transforms and render state");

    const auto crowded = apex::formats::convertFbxScene(skinFixture(5u));
    const auto& crowded_influence =
        crowded.meshes[0].skin->vertex_influences[0];
    require(crowded.complete &&
                crowded_influence.bones ==
                    std::array<std::uint32_t, 4u>{0u, 1u, 2u, 3u} &&
                std::any_of(crowded.diagnostics.begin(),
                            crowded.diagnostics.end(), [](const auto& value) {
                                return value.code == "skin_influence_dropped";
                            }),
            "FBX skin retains the first four source-order influences");

    auto malformed = skinFixture();
    auto& cluster = malformed.roots[0].children[4];
    cluster.children[1].properties[0].values[0] =
        FbxArray{std::vector<double>{0.5}};
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "invalid_skin");

    malformed = skinFixture();
    malformed.roots[0].children[4].children[1].properties[0].values[0] =
        FbxArray{std::vector<double>{0.25, 0.5, 0.75, -1.0}};
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "skin_weight_invalid");

    malformed = skinFixture();
    malformed.roots[0].children[4].children[0].properties[0].values[0] =
        FbxArray{std::vector<std::int64_t>{0, 1, 2, 99}};
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "skin_index_invalid");

    malformed = skinFixture();
    malformed.roots[0].children[4].children[2].properties[0].values[0] =
        FbxArray{std::vector<double>(15u, 0.0)};
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "skin_matrix_invalid");

    malformed = skinFixture();
    malformed.roots[0].children[4].children[2].properties[0].values[0] =
        FbxArray{std::vector<double>(16u, 0.0)};
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "skin_matrix_invalid");

    auto ignored_transform = skinFixture();
    ignored_transform.roots[0].children[4].children.push_back(propertyNode(
        "Transform", {FbxArray{std::vector<double>{7.0}}}));
    require(apex::formats::convertFbxScene(ignored_transform).complete,
            "native skin conversion intentionally ignores Cluster Transform");

    auto limited = skinFixture();
    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_bones_per_skin = 0u;
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(limited, limits); },
        "skin_bone_limit");
    limits = apex::formats::FbxConversionLimits{};
    limits.max_skin_deformers = 0u;
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(limited, limits); },
        "skin_deformer_limit");
    limits = apex::formats::FbxConversionLimits{};
    limits.max_skin_clusters = 0u;
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(limited, limits); },
        "skin_cluster_limit");
}

void appliesNativeGeometricMeshTransform() {
    const auto result = apex::formats::convertFbxScene(geometricTransformFixture());
    require(result.snapshot.nodes.size() == 2u && result.snapshot.nodes[1].kind == apex::scene::NodeKind::mesh,
            "FBX geometric transform retains mesh node output");
    require(std::abs(result.snapshot.nodes[1].transform[12] - 1.0F) < 1e-6F &&
                std::abs(result.snapshot.nodes[1].transform[13] - 12.0F) < 1e-6F &&
                std::abs(result.snapshot.nodes[1].transform[14] - 3.0F) < 1e-6F,
            "FBX geometric translation and scale compose after the local transform");
    require(std::abs(result.snapshot.nodes[1].bounds_center[0] - 0.0F) < 1e-6F &&
                std::abs(result.snapshot.nodes[1].bounds_center[1] - 13.0F) < 1e-6F &&
                std::abs(result.snapshot.nodes[1].bounds_center[2] - 3.0F) < 1e-6F,
            "FBX bounds include the native geometric mesh transform");
    require(result.node_geometry.size() == 1u &&
                std::abs(result.node_geometry[0].geometric[12] - 10.0F) < 1e-6F &&
                std::abs(result.node_geometry[0].geometric[0] - 2.0F) < 1e-6F,
            "FBX conversion retains geometric TRS for the render adapter");

    auto malformed = geometricTransformFixture();
    malformed.roots[0].children[0].children[0].children.back().properties[0].values.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); }, "invalid_transform");
}

void preservesBoundedFileTextureCandidates() {
    const auto result = apex::formats::convertFbxScene(fileTextureFixture());
    require(result.complete && result.file_texture_candidates.size() == 1u,
            "FBX file texture connection becomes one candidate");
    require(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const auto& diagnostic) {
                             return diagnostic.code == "unsupported_images";
                         }),
            "supported external file texture is not an embedded-image gap");
    const auto& candidate = result.file_texture_candidates.front();
    require(candidate.material == 0u && candidate.texture_object_id == 400 &&
                candidate.channel == "DiffuseColor" &&
                candidate.basename == "paint.png",
            "FBX texture candidate keeps material, channel, and native basename");
    require(candidate.basename.find("cars") == std::string::npos,
            "FBX texture candidate does not expose the source directory");

    auto ordered = fixture();
    auto& orderedObjects = ordered.roots[0].children;
    auto& orderedConnections = ordered.roots[1].children;
    orderedObjects.push_back(node(
        "Texture",
        {std::int64_t(400), std::string("Texture::Ambient"),
         std::string("TextureVideoClip")},
        {propertyNode("FileName", {std::string("ambient.png")})}));
    orderedObjects.push_back(node(
        "Texture",
        {std::int64_t(401), std::string("Texture::Diffuse"),
         std::string("TextureVideoClip")},
        {propertyNode("FileName", {std::string("diffuse.png")})}));
    orderedObjects.push_back(node(
        "Texture",
        {std::int64_t(402), std::string("Texture::Normal"),
         std::string("TextureVideoClip")},
        {propertyNode("FileName", {std::string("normal.png")})}));
    orderedConnections.push_back(node(
        "C", {std::string("OP"), std::int64_t(400), std::int64_t(300),
              std::string("AmbientColor")}));
    orderedConnections.push_back(node(
        "C", {std::string("OP"), std::int64_t(402), std::int64_t(300),
              std::string("NormalMap")}));
    orderedConnections.push_back(node(
        "C", {std::string("OP"), std::int64_t(401), std::int64_t(300),
              std::string("DiffuseColor")}));
    const auto orderedResult = apex::formats::convertFbxScene(ordered);
    require(orderedResult.file_texture_candidates.size() == 2u &&
                orderedResult.file_texture_candidates[0].texture_object_id == 401 &&
                orderedResult.file_texture_candidates[1].texture_object_id == 400,
            "FBX texture candidates use the recovered eight-channel order");
    require(orderedResult.file_texture_candidates[0].connection_order == 4u &&
                orderedResult.file_texture_candidates[1].connection_order == 2u,
            "FBX texture candidates retain raw connection provenance");

    auto missingFileName = fileTextureFixture();
    missingFileName.roots[0].children.back().children.clear();
    const auto missingResult = apex::formats::convertFbxScene(missingFileName);
    require(missingResult.file_texture_candidates.empty(),
            "FBX texture without FileName remains unresolved");

    auto truncatedFileName = fileTextureFixture();
    truncatedFileName.roots[0].children.back().children.front().properties.clear();
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(truncatedFileName); },
        "invalid_texture");

    auto duplicateFileName = fileTextureFixture();
    duplicateFileName.roots[0].children.back().children.push_back(
        propertyNode("FileName", {std::string("other.png")}));
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(duplicateFileName); },
        "invalid_texture");

    auto unsafeBasename = fileTextureFixture("textures/..");
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(unsafeBasename); },
        "invalid_texture");

    auto nullBasename = fileTextureFixture(std::string("paint\0.png", 10u));
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(nullBasename); },
        "invalid_texture");

    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_texture_references = 0u;
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(fileTextureFixture(), limits); },
        "texture_reference_limit");

    limits = apex::formats::FbxConversionLimits{};
    limits.max_textures = 0u;
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(fileTextureFixture(), limits); },
        "count_limit");
}

void preservesBoundedEmbeddedTextureCandidates() {
    const std::vector<std::uint8_t> bytes = {0x89U, 0x50U, 0x4eU, 0x47U};
    const auto result =
        apex::formats::convertFbxScene(embeddedTextureFixture(bytes));
    require(result.complete && result.embedded_image_compatibility &&
                result.file_texture_candidates.size() == 1U &&
                result.embedded_images.size() == 1U &&
                result.embedded_texture_candidates.size() == 1U,
            "FBX Video content is retained beside the external fallback");
    require(result.embedded_images[0].video_object_id == 500 &&
                result.embedded_images[0].basename == "embedded.png" &&
                result.embedded_images[0].content == bytes,
            "FBX embedded image retains bounded identity, name, and bytes");
    const auto& candidate = result.embedded_texture_candidates[0];
    require(candidate.material == 0U && candidate.texture_object_id == 400 &&
                candidate.video_object_id == 500 &&
                candidate.embedded_image_index == 0U &&
                candidate.channel == "DiffuseColor",
            "FBX embedded candidate follows Video to Texture to Material");

    auto emptyRelativeName = embeddedTextureFixture(bytes);
    emptyRelativeName.roots[0].children.back().children[0]
        .properties[0].values[0] = std::string();
    const auto emptyRelativeResult =
        apex::formats::convertFbxScene(emptyRelativeName);
    require(emptyRelativeResult.embedded_images[0].basename ==
                "fallback.png",
            "empty RelativeFilename falls back to the Video FileName");

    auto reused = embeddedTextureFixture(bytes);
    reused.roots[1].children.push_back(node(
        "C", {std::string("OP"), std::int64_t(400),
              std::int64_t(300), std::string("AmbientColor")}));
    const auto reusedResult = apex::formats::convertFbxScene(reused);
    require(reusedResult.embedded_images.size() == 1U &&
                reusedResult.embedded_texture_candidates.size() == 2U &&
                reusedResult.embedded_texture_candidates[0]
                        .embedded_image_index == 0U &&
                reusedResult.embedded_texture_candidates[1]
                        .embedded_image_index == 0U,
            "one embedded Video payload is reused without copying");

    auto empty = embeddedTextureFixture({});
    const auto emptyResult = apex::formats::convertFbxScene(empty);
    require(emptyResult.complete && emptyResult.embedded_images.empty() &&
                emptyResult.embedded_texture_candidates.empty() &&
                emptyResult.file_texture_candidates.size() == 1U,
            "empty embedded Content keeps the external file fallback");

    auto malformed = embeddedTextureFixture(bytes);
    malformed.roots[0].children.back().children.back().properties[0]
        .values[0] = std::string("not raw");
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "invalid_embedded_image");

    auto multiple = embeddedTextureFixture(bytes);
    multiple.roots[0].children.push_back(node(
        "Video",
        {std::int64_t(501), std::string("Video::Other"),
         std::string("Clip")},
        {propertyNode("Content", {bytes})}));
    multiple.roots[1].children.push_back(node(
        "C", {std::string("OO"), std::int64_t(501),
              std::int64_t(400)}));
    expectsError([&] { (void)apex::formats::convertFbxScene(multiple); },
                 "invalid_embedded_image");

    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_embedded_image_bytes = 3U;
    expectsError(
        [&] {
            (void)apex::formats::convertFbxScene(
                embeddedTextureFixture(bytes), limits);
        },
        "embedded_image_limit");
    limits = apex::formats::FbxConversionLimits{};
    limits.max_embedded_image_total_bytes = 3U;
    expectsError(
        [&] {
            (void)apex::formats::convertFbxScene(
                embeddedTextureFixture(bytes), limits);
        },
        "embedded_image_limit");
    limits = apex::formats::FbxConversionLimits{};
    limits.max_embedded_images = 0U;
    expectsError(
        [&] {
            (void)apex::formats::convertFbxScene(
                embeddedTextureFixture(bytes), limits);
        },
        "embedded_image_limit");

    auto byteArray = fixture();
    byteArray.roots[0].children[1].children.push_back(
        propertyNode("UserByteArray",
                     {FbxArray{std::vector<std::uint8_t>{1U, 2U, 3U}}}));
    const auto byteArrayResult = apex::formats::convertFbxScene(
        byteArray, limits);
    require(byteArrayResult.complete,
            "FBX b arrays do not consume the raw Content count limit");
}

void ignoresDisplayLayerMembershipEdges() {
    auto document = fixture();
    document.roots[0].children.push_back(node("CollectionExclusive", {
        std::int64_t(400), std::string("DisplayLayer::WHEEL_DUMMIES"), std::string("DisplayLayer")}));
    document.roots[0].children.push_back(node("Deformer", {
        std::int64_t(401), std::string("SubDeformer::BoneCluster"), std::string("Cluster")}));
    document.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(400)}));
    document.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(0)}));
    document.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(401)}));
    document.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(400)}));
    const auto result = apex::formats::convertFbxScene(document);
    require(result.snapshot.nodes.size() == 2u && result.snapshot.nodes[1].parent == 0u &&
                std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                            [](const auto& diagnostic) { return diagnostic.code == "unreferenced_deformer"; }),
            "FBX display-layer and skin-cluster edges do not become model parents");

    auto malformed = fixture();
    malformed.roots[0].children.push_back(node("CollectionExclusive", {
        std::int64_t(400), std::string("DisplayLayer::WHEEL_DUMMIES"), std::string("DisplayLayer")}));
    malformed.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(300)}));
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); }, "invalid_reference");

    auto duplicateParent = fixture();
    duplicateParent.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(0)}));
    duplicateParent.roots[1].children.push_back(node("C", {
        std::string("OO"), std::int64_t(200), std::int64_t(0)}));
    expectsError([&] { (void)apex::formats::convertFbxScene(duplicateParent); }, "invalid_hierarchy");
}

void handlesConstraintPoConnectionsStrictly() {
    auto valid = fixture();
    valid.roots[0].children.push_back(node("Constraint", {
        std::int64_t(400), std::string("Constraint::Drive"), std::string("Constraint")}));
    valid.roots[1].children.push_back(node("C", {
        std::string("PO"), std::int64_t(400), std::string("Constrained Object"), std::int64_t(200)}));
    const auto result = apex::formats::convertFbxScene(valid);
    require(result.snapshot.nodes.size() == 2u,
            "Constraint PO ownership does not alter the model hierarchy");

    auto truncated = valid;
    truncated.roots[1].children.back().properties[0].values.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(truncated); }, "invalid_connection");

    auto nonStringProperty = valid;
    nonStringProperty.roots[1].children.back().properties[0].values[2] = std::int64_t(7);
    expectsError([&] { (void)apex::formats::convertFbxScene(nonStringProperty); }, "invalid_connection");

    auto unknownTarget = valid;
    unknownTarget.roots[1].children.back().properties[0].values[3] = std::int64_t(9999);
    expectsError([&] { (void)apex::formats::convertFbxScene(unknownTarget); }, "invalid_reference");

    auto wrongType = valid;
    wrongType.roots[1].children.back().properties[0].values[3] = std::int64_t(100);
    expectsError([&] { (void)apex::formats::convertFbxScene(wrongType); }, "unsupported_connection");

    auto wrongProperty = valid;
    wrongProperty.roots[1].children.back().properties[0].values[2] = std::string("Other");
    expectsError([&] { (void)apex::formats::convertFbxScene(wrongProperty); }, "unsupported_connection");
}

void ignoresMissingOptionalAnimationCurveLinks() {
    auto document = animationFixture();
    document.roots[1].children.push_back(node("C", {
        std::string("OP"), std::int64_t(999), std::int64_t(502), std::string("d|X")}));
    const auto result = apex::formats::convertFbxScene(document);
    require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const auto& diagnostic) {
                             return diagnostic.code == "missing_animation_curve";
                         }),
            "missing optional FBX animation curve link is diagnosed");

    auto malformed = document;
    malformed.roots[1].children.back().properties[0].values[1] = std::int64_t(503);
    malformed.roots[1].children.back().properties[0].values.pop_back();
    malformed.roots[1].children.back().properties[0].values.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); },
                 "invalid_connection");

    auto wrongTarget = document;
    wrongTarget.roots[1].children.back().properties[0].values[2] = std::int64_t(200);
    expectsError([&] { (void)apex::formats::convertFbxScene(wrongTarget); },
                 "invalid_reference");
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

void convertsBoundedLinearAnimationToKsanimV2() {
    const auto result = apex::formats::convertFbxScene(animationFixture());
    require(result.animations.size() == 1u, "FBX linear animation stack converts to one clip");
    const auto& clip = result.animations.front();
    require(clip.animation.version == 2u && clip.animation.frameCount == 100u &&
                clip.source_track_count == 1u && clip.animation.tracks.size() == 1u &&
                clip.animation.tracks.front().frames.size() == 100u,
            "FBX animation uses native 100-frame KSANIM v2 shape");
    const auto& frames = clip.animation.tracks.front().frames;
    require(std::abs(frames.front().position[0] - 0.0F) < 1e-6F &&
                std::abs(frames[50].position[0] - 5.0F) < 1e-5F &&
                std::abs(frames[99].position[0] - 9.9F) < 1e-4F &&
                frames.front().position[1] == 2.0F && frames.front().position[2] == 3.0F,
            "FBX animation samples local translation before the end of the span");
    const auto bytes = apex::formats::serializeKsAnimation(clip.animation);
    const auto parsed = apex::formats::parseKsAnimation(bytes, "linear.ksanim");
    require(parsed.version == 2u && parsed.tracks.size() == 1u && parsed.frameCount == 100u,
            "FBX animation exports and parses as KSANIM v2");

    auto negativeTimeline = animationFixture();
    negativeTimeline.roots[0].children[3].children[0].properties[0].values[0] =
        FbxValue{std::int64_t(-100)};
    negativeTimeline.roots[0].children[3].children[1].properties[0].values[0] =
        FbxValue{std::int64_t(100)};
    negativeTimeline.roots[0].children[6].children[0].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<std::int64_t>{-100, 100}}};
    const auto negativeResult = apex::formats::convertFbxScene(negativeTimeline);
    require(negativeResult.animations.size() == 1u &&
                negativeResult.animations.front().animation.tracks.front().frames.size() == 100u,
            "FBX animation preserves valid signed timeline ticks");
}

void selectsNativeAnimationModelsInHierarchyOrder() {
    const auto result = apex::formats::convertFbxScene(animationSelectionFixture());
    require(result.animations.size() == 1u, "FBX selection fixture converts one clip");
    const auto& clip = result.animations.front();
    require(clip.source_track_count == 1u && clip.animation.tracks.size() == 3u,
            "FBX animation keeps curve count but emits all eligible hierarchy models");
    require(clip.animation.tracks[0].name == "Triangle" &&
                clip.animation.tracks[1].name == "StaticNull" &&
                clip.animation.tracks[2].name == "StaticLimb",
            "FBX animation tracks follow native ordered hierarchy traversal");
    for (const auto& track : clip.animation.tracks)
        require(track.frames.size() == 100u, "FBX static and animated tracks use native frame count");
    require(clip.animation.tracks[1].frames.front().position ==
                std::array<float, 3>{0.0F, 0.0F, 0.0F} &&
                clip.animation.tracks[2].frames.front().position ==
                    std::array<float, 3>{0.0F, 0.0F, 0.0F},
            "FBX static eligible Models receive bounded base-transform frames");

    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_animation_tracks = 2u;
    expectsError([&] { (void)apex::formats::convertFbxScene(animationSelectionFixture(), limits); },
                 "animation_track_limit");
}

void emitsBoundedStaticAnimationWithoutCurves() {
    const auto result = apex::formats::convertFbxScene(staticAnimationFixture());
    require(result.animations.size() == 1u, "FBX bounded stack without curves converts a clip");
    const auto& clip = result.animations.front();
    require(clip.source_track_count == 0u && clip.animation.tracks.size() == 1u &&
                clip.animation.tracks.front().frames.size() == 100u &&
                !clip.animation.tracks.front().animated,
            "FBX animation emits a static base-transform track without curve records");
    require(clip.animation.tracks.front().frames.front().position ==
                std::array<float, 3>{1.0F, 2.0F, 3.0F},
            "FBX curve-free animation preserves the Model base transform");
}

void mergesDuplicateNativeAnimationSetNames() {
    const auto result = apex::formats::convertFbxScene(duplicateNameAnimationFixture());
    require(result.animations.size() == 1u, "FBX duplicate-name fixture converts one clip");
    const auto& clip = result.animations.front();
    require(clip.source_track_count == 1u && clip.animation.tracks.size() == 2u &&
                clip.animation.frameCount == 200u,
            "FBX duplicate Model names merge into one first-seen animation set");
    require(clip.animation.tracks[0].name == "Triangle" &&
                clip.animation.tracks[0].frames.size() == 200u &&
                clip.animation.tracks[1].name == "StaticLimb" &&
                clip.animation.tracks[1].frames.size() == 100u,
            "FBX duplicate-name frames append before the next hierarchy track");
    require(std::abs(clip.animation.tracks[0].frames[99].position[0] - 9.9F) < 1e-4F &&
                clip.animation.tracks[0].frames[100].position ==
                    std::array<float, 3>{0.0F, 0.0F, 0.0F},
            "FBX duplicate-name output preserves each Model frame contribution");

    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_animation_tracks = 1u;
    auto oneName = duplicateNameAnimationFixture();
    oneName.roots[0].children[9].properties[0].values[1] = std::string("Model::Triangle");
    const auto bounded = apex::formats::convertFbxScene(oneName, limits);
    require(bounded.animations.size() == 1u && bounded.animations.front().animation.tracks.size() == 1u &&
                bounded.animations.front().animation.tracks.front().frames.size() == 300u,
            "FBX track limits count unique native animation-set names");

    auto frameLimits = apex::formats::FbxConversionLimits{};
    frameLimits.max_animation_merged_frames = 199u;
    expectsError(
        [&] {
            (void)apex::formats::convertFbxScene(
                duplicateNameAnimationFixture(), frameLimits);
        },
        "animation_frame_limit");

    auto malformed = duplicateNameAnimationFixture();
    malformed.roots[0].children[7].properties[0].values[1] = FbxValue{std::int64_t(7)};
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); }, "invalid_object");
}

void rejectsMalformedAnimationCurvesAndUnsupportedInterpolation() {
    auto mismatch = animationFixture();
    mismatch.roots[0].children[6].children[1].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<float>{0.0F}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(mismatch); }, "invalid_animation");

    auto nonFinite = animationFixture();
    nonFinite.roots[0].children[6].children[1].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<float>{0.0F, std::numeric_limits<float>::infinity()}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(nonFinite); }, "non_finite");

    auto unsupported = animationFixture();
    unsupported.roots[0].children[6].children.pop_back();
    const auto unsupportedResult = apex::formats::convertFbxScene(unsupported);
    require(unsupportedResult.animations.empty() && !unsupportedResult.complete &&
                std::any_of(unsupportedResult.diagnostics.begin(), unsupportedResult.diagnostics.end(),
                            [](const auto& diagnostic) { return diagnostic.code == "unsupported_animation_interpolation"; }),
            "FBX curves without explicit linear flags are rejected explicitly");

    auto fractionalFlags = animationFixture();
    fractionalFlags.roots[0].children[6].children[2].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<double>{4.5, 4.0}}};
    const auto fractionalResult = apex::formats::convertFbxScene(fractionalFlags);
    require(fractionalResult.animations.empty() && !fractionalResult.complete &&
                std::any_of(fractionalResult.diagnostics.begin(), fractionalResult.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == "unsupported_animation_interpolation";
                            }),
            "fractional interpolation flags are not truncated to linear flags");

    auto missingCurveLink = animationFixture();
    missingCurveLink.roots[1].children.pop_back();
    const auto missingCurveResult = apex::formats::convertFbxScene(missingCurveLink);
    require(missingCurveResult.animations.empty() && !missingCurveResult.complete &&
                std::any_of(missingCurveResult.diagnostics.begin(),
                            missingCurveResult.diagnostics.end(), [](const auto& diagnostic) {
                                return diagnostic.code == "unsupported_animation_channel";
                            }),
            "curve nodes without axis links do not produce partial clips");

    auto duplicateCurveLink = animationFixture();
    duplicateCurveLink.roots[1].children.push_back(
        duplicateCurveLink.roots[1].children.back());
    expectsError([&] { (void)apex::formats::convertFbxScene(duplicateCurveLink); },
                 "invalid_animation");

    auto keyLimited = animationFixture();
    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_animation_keys = 1u;
    expectsError([&] { (void)apex::formats::convertFbxScene(keyLimited, limits); }, "animation_key_limit");

    auto frameLimited = animationFixture();
    limits = apex::formats::FbxConversionLimits{};
    limits.max_animation_frames = 99u;
    expectsError([&] { (void)apex::formats::convertFbxScene(frameLimited, limits); }, "animation_frame_limit");
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

void preservesNativeNormalMappingsAndRejectsMalformedLayers() {
    auto indexed = fixture();
    indexed.roots[0].children[1].children[2] = normalLayer(
        "ByPolygonVertex", "IndexToDirect",
        FbxArray{std::vector<double>{1.0, 0.0, 0.0, 0.0, 1.0, 0.0}},
        FbxArray{std::vector<std::int64_t>{0, 1, 0}});
    const auto indexedResult = apex::formats::convertFbxScene(indexed);
    require(indexedResult.meshes[0].normals ==
                std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                   0.0F, 1.0F, 0.0F, 0.0F},
            "indexed polygon-corner normals retain their source order");

    auto byPolygon = seamFixture();
    byPolygon.roots[0].children[1].children[2] = normalLayer(
        "ByPolygon", "Direct",
        FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 1.0, 0.0}});
    const auto polygonResult = apex::formats::convertFbxScene(byPolygon);
    require(polygonResult.meshes[0].normals ==
                std::vector<float>{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                                   0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F,
                                   0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F},
            "by-polygon normals expand to every triangulated corner");

    auto allSame = fixture();
    allSame.roots[0].children[1].children[2] = normalLayer(
        "AllSame", "Direct",
        FbxArray{std::vector<double>{0.0, -1.0, 0.0}});
    const auto allSameResult = apex::formats::convertFbxScene(allSame);
    require(allSameResult.meshes[0].normals ==
                std::vector<float>{0.0F, -1.0F, 0.0F, 0.0F, -1.0F,
                                   0.0F, 0.0F, -1.0F, 0.0F},
            "all-same normals expand to every corner");

    auto missing = fixture();
    missing.roots[0].children[1].children[2].children.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(missing); },
                 "invalid_normal");

    auto odd = fixture();
    odd.roots[0].children[1].children[2].children[2].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<double>{0.0, 1.0}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(odd); },
                 "invalid_normal");

    auto missingIndices = fixture();
    missingIndices.roots[0].children[1].children[2] = normalLayer(
        "ByPolygonVertex", "IndexToDirect");
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(missingIndices); },
        "invalid_normal");

    auto shortIndices = fixture();
    shortIndices.roots[0].children[1].children[2] = normalLayer(
        "ByPolygonVertex", "IndexToDirect", FbxArray{std::vector<double>{
                                                  0.0, 0.0, 1.0}},
        FbxArray{std::vector<std::int64_t>{0, 0}});
    expectsError([&] { (void)apex::formats::convertFbxScene(shortIndices); },
                 "invalid_normal");

    auto outOfRange = fixture();
    outOfRange.roots[0].children[1].children[2] = normalLayer(
        "ByPolygonVertex", "IndexToDirect", FbxArray{std::vector<double>{
                                                  0.0, 0.0, 1.0}},
        FbxArray{std::vector<std::int64_t>{0, 1, 0}});
    expectsError([&] { (void)apex::formats::convertFbxScene(outOfRange); },
                 "invalid_normal");

    auto nonFinite = fixture();
    nonFinite.roots[0].children[1].children[2].children[2]
        .properties[0]
        .values[0] = FbxValue{FbxArray{std::vector<double>{
        0.0, 0.0, std::numeric_limits<double>::infinity()}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(nonFinite); },
                 "non_finite");

    auto unsupported = fixture();
    unsupported.roots[0].children[1].children[2] = normalLayer(
        "ByEdge", "Direct", FbxArray{std::vector<double>{0.0, 0.0, 1.0}});
    const auto unsupportedResult = apex::formats::convertFbxScene(unsupported);
    require(unsupportedResult.meshes[0].normals.empty() &&
                !unsupportedResult.complete &&
                std::any_of(unsupportedResult.diagnostics.begin(),
                            unsupportedResult.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code ==
                                       "unsupported_layer_mapping";
                            }),
            "unsupported normal mapping remains explicitly incomplete");

    auto overLimit = fixture();
    overLimit.roots[0].children[1].children[2] = normalLayer(
        "AllSame", "Direct",
        FbxArray{std::vector<double>{0.0, 0.0, 1.0, 0.0, 1.0, 0.0,
                                     1.0, 0.0, 0.0, -1.0, 0.0, 0.0}});
    auto limits = apex::formats::FbxConversionLimits{};
    limits.max_vertices = 3u;
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(overLimit, limits); },
        "vertex_limit");
}

void preservesPolygonMaterialSlotsAndRejectsMalformedLayers() {
    auto ordered = seamFixture();
    auto extraMaterial = ordered.roots[0].children[2];
    extraMaterial.properties[0].values[0] = std::int64_t(301);
    extraMaterial.properties[0].values[1] = std::string("Material::Glass");
    ordered.roots[0].children.push_back(std::move(extraMaterial));
    ordered.roots[0].children[1].children[4] = materialLayer(
        "ByPolygon", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{0, 1}});
    ordered.roots[1].children.insert(
        ordered.roots[1].children.begin() + 1,
        node("C", {std::string("OO"), std::int64_t(301),
                   std::int64_t(200)}));
    const auto orderedResult = apex::formats::convertFbxScene(ordered);
    require(orderedResult.meshes[0].triangle_material_slots ==
                std::vector<std::int32_t>{0, 1} &&
                orderedResult.node_geometry[0].materials ==
                    std::vector<apex::scene::MaterialId>{1u, 0u} &&
                orderedResult.complete,
            "polygon material slots and model connection order are retained");

    auto fan = seamFixture();
    fan.roots[0].children[1].children[1].properties[0].values[0] =
        FbxValue{FbxArray{std::vector<std::int64_t>{0, 1, 2, -4}}};
    fan.roots[0].children[1].children[3].children[3]
        .properties[0]
        .values[0] = FbxValue{FbxArray{
        std::vector<std::int64_t>{0, 1, 2, 3}}};
    fan.roots[0].children[1].children[4] = materialLayer(
        "ByPolygon", "Direct",
        FbxArray{std::vector<std::int64_t>{7}});
    const auto fanResult = apex::formats::convertFbxScene(fan);
    require(fanResult.meshes[0].triangle_indices.size() == 6u &&
                fanResult.meshes[0].triangle_material_slots ==
                    std::vector<std::int32_t>{7, 7},
            "one polygon material slot is replicated across fan triangles");

    auto allSameMultiple = seamFixture();
    allSameMultiple.roots[0].children[1].children[4] = materialLayer(
        "AllSame", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{7}});
    const auto allSameMultipleResult =
        apex::formats::convertFbxScene(allSameMultiple);
    require(allSameMultipleResult.meshes[0].triangle_material_slots ==
                std::vector<std::int32_t>{7, 7},
            "AllSame safely reproduces the native retained first slot");

    auto negative = fixture();
    negative.roots[0].children[1].children[4] = materialLayer(
        "AllSame", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{-1}});
    const auto negativeResult = apex::formats::convertFbxScene(negative);
    require(negativeResult.meshes[0].triangle_material_slots ==
                std::vector<std::int32_t>{-1},
            "native signed material slot is retained for safe fallback");

    auto missingData = fixture();
    missingData.roots[0].children[1].children[4].children.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(missingData); },
                 "invalid_material_layer");

    auto empty = fixture();
    empty.roots[0].children[1].children[4].children[2]
        .properties[0]
        .values[0] =
        FbxValue{FbxArray{std::vector<std::int64_t>{}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(empty); },
                 "invalid_material_layer");

    auto wrongType = fixture();
    wrongType.roots[0].children[1].children[4].children[2]
        .properties[0]
        .values[0] = FbxValue{FbxArray{std::vector<double>{0.0}}};
    expectsError([&] { (void)apex::formats::convertFbxScene(wrongType); },
                 "invalid_material_layer");

    auto extraValue = fixture();
    extraValue.roots[0].children[1].children[4].children[2]
        .properties[0]
        .values.push_back(std::int64_t(0));
    expectsError([&] { (void)apex::formats::convertFbxScene(extraValue); },
                 "invalid_material_layer");

    auto duplicateField = fixture();
    duplicateField.roots[0].children[1].children[4].children.push_back(
        duplicateField.roots[0].children[1].children[4].children.front());
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(duplicateField); },
        "invalid_material_layer");

    auto duplicateLayer = fixture();
    duplicateLayer.roots[0].children[1].children.push_back(
        duplicateLayer.roots[0].children[1].children[4]);
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(duplicateLayer); },
        "invalid_material_layer");

    auto shortByPolygon = seamFixture();
    shortByPolygon.roots[0].children[1].children[4] = materialLayer(
        "ByPolygon", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{0}});
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(shortByPolygon); },
        "invalid_material_layer");

    auto longByPolygon = seamFixture();
    longByPolygon.roots[0].children[1].children[4] = materialLayer(
        "ByPolygon", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{0, 0, 0}});
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(longByPolygon); },
        "invalid_material_layer");

    auto integerOverflow = fixture();
    integerOverflow.roots[0].children[1].children[4] = materialLayer(
        "AllSame", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{
            std::numeric_limits<std::int64_t>::max()}});
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(integerOverflow); },
        "invalid_material_layer");

    auto integerUnderflow = fixture();
    integerUnderflow.roots[0].children[1].children[4] = materialLayer(
        "AllSame", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{
            std::numeric_limits<std::int64_t>::min()}});
    expectsError(
        [&] { (void)apex::formats::convertFbxScene(integerUnderflow); },
        "invalid_material_layer");

    auto unsupportedMapping = fixture();
    unsupportedMapping.roots[0].children[1].children[4] = materialLayer(
        "ByControlPoint", "IndexToDirect");
    const auto unsupportedMappingResult =
        apex::formats::convertFbxScene(unsupportedMapping);
    require(!unsupportedMappingResult.complete &&
                unsupportedMappingResult.meshes[0]
                    .triangle_material_slots.empty(),
            "unsupported material mapping is not silently applied");

    auto unsupportedReference = fixture();
    unsupportedReference.roots[0].children[1].children[4] =
        materialLayer("ByPolygon", "Index");
    const auto unsupportedReferenceResult =
        apex::formats::convertFbxScene(unsupportedReference);
    require(!unsupportedReferenceResult.complete &&
                unsupportedReferenceResult.meshes[0]
                    .triangle_material_slots.empty(),
            "unsupported material reference is not silently applied");

    auto missingLayer = fixture();
    missingLayer.roots[0].children[1].children.pop_back();
    const auto missingLayerResult =
        apex::formats::convertFbxScene(missingLayer);
    require(!missingLayerResult.complete &&
                std::any_of(missingLayerResult.diagnostics.begin(),
                            missingLayerResult.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code ==
                                       "missing_material_layer";
                            }),
            "missing native material layer is explicitly incomplete");

    auto noNodeMaterials = fixture();
    noNodeMaterials.roots[1].children.pop_back();
    const auto noNodeMaterialsResult =
        apex::formats::convertFbxScene(noNodeMaterials);
    require(noNodeMaterialsResult.node_geometry[0].materials.empty() &&
                noNodeMaterialsResult.meshes[0].triangle_material_slots ==
                    std::vector<std::int32_t>{0},
            "unresolved raw slots remain bounded for native fallback");

    auto duplicateMaterialLink = fixture();
    duplicateMaterialLink.roots[1].children.push_back(
        duplicateMaterialLink.roots[1].children.back());
    const auto duplicateMaterialLinkResult =
        apex::formats::convertFbxScene(duplicateMaterialLink);
    require(duplicateMaterialLinkResult.node_geometry[0].materials ==
                std::vector<apex::scene::MaterialId>{0u, 0u},
            "duplicate node material links retain distinct slot positions");

    auto unexpectedPropertyConnection = fixture();
    unexpectedPropertyConnection.roots[1].children[1] = node(
        "C", {std::string("OP"), std::int64_t(300), std::int64_t(200),
              std::string("Materials")});
    expectsError(
        [&] {
            (void)apex::formats::convertFbxScene(
                unexpectedPropertyConnection);
        },
        "unsupported_connection");

    auto slotLimited = fixture();
    slotLimited.roots[0].children[1].children[4] = materialLayer(
        "AllSame", "IndexToDirect",
        FbxArray{std::vector<std::int64_t>{0, 0, 0, 0}});
    auto slotLimits = apex::formats::FbxConversionLimits{};
    slotLimits.max_indices = 3u;
    expectsError(
        [&] {
            (void)apex::formats::convertFbxScene(slotLimited, slotLimits);
        },
        "index_limit");
}

void rejectsMalformedNativeMaterialParameters() {
    auto duplicate = fixture();
    duplicate.roots[0].children[2].children[1].children.push_back(
        duplicate.roots[0].children[2].children[1].children.front());
    expectsError([&] { (void)apex::formats::convertFbxScene(duplicate); },
                 "invalid_material");

    auto truncated = fixture();
    truncated.roots[0].children[2].children[1].children.front()
        .properties[0]
        .values.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(truncated); },
                 "invalid_material");

    auto nonFinite = fixture();
    nonFinite.roots[0].children[2].children[1].children.front()
        .properties[0]
        .values[4] = std::numeric_limits<double>::infinity();
    expectsError([&] { (void)apex::formats::convertFbxScene(nonFinite); },
                 "invalid_material");

    auto legacy = fixture();
    legacy.roots[0].children[2].children[1].name = "Properties60";
    const auto legacyResult = apex::formats::convertFbxScene(legacy);
    require(legacyResult.material_parameters[0].diffuse_color ==
                std::array<float, 3u>{0.6F, 0.7F, 0.8F},
            "Properties60 retains native material values");
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
    unsupported.roots.push_back(node(
        "Image", {std::int64_t(600), std::string("Image::A"),
                  std::string("Clip")}));
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
    require(multipleGeometry && !multipleMaterial &&
                !multipleResult.complete &&
                multipleResult.meshes.size() == 2u &&
                multipleResult.node_geometry[0].materials ==
                    std::vector<apex::scene::MaterialId>{0u, 1u},
            "multiple geometries remain first-wins while material slots retain order");

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
    require(capability.static_geometry && capability.node_transforms &&
                capability.material_assignment &&
                capability.external_texture_references && capability.skinning &&
                capability.animation && capability.images &&
                !capability.layer_mappings,
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
        convertsBoundedNativeSkinning();
        appliesNativeGeometricMeshTransform();
        preservesBoundedFileTextureCandidates();
        preservesBoundedEmbeddedTextureCandidates();
        ignoresDisplayLayerMembershipEdges();
        handlesConstraintPoConnectionsStrictly();
        ignoresMissingOptionalAnimationCurveLinks();
        convertsUvSeamsAndFlipsV();
        convertsBoundedLinearAnimationToKsanimV2();
        selectsNativeAnimationModelsInHierarchyOrder();
        emitsBoundedStaticAnimationWithoutCurves();
        mergesDuplicateNativeAnimationSetNames();
        rejectsMalformedAnimationCurvesAndUnsupportedInterpolation();
        rejectsInvalidReferencesIndicesAndNonFiniteValues();
        preservesNativeNormalMappingsAndRejectsMalformedLayers();
        preservesPolygonMaterialSlotsAndRejectsMalformedLayers();
        rejectsMalformedNativeMaterialParameters();
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
