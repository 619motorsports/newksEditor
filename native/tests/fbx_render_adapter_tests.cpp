#include "apex/render/fbx_render_adapter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using apex::formats::FbxMaterialParameters;
using apex::formats::FbxSceneConversion;
using apex::formats::FbxStaticMesh;
using apex::render::ExternalTextureAuthorityStatus;
using apex::render::FbxExternalTextureAuthorityResult;
using apex::render::FbxExternalTextureSelection;
using apex::render::FbxRenderAdapterStatus;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

apex::scene::Matrix4 translation(float x, float y, float z) {
    auto result = apex::scene::identity_matrix;
    result[12] = x;
    result[13] = y;
    result[14] = z;
    return result;
}

FbxMaterialParameters parameters(float base = 0.2F,
                                 float shininess = 0.5F) {
    FbxMaterialParameters result;
    result.recognized_surface = true;
    result.ambient_color = std::array<float, 3U>{base, 0.4F, 0.6F};
    result.diffuse_color = std::array<float, 3U>{base + 0.1F, 0.5F, 0.7F};
    result.specular_color = std::array<float, 3U>{base + 0.2F, 0.6F, 0.8F};
    result.shininess = shininess;
    return result;
}

FbxSceneConversion fixture(std::size_t material_count = 1U) {
    FbxSceneConversion result;
    for (std::size_t index = 0U; index < material_count; ++index) {
        (void)result.snapshot.add_material(
            {"Material" + std::to_string(index), "ksPerPixel",
             apex::scene::BlendMode::opaque});
        result.material_parameters.push_back(
            parameters(0.2F + static_cast<float>(index) * 0.1F));
    }

    apex::scene::SceneNode root;
    root.name = "FBX";
    root.kind = apex::scene::NodeKind::node;
    const auto root_id = result.snapshot.add_node(std::move(root));
    apex::scene::SceneNode model;
    model.name = "Body";
    model.kind = apex::scene::NodeKind::mesh;
    model.material = material_count == 0U ? apex::scene::invalid_material_id
                                          : 0U;
    const auto model_id =
        result.snapshot.add_node(std::move(model), root_id);

    FbxStaticMesh mesh;
    mesh.object_id = 100;
    mesh.name = "Body";
    mesh.positions = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                      0.0F, 0.0F, 1.0F, 0.0F};
    mesh.normals = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                    1.0F, 0.0F, 0.0F, 1.0F};
    mesh.uvs = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -1.0F};
    mesh.triangle_indices = {0U, 1U, 2U};
    mesh.triangle_material_slots = {0};
    result.meshes.push_back(std::move(mesh));
    result.transforms.push_back(
        {model_id, translation(5.0F, 0.0F, 0.0F),
         translation(5.0F, 0.0F, 0.0F)});
    std::vector<apex::scene::MaterialId> materials;
    for (std::size_t index = 0U; index < material_count; ++index)
        materials.push_back(static_cast<apex::scene::MaterialId>(index));
    result.node_geometry.push_back(
        {model_id, 0U, translation(0.0F, 2.0F, 0.0F),
         std::move(materials)});
    return result;
}

FbxExternalTextureAuthorityResult textures(
    std::size_t material_count,
    const std::vector<std::string>& selected_basenames) {
    FbxExternalTextureAuthorityResult result;
    result.status = ExternalTextureAuthorityStatus::ready;
    result.authority.status = ExternalTextureAuthorityStatus::ready;
    result.material_selection_indices.assign(
        material_count, apex::render::invalid_fbx_texture_selection);
    for (std::size_t material = 0U; material < selected_basenames.size();
         ++material) {
        apex::render::ExternalTextureResource resource;
        resource.source_bytes = {
            static_cast<std::uint8_t>(0x40U + material), 0x50U, 0x60U, 0x70U};
        const auto authority_index = result.authority.resources.size();
        result.authority.resources.push_back(std::move(resource));
        FbxExternalTextureSelection selection;
        selection.material_index = material;
        selection.candidate_index = material;
        selection.texture_object_id = static_cast<std::int64_t>(400U + material);
        selection.channel = "DiffuseColor";
        selection.basename = selected_basenames[material];
        selection.authority_resource_index = authority_index;
        result.material_selection_indices[material] = result.selections.size();
        result.selections.push_back(std::move(selection));
    }
    return result;
}

