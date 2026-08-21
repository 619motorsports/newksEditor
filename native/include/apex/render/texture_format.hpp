#pragma once

#include <cstdint>

namespace apex::render {

// Canonical backend-neutral texture vocabulary shared by source upload
// planning and GPU resource creation. Backends can support a deliberate
// subset and must return an explicit unsupported diagnostic for the rest.
enum class TextureFormat : std::uint8_t {
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

enum class TextureFormatClass : std::uint8_t {
    unknown,
    uncompressed,
    block_compressed,
};

struct TextureFormatInfo {
    TextureFormatClass classification = TextureFormatClass::unknown;
    std::uint8_t bytes_per_pixel = 0;
    std::uint8_t block_width = 0;
    std::uint8_t block_height = 0;
    std::uint8_t block_bytes = 0;
    bool srgb = false;
    bool signed_channels = false;
};

// This metadata is deliberately independent of Vulkan/DXGI headers. The
// native texture contract accepts all uncompressed entries. A separate
// predicate selects the bounded block-compressed upload subset.
[[nodiscard]] constexpr TextureFormatInfo texture_format_info(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::r8_unorm: return {TextureFormatClass::uncompressed, 1, 0, 0, 0, false, false};
    case TextureFormat::r5g6b5_unorm: return {TextureFormatClass::uncompressed, 2, 0, 0, 0, false, false};
    case TextureFormat::rgba8_unorm: return {TextureFormatClass::uncompressed, 4, 0, 0, 0, false, false};
    case TextureFormat::rgba8_srgb: return {TextureFormatClass::uncompressed, 4, 0, 0, 0, true, false};
    case TextureFormat::bgra8_unorm: return {TextureFormatClass::uncompressed, 4, 0, 0, 0, false, false};
    case TextureFormat::bgra8_srgb: return {TextureFormatClass::uncompressed, 4, 0, 0, 0, true, false};
    case TextureFormat::bc1_unorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 8, false, false};
    case TextureFormat::bc1_srgb: return {TextureFormatClass::block_compressed, 0, 4, 4, 8, true, false};
    case TextureFormat::bc2_unorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, false};
    case TextureFormat::bc2_srgb: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, true, false};
    case TextureFormat::bc3_unorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, false};
    case TextureFormat::bc3_srgb: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, true, false};
    case TextureFormat::bc4_unorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 8, false, false};
    case TextureFormat::bc4_snorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 8, false, true};
    case TextureFormat::bc5_unorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, false};
    case TextureFormat::bc5_snorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, true};
    case TextureFormat::bc6h_ufloat: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, false};
    case TextureFormat::bc6h_sfloat: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, true};
    case TextureFormat::bc7_unorm: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, false, false};
    case TextureFormat::bc7_srgb: return {TextureFormatClass::block_compressed, 0, 4, 4, 16, true, false};
    case TextureFormat::unknown: break;
    }
    return {};
}

[[nodiscard]] constexpr bool texture_format_is_known(TextureFormat format) noexcept {
    return texture_format_info(format).classification != TextureFormatClass::unknown;
}

[[nodiscard]] constexpr bool texture_format_is_compressed(TextureFormat format) noexcept {
    return texture_format_info(format).classification == TextureFormatClass::block_compressed;
}

[[nodiscard]] constexpr bool texture_format_cpu_upload_supported(TextureFormat format) noexcept {
    return texture_format_info(format).classification == TextureFormatClass::uncompressed;
}

// The neutral upload validator deliberately admits only the block formats
// whose layout is fully described by the backend-independent metadata.
[[nodiscard]] constexpr bool texture_format_neutral_block_upload_supported(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::bc1_unorm:
    case TextureFormat::bc1_srgb:
    case TextureFormat::bc3_unorm:
    case TextureFormat::bc3_srgb:
        return true;
    default:
        return false;
    }
}

} // namespace apex::render
