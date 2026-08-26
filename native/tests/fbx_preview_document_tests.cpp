#include "apex/app/fbx_preview_document.hpp"

#include "apex/domain/animation_preview.hpp"
#include "apex/formats/acd.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/scene/kn5_scene.hpp"
#include "apex/workspace/workspace_scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "DDS write is in bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
}

struct BinaryNode {
    std::string name;
    std::uint32_t property_count = 0U;
    std::vector<std::uint8_t> properties;
    std::vector<BinaryNode> children;
};

std::size_t binary_node_size(const BinaryNode& node) {
    std::size_t size = 13U + node.name.size() + node.properties.size() + 13U;
    for (const auto& child : node.children)
        size += binary_node_size(child);
    return size;
}

std::vector<std::uint8_t> binary_node_bytes(const BinaryNode& node,
                                            std::size_t start) {
    const auto end = start + binary_node_size(node);
    require(end <= std::numeric_limits<std::uint32_t>::max(),
            "binary fixture node offset fits FBX 7.4 fields");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(end - start);
    append32(bytes, static_cast<std::uint32_t>(end));
    append32(bytes, node.property_count);
    append32(bytes, static_cast<std::uint32_t>(node.properties.size()));
    require(node.name.size() <= 255U,
            "binary fixture node name fits FBX name length field");
    bytes.push_back(static_cast<std::uint8_t>(node.name.size()));
    bytes.insert(bytes.end(), node.name.begin(), node.name.end());
    bytes.insert(bytes.end(), node.properties.begin(), node.properties.end());
    auto child_start = start + 13U + node.name.size() + node.properties.size();
    for (const auto& child : node.children) {
        auto encoded = binary_node_bytes(child, child_start);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        child_start += encoded.size();
    }
    bytes.resize(bytes.size() + 13U, 0U);
    require(start + bytes.size() == end,
            "binary fixture node size matches its end offset");
    return bytes;
}

std::vector<std::uint8_t> binary_string_property(std::string_view value) {
    require(value.size() <= std::numeric_limits<std::uint32_t>::max(),
            "binary fixture string fits FBX length field");
    std::vector<std::uint8_t> bytes{'S'};
    append32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return bytes;
}

std::vector<std::uint8_t> binary_id_property(std::int64_t value) {
    std::vector<std::uint8_t> bytes{'L'};
    append64(bytes, static_cast<std::uint64_t>(value));
    return bytes;
}

std::vector<std::uint8_t> binary_double_array_property(
    std::initializer_list<double> values) {
    std::vector<std::uint8_t> bytes{'d'};
    append32(bytes, static_cast<std::uint32_t>(values.size()));
    append32(bytes, 0U);
    append32(bytes, static_cast<std::uint32_t>(values.size() * sizeof(double)));
    for (const auto value : values)
        append64(bytes, std::bit_cast<std::uint64_t>(value));
    return bytes;
}

std::vector<std::uint8_t> binary_double_property(double value) {
    std::vector<std::uint8_t> bytes{'D'};
    append64(bytes, std::bit_cast<std::uint64_t>(value));
    return bytes;
}

std::vector<std::uint8_t> binary_index_array_property(
    std::initializer_list<std::int32_t> values) {
    std::vector<std::uint8_t> bytes{'i'};
    append32(bytes, static_cast<std::uint32_t>(values.size()));
    append32(bytes, 0U);
    append32(bytes, static_cast<std::uint32_t>(values.size() * sizeof(std::int32_t)));
    for (const auto value : values)
        append32(bytes, static_cast<std::uint32_t>(value));
    return bytes;
}

std::vector<std::uint8_t> binary_raw_property(
    std::span<const std::uint8_t> bytes_in) {
    require(bytes_in.size() <= std::numeric_limits<std::uint32_t>::max(),
            "binary fixture raw content fits FBX length field");
    std::vector<std::uint8_t> bytes{'R'};
    append32(bytes, static_cast<std::uint32_t>(bytes_in.size()));
    bytes.insert(bytes.end(), bytes_in.begin(), bytes_in.end());
    return bytes;
}

BinaryNode binary_property_node(std::string name,
                                std::vector<std::uint8_t> property) {
    BinaryNode result;
    result.name = std::move(name);
    result.property_count = 1U;
    result.properties = std::move(property);
    return result;
}

BinaryNode binary_string_node(std::string name, std::string_view value) {
    return binary_property_node(std::move(name), binary_string_property(value));
}

