#pragma once

#include "apex/core/parse_limits.hpp"
#include "apex/formats/dds.hpp"
#include "apex/render/device.hpp"
#include "apex/render/texture_upload.hpp"

#include <span>
#include <string>
#include <vector>

namespace apex::render {

// A portable CPU-decoded DDS plan. This is an exact texel decode for the
// formats supported by formats::decodeDdsRgba; it does not claim a native
// block-compressed upload path. Explicit sRGB metadata is retained in the GPU
// texture format.
struct DecodedDdsTexturePlan {
    TextureDescription description{};
    std::vector<formats::DdsLevel> levels;

    // The returned spans borrow level pixels and remain valid while this plan
    // is alive and unchanged.
    [[nodiscard]] TextureUploadPlan make_upload_plan() const;
};

struct DecodedDdsTexturePlanResult {
    TextureUploadStatus status = TextureUploadStatus::invalid;
    TextureUploadDiagnostic diagnostic;
    DecodedDdsTexturePlan plan;

    [[nodiscard]] bool ok() const noexcept {
        return status == TextureUploadStatus::ready && !plan.levels.empty();
    }
};

// Parse, validate, and decode one 2D DDS mip chain without creating backend
// resources. Arrays, cubes, 1D/3D resources, BC6H, and legacy float textures
// remain explicit unsupported inputs.
[[nodiscard]] DecodedDdsTexturePlanResult plan_decoded_dds_texture(
    std::span<const std::uint8_t> bytes,
    std::string source = "texture.dds",
    apex::core::ParseLimits limits = {});

}  // namespace apex::render
