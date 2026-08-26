#pragma once

#include "apex/assets/asset_source.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/render/decoded_dds_texture.hpp"
#include "apex/render/material_binding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

inline constexpr std::size_t invalid_external_texture_resource = std::numeric_limits<std::size_t>::max();

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
    std::size_t max_materials = 4096U;
    std::size_t max_solid_colors = 4096U;
    std::size_t max_effective_nodes = 1'000'000U;
    std::size_t max_effective_node_depth = 4096U;
    std::size_t max_effective_textures = 65'536U;
    std::size_t max_effective_texture_name_bytes = 1U << 20U;
    std::uint64_t max_total_source_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_decoded_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_plan_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_effective_source_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_effective_copy_bytes = 1024ULL * 1024ULL * 1024ULL;
    apex::core::ParseLimits decode{};
};

struct ExternalTextureGrant {
    // This identifier is opaque to the authority. It is retained in every
    // result so a caller cannot accidentally confuse equal paths from two
    // selected roots.
    std::string id;
    // Non-owning. The source must remain alive and immutable for the complete
    // synchronous authority call.
    const apex::assets::AssetSource* source = nullptr;
};

struct ExternalTextureRequest {
    std::string grant_id;
    MaterialTextureBinding binding;
    // Effective-stock-scene preparation requires these fields. They are
    // optional for the lower-level resolver so existing callers can retain
    // its path-only contract.
    std::size_t material_index = invalid_external_texture_resource;
    std::optional<std::size_t> workspace_file_index;
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
    // The exact, bounded supported image bytes read under the grant. This is
    // retained so
    // the effective scene can hand only owned embedded payloads to a renderer
    // and cannot be affected by a later source mutation.
    std::vector<std::uint8_t> source_bytes;
    DecodedTexturePlan plan;
};

struct ExternalTextureAuthorityResult {
    ExternalTextureAuthorityStatus status = ExternalTextureAuthorityStatus::invalid_request;
    std::vector<ExternalTextureAuthorityDiagnostic> diagnostics;
    std::vector<ExternalTextureResource> resources;
    // One entry per request. A failed result never exposes a partial resource
    // table; successful duplicate requests point at one shared decoded plan.
    std::vector<std::size_t> request_resource_indices;

    [[nodiscard]] bool ok() const noexcept { return status == ExternalTextureAuthorityStatus::ready; }
};

[[nodiscard]] const char* external_texture_authority_status_name(ExternalTextureAuthorityStatus status) noexcept;

// Resolve only MaterialTextureBinding values whose kind is external_file.
// Every request must name one of the supplied grants. Resolution retains the
// AssetSource exact/suffix/basename and directory-over-ACD rules. All paths,
// source bytes, decoded pixels, and retained plan metadata are bounded before
// this function returns a successful table. No backend API is called.
[[nodiscard]] ExternalTextureAuthorityResult
resolve_external_texture_authority(std::span<const ExternalTextureGrant> grants,
                                   std::span<const ExternalTextureRequest> requests,
                                   ExternalTextureAuthorityLimits limits = {});

struct ExternalTextureEffectiveBinding {
    std::size_t material_index = invalid_external_texture_resource;
    std::optional<std::size_t> workspace_file_index;
    std::string slot;
    std::string grant_id;
    std::string requested_path;
    std::string resolved_path;
    std::string synthetic_texture_name;
    std::size_t authority_resource_index = invalid_external_texture_resource;
};

struct SolidColorEffectiveBinding {
    std::size_t material_index = invalid_external_texture_resource;
    std::optional<std::size_t> workspace_file_index;
    std::string slot;
    // Clamped RGBA values that produced the synthetic BGRA DDS payload.
    std::array<float, 4> color{};
    std::string synthetic_texture_name;
};

// This is the owned handoff consumed by StockSceneExecutionRequest. Its
// model texture table contains validated external image bytes and exact
// source-compatible solid-color DDS bytes under opaque names. No AssetSource,
// grant, or external path is needed by the renderer.
struct ExternalTextureEffectiveStockSceneInput {
    apex::formats::Kn5File model;
    std::vector<MaterialBindingOverrides> overrides_by_material;
    std::vector<ExternalTextureEffectiveBinding> external_bindings;
    std::vector<SolidColorEffectiveBinding> solid_color_bindings;
};

struct ExternalTextureEffectiveStockSceneResult {
    ExternalTextureAuthorityStatus status = ExternalTextureAuthorityStatus::invalid_request;
    std::vector<ExternalTextureAuthorityDiagnostic> diagnostics;
    std::unique_ptr<ExternalTextureEffectiveStockSceneInput> input;

    [[nodiscard]] bool ok() const noexcept {
        return status == ExternalTextureAuthorityStatus::ready && input != nullptr;
    }
};

// Materialize supported external-image and solid-color overrides into an
// owned effective stock-scene input. Each external request must carry a
// material index and exact workspace scope matching the source material.
// Solid colors require no filesystem grant. Only materialized resource
// overrides are consumed. Unrelated overrides remain visible. Failure is
// atomic: no partial model is returned. A color follows the source material
// binding precedence and takes priority over file and texture fields.
// Severe allocation exhaustion can still throw while a diagnostic is built.
[[nodiscard]] ExternalTextureEffectiveStockSceneResult prepare_effective_stock_scene_input(
    const apex::formats::Kn5File& model, std::span<const MaterialBindingOverrides> overrides_by_material,
    std::span<const ExternalTextureGrant> grants, std::span<const ExternalTextureRequest> requests,
    ExternalTextureAuthorityLimits limits = {});

} // namespace apex::render