BinaryNode binary_array_node(std::string name, std::vector<std::uint8_t> property) {
    return binary_property_node(std::move(name), std::move(property));
}

void append_property(std::vector<std::uint8_t>& properties,
                     std::vector<std::uint8_t> property) {
    properties.insert(properties.end(), property.begin(), property.end());
}

BinaryNode binary_object(std::string name, std::int64_t id,
                         std::string object_name, std::string type,
                         std::vector<BinaryNode> children = {}) {
    BinaryNode result;
    result.name = std::move(name);
    result.property_count = 3U;
    append_property(result.properties, binary_id_property(id));
    append_property(result.properties, binary_string_property(object_name));
    append_property(result.properties, binary_string_property(type));
    result.children = std::move(children);
    return result;
}

BinaryNode binary_connection(std::string kind, std::int64_t source,
                             std::int64_t target,
                             std::string property = {}) {
    BinaryNode result;
    result.name = "C";
    result.property_count = property.empty() ? 3U : 4U;
    append_property(result.properties, binary_string_property(kind));
    append_property(result.properties, binary_id_property(source));
    append_property(result.properties, binary_id_property(target));
    if (!property.empty())
        append_property(result.properties, binary_string_property(property));
    return result;
}

std::vector<std::uint8_t> rgba8_dds(
    std::array<std::uint8_t, 4U> pixel) {
    std::vector<std::uint8_t> bytes(152U, 0U);
    put32(bytes, 0U, 0x20534444U);
    put32(bytes, 4U, 124U);
    put32(bytes, 12U, 1U);
    put32(bytes, 16U, 1U);
    put32(bytes, 20U, 4U);
    put32(bytes, 28U, 1U);
    put32(bytes, 76U, 32U);
    put32(bytes, 80U, 0x40U);
    put32(bytes, 88U, 32U);
    put32(bytes, 92U, 0x000000ffU);
    put32(bytes, 96U, 0x0000ff00U);
    put32(bytes, 100U, 0x00ff0000U);
    put32(bytes, 104U, 0xff000000U);
    for (std::size_t index = 0U; index < pixel.size(); ++index)
        bytes[128U + index] = pixel[index];
    return bytes;
}

apex::formats::AcdEntry entry(std::string path,
                              std::vector<std::uint8_t> bytes) {
    apex::formats::AcdEntry result;
    result.name = path;
    result.path = std::move(path);
    result.safe = true;
    result.size = bytes.size();
    result.data = std::move(bytes);
    return result;
}

apex::assets::AssetSource source_with(
    std::vector<apex::formats::AcdEntry> entries) {
    apex::formats::AcdArchive archive;
    archive.source = "fixture/data.acd";
    archive.assetName = "fixture";
    archive.entries = std::move(entries);
    apex::assets::AssetSource source;
    source.addAcdArchive(std::move(archive));
    return source;
}

std::string ascii_fbx(bool material_layer = true) {
    std::string geometry_layers =
        "  LayerElementNormal: 0 {\n"
        "   MappingInformationType: \"ByPolygonVertex\"\n"
        "   ReferenceInformationType: \"Direct\"\n"
        "   Normals: *9 { a: 0.0,0.0,1.0,0.0,0.0,1.0,0.0,0.0,1.0 }\n"
        "  }\n"
        "  LayerElementUV: 0 {\n"
        "   MappingInformationType: \"ByPolygonVertex\"\n"
        "   ReferenceInformationType: \"IndexToDirect\"\n"
        "   UV: *6 { a: 0.0,0.0,1.0,0.0,0.0,1.0 }\n"
        "   UVIndex: *3 { a: 0,1,2 }\n"
        "  }\n";
    if (material_layer) {
        geometry_layers +=
            "  LayerElementMaterial: 0 {\n"
            "   MappingInformationType: \"AllSame\"\n"
            "   ReferenceInformationType: \"IndexToDirect\"\n"
            "   Materials: *1 { a: 0 }\n"
            "  }\n";
    }
    return
        "FBXVersion: 7400\n"
        "Objects: {\n"
        " Model: 200, \"Model::Triangle\", \"Mesh\" { }\n"
        " Geometry: 100, \"Geometry::Triangle\", \"Mesh\" {\n"
        "  Vertices: *9 { a: 0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0,0.0 }\n"
        "  PolygonVertexIndex: *3 { a: 0,1,-3 }\n" +
        geometry_layers +
        " }\n"
        " Material: 300, \"Material::Paint\", \"Material\" {\n"
        "  ShadingModel: \"Phong\"\n"
        "  Properties70: {\n"
        "   P: \"AmbientColor\", \"ColorRGB\", \"Color\", \"\", 0.2,0.2,0.2\n"
        "   P: \"DiffuseColor\", \"ColorRGB\", \"Color\", \"\", 0.8,0.8,0.8\n"
        "   P: \"SpecularColor\", \"ColorRGB\", \"Color\", \"\", 0.4,0.4,0.4\n"
        "   P: \"Shininess\", \"double\", \"Number\", \"\", 20\n"
        "  }\n"
        " }\n"
        " Texture: 400, \"Texture::Paint\", \"TextureVideoClip\" {\n"
        "  FileName: \"C:\\\\car\\\\texture\\\\paint.dds\"\n"
        " }\n"
        "}\n"
        "Connections: {\n"
        " C: \"OO\", 100, 200\n"
        " C: \"OO\", 300, 200\n"
        " C: \"OP\", 400, 300, \"DiffuseColor\"\n"
        "}\n";
}

