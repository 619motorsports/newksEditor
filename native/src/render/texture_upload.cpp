#include "apex/render/texture_upload.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace apex::render {

namespace {

constexpr std::uint32_t MAX_DIMENSION = 32768u;

// Values from the Vulkan 1.3 VkFormat enum (vulkan_core.h). They are kept as
// numbers here so the portable planning layer does not pull platform headers
// into the renderer boundary.
constexpr std::uint32_t VK_FORMAT_R5G6B5_UNORM_PACK16 = 4u;
constexpr std::uint32_t VK_FORMAT_R8_UNORM = 9u;
constexpr std::uint32_t VK_FORMAT_R8G8B8A8_UNORM = 37u;
constexpr std::uint32_t VK_FORMAT_R8G8B8A8_SRGB = 43u;
constexpr std::uint32_t VK_FORMAT_B8G8R8A8_UNORM = 44u;
constexpr std::uint32_t VK_FORMAT_B8G8R8A8_SRGB = 50u;
constexpr std::uint32_t VK_FORMAT_BC1_RGBA_UNORM_BLOCK = 133u;
constexpr std::uint32_t VK_FORMAT_BC1_RGBA_SRGB_BLOCK = 134u;
constexpr std::uint32_t VK_FORMAT_BC2_UNORM_BLOCK = 135u;
constexpr std::uint32_t VK_FORMAT_BC2_SRGB_BLOCK = 136u;
constexpr std::uint32_t VK_FORMAT_BC3_UNORM_BLOCK = 137u;
constexpr std::uint32_t VK_FORMAT_BC3_SRGB_BLOCK = 138u;
constexpr std::uint32_t VK_FORMAT_BC4_UNORM_BLOCK = 139u;
constexpr std::uint32_t VK_FORMAT_BC4_SNORM_BLOCK = 140u;
constexpr std::uint32_t VK_FORMAT_BC5_UNORM_BLOCK = 141u;
constexpr std::uint32_t VK_FORMAT_BC5_SNORM_BLOCK = 142u;
constexpr std::uint32_t VK_FORMAT_BC6H_UFLOAT_BLOCK = 143u;
constexpr std::uint32_t VK_FORMAT_BC6H_SFLOAT_BLOCK = 144u;
constexpr std::uint32_t VK_FORMAT_BC7_UNORM_BLOCK = 145u;
constexpr std::uint32_t VK_FORMAT_BC7_SRGB_BLOCK = 146u;

// Values from the Windows 10 DXGI_FORMAT enum (dxgiformat.h).
constexpr std::uint32_t DXGI_FORMAT_R8_UNORM = 61u;
constexpr std::uint32_t DXGI_FORMAT_B5G6R5_UNORM = 85u;
constexpr std::uint32_t DXGI_FORMAT_R8G8B8A8_UNORM = 28u;
constexpr std::uint32_t DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29u;
constexpr std::uint32_t DXGI_FORMAT_B8G8R8A8_UNORM = 87u;
constexpr std::uint32_t DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91u;
constexpr std::uint32_t DXGI_FORMAT_BC1_UNORM = 71u;
constexpr std::uint32_t DXGI_FORMAT_BC1_UNORM_SRGB = 72u;
constexpr std::uint32_t DXGI_FORMAT_BC2_UNORM = 74u;
constexpr std::uint32_t DXGI_FORMAT_BC2_UNORM_SRGB = 75u;
constexpr std::uint32_t DXGI_FORMAT_BC3_UNORM = 77u;
constexpr std::uint32_t DXGI_FORMAT_BC3_UNORM_SRGB = 78u;
constexpr std::uint32_t DXGI_FORMAT_BC4_UNORM = 80u;
constexpr std::uint32_t DXGI_FORMAT_BC4_SNORM = 81u;
constexpr std::uint32_t DXGI_FORMAT_BC5_UNORM = 83u;
constexpr std::uint32_t DXGI_FORMAT_BC5_SNORM = 84u;
constexpr std::uint32_t DXGI_FORMAT_BC6H_UF16 = 95u;
constexpr std::uint32_t DXGI_FORMAT_BC6H_SF16 = 96u;
constexpr std::uint32_t DXGI_FORMAT_BC7_UNORM = 98u;
constexpr std::uint32_t DXGI_FORMAT_BC7_UNORM_SRGB = 99u;

[[nodiscard]] TextureFormatMappingResult mappingFailure(std::string_view code,
                                                         std::string_view message) {
    TextureFormatMappingResult result;
    result.diagnostic = {std::string(code), std::string(message), 0, {}};
    return result;
}

[[nodiscard]] TextureFormatMappingResult mapped(TextureFormat format, std::uint32_t vulkan,
                                                std::uint32_t dxgi, bool compressed, bool srgb,
                                                bool signedChannels = false,
                                                std::uint8_t sourceBytesPerPixel = 0,
                                                bool cpuConversion = false) {
    TextureFormatMappingResult result;
    result.mapping = TextureFormatMapping{format, vulkan, dxgi, compressed, srgb, signedChannels,
                                          sourceBytesPerPixel, cpuConversion};
    return result;
}

[[nodiscard]] std::uint32_t maximumMipCount(std::uint32_t width, std::uint32_t height) noexcept {
    auto largest = std::max(width, height);
    std::uint32_t count = 1;
    while (largest > 1u) {
        largest >>= 1u;
        ++count;
    }
    return count;
}

[[nodiscard]] bool addSize(std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool multiplySize(std::size_t left, std::size_t right,
                                std::size_t& result) noexcept {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left) return false;
    result = left * right;
    return true;
}

[[nodiscard]] DdsUploadPlanResult planFailure(TextureUploadStatus status,
                                              std::string_view code, std::string_view message,
                                              std::size_t offset = 0,
                                              std::string_view source = {}) {
    DdsUploadPlanResult result;
    result.status = status;
    result.diagnostic = {std::string(code), std::string(message), offset, std::string(source)};
    return result;
}

[[nodiscard]] bool isRgbaMask(const apex::formats::DdsDescriptor& descriptor) noexcept {
    return descriptor.masks == std::array<std::uint32_t, 4>{0xffu, 0xff00u, 0xff0000u, 0xff000000u};
}

[[nodiscard]] bool isBgraMask(const apex::formats::DdsDescriptor& descriptor) noexcept {
    return descriptor.masks == std::array<std::uint32_t, 4>{0xff0000u, 0xff00u, 0xffu, 0xff000000u};
}

[[nodiscard]] bool isR8Mask(const apex::formats::DdsDescriptor& descriptor) noexcept {
    return descriptor.masks == std::array<std::uint32_t, 4>{0xffu, 0u, 0u, 0u};
}

[[nodiscard]] bool isRgb565Mask(const apex::formats::DdsDescriptor& descriptor) noexcept {
    return descriptor.masks == std::array<std::uint32_t, 4>{0xf800u, 0x07e0u, 0x001fu, 0u};
}

[[nodiscard]] std::size_t blockBytes(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::bc1_unorm:
    case TextureFormat::bc1_srgb:
    case TextureFormat::bc4_unorm:
    case TextureFormat::bc4_snorm: return 8u;
    case TextureFormat::bc2_unorm:
    case TextureFormat::bc2_srgb:
    case TextureFormat::bc3_unorm:
    case TextureFormat::bc3_srgb:
    case TextureFormat::bc5_unorm:
    case TextureFormat::bc5_snorm:
    case TextureFormat::bc6h_ufloat:
    case TextureFormat::bc6h_sfloat:
    case TextureFormat::bc7_unorm:
    case TextureFormat::bc7_srgb: return 16u;
    default: return 0u;
    }
}

