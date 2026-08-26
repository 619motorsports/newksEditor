#include "apex/app/fbx_preview_document.hpp"

#include "apex/formats/acd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
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
}

} // namespace

int main() {
    try {
        opens_ready_owned_workspace_document();
        stages_incomplete_resources_without_backend_readiness();
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