std::string ascii_embedded_fbx(std::string_view base64) {
    auto text = ascii_fbx();
    const auto objects_end = text.find("}\nConnections: {");
    require(objects_end != std::string::npos,
            "ASCII FBX fixture has an Objects terminator");
    text.insert(
        objects_end,
        " Video: 500, \"Video::Paint\", \"Clip\" {\n"
        "  RelativeFilename: \"embedded.png\"\n"
        "  Content: ,\n"
        "  \"" + std::string(base64) + "\"\n"
        " }\n");
    const auto connections_end = text.rfind("}\n");
    require(connections_end != std::string::npos,
            "ASCII FBX fixture has a Connections terminator");
    text.insert(connections_end,
                " C: \"OO\", 500, 400\n");
    return text;
}

std::string ascii_skinned_fbx() {
    auto text = ascii_fbx();
    constexpr std::string_view mesh_model =
        " Model: 200, \"Model::Triangle\", \"Mesh\" { }\n";
    const auto mesh_model_at = text.find(mesh_model);
    require(mesh_model_at != std::string::npos,
            "ASCII FBX fixture has its mesh Model");
    text.replace(
        mesh_model_at, mesh_model.size(),
        " Model: 200, \"Model::Triangle\", \"Mesh\" {\n"
        "  Properties70: {\n"
        "   P: \"GeometricTranslation\", \"GeometricTranslation\", \"\", \"A\", 3.0,0.0,0.0\n"
        "  }\n"
        " }\n");

    const auto objects_end = text.find("}\nConnections: {");
    require(objects_end != std::string::npos,
            "ASCII FBX fixture has an Objects terminator");
    text.insert(
        objects_end,
        " Model: 600, \"Model::Bone0\", \"LimbNode\" {\n"
        "  Properties70: {\n"
        "   P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 2.0,0.0,0.0\n"
        "  }\n"
        " }\n"
        " Deformer: 700, \"Deformer::Skin\", \"Skin\" { }\n"
        " Deformer: 800, \"SubDeformer::Bone0\", \"Cluster\" {\n"
        "  Indexes: *3 { a: 0,1,2 }\n"
        "  Weights: *3 { a: 1.0,1.0,1.0 }\n"
        "  Transform: *1 { a: 7.0 }\n"
        "  TransformLink: *16 { a: 1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,2.0,0.0,0.0,1.0 }\n"
        " }\n"
        " AnimationStack: 900, \"AnimationStack::Bone Move\", \"AnimationStack\" {\n"
        "  LocalStart: 0\n"
        "  LocalStop: 100\n"
        " }\n"
        " AnimationLayer: 901, \"AnimationLayer::BaseLayer\", \"AnimationLayer\" { }\n"
        " AnimationCurveNode: 902, \"AnimationCurveNode::T\", \"AnimationCurveNode\" { }\n"
        " AnimationCurve: 903, \"AnimationCurve::TX\", \"AnimationCurve\" {\n"
        "  KeyTime: *2 { a: 0,100 }\n"
        "  KeyValueFloat: *2 { a: 2.0,4.0 }\n"
        "  KeyAttrFlags: *2 { a: 4,4 }\n"
        " }\n");
    const auto connections_end = text.rfind("}\n");
    require(connections_end != std::string::npos,
            "ASCII FBX fixture has a Connections terminator");
    text.insert(
        connections_end,
        " C: \"OO\", 700, 100\n"
        " C: \"OO\", 800, 700\n"
        " C: \"OO\", 600, 800\n"
        " C: \"OO\", 901, 900\n"
        " C: \"OO\", 902, 901\n"
        " C: \"OP\", 902, 600, \"Lcl Translation\"\n"
        " C: \"OP\", 903, 902, \"d|X\"\n");
    return text;
}