[[nodiscard]] bool isKnownCompressed(apex::formats::DdsFormat format) noexcept {
    using apex::formats::DdsFormat;
    return format == DdsFormat::BC1 || format == DdsFormat::BC2 || format == DdsFormat::BC3 ||
           format == DdsFormat::BC4Unorm || format == DdsFormat::BC4Snorm ||
           format == DdsFormat::BC5Unorm || format == DdsFormat::BC5Snorm ||
           format == DdsFormat::BC6HUf16 || format == DdsFormat::BC6HSf16 ||
           format == DdsFormat::BC7 || format == DdsFormat::BC7Srgb;
}

[[nodiscard]] std::size_t bytesPerPixel(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::r8_unorm: return 1u;
    case TextureFormat::r5g6b5_unorm: return 2u;
    case TextureFormat::rgba8_unorm:
    case TextureFormat::rgba8_srgb:
    case TextureFormat::bgra8_unorm:
    case TextureFormat::bgra8_srgb: return 4u;
    default: return 0u;
    }
}

[[nodiscard]] std::uint8_t maskByte(std::uint32_t pixel, std::uint32_t mask) noexcept {
    if (mask == 0u) return 0u;
    std::uint32_t shift = 0u;
    while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
    if (shift == 32u) return 0u;
    const auto maximum = mask >> shift;
    const auto value = (pixel & mask) >> shift;
    if (maximum == 0u) return 0u;
    const auto scaled = static_cast<double>(value) * 255.0 / static_cast<double>(maximum);
    return static_cast<std::uint8_t>(std::clamp(std::floor(scaled + 0.5), 0.0, 255.0));
}

} // namespace