const apex::formats::Kn5MaterialProperty* property(
    const apex::formats::Kn5Material& material, const std::string& name) {
    const auto found = std::find_if(
        material.properties.begin(), material.properties.end(),
        [&](const auto& value) { return value.name == name; });
    return found == material.properties.end() ? nullptr : &*found;
}

void buildsOwnedCanonicalScene() {
    auto conversion = fixture();
    auto authority = textures(1U, {"paint.png"});
    const auto original_bytes = authority.authority.resources[0].source_bytes;
    auto result = apex::render::build_fbx_render_scene(
        conversion, &authority, "body.fbx");
    require(result.status == FbxRenderAdapterStatus::ready && result.ok() &&
                result.gpu_renderable(),
            "complete textured FBX conversion is ready");
    require(result.model->source == "body.fbx" && result.model->version == 6U &&
                result.model->root.type == 1U &&
                result.model->root.children.size() == 1U,
            "canonical KN5 root");
    const auto& wrapper = result.model->root.children[0];
    require(wrapper.type == 1U && wrapper.name == "Body" &&
                wrapper.transform[12] == 5.0F &&
                wrapper.children.size() == 1U,
            "FBX model becomes a KN5 transform wrapper");
    const auto& mesh = wrapper.children[0];
    require(mesh.type == 2U && mesh.kind == "mesh" && mesh.name == "Body" &&
                mesh.vertexStride == 11U && mesh.vertices.size() == 33U &&
                mesh.indices == std::vector<std::uint16_t>{0U, 1U, 2U} &&
                mesh.vertices[1] == 2.0F && mesh.renderable,
            "geometric transform is baked into canonical static geometry");
    require(std::all_of(mesh.vertices.begin(), mesh.vertices.end(),
                        [](float value) { return std::isfinite(value); }) &&
                std::abs(mesh.vertices[8] + 1.0F) < 1.0e-6F,
            "recovered tangent output is finite and ordered");
    require(mesh.bounds[3] > 0.7F && result.scene->snapshot.nodes.size() == 3U &&
                result.scene->preview_bounds.has_value() &&
                result.scene->preview_bounds->minimum[0] == 5.0F &&
                result.scene->preview_bounds->minimum[1] == 2.0F,
            "canonical scene is regenerated from the owned model");

    const auto& material = result.model->materials[0];
    require(material.shader == "ksPerPixel" &&
                property(material, "ksAmbient") != nullptr &&
                property(material, "ksAmbient")->value == 0.2F &&
                property(material, "ksDiffuse")->value == 0.3F &&
                property(material, "ksSpecular")->value == 0.4F &&
                property(material, "ksSpecularEXP")->value == 10.0F &&
                material.resources.size() == 1U &&
                material.resources[0].slot == "txDiffuse" &&
                material.resources[0].textureId == 0U &&
                material.resources[0].texture == "paint.png",
            "native material scalars and diffuse binding");
    require(result.model->textures.size() == 1U &&
                result.model->textures[0].data == original_bytes &&
                result.model->textures[0].size == original_bytes.size(),
            "adapter owns exact selected texture bytes");
    authority.authority.resources[0].source_bytes.clear();
    require(result.model->textures[0].data == original_bytes,
            "authority mutation cannot affect the canonical model");
}