const apex::formats::Kn5Node* find_skinned_node(
    const apex::formats::Kn5Node& node) {
    if (node.kind == "skinnedMesh") return &node;
    for (const auto& child : node.children)
        if (const auto* found = find_skinned_node(child); found != nullptr)
            return found;
    return nullptr;
}

std::vector<std::uint8_t> binary_embedded_fbx(
    std::span<const std::uint8_t> png) {
    BinaryNode normal;
    normal.name = "LayerElementNormal";
    normal.children = {
        binary_string_node("MappingInformationType", "ByPolygonVertex"),
        binary_string_node("ReferenceInformationType", "Direct"),
        binary_array_node("Normals", binary_double_array_property(
                                      {0.0, 0.0, 1.0, 0.0, 0.0, 1.0,
                                       0.0, 0.0, 1.0}))};

    BinaryNode uv;
    uv.name = "LayerElementUV";
    uv.children = {
        binary_string_node("MappingInformationType", "ByPolygonVertex"),
        binary_string_node("ReferenceInformationType", "IndexToDirect"),
        binary_array_node("UV", binary_double_array_property(
                                  {0.0, 0.0, 1.0, 0.0, 0.0, 1.0})),
        binary_array_node("UVIndex", binary_index_array_property({0, 1, 2}))};

    BinaryNode materials;
    materials.name = "LayerElementMaterial";
    materials.children = {
        binary_string_node("MappingInformationType", "AllSame"),
        binary_string_node("ReferenceInformationType", "IndexToDirect"),
        binary_array_node("Materials", binary_index_array_property({0}))};

    BinaryNode geometry = binary_object(
        "Geometry", 100, "Geometry::Triangle", "Mesh",
        {binary_array_node("Vertices", binary_double_array_property(
                                        {0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                         0.0, 1.0, 0.0})),
         binary_array_node("PolygonVertexIndex",
                           binary_index_array_property({0, 1, -3})),
         std::move(normal), std::move(uv), std::move(materials)});

    BinaryNode diffuse;
    diffuse.name = "P";
    diffuse.property_count = 7U;
    append_property(diffuse.properties,
                    binary_string_property("DiffuseColor"));
    append_property(diffuse.properties, binary_string_property("ColorRGB"));
    append_property(diffuse.properties, binary_string_property("Color"));
    append_property(diffuse.properties, binary_string_property(""));
    append_property(diffuse.properties, binary_double_property(0.8));
    append_property(diffuse.properties, binary_double_property(0.8));
    append_property(diffuse.properties, binary_double_property(0.8));

    BinaryNode properties70;
    properties70.name = "Properties70";
    properties70.children.push_back(std::move(diffuse));
    BinaryNode material = binary_object(
        "Material", 300, "Material::Paint", "Material",
        {binary_string_node("ShadingModel", "Phong"),
         std::move(properties70)});

    BinaryNode texture = binary_object(
        "Texture", 400, "Texture::Paint", "TextureVideoClip",
        {binary_string_node("FileName", "C:\\car\\texture\\paint.dds")});
    BinaryNode video = binary_object(
        "Video", 500, "Video::Paint", "Clip",
        {binary_string_node("RelativeFilename", "embedded.png"),
         binary_property_node("Content", binary_raw_property(png))});
    BinaryNode model = binary_object(
        "Model", 200, "Model::Triangle", "Mesh");

    BinaryNode objects;
    objects.name = "Objects";
    objects.children = {std::move(model), std::move(geometry),
                        std::move(material), std::move(texture),
                        std::move(video)};

    BinaryNode connections;
    connections.name = "Connections";
    connections.children = {
        binary_connection("OO", 100, 200),
        binary_connection("OO", 300, 200),
        binary_connection("OP", 400, 300, "DiffuseColor"),
        binary_connection("OO", 500, 400)};

    std::vector<std::uint8_t> bytes{
        'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X', ' ', 'B',
        'i', 'n', 'a', 'r', 'y', ' ', ' ', 0x1aU, 0x00U, 0x00U};
    append32(bytes, 7400U);
    auto encoded_objects = binary_node_bytes(objects, bytes.size());
    bytes.insert(bytes.end(), encoded_objects.begin(), encoded_objects.end());
    auto encoded_connections = binary_node_bytes(connections, bytes.size());
    bytes.insert(bytes.end(), encoded_connections.begin(), encoded_connections.end());
    bytes.resize(bytes.size() + 13U, 0U);
    return bytes;
}