const char* textureFormatName(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::r8_unorm: return "R8_UNORM";
    case TextureFormat::r5g6b5_unorm: return "R5G6B5_UNORM";
    case TextureFormat::rgba8_unorm: return "RGBA8_UNORM";
    case TextureFormat::rgba8_srgb: return "RGBA8_SRGB";
    case TextureFormat::bgra8_unorm: return "BGRA8_UNORM";
    case TextureFormat::bgra8_srgb: return "BGRA8_SRGB";
    case TextureFormat::bc1_unorm: return "BC1_UNORM";
    case TextureFormat::bc1_srgb: return "BC1_SRGB";
    case TextureFormat::bc2_unorm: return "BC2_UNORM";
    case TextureFormat::bc2_srgb: return "BC2_SRGB";
    case TextureFormat::bc3_unorm: return "BC3_UNORM";
    case TextureFormat::bc3_srgb: return "BC3_SRGB";
    case TextureFormat::bc4_unorm: return "BC4_UNORM";
    case TextureFormat::bc4_snorm: return "BC4_SNORM";
    case TextureFormat::bc5_unorm: return "BC5_UNORM";
    case TextureFormat::bc5_snorm: return "BC5_SNORM";
    case TextureFormat::bc6h_ufloat: return "BC6H_UFLOAT";
    case TextureFormat::bc6h_sfloat: return "BC6H_SFLOAT";
    case TextureFormat::bc7_unorm: return "BC7_UNORM";
    case TextureFormat::bc7_srgb: return "BC7_SRGB";
    default: return "UNKNOWN";
    }
}

const char* textureUploadStatusName(TextureUploadStatus status) noexcept {
    switch (status) {
    case TextureUploadStatus::ready: return "ready";
    case TextureUploadStatus::invalid: return "invalid";
    case TextureUploadStatus::unsupported: return "unsupported";
    }
    return "unknown";
}

