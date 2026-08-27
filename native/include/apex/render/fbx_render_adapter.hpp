#pragma once

#include "apex/formats/fbx_conversion.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/render/fbx_external_texture_authority.hpp"
#include "apex/render/texture_payload_authority.hpp"
#include "apex/scene/kn5_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apex::render {

enum class FbxRenderAdapterStatus : std::uint8_t {
    ready,
    staged,
    invalid_request,
    unsupported,
    resource_limit,
};

struct FbxRenderAdapterDiagnostic {
    std::string code;
    std::string message;
    std::string path;
};

struct FbxRenderAdapterLimits {
    std::size_t max_materials = 1'000'000U;
    std::size_t max_textures = 65'536U;
    std::size_t max_embedded_images = 4'096U;
    std::size_t max_embedded_candidates = 65'536U;
    std::size_t max_nodes = 1'000'000U;
    std::size_t max_meshes = 1'000'000U;
    std::size_t max_batches_per_geometry = 65'536U;
    std::size_t max_vertices = 10'000'000U;
    std::size_t max_indices = 20'000'000U;
    std::size_t max_bones_per_mesh = 4'096U;
    // Native KN5 static indices are 16-bit. The original importer does not
    // safely handle a larger material batch, so the adapter rejects it.
    std::size_t max_vertices_per_mesh = 65'535U;
    std::size_t max_depth = 1'024U;
    std::size_t max_name_bytes = 1U * 1024U * 1024U;
    std::size_t max_diagnostics = 10'000U;
    std::size_t max_diagnostic_bytes = 16U * 1024U * 1024U;
    std::size_t max_output_bytes = 512U * 1024U * 1024U;
    std::size_t max_total_embedded_source_bytes = 128U * 1024U * 1024U;
    std::size_t max_total_embedded_decoded_bytes = 512U * 1024U * 1024U;
    apex::core::ParseLimits embedded_decode{};
    scene::Kn5SceneLimits scene{};
};

// This result owns the complete KN5-compatible CPU model and its regenerated
// backend-neutral scene. A staged result has valid geometry but cannot enter a
// GPU backend because source behavior or native diffuse resources are missing.
struct FbxRenderAdapterResult {
    FbxRenderAdapterStatus status =
        FbxRenderAdapterStatus::invalid_request;
    std::optional<formats::Kn5File> model;
    std::optional<scene::Kn5SceneConversion> scene;
    std::vector<FbxRenderAdapterDiagnostic> diagnostics;
    // Conservative byte charge for the canonical model and bounded adapter
    // metadata. Application composition uses this instead of source FBX size.
    std::size_t accounted_model_bytes = 0U;
    // Every published texture payload is copied into model->textures. Pass
    // this value to the shared static-scene or stock-scene handoff so both
    // backends decode and own the copied bytes.
    TexturePayloadAuthority texture_authority =
        TexturePayloadAuthority::caller_tables;

    [[nodiscard]] bool ok() const noexcept {
        return (status == FbxRenderAdapterStatus::ready ||
                status == FbxRenderAdapterStatus::staged) &&
               model.has_value() && scene.has_value();
    }

    [[nodiscard]] bool gpu_renderable() const noexcept {
        return status == FbxRenderAdapterStatus::ready && model.has_value() &&
               scene.has_value();
    }
};

// Build the canonical model that both Vulkan and D3D12 consume. The optional
// texture result must already own all selected source bytes. This function
// retains no AssetSource, file handle, grant, or external path.
[[nodiscard]] FbxRenderAdapterResult build_fbx_render_scene(
    const formats::FbxSceneConversion& conversion,
    const FbxExternalTextureAuthorityResult* textures = nullptr,
    std::string source = "scene.fbx", FbxRenderAdapterLimits limits = {});

} // namespace apex::render