apex::app::FbxPreviewDocumentRequest request_for(
    const std::string& text,
    const apex::assets::AssetSource* source = nullptr) {
    apex::app::FbxPreviewDocumentRequest request;
    request.source = "triangle.fbx";
    request.bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    if (source != nullptr)
        request.textures = apex::app::FbxPreviewTextureGrant{
            "fbx-preview-root", source};
    return request;
}

apex::app::FbxPreviewDocumentRequest request_for(
    const std::vector<std::uint8_t>& bytes, std::string source = "scene.fbx",
    const apex::assets::AssetSource* texture_source = nullptr) {
    apex::app::FbxPreviewDocumentRequest request;
    request.source = std::move(source);
    request.bytes = bytes;
    if (texture_source != nullptr)
        request.textures = apex::app::FbxPreviewTextureGrant{
            "fbx-preview-root", texture_source};
    return request;
}

void opens_ready_owned_workspace_document() {
    const auto text = ascii_fbx();
    auto source = source_with({
        entry("texture/paint.dds", rgba8_dds({10U, 20U, 30U, 255U}))});
    const auto result = apex::app::open_fbx_preview_document(
        request_for(text, &source));
    if (!result.gpu_renderable()) {
        std::cerr << "ready fixture status="
                  << apex::app::fbx_preview_document_status_name(result.status)
                  << '\n';
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << " [" << diagnostic.path
                      << "]: " << diagnostic.message << '\n';
    }
    require(result.status == apex::app::FbxPreviewDocumentStatus::ready &&
                result.ok() && result.gpu_renderable() &&
                result.document->assembly.workspace.kind == "generic" &&
                result.document->assembly.workspace.files.size() == 1U &&
                result.document->scene.snapshot.workspace_kind == "generic" &&
                result.document->sceneBinding.file_root_nodes.size() == 1U,
            "authorized FBX becomes a standard ready workspace document");
    const auto& model = result.document->assembly.model;
    require(model.textures.size() == 1U &&
                model.textures[0].name == "paint.dds" &&
                model.textures[0].data ==
                    rgba8_dds({10U, 20U, 30U, 255U}) &&
                model.materials.size() == 1U &&
                model.materials[0].resources.size() == 1U,
            "workspace owns the authorized FBX texture payload");
    source = source_with({});
    require(model.textures[0].data ==
                rgba8_dds({10U, 20U, 30U, 255U}),
            "source replacement cannot mutate the published document");
}