TextureFormatMappingResult mapDdsTextureFormat(const apex::formats::DdsDescriptor& descriptor) {
    using apex::formats::DdsFormat;
    const auto invalid = [&] {
        return mappingFailure("format_mismatch", "DDS compression flag does not match its format");
    };
    if (descriptor.compressed && !isKnownCompressed(descriptor.format)) return invalid();
    if (!descriptor.compressed && isKnownCompressed(descriptor.format)) return invalid();

    switch (descriptor.format) {
    case DdsFormat::Raw8:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_R8_UNORM) return mappingFailure("unsupported_format", "DDS R8 metadata has an unknown DXGI format");
        if (!isR8Mask(descriptor) && descriptor.dxgi == 0u) return mappingFailure("unsupported_format", "masked 8-bit DDS is not a canonical R8 texture");
        return mapped(TextureFormat::r8_unorm, VK_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, false, false);
    case DdsFormat::Raw16:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_B5G6R5_UNORM) return mappingFailure("unsupported_format", "masked 16-bit DDS is not a canonical RGB565 texture");
        if (!isRgb565Mask(descriptor)) return mappingFailure("unsupported_format", "masked 16-bit DDS is not a canonical RGB565 texture");
        return mapped(TextureFormat::r5g6b5_unorm, VK_FORMAT_R5G6B5_UNORM_PACK16, DXGI_FORMAT_B5G6R5_UNORM, false, false);
    case DdsFormat::Raw24:
        if (descriptor.dxgi != 0u)
            return mappingFailure("unsupported_format", "24-bit RGB cannot carry a DXGI format tag");
        return mapped(TextureFormat::rgba8_unorm, VK_FORMAT_R8G8B8A8_UNORM,
                      DXGI_FORMAT_R8G8B8A8_UNORM, false, false, false, 3u, true);
    case DdsFormat::Raw32:
        if (descriptor.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            return mapped(TextureFormat::rgba8_srgb, VK_FORMAT_R8G8B8A8_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, true);
        if (descriptor.dxgi == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            return mapped(TextureFormat::bgra8_srgb, VK_FORMAT_B8G8R8A8_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false, true);
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_R8G8B8A8_UNORM && descriptor.dxgi != DXGI_FORMAT_B8G8R8A8_UNORM)
            return mappingFailure("unsupported_format", "32-bit DDS metadata has an unknown DXGI format");
        if (descriptor.dxgi == DXGI_FORMAT_B8G8R8A8_UNORM || isBgraMask(descriptor))
            return mapped(TextureFormat::bgra8_unorm, VK_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, false, false);
        if (descriptor.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM || isRgbaMask(descriptor))
            return mapped(TextureFormat::rgba8_unorm, VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, false);
        return mappingFailure("unsupported_format", "masked 32-bit DDS is not a canonical RGBA8 or BGRA8 texture");
    case DdsFormat::BC1:
        if (descriptor.dxgi == 0u || descriptor.dxgi == DXGI_FORMAT_BC1_UNORM)
            return mapped(TextureFormat::bc1_unorm, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, DXGI_FORMAT_BC1_UNORM, true, false);
        if (descriptor.dxgi == DXGI_FORMAT_BC1_UNORM_SRGB)
            return mapped(TextureFormat::bc1_srgb, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, DXGI_FORMAT_BC1_UNORM_SRGB, true, true);
        return mappingFailure("unsupported_format", "BC1 metadata has an unknown DXGI format");
    case DdsFormat::BC2:
        if (descriptor.dxgi == 0u || descriptor.dxgi == DXGI_FORMAT_BC2_UNORM)
            return mapped(TextureFormat::bc2_unorm, VK_FORMAT_BC2_UNORM_BLOCK, DXGI_FORMAT_BC2_UNORM, true, false);
        if (descriptor.dxgi == DXGI_FORMAT_BC2_UNORM_SRGB)
            return mapped(TextureFormat::bc2_srgb, VK_FORMAT_BC2_SRGB_BLOCK, DXGI_FORMAT_BC2_UNORM_SRGB, true, true);
        return mappingFailure("unsupported_format", "BC2 metadata has an unknown DXGI format");
    case DdsFormat::BC3:
        if (descriptor.dxgi == 0u || descriptor.dxgi == DXGI_FORMAT_BC3_UNORM)
            return mapped(TextureFormat::bc3_unorm, VK_FORMAT_BC3_UNORM_BLOCK, DXGI_FORMAT_BC3_UNORM, true, false);
        if (descriptor.dxgi == DXGI_FORMAT_BC3_UNORM_SRGB)
            return mapped(TextureFormat::bc3_srgb, VK_FORMAT_BC3_SRGB_BLOCK, DXGI_FORMAT_BC3_UNORM_SRGB, true, true);
        return mappingFailure("unsupported_format", "BC3 metadata has an unknown DXGI format");
    case DdsFormat::BC4Unorm:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC4_UNORM) return mappingFailure("unsupported_format", "BC4 UNORM metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc4_unorm, VK_FORMAT_BC4_UNORM_BLOCK, DXGI_FORMAT_BC4_UNORM, true, false);
    case DdsFormat::BC4Snorm:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC4_SNORM) return mappingFailure("unsupported_format", "BC4 SNORM metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc4_snorm, VK_FORMAT_BC4_SNORM_BLOCK, DXGI_FORMAT_BC4_SNORM, true, false, true);
    case DdsFormat::BC5Unorm:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC5_UNORM) return mappingFailure("unsupported_format", "BC5 UNORM metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc5_unorm, VK_FORMAT_BC5_UNORM_BLOCK, DXGI_FORMAT_BC5_UNORM, true, false);
    case DdsFormat::BC5Snorm:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC5_SNORM) return mappingFailure("unsupported_format", "BC5 SNORM metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc5_snorm, VK_FORMAT_BC5_SNORM_BLOCK, DXGI_FORMAT_BC5_SNORM, true, false, true);
    case DdsFormat::BC6HUf16:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC6H_UF16) return mappingFailure("unsupported_format", "BC6H UF16 metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc6h_ufloat, VK_FORMAT_BC6H_UFLOAT_BLOCK, DXGI_FORMAT_BC6H_UF16, true, false);
    case DdsFormat::BC6HSf16:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC6H_SF16) return mappingFailure("unsupported_format", "BC6H SF16 metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc6h_sfloat, VK_FORMAT_BC6H_SFLOAT_BLOCK, DXGI_FORMAT_BC6H_SF16, true, false, true);
    case DdsFormat::BC7:
        if (descriptor.dxgi == 0u || descriptor.dxgi == DXGI_FORMAT_BC7_UNORM)
            return mapped(TextureFormat::bc7_unorm, VK_FORMAT_BC7_UNORM_BLOCK, DXGI_FORMAT_BC7_UNORM, true, false);
        if (descriptor.dxgi == DXGI_FORMAT_BC7_UNORM_SRGB)
            return mapped(TextureFormat::bc7_srgb, VK_FORMAT_BC7_SRGB_BLOCK, DXGI_FORMAT_BC7_UNORM_SRGB, true, true);
        return mappingFailure("unsupported_format", "BC7 metadata has an unknown DXGI format");
    case DdsFormat::BC7Srgb:
        if (descriptor.dxgi != 0u && descriptor.dxgi != DXGI_FORMAT_BC7_UNORM_SRGB) return mappingFailure("unsupported_format", "BC7 sRGB metadata has an unknown DXGI format");
        return mapped(TextureFormat::bc7_srgb, VK_FORMAT_BC7_SRGB_BLOCK, DXGI_FORMAT_BC7_UNORM_SRGB, true, true);
    default:
        return mappingFailure("unsupported_format", "DDS format has no Vulkan/DXGI upload mapping");
    }
}

