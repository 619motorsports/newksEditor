#pragma once

#include "apex/assets/asset_source.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/render/decoded_dds_texture.hpp"
#include "apex/render/material_binding.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

inline constexpr std::size_t invalid_external_texture_resource =
    std::numeric_limits<std::size_t>::max();

// The authority owns no GPU objects. It resolves external material files
// through an explicitly selected AssetSource, reads them through that source's
// confinement checks, and returns CPU-decoded DDS plans for a later renderer
// handoff.
enum class ExternalTextureAuthorityStatus : std::uint8_t {
    ready,
    invalid_request,
    rejected,
    missing,
    ambiguous,
    unsupported,
    resource_limit,
    read_failed,
};

struct ExternalTextureAuthorityLimits {
    std::size_t max_requests = 4096U;
    std::size_t max_grants = 256U;
    std::size_t max_grant_id_bytes = 256U;
    std::size_t max_path_bytes = 1U << 20U;
    std::size_t max_file_bytes = 256U * 1024U * 1024U;
    std::size_t max_mip_levels = 32U;
    std::uint64_t max_total_source_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_decoded_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_plan_bytes = 512ULL * 1024ULL * 1024ULL;
    apex::core::ParseLimits decode{};
};

struct ExternalTextureGrant {
    // This identifier is opaque to the authority. It is retained in every
    // result so a caller cannot accidentally confuse equal paths from two
    // selected roots.
    std::string id;
    const apex::assets::AssetSource* source = nullptr;
};

struct ExternalTextureRequest {
    std::string grant_id;
    MaterialTextureBinding binding;
};

struct ExternalTextureAuthorityDiagnostic {
    std::size_t request_index = invalid_external_texture_resource;
    std::string grant_id;
    std::string path;
    std::string code;
    std::string message;
};

struct ExternalTextureResource {
    std::string grant_id;
    std::string requested_path;
    std::string resolved_path;
    apex::assets::AssetSourceKind source_kind = apex::assets::AssetSourceKind::directory;
    DecodedDdsTexturePlan plan;
};

struct ExternalTextureAuthorityResult {
    ExternalTextureAuthorityStatus status = ExternalTextureAuthorityStatus::invalid_request;
    std::vector<ExternalTextureAuthorityDiagnostic> diagnostics;
    std::vector<ExternalTextureResource> resources;
    // One entry per request. A failed result never exposes a partial resource
    // table; successful duplicate requests point at one shared decoded plan.
    std::vector<std::size_t> request_resource_indices;

    [[nodiscard]] bool ok() const noexcept {
        return status == ExternalTextureAuthorityStatus::ready;
    }
};

[[nodiscard]] const char* external_texture_authority_status_name(
    ExternalTextureAuthorityStatus status) noexcept;

// Resolve only MaterialTextureBinding values whose kind is external_file.
// Every request must name one of the supplied grants. Resolution retains the
// AssetSource exact/suffix/basename and directory-over-ACD rules. All paths,
// source bytes, decoded pixels, and retained plan metadata are bounded before
// this function returns a successful table. No backend API is called.
[[nodiscard]] ExternalTextureAuthorityResult resolve_external_texture_authority(
    std::span<const ExternalTextureGrant> grants,
    std::span<const ExternalTextureRequest> requests,
    ExternalTextureAuthorityLimits limits = {});

}  // namespace apex::render