void preservesNativeBatchOrderNamesAndMaterialResolution() {
    auto conversion = fixture(2U);
    auto& mesh = conversion.meshes[0];
    mesh.positions.insert(mesh.positions.end(),
                          {2.0F, 0.0F, 0.0F, 3.0F, 0.0F, 0.0F,
                           2.0F, 1.0F, 0.0F});
    mesh.normals.insert(mesh.normals.end(),
                        {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                         0.0F, 0.0F, 1.0F});
    mesh.uvs.insert(mesh.uvs.end(),
                    {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -1.0F});
    mesh.triangle_indices = {0U, 1U, 2U, 3U, 4U, 5U};
    mesh.triangle_material_slots = {1, 0};
    conversion.node_geometry[0].materials = {1U, 0U};
    auto authority = textures(2U, {"first.png", "second.png"});
    const auto result =
        apex::render::build_fbx_render_scene(conversion, &authority);
    require(result.status == FbxRenderAdapterStatus::ready &&
                result.model->root.children[0].children.size() == 2U,
            "multi-material FBX conversion is ready");
    const auto& batches = result.model->root.children[0].children;
    require(batches[0].name == "Body_SUB0" &&
                batches[1].name == "Body_SUB1" &&
                batches[0].materialId == 0U &&
                batches[1].materialId == 1U &&
                batches[0].vertices[0] == 0.0F &&
                batches[1].vertices[0] == 2.0F,
            "first-seen batches use recovered names and local material slots");

    auto single = fixture(2U);
    single.meshes[0].triangle_material_slots = {7};
    single.node_geometry[0].materials = {1U, 0U};
    const auto singleResult =
        apex::render::build_fbx_render_scene(single, &authority);
    require(singleResult.model->root.children[0].children[0].materialId == 1U,
            "single native batch resolves literal local material slot zero");
}

void stagesMissingResourcesAndUsesBoundedFallback() {
    auto conversion = fixture();
    const auto staged = apex::render::build_fbx_render_scene(conversion);
    require(staged.status == FbxRenderAdapterStatus::staged && staged.ok() &&
                !staged.gpu_renderable() && staged.model.has_value() &&
                staged.scene.has_value() &&
                staged.model->root.children[0].children[0].renderable &&
                staged.model->materials[0].resources.empty(),
            "missing texture stages but does not discard canonical geometry");

    auto invalidSlot = fixture();
    auto& mesh = invalidSlot.meshes[0];
    const auto positions_copy = mesh.positions;
    const auto normals_copy = mesh.normals;
    const auto uvs_copy = mesh.uvs;
    mesh.positions.insert(mesh.positions.end(), positions_copy.begin(),
                          positions_copy.end());
    mesh.normals.insert(mesh.normals.end(), normals_copy.begin(),
                        normals_copy.end());
    mesh.uvs.insert(mesh.uvs.end(), uvs_copy.begin(), uvs_copy.end());
    mesh.triangle_indices = {0U, 1U, 2U, 3U, 4U, 5U};
    mesh.triangle_material_slots = {0, -1};
    auto authority = textures(1U, {"paint.png"});
    const auto fallback =
        apex::render::build_fbx_render_scene(invalidSlot, &authority);
    require(fallback.status == FbxRenderAdapterStatus::staged &&
                fallback.model->materials.size() == 2U &&
                fallback.model->materials[1].name == "FBX_DEFAULT" &&
                fallback.model->root.children[0].children[1].materialId == 1U,
            "invalid multi-batch slot gets the recovered plain material fallback");

    auto incomplete = fixture();
    incomplete.complete = false;
    const auto incompleteResult =
        apex::render::build_fbx_render_scene(incomplete, &authority);
    require(incompleteResult.status == FbxRenderAdapterStatus::staged &&
                std::any_of(incompleteResult.diagnostics.begin(),
                            incompleteResult.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code ==
                                       "incomplete_fbx_conversion";
                            }),
            "unsupported source behavior remains explicitly staged");
}

