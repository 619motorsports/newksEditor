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

    auto malformed = geometricTransformFixture();
    malformed.roots[0].children[0].children[0].children.back().properties[0].values.pop_back();
    expectsError([&] { (void)apex::formats::convertFbxScene(malformed); }, "invalid_transform");
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
                            [](const auto& diagnostic) { return diagnostic.code == "unsupported_skinning"; }),
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
                !capability.skinning && capability.animation && !capability.images && !capability.layer_mappings,
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
        appliesNativeGeometricMeshTransform();
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
