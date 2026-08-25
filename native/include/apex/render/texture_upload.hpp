#pragma once

#include "apex/core/parse_limits.hpp"
#include "apex/formats/dds.hpp"
#include "apex/render/texture_format.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

struct TextureFormatMapping {
    TextureFormat format = TextureFormat::unknown;
    // Numeric VkFormat and DXGI_FORMAT values.  Public code can pass these to
    // an adapter after including that platform's SDK, without this API doing so.
    std::uint32_t vulkanFormat = 0; // VK_FORMAT_UNDEFINED
    std::uint32_t dxgiFormat = 0;   // DXGI_FORMAT_UNKNOWN
    bool compressed = false;
    bool srgb = false;
    bool signedChannels = false;
    std::uint8_t sourceBytesPerPixel = 0;
    bool cpuConversion = false;
};

struct TextureUploadDiagnostic {
    std::string code;
    std::string message;
    std::size_t offset = 0;
    std::string source;
};

struct TextureFormatMappingResult {
    std::optional<TextureFormatMapping> mapping;
    TextureUploadDiagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return mapping.has_value(); }
};

struct DdsUploadSubresource {
    std::uint32_t mipLevel = 0;
    // DX10 array index and cube face. For ordinary 2D textures these are 0.
    std::uint32_t arrayLayer = 0;
    std::uint32_t cubeFace = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t rowPitch = 0;
    std::size_t rowCount = 0;
    std::size_t slicePitch = 0;
    std::size_t blocksWide = 0;
    std::size_t blocksHigh = 0;
    // Non-zero only when the source payload needs a CPU conversion (currently
    // masked 24-bit RGB to tightly packed RGBA8). These offsets address the
    // plan-owned convertedPayload, not the DDS input bytes.
    std::size_t convertedOffset = 0;
    std::size_t convertedSize = 0;
    std::size_t convertedRowPitch = 0;
};

struct DdsUploadPlan {
    apex::formats::DdsDescriptor descriptor;
    TextureFormatMapping mapping;
    std::vector<DdsUploadSubresource> subresources;
    std::size_t payloadBytes = 0;
    std::vector<std::uint8_t> convertedPayload;
};

enum class TextureUploadStatus {
    ready,
    invalid,
    unsupported,
};

struct DdsUploadPlanResult {
    TextureUploadStatus status = TextureUploadStatus::invalid;
    TextureUploadDiagnostic diagnostic;
    std::optional<DdsUploadPlan> plan;

    [[nodiscard]] bool ok() const noexcept {
        return status == TextureUploadStatus::ready && plan.has_value();
    }
};

// Map source DDS metadata to portable format and authoritative SDK numeric
// constants. 24-bit RGB is converted exactly from its source masks to RGBA8
// in the bounded upload plan. Legacy D3DFMT_R32F payloads map directly to a
// scalar native texture. Other legacy floating-point layouts remain unsupported.
// No Vulkan or DirectX headers are required by this function.
[[nodiscard]] TextureFormatMappingResult mapDdsTextureFormat(
    const apex::formats::DdsDescriptor& descriptor);

// Build absolute input offsets and row/slice pitches for every mip. The plan
// does not retain the input bytes and does not create a GPU object.
[[nodiscard]] DdsUploadPlanResult buildDdsUploadPlan(
    std::span<const std::uint8_t> bytes, const apex::formats::DdsDescriptor& descriptor,
    std::string source = "texture.dds", apex::core::ParseLimits limits = {});

[[nodiscard]] DdsUploadPlanResult buildDdsUploadPlan(
    std::span<const std::uint8_t> bytes, std::string source = "texture.dds",
    apex::core::ParseLimits limits = {});

[[nodiscard]] const char* textureFormatName(TextureFormat format) noexcept;
[[nodiscard]] const char* textureUploadStatusName(TextureUploadStatus status) noexcept;

}