void rejectsMalformedInputAtomically() {
    const auto rejected = [](FbxSceneConversion conversion,
                             FbxRenderAdapterStatus expected) {
        const auto result = apex::render::build_fbx_render_scene(conversion);
        require(result.status == expected && !result.model.has_value() &&
                    !result.scene.has_value() &&
                    !result.diagnostics.empty(),
                "malformed adapter input is rejected atomically");
    };

    auto materialMismatch = fixture();
    materialMismatch.material_parameters.clear();
    rejected(std::move(materialMismatch),
             FbxRenderAdapterStatus::invalid_request);

    auto positions = fixture();
    positions.meshes[0].positions.pop_back();
    rejected(std::move(positions), FbxRenderAdapterStatus::invalid_request);

    auto normals = fixture();
    normals.meshes[0].normals.pop_back();
    rejected(std::move(normals), FbxRenderAdapterStatus::invalid_request);

    auto uvs = fixture();
    uvs.meshes[0].uvs.pop_back();
    rejected(std::move(uvs), FbxRenderAdapterStatus::invalid_request);

    auto indices = fixture();
    indices.meshes[0].triangle_indices.back() = 99U;
    rejected(std::move(indices), FbxRenderAdapterStatus::invalid_request);

    auto shortIndices = fixture();
    shortIndices.meshes[0].triangle_indices.pop_back();
    rejected(std::move(shortIndices),
             FbxRenderAdapterStatus::invalid_request);

    auto slots = fixture();
    slots.meshes[0].triangle_material_slots.push_back(0);
    rejected(std::move(slots), FbxRenderAdapterStatus::invalid_request);

    auto transform = fixture();
    transform.node_geometry[0].geometric[0] =
        std::numeric_limits<float>::infinity();
    rejected(std::move(transform), FbxRenderAdapterStatus::invalid_request);

    auto hierarchy = fixture();
    hierarchy.transforms.clear();
    rejected(std::move(hierarchy), FbxRenderAdapterStatus::invalid_request);

    auto parentedRoot = fixture();
    parentedRoot.snapshot.nodes[parentedRoot.snapshot.root].parent = 1U;
    rejected(std::move(parentedRoot),
             FbxRenderAdapterStatus::invalid_request);

    auto authorityConversion = fixture();
    auto badAuthority = textures(1U, {"paint.png"});
    badAuthority.selections[0].authority_resource_index = 99U;
    const auto authorityResult = apex::render::build_fbx_render_scene(
        authorityConversion, &badAuthority);
    require(authorityResult.status == FbxRenderAdapterStatus::invalid_request &&
                !authorityResult.model.has_value() &&
                !authorityResult.scene.has_value(),
            "malformed texture authority is rejected atomically");

    auto collisionConversion = fixture(2U);
    auto collision = textures(2U, {"same.png", "same.png"});
    const auto collisionResult = apex::render::build_fbx_render_scene(
        collisionConversion, &collision);
    require(collisionResult.status == FbxRenderAdapterStatus::invalid_request &&
                !collisionResult.model.has_value(),
            "distinct payloads cannot alias one KN5 texture name");
}

