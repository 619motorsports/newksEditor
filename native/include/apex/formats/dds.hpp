#pragma once

#include "apex/core/parse_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::formats {

// The descriptor intentionally keeps the original DDS metadata.  A caller can
// use it to select a GPU upload path without having to parse the header again.
enum class DdsFormat {
    Unknown,
    Raw8,
    Raw16,
    Raw24,
    Raw32,
    BC1,
    BC2,
    BC3,
    BC4Unorm,
    BC4Snorm,
    BC5Unorm,
    BC5Snorm,
    BC6HUf16,
    BC6HSf16,
    BC7,
    BC7Srgb,
    // Legacy D3D9 floating-point FOURCC payloads are intentionally retained
    // as recognized GPU-only metadata. They are not approximated by RGBA8.
    LegacyFloat,
};

struct DdsDescriptor {
    DdsFormat format = DdsFormat::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    std::size_t dataOffset = 0;
    std::uint32_t pitch = 0;
    std::uint32_t bitsPerPixel = 0;
    std::array<std::uint32_t, 4> masks{};
    std::uint32_t dxgi = 0;
    // DX10 extension metadata. A zero resource dimension means legacy DDS;
    // nonzero values are D3D10_RESOURCE_DIMENSION enum values.
    std::uint32_t resourceDimension = 0;
    std::uint32_t miscFlags = 0;
    std::uint32_t arraySize = 1;
    std::uint32_t blockBytes = 0;
    // Explicit DX10 color-space metadata. CPU byte decoding is unchanged, but
    // upload callers must preserve this distinction for the native format.
    bool srgb = false;
    bool compressed = false;
    bool luminance = false;
    bool signedChannels = false;
    bool premultiplied = false;
    bool gpuRequired = false;
    std::string fourCC;
};

struct DdsLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8, row-major, top-to-bottom.
};

// Returns nullopt for a header that is not a structurally valid DDS or uses an
// unsupported header shape. Unknown pixel formats are still described so a
// renderer can report or route them to a native GPU upload path.
[[nodiscard]] std::optional<DdsDescriptor> inspectDds(
    std::span<const std::uint8_t> bytes, std::string source = "texture.dds",
    apex::core::ParseLimits limits = {});

// DDS compressed uploads require every supplied mip to have dimensions that
// are multiples of four. This is a WebGL constraint, not a DDS validity rule.
[[nodiscard]] bool webglCompressedMipChainSafe(const DdsDescriptor& descriptor) noexcept;

// Decode supported uncompressed and BC1-BC7 textures into RGBA8. 24-bit RGB,
// legacy floating-point FOURCC layouts, and BC6H are deliberately not
// approximated by a guessed CPU fallback: recognized GPU-only formats throw a
// ParseError with code GPU_REQUIRED, while unsupported layouts remain explicit
// unknown/unsupported diagnostics.
[[nodiscard]] std::vector<DdsLevel> decodeDdsRgba(
    std::span<const std::uint8_t> bytes, const DdsDescriptor& descriptor,
    std::string source = "texture.dds", apex::core::ParseLimits limits = {});

[[nodiscard]] std::vector<DdsLevel> decodeDdsRgba(
    std::span<const std::uint8_t> bytes, std::string source = "texture.dds",
    apex::core::ParseLimits limits = {});

[[nodiscard]] const char* ddsFormatName(DdsFormat format) noexcept;

}