void opens_skinned_ascii_through_workspace_packets() {
    const auto text = ascii_skinned_fbx();
    auto source = source_with({
        entry("texture/paint.dds", rgba8_dds({10U, 20U, 30U, 255U}))});
    auto result = apex::app::open_fbx_preview_document(
        request_for(text, &source));
    if (!result.gpu_renderable()) {
        std::cerr << "skinned fixture status="
                  << apex::app::fbx_preview_document_status_name(result.status)
                  << '\n';
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << " [" << diagnostic.path
                      << "]: " << diagnostic.message << '\n';
    }
    require(result.status == apex::app::FbxPreviewDocumentStatus::ready &&
                result.gpu_renderable() && result.document.has_value(),
            "serialized ASCII FBX skin becomes a ready workspace document");

    const auto& model = result.document->assembly.model;
    const auto* mesh = find_skinned_node(model.root);
    constexpr apex::formats::Kn5Matrix4 inverse_bind = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        -2.0F, 0.0F, 0.0F, 1.0F};
    require(mesh != nullptr && mesh->type == 3U &&
                mesh->vertexStride == 19U && mesh->vertices.size() == 57U &&
                mesh->bones.size() == 1U && mesh->bones[0].name == "Bone0" &&
                mesh->bones[0].transform == inverse_bind &&
                mesh->vertices[0] == 3.0F && mesh->vertices[11] == 1.0F &&
                mesh->vertices[15] == 0.0F &&
                model.materials[mesh->materialId].shader == "ksSkinnedMesh",
            "parser, converter, adapter, and workspace retain the skin ABI");

    const auto packets = apex::render::build_draw_packets(
        model, result.document->scene.snapshot,
        apex::render::DrawPacketOptions{},
        apex::render::DrawPacketLimits{});
    require(packets.supported && packets.packets.size() == 1U &&
                packets.packets[0].primitive ==
                    apex::render::DrawPrimitiveKind::skinned_mesh &&
                packets.packets[0].bone_palette.size() == 1U &&
                packets.packets[0].bone_palette[0] ==
                    apex::scene::identity_matrix &&
                std::any_of(
                    packets.unsupported_effects.begin(),
                    packets.unsupported_effects.end(), [](const auto& value) {
                        return value.code == "skinning_execution_staged";
                    }),
            "bind pose resolves to identity through workspace draw packets");

    require(result.animations.size() == 1U,
            "serialized skinned FBX publishes one bounded animation clip");
    auto& workspace_document = *result.document;
    const auto applied = apex::domain::apply_animation_preview(
        workspace_document.assembly.model,
        result.animations[0].animation, 0.5F);
    require(applied.matched_nodes == 1U && applied.skinning_required,
            "FBX bone animation requests CPU skinning");
    workspace_document.scene = apex::scene::convertKn5Scene(
        workspace_document.assembly.model);
    workspace_document.sceneBinding = apex::workspace::bindWorkspaceScene(
        workspace_document.scene.snapshot,
        workspace_document.assembly.workspace);
    const auto animated_packets = apex::render::build_draw_packets(
        workspace_document.assembly.model,
        workspace_document.scene.snapshot,
        apex::render::DrawPacketOptions{},
        apex::render::DrawPacketLimits{});
    require(animated_packets.supported &&
                animated_packets.packets.size() == 1U &&
                animated_packets.packets[0].bone_palette.size() == 1U &&
                std::abs(animated_packets.packets[0].bone_palette[0][12] -
                         1.0F) < 1.0e-6F,
            "animated FBX bone reaches the workspace draw-packet palette");
    const auto skinned = apex::render::skin_vertices_reference(
        mesh->vertices, animated_packets.packets[0].bone_palette,
        animated_packets.packets[0].world_matrix);
    require(skinned.size() == mesh->vertices.size() &&
                std::abs(skinned[0] - 4.0F) < 1.0e-6F,
            "animated FBX palette moves the geometrically baked vertex once");
}

void stages_incomplete_resources_without_backend_readiness() {
    const auto text = ascii_fbx();
    const auto result = apex::app::open_fbx_preview_document(
        request_for(text));
    require(result.status == apex::app::FbxPreviewDocumentStatus::staged &&
                result.ok() && !result.gpu_renderable() &&
                result.document.has_value() &&
                result.document->assembly.model.textures.empty(),
            "FBX without an explicit texture grant remains staged");

    const auto missing_layers = ascii_fbx(false);
    auto source = source_with({
        entry("paint.dds", rgba8_dds({1U, 2U, 3U, 255U}))});
    const auto incomplete = apex::app::open_fbx_preview_document(
        request_for(missing_layers, &source));
    require(incomplete.status ==
                apex::app::FbxPreviewDocumentStatus::staged &&
                incomplete.document.has_value(),
            "unsupported source behavior publishes only a staged document");
}

void opens_embedded_content_without_external_authority() {
    constexpr std::string_view png_base64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAEElEQVR4AQEFAPr/AAcLDf8BWwEfJ8p0dAAAAABJRU5ErkJggg==";
    const auto text = ascii_embedded_fbx(png_base64);
    const auto result = apex::app::open_fbx_preview_document(
        request_for(text));
    require(result.status == apex::app::FbxPreviewDocumentStatus::ready &&
                result.gpu_renderable() && result.document.has_value() &&
                result.document->assembly.model.textures.size() == 1U &&
                result.document->assembly.model.textures[0].name ==
                    "embedded.png" &&
                result.document->assembly.model.textures[0].data.size() == 73U &&
                result.document->assembly.model.textures[0].data[0] == 0x89U &&
                result.document->assembly.model.textures[0].data[1] == 0x50U,
            "embedded FBX opens ready without an external asset grant");

    auto ambiguous = source_with({
        entry("one/paint.dds", rgba8_dds({1U, 2U, 3U, 255U})),
        entry("two/paint.dds", rgba8_dds({4U, 5U, 6U, 255U}))});
    const auto shadowed = apex::app::open_fbx_preview_document(
        request_for(text, &ambiguous));
    require(shadowed.status ==
                apex::app::FbxPreviewDocumentStatus::ready &&
                shadowed.gpu_renderable(),
            "embedded FBX content shadows ambiguous external fallbacks");

    const auto truncated = ascii_embedded_fbx(
        "iVBORw0KGgoAAAANSUhEUgAAAAE=");
    const auto staged = apex::app::open_fbx_preview_document(
        request_for(truncated));
    require(staged.status == apex::app::FbxPreviewDocumentStatus::staged &&
                staged.ok() && !staged.gpu_renderable() &&
                staged.document->assembly.model.textures.empty() &&
                std::any_of(staged.diagnostics.begin(),
                            staged.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code.find(
                                           "embedded_texture_decode_") == 0U;
                            }),
            "truncated embedded image stages before backend preparation");
}

