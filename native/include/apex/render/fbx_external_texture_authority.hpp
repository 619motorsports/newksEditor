#pragma once

#include "apex/assets/asset_source.hpp"
#include "apex/formats/fbx_conversion.hpp"
#include "apex/render/external_texture_authority.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace apex::render {

inline constexpr std::size_t invalid_fbx_texture_selection =
    std::numeric_limits<std::size_t>::max();

struct FbxExternalTextureAuthorityLimits {
    std::size_t max_candidates = 65'536U;
    std::size_t max_candidates_per_material = 1'024U;
    std::size_t max_basename_bytes = 1U << 20U;
    std::size_t max_metadata_bytes = 64U * 1024U * 1024U;
    std::size_t max_diagnostics = 10'000U;
    std::size_t max_diagnostic_bytes = 16U * 1024U * 1024U;
    ExternalTextureAuthorityLimits authority{};
};

struct FbxExternalTextureDiagnostic {
    std::size_t material_index = invalid_external_texture_resource;
    std::size_t candidate_index = invalid_fbx_texture_selection;
    std::string basename;
    std::string code;
    std::string message;
};

struct FbxExternalTextureSelection {
    std::size_t material_index = invalid_external_texture_resource;
    std::size_t candidate_index = invalid_fbx_texture_selection;
    std::int64_t texture_object_id = 0;
    std::string channel;
    std::string basename;
    std::size_t authority_resource_index = invalid_external_texture_resource;
};

// This result owns all selected image bytes through authority.resources.
// No AssetSource, AssetFile, filesystem path, or grant pointer survives the
// synchronous resolver call.
struct FbxExternalTextureAuthorityResult {
    ExternalTextureAuthorityStatus status =
        ExternalTextureAuthorityStatus::invalid_request;
    std::vector<FbxExternalTextureDiagnostic> diagnostics;
    std::vector<FbxExternalTextureSelection> selections;
    // One selection index per FBX material. A material with no existing file
    // retains invalid_fbx_texture_selection, matching the native null resource.
    std::vector<std::size_t> material_selection_indices;
    ExternalTextureAuthorityResult authority;

    [[nodiscard]] bool ok() const noexcept {
        return status == ExternalTextureAuthorityStatus::ready && authority.ok();
    }
};

// Select the first existing file candidate for each material in the recovered
// eight-channel order, then resolve and decode all selections atomically with
// the existing external-texture authority. Missing files are non-fatal and do
// not create a synthetic fallback. Ambiguous or unsafe candidates fail closed.
[[nodiscard]] FbxExternalTextureAuthorityResult
resolve_fbx_external_texture_authority(
    const apex::formats::FbxSceneConversion& conversion, std::string grant_id,
    const apex::assets::AssetSource& source,
    FbxExternalTextureAuthorityLimits limits = {});

} // namespace apex::render
