#pragma once

#include "apex/core/parse_limits.hpp"
#include "apex/formats/dds.hpp"
#include "apex/render/device.hpp"
#include "apex/render/texture_upload.hpp"

#include <span>
#include <string>
#include <vector>

namespace apex::render {

struct DecodedTextureLevel {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  // RGBA8, row-major, top-to-bottom.
  std::vector<std::uint8_t> pixels;
};

// A portable, owned CPU-decoded texture plan. DDS preserves explicit sRGB
// metadata. PNG follows the production WebGL upload contract: straight-alpha,
// top-to-bottom RGBA8 UNORM with no implicit color-space conversion.
struct DecodedTexturePlan {
  TextureDescription description{};
  std::vector<DecodedTextureLevel> levels;

  // The returned spans borrow level pixels and remain valid while this plan
  // is alive and unchanged.
  [[nodiscard]] TextureUploadPlan make_upload_plan() const;
};

struct DecodedTexturePlanResult {
  TextureUploadStatus status = TextureUploadStatus::invalid;
  TextureUploadDiagnostic diagnostic;
  DecodedTexturePlan plan;

  [[nodiscard]] bool ok() const noexcept {
    return status == TextureUploadStatus::ready && !plan.levels.empty();
  }
};

// Compatibility names for callers that explicitly require the DDS-only
// planner below.
using DecodedDdsTexturePlan = DecodedTexturePlan;
using DecodedDdsTexturePlanResult = DecodedTexturePlanResult;

// Parse, validate, and decode one 2D DDS mip chain without creating backend
// resources. Arrays, cubes, 1D/3D resources, BC6H, and legacy float textures
// remain explicit unsupported inputs.
[[nodiscard]] DecodedDdsTexturePlanResult
plan_decoded_dds_texture(std::span<const std::uint8_t> bytes,
                         std::string source = "texture.dds",
                         apex::core::ParseLimits limits = {});

// Identify a supported payload by its bytes, then validate and decode it
// without creating backend resources. Filenames and extensions are not used
// as format authority.
[[nodiscard]] DecodedTexturePlanResult
plan_decoded_texture_payload(std::span<const std::uint8_t> bytes,
                             std::string source = "texture",
                             apex::core::ParseLimits limits = {});

} // namespace apex::render