void opens_binary_embedded_content_end_to_end() {
    const std::vector<std::uint8_t> png = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
        0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1fU, 0x15U, 0xc4U,
        0x89U, 0x00U, 0x00U, 0x00U, 0x10U, 0x49U, 0x44U, 0x41U,
        0x54U, 0x78U, 0x01U, 0x01U, 0x05U, 0x00U, 0xfaU, 0xffU,
        0x00U, 0x07U, 0x0bU, 0x0dU, 0xffU, 0x01U, 0x5bU, 0x01U,
        0x1fU, 0x27U, 0xcaU, 0x74U, 0x74U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U,
        0x82U};
    require(png.size() == 73U, "binary fixture embeds the complete PNG payload");
    const std::vector<std::uint8_t> png_bytes(png.begin(), png.end());
    const auto binary = binary_embedded_fbx(png);

    const auto parsed = apex::formats::parseFbx(binary, "embedded-binary.fbx");
    require(parsed.header.format == apex::formats::FbxFormat::binary &&
                parsed.header.version == 7400U && parsed.roots.size() == 2U,
            "binary embedded fixture parses as a bounded FBX document");
    const auto conversion = apex::formats::convertFbxScene(parsed);
    require(conversion.complete && conversion.embedded_images.size() == 1U &&
                conversion.embedded_images[0].basename == "embedded.png" &&
                conversion.embedded_images[0].content == png_bytes &&
                conversion.embedded_texture_candidates.size() == 1U &&
                conversion.embedded_texture_candidates[0].texture_object_id ==
                    400 &&
                conversion.embedded_texture_candidates[0].video_object_id ==
                    500 &&
                conversion.embedded_texture_candidates[0].channel ==
                    "DiffuseColor",
            "binary Video raw content follows its Texture and Material links");

    const auto result = apex::app::open_fbx_preview_document(
        request_for(binary, "embedded-binary.fbx"));
    require(result.status == apex::app::FbxPreviewDocumentStatus::ready &&
                result.gpu_renderable() && result.document.has_value() &&
                result.document->assembly.model.textures.size() == 1U &&
                result.document->assembly.model.textures[0].name ==
                    "embedded.png" &&
                result.document->assembly.model.textures[0].data == png_bytes,
            "binary embedded FBX is ready through preview parsing and conversion");

    const auto payload = std::search(binary.begin(), binary.end(),
                                     png.begin(), png.end());
    require(payload != binary.end(),
            "binary fixture retains a searchable embedded PNG payload");
    const auto payload_offset = static_cast<std::size_t>(
        std::distance(binary.begin(), payload));
    auto truncated = binary;
    truncated.resize(payload_offset + png.size() - 1U);
    bool rejected = false;
    try {
        (void)apex::formats::parseFbx(truncated, "embedded-binary-truncated.fbx");
    } catch (const apex::formats::FbxError& error) {
        rejected = error.stage() == apex::formats::FbxStage::binary_dom &&
                   (error.code() == "truncated" || error.code() == "offset");
    }
    require(rejected,
            "binary embedded Content truncation is rejected at the parser boundary");
}

