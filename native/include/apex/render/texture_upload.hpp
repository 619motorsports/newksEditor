#pragma once

#include "apex/core/parse_limits.hpp"
#include "apex/formats/dds.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

// This enum is deliberately independent of Vulkan and DirectX headers. It is
// the small common format vocabulary used by the upload planner.
enum class TextureFormat {
    unknown,
    r8_unorm,
    r5g6b5_unorm,
    rgba8_unorm,
    rgba8_srgb,
    bgra8_unorm,
    bgra8_srgb,
    bc1_unorm,
    bc1_srgb,
    bc2_unorm,
    bc2_srgb,
    bc3_unorm,
    bc3_srgb,
    bc4_unorm,
    bc4_snorm,
    bc5_unorm,
    bc5_snorm,
    bc6h_ufloat,
    bc6h_sfloat,
    bc7_unorm,
    bc7_srgb,
};

struct TextureFormatMapping {
    TextureFormat format = TextureFormat::unknown;
    // Numeric VkFormat and DXGI_FORMAT values.  Public code can pass these to
    // an adapter after including that platform's SDK, without this API doing so.
    std::uint32_t vulkanFormat = 0; // VK_FORMAT_UNDEFINED
    std::uint32_t dxgiFormat = 0;   // DXGI_FORMAT_UNKNOWN
    bool compressed = false;
    bool srgb = false;
    bool signedChannels = false;
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
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t rowPitch = 0;
    std::size_t rowCount = 0;
    std::size_t slicePitch = 0;
    std::size_t blocksWide = 0;
    std::size_t blocksHigh = 0;
};

struct DdsUploadPlan {
    apex::formats::DdsDescriptor descriptor;
    TextureFormatMapping mapping;
    std::vector<DdsUploadSubresource> subresources;
    std::size_t payloadBytes = 0;
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
// constants. 24-bit RGB and legacy floating-point layouts have no portable
// CPU approximation in this boundary; callers receive an unsupported result.
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