DdsUploadPlanResult buildDdsUploadPlan(std::span<const std::uint8_t> bytes,
                                       const apex::formats::DdsDescriptor& descriptor,
                                       std::string source, apex::core::ParseLimits limits) {
    const auto formatResult = mapDdsTextureFormat(descriptor);
    if (!formatResult.ok())
        return planFailure(TextureUploadStatus::unsupported, formatResult.diagnostic.code,
                           formatResult.diagnostic.message, formatResult.diagnostic.offset, source);
    if (bytes.size() > limits.maxInputBytes)
        return planFailure(TextureUploadStatus::invalid, "input_too_large", "DDS input exceeds configured size limit", 0, source);
    if (descriptor.width == 0u || descriptor.height == 0u || descriptor.width > MAX_DIMENSION ||
        descriptor.height > MAX_DIMENSION || descriptor.mipCount == 0u ||
        descriptor.mipCount > maximumMipCount(descriptor.width, descriptor.height))
        return planFailure(TextureUploadStatus::invalid, "invalid_dimensions", "DDS dimensions or mip count are invalid", 0, source);
    if (descriptor.resourceDimension != 0u && descriptor.resourceDimension != 3u)
        return planFailure(TextureUploadStatus::unsupported, "unsupported_layout",
                           "Only DX10 2D texture resources are supported", 0, source);
    const bool dx10 = descriptor.resourceDimension != 0u;
    if (dx10 && descriptor.arraySize == 0u)
        return planFailure(TextureUploadStatus::invalid, "invalid_array_size",
                           "DX10 array size must be non-zero", 140u, source);
    // D3D11_RESOURCE_MISC_TEXTURECUBE is bit 2 in the DX10 extension misc flag.
    const bool cube = dx10 && (descriptor.miscFlags & 0x4u) != 0u;
    const std::uint32_t arrayCount = dx10 ? descriptor.arraySize : 1u;
    const std::uint32_t faceCount = cube ? 6u : 1u;
    std::size_t subresourceCount = 0u;
    if (!multiplySize(static_cast<std::size_t>(arrayCount), static_cast<std::size_t>(faceCount),
                      subresourceCount) ||
        !multiplySize(subresourceCount, static_cast<std::size_t>(descriptor.mipCount),
                      subresourceCount) ||
        subresourceCount > 65535u)
        return planFailure(TextureUploadStatus::invalid, "subresource_limit",
                           "DDS array, cube, and mip subresources exceed the bounded planner limit", 0, source);
    if (descriptor.dataOffset > bytes.size())
        return planFailure(TextureUploadStatus::invalid, "truncated_header", "DDS data offset exceeds input", descriptor.dataOffset, source);
    const auto mapping = *formatResult.mapping;
    const auto compressed = mapping.compressed;
    const auto expectedBlockBytes = blockBytes(mapping.format);
    const auto expectedBytesPerPixel = bytesPerPixel(mapping.format);
    const auto expectedSourceBytesPerPixel = mapping.sourceBytesPerPixel != 0u
                                                 ? static_cast<std::size_t>(mapping.sourceBytesPerPixel)
                                                 : expectedBytesPerPixel;
    if (compressed) {
        if (!descriptor.compressed || descriptor.blockBytes != expectedBlockBytes)
            return planFailure(TextureUploadStatus::invalid, "block_size_mismatch", "DDS block size does not match its mapped format", descriptor.dataOffset, source);
    } else if (descriptor.compressed || descriptor.bitsPerPixel % 8u != 0u ||
               descriptor.bitsPerPixel / 8u != expectedSourceBytesPerPixel) {
        return planFailure(TextureUploadStatus::invalid, "pixel_size_mismatch", "DDS pixel size does not match its mapped format", descriptor.dataOffset, source);
    }

    try {
        DdsUploadPlan plan;
        plan.descriptor = descriptor;
        plan.mapping = mapping;
        plan.subresources.reserve(subresourceCount);
        std::size_t offset = descriptor.dataOffset;
        std::size_t totalPayload = 0u;
        std::size_t totalConverted = 0u;
        for (std::uint32_t arrayLayer = 0u; arrayLayer < arrayCount; ++arrayLayer) {
            for (std::uint32_t face = 0u; face < faceCount; ++face) {
                auto width = descriptor.width;
                auto height = descriptor.height;
                for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
                    std::size_t rowPitch = 0u;
                    std::size_t rowCount = 0u;
                    std::size_t blocksWide = 0u;
                    std::size_t blocksHigh = 0u;
                    if (compressed) {
                        blocksWide = (static_cast<std::size_t>(width) + 3u) / 4u;
                        blocksHigh = (static_cast<std::size_t>(height) + 3u) / 4u;
                        if (!multiplySize(blocksWide, expectedBlockBytes, rowPitch))
                            return planFailure(TextureUploadStatus::invalid, "size_overflow",
                                               "DDS row pitch overflows", offset, source);
                        rowCount = blocksHigh;
                    } else {
                        std::size_t minimumPitch = 0u;
                        if (!multiplySize(static_cast<std::size_t>(width), expectedSourceBytesPerPixel,
                                          minimumPitch))
                            return planFailure(TextureUploadStatus::invalid, "size_overflow",
                                               "DDS row pitch overflows", offset, source);
                        if (level == 0u && descriptor.pitch != 0u &&
                            static_cast<std::size_t>(descriptor.pitch) < minimumPitch)
                            return planFailure(TextureUploadStatus::invalid, "row_pitch_mismatch",
                                               "DDS top-level row pitch is smaller than the packed row",
                                               offset, source);
                        rowPitch = level == 0u && descriptor.pitch != 0u &&
                                           static_cast<std::size_t>(descriptor.pitch) <= minimumPitch + 16u
                                       ? static_cast<std::size_t>(descriptor.pitch)
                                       : minimumPitch;
                        rowCount = height;
                    }
                    std::size_t slicePitch = 0u;
                    if (!multiplySize(rowPitch, rowCount, slicePitch))
                        return planFailure(TextureUploadStatus::invalid, "size_overflow",
                                           "DDS mip size overflows", offset, source);
                    if (slicePitch > bytes.size() - offset)
                        return planFailure(TextureUploadStatus::invalid, "truncated_payload",
                                           "DDS mip payload exceeds input", offset, source);
                    std::size_t nextTotal = 0u;
                    if (!addSize(totalPayload, slicePitch, nextTotal))
                        return planFailure(TextureUploadStatus::invalid, "size_overflow",
                                           "DDS payload size overflows", offset, source);
                    if (nextTotal > limits.maxOutputBytes)
                        return planFailure(TextureUploadStatus::invalid, "payload_too_large",
                                           "DDS upload payload exceeds configured size limit", offset, source);
                    DdsUploadSubresource subresource;
                    subresource.mipLevel = level;
                    subresource.arrayLayer = arrayLayer;
                    subresource.cubeFace = face;
                    subresource.width = width;
                    subresource.height = height;
                    subresource.offset = offset;
                    subresource.size = slicePitch;
                    subresource.rowPitch = rowPitch;
                    subresource.rowCount = rowCount;
                    subresource.slicePitch = slicePitch;
                    subresource.blocksWide = blocksWide;
                    subresource.blocksHigh = blocksHigh;
                    if (mapping.cpuConversion) {
                        std::size_t convertedRowPitch = 0u;
                        std::size_t convertedSize = 0u;
                        if (!multiplySize(static_cast<std::size_t>(width), expectedBytesPerPixel,
                                          convertedRowPitch) ||
                            !multiplySize(convertedRowPitch, static_cast<std::size_t>(height),
                                          convertedSize))
                            return planFailure(TextureUploadStatus::invalid, "size_overflow",
                                               "converted DDS payload overflows", offset, source);
                        std::size_t nextConverted = 0u;
                        if (!addSize(totalConverted, convertedSize, nextConverted) ||
                            nextConverted > limits.maxOutputBytes)
                            return planFailure(TextureUploadStatus::invalid, "payload_too_large",
                                               "converted DDS payload exceeds configured size limit",
                                               offset, source);
                        const auto convertedOffset = totalConverted;
                        plan.convertedPayload.resize(nextConverted);
                        for (std::uint32_t y = 0u; y < height; ++y) {
                            for (std::uint32_t x = 0u; x < width; ++x) {
                                const auto sourcePixel = offset + static_cast<std::size_t>(y) * rowPitch +
                                                          static_cast<std::size_t>(x) * expectedSourceBytesPerPixel;
                                const auto packed = static_cast<std::uint32_t>(bytes[sourcePixel]) |
                                                     (static_cast<std::uint32_t>(bytes[sourcePixel + 1u]) << 8u) |
                                                     (static_cast<std::uint32_t>(bytes[sourcePixel + 2u]) << 16u);
                                const auto target = convertedOffset +
                                                    static_cast<std::size_t>(y) * convertedRowPitch +
                                                    static_cast<std::size_t>(x) * 4u;
                                const auto red = maskByte(packed, descriptor.masks[0]);
                                plan.convertedPayload[target] = red;
                                plan.convertedPayload[target + 1u] =
                                    descriptor.luminance || descriptor.masks[1] == 0u
                                        ? red
                                        : maskByte(packed, descriptor.masks[1]);
                                plan.convertedPayload[target + 2u] =
                                    descriptor.luminance || descriptor.masks[2] == 0u
                                        ? red
                                        : maskByte(packed, descriptor.masks[2]);
                                plan.convertedPayload[target + 3u] = descriptor.masks[3] == 0u
                                                                         ? 255u
                                                                         : maskByte(packed, descriptor.masks[3]);
                            }
                        }
                        subresource.convertedOffset = convertedOffset;
                        subresource.convertedSize = convertedSize;
                        subresource.convertedRowPitch = convertedRowPitch;
                        totalConverted = nextConverted;
                    }
                    plan.subresources.push_back(subresource);
                    totalPayload = nextTotal;
                    offset += slicePitch;
                    width = std::max(1u, width >> 1u);
                    height = std::max(1u, height >> 1u);
                }
            }
        }
        plan.payloadBytes = totalPayload;
        DdsUploadPlanResult result;
        result.status = TextureUploadStatus::ready;
        result.plan = std::move(plan);
        return result;
    } catch (const std::bad_alloc&) {
        return planFailure(TextureUploadStatus::invalid, "allocation_failed",
                           "DDS upload plan allocation failed", descriptor.dataOffset, source);
    }
}

DdsUploadPlanResult buildDdsUploadPlan(std::span<const std::uint8_t> bytes, std::string source,
                                       apex::core::ParseLimits limits) {
    const auto descriptor = apex::formats::inspectDds(bytes, source, limits);
    if (!descriptor)
        return planFailure(TextureUploadStatus::invalid, "invalid_header", "not a structurally valid DDS header", 0, source);
    return buildDdsUploadPlan(bytes, *descriptor, std::move(source), limits);
}

} // namespace apex::render