void enforcesLimitsAndFiniteNativeFallbacks() {
    auto fallbackConversion = fixture();
    fallbackConversion.meshes[0].normals.clear();
    fallbackConversion.meshes[0].uvs.clear();
    auto authority = textures(1U, {"paint.png"});
    const auto fallback = apex::render::build_fbx_render_scene(
        fallbackConversion, &authority);
    const auto& fallbackMesh = fallback.model->root.children[0].children[0];
    require(fallback.status == FbxRenderAdapterStatus::ready &&
                std::abs(fallbackMesh.vertices[8] - 0.57735026F) < 1.0e-6F &&
                std::abs(fallbackMesh.vertices[9] - 0.57735026F) < 1.0e-6F &&
                std::abs(fallbackMesh.vertices[10] - 0.57735026F) < 1.0e-6F,
            "zero normal and UV data uses the recovered finite tangent fallback");

    auto scaled = fixture();
    scaled.meshes[0].normals = {1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
                                0.0F, 1.0F, 1.0F, 0.0F};
    scaled.node_geometry[0].geometric = apex::scene::identity_matrix;
    scaled.node_geometry[0].geometric[0] = 2.0F;
    const auto scaledResult =
        apex::render::build_fbx_render_scene(scaled, &authority);
    const auto& scaledMesh = scaledResult.model->root.children[0].children[0];
    require(std::abs(scaledMesh.vertices[3] - 2.0F / std::sqrt(5.0F)) <
                    1.0e-6F &&
                std::abs(scaledMesh.vertices[4] - 1.0F / std::sqrt(5.0F)) <
                    1.0e-6F,
            "normal uses the recovered upper-left geometric matrix");

    auto byteLimits = apex::render::FbxRenderAdapterLimits{};
    byteLimits.max_output_bytes = 1U;
    const auto byteLimited = apex::render::build_fbx_render_scene(
        fixture(), &authority, "scene.fbx", byteLimits);
    require(byteLimited.status == FbxRenderAdapterStatus::resource_limit &&
                !byteLimited.model.has_value(),
            "aggregate output budget is enforced before publication");

    auto batchConversion = fixture(2U);
    auto& batchMesh = batchConversion.meshes[0];
    const auto batch_positions = batchMesh.positions;
    const auto batch_normals = batchMesh.normals;
    const auto batch_uvs = batchMesh.uvs;
    batchMesh.positions.insert(batchMesh.positions.end(),
                               batch_positions.begin(), batch_positions.end());
    batchMesh.normals.insert(batchMesh.normals.end(), batch_normals.begin(),
                             batch_normals.end());
    batchMesh.uvs.insert(batchMesh.uvs.end(), batch_uvs.begin(),
                         batch_uvs.end());
    batchMesh.triangle_indices = {0U, 1U, 2U, 3U, 4U, 5U};
    batchMesh.triangle_material_slots = {0, 1};
    auto batchLimits = apex::render::FbxRenderAdapterLimits{};
    batchLimits.max_batches_per_geometry = 1U;
    const auto batchLimited = apex::render::build_fbx_render_scene(
        batchConversion, nullptr, "scene.fbx", batchLimits);
    require(batchLimited.status == FbxRenderAdapterStatus::resource_limit &&
                !batchLimited.model.has_value(),
            "per-geometry batch limit is enforced");

    auto maximum = fixture();
    maximum.meshes[0].triangle_indices.clear();
    maximum.meshes[0].triangle_material_slots.assign(21'845U, 0);
    maximum.meshes[0].triangle_indices.reserve(65'535U);
    for (std::size_t triangle = 0U; triangle < 21'845U; ++triangle)
        maximum.meshes[0].triangle_indices.insert(
            maximum.meshes[0].triangle_indices.end(), {0U, 1U, 2U});
    const auto maximumResult =
        apex::render::build_fbx_render_scene(maximum, &authority);
    require(maximumResult.status == FbxRenderAdapterStatus::ready &&
                maximumResult.model->root.children[0].children[0]
                        .indices.size() == 65'535U,
            "largest safe triangle-aligned KN5 batch is accepted");

    maximum.meshes[0].triangle_material_slots.push_back(0);
    maximum.meshes[0].triangle_indices.insert(
        maximum.meshes[0].triangle_indices.end(), {0U, 1U, 2U});
    const auto overflow =
        apex::render::build_fbx_render_scene(maximum, &authority);
    require(overflow.status == FbxRenderAdapterStatus::unsupported &&
                !overflow.model.has_value() && !overflow.scene.has_value(),
            "unsafe KN5 16-bit batch overflow is rejected atomically");
}

} // namespace

int main() {
    try {
        buildsOwnedCanonicalScene();
        preservesNativeBatchOrderNamesAndMaterialResolution();
        stagesMissingResourcesAndUsesBoundedFallback();
        rejectsMalformedInputAtomically();
        enforcesLimitsAndFiniteNativeFallbacks();
        std::cout << "fbx render adapter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fbx render adapter tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