void rejects_malformed_input_and_authority_atomically() {
    std::string truncated = "Kaydara FBX Binary  \0";
    auto invalid = apex::app::open_fbx_preview_document(
        request_for(truncated));
    require(invalid.status ==
                apex::app::FbxPreviewDocumentStatus::invalid_request &&
                !invalid.document.has_value() &&
                !invalid.diagnostics.empty(),
            "truncated FBX returns no document");

    const auto text = ascii_fbx();
    auto unsafe_request = request_for(text);
    unsafe_request.source = "../triangle.fbx";
    invalid = apex::app::open_fbx_preview_document(unsafe_request);
    require(invalid.status ==
                apex::app::FbxPreviewDocumentStatus::invalid_request &&
                !invalid.document.has_value(),
            "preview source is a logical name, not an external path");

    auto missing_grant = request_for(text);
    missing_grant.textures = apex::app::FbxPreviewTextureGrant{};
    invalid = apex::app::open_fbx_preview_document(missing_grant);
    require(invalid.status ==
                apex::app::FbxPreviewDocumentStatus::invalid_request &&
                !invalid.document.has_value(),
            "malformed texture grant fails before parsing resources");

    auto ambiguous = source_with({
        entry("one/paint.dds", rgba8_dds({1U, 2U, 3U, 255U})),
        entry("two/paint.dds", rgba8_dds({4U, 5U, 6U, 255U}))});
    invalid = apex::app::open_fbx_preview_document(
        request_for(text, &ambiguous));
    require(invalid.status ==
                apex::app::FbxPreviewDocumentStatus::invalid_request &&
                !invalid.document.has_value(),
            "ambiguous authorized texture names fail atomically");

    auto malformed_skin = ascii_skinned_fbx();
    constexpr std::string_view weights =
        "  Weights: *3 { a: 1.0,1.0,1.0 }";
    const auto weights_at = malformed_skin.find(weights);
    require(weights_at != std::string::npos,
            "ASCII skin fixture has its weight array");
    malformed_skin.replace(weights_at, weights.size(),
                           "  Weights: *2 { a: 1.0,1.0 }");
    invalid = apex::app::open_fbx_preview_document(
        request_for(malformed_skin));
    require(invalid.status ==
                apex::app::FbxPreviewDocumentStatus::invalid_request &&
                !invalid.document.has_value(),
            "mismatched serialized skin arrays publish no partial document");

    auto malformed_animation = ascii_skinned_fbx();
    constexpr std::string_view animation_values =
        "  KeyValueFloat: *2 { a: 2.0,4.0 }";
    const auto animation_values_at =
        malformed_animation.find(animation_values);
    require(animation_values_at != std::string::npos,
            "ASCII skin fixture has its animation value array");
    malformed_animation.replace(animation_values_at,
                                animation_values.size(),
                                "  KeyValueFloat: *1 { a: 2.0 }");
    invalid = apex::app::open_fbx_preview_document(
        request_for(malformed_animation));
    require(invalid.status ==
                apex::app::FbxPreviewDocumentStatus::invalid_request &&
                !invalid.document.has_value() &&
                std::any_of(invalid.diagnostics.begin(),
                            invalid.diagnostics.end(), [](const auto& value) {
                                return value.code == "invalid_animation";
                            }),
            "mismatched serialized animation arrays publish no document");
}

void enforces_composition_limits() {
    const auto text = ascii_fbx(false);
    auto limits = apex::app::FbxPreviewDocumentLimits{};
    limits.max_diagnostics = 1U;
    const auto diagnostic_limited =
        apex::app::open_fbx_preview_document(request_for(text), limits);
    require(diagnostic_limited.status ==
                apex::app::FbxPreviewDocumentStatus::resource_limit &&
                !diagnostic_limited.document.has_value(),
            "aggregate preview diagnostic limit is enforced atomically");

    limits = {};
    limits.workspace.maxNodes = 1U;
    const auto workspace_limited =
        apex::app::open_fbx_preview_document(request_for(text), limits);
    require(workspace_limited.status ==
                apex::app::FbxPreviewDocumentStatus::resource_limit &&
                !workspace_limited.document.has_value(),
            "workspace composition limit publishes no partial document");

    const auto textured = ascii_fbx();
    auto large_texture = rgba8_dds({1U, 2U, 3U, 255U});
    large_texture.resize(1U * 1024U * 1024U, 0U);
    auto source = source_with({
        entry("paint.dds", std::move(large_texture))});
    limits = {};
    limits.workspace.maxAggregateBytes = 64U * 1024U;
    const auto generated_limited = apex::app::open_fbx_preview_document(
        request_for(textured, &source), limits);
    require(generated_limited.status ==
                apex::app::FbxPreviewDocumentStatus::resource_limit &&
                !generated_limited.document.has_value(),
            "workspace charges generated FBX model bytes, not only source bytes");
}

} // namespace

int main() {
    try {
        opens_ready_owned_workspace_document();
        opens_skinned_ascii_through_workspace_packets();
        stages_incomplete_resources_without_backend_readiness();
        opens_embedded_content_without_external_authority();
        opens_binary_embedded_content_end_to_end();
        rejects_malformed_input_and_authority_atomically();
        enforces_composition_limits();
        std::cout << "fbx preview document tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fbx preview document tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
