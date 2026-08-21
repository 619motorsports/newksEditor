#include "apex/formats/dds.hpp"

#include "apex/core/byte_reader.hpp"
#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace apex::formats {

namespace {

using apex::core::ParseError;

constexpr std::uint32_t DDS_MAGIC = 0x20534444u;
constexpr std::uint32_t DDPF_FOURCC = 0x4u;
constexpr std::uint32_t DDPF_LUMINANCE = 0x20000u;
constexpr std::uint32_t MAX_DIMENSION = 32768u;
constexpr std::size_t MAX_CPU_DECODED_BYTES = 512u * 1024u * 1024u;

[[nodiscard]] ParseError ddsError(std::string_view source, std::size_t offset,
                                  std::string_view code, std::string_view message) {
    return ParseError("DDS", std::string(source), offset, std::string(code), std::string(message));
}

[[nodiscard]] std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

[[nodiscard]] std::string fourCC(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::string result;
    result.reserve(4);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto value = bytes[offset + index];
        if (value != 0) result.push_back(static_cast<char>(value));
    }
    return result;
}

[[nodiscard]] std::uint32_t maximumMipCount(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t largest = std::max(width, height);
    std::uint32_t count = 1;
    while (largest > 1u) {
        largest >>= 1u;
        ++count;
    }
    return count;
}

[[nodiscard]] bool isRaw(DdsFormat format) noexcept {
    return format == DdsFormat::Raw8 || format == DdsFormat::Raw16 ||
           format == DdsFormat::Raw24 || format == DdsFormat::Raw32;
}

[[nodiscard]] bool isKnownCompressed(DdsFormat format) noexcept {
    return format == DdsFormat::BC1 || format == DdsFormat::BC2 ||
           format == DdsFormat::BC3 || format == DdsFormat::BC4Unorm ||
           format == DdsFormat::BC4Snorm || format == DdsFormat::BC5Unorm ||
           format == DdsFormat::BC5Snorm || format == DdsFormat::BC6HUf16 ||
           format == DdsFormat::BC6HSf16 || format == DdsFormat::BC7 ||
           format == DdsFormat::BC7Srgb;
}

[[nodiscard]] std::uint32_t bitsForRaw(DdsFormat format) noexcept {
    switch (format) {
    case DdsFormat::Raw8: return 8;
    case DdsFormat::Raw16: return 16;
    case DdsFormat::Raw24: return 24;
    case DdsFormat::Raw32: return 32;
    default: return 0;
    }
}

[[nodiscard]] std::uint32_t blockBytesFor(DdsFormat format) noexcept {
    switch (format) {
    case DdsFormat::BC1:
    case DdsFormat::BC4Unorm:
    case DdsFormat::BC4Snorm: return 8;
    case DdsFormat::BC2:
    case DdsFormat::BC3:
    case DdsFormat::BC5Unorm:
    case DdsFormat::BC5Snorm:
    case DdsFormat::BC6HUf16:
    case DdsFormat::BC6HSf16:
    case DdsFormat::BC7:
    case DdsFormat::BC7Srgb: return 16;
    default: return 0;
    }
}

[[nodiscard]] bool isGpuOnly(DdsFormat format) noexcept {
    return format == DdsFormat::BC6HUf16 || format == DdsFormat::BC6HSf16 ||
           format == DdsFormat::BC7 || format == DdsFormat::BC7Srgb;
}

[[nodiscard]] std::uint8_t roundByte(double value) noexcept {
    if (value <= 0.0) return 0;
    if (value >= 255.0) return 255;
    return static_cast<std::uint8_t>(std::floor(value + 0.5));
}

[[nodiscard]] std::uint8_t maskByte(std::uint32_t pixel, std::uint32_t mask) noexcept {
    if (mask == 0) return 0;
    std::uint32_t shift = 0;
    while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
    if (shift == 32u) return 0;
    const auto maximum = mask >> shift;
    const auto value = (pixel & mask) >> shift;
    return roundByte(static_cast<double>(value) * 255.0 / static_cast<double>(maximum));
}

[[nodiscard]] std::array<std::uint8_t, 3> rgb565(std::uint16_t value) noexcept {
    return {roundByte(static_cast<double>((value >> 11u) & 31u) * 255.0 / 31.0),
            roundByte(static_cast<double>((value >> 5u) & 63u) * 255.0 / 63.0),
            roundByte(static_cast<double>(value & 31u) * 255.0 / 31.0)};
}

[[nodiscard]] std::array<std::uint8_t, 3> mixRgb(const std::array<std::uint8_t, 3>& first,
                                                 const std::array<std::uint8_t, 3>& second,
                                                 int firstWeight, int divisor) noexcept {
    std::array<std::uint8_t, 3> result{};
    for (std::size_t index = 0; index < 3; ++index)
        result[index] = roundByte((static_cast<double>(first[index]) * firstWeight +
                                   static_cast<double>(second[index]) * (divisor - firstWeight)) /
                                  divisor);
    return result;
}

struct BcPalette {
    std::array<std::uint8_t, 8> values{};
};

[[nodiscard]] BcPalette bcPalette(std::span<const std::uint8_t> bytes, std::size_t offset,
                                  bool signedChannels) noexcept {
    const auto convert = [signedChannels](std::uint8_t value) -> int {
        if (!signedChannels) return static_cast<int>(value);
        return value > 127u ? static_cast<int>(value) - 256 : static_cast<int>(value);
    };
    const int first = convert(bytes[offset]);
    const int second = convert(bytes[offset + 1]);
    std::array<double, 8> values{};
    values[0] = first;
    values[1] = second;
    if (first > second) {
        for (int index = 1; index <= 6; ++index)
            values[static_cast<std::size_t>(index + 1)] =
                (static_cast<double>(7 - index) * first + static_cast<double>(index) * second) / 7.0;
    } else {
        for (int index = 1; index <= 4; ++index)
            values[static_cast<std::size_t>(index + 1)] =
                (static_cast<double>(5 - index) * first + static_cast<double>(index) * second) / 5.0;
        values[6] = signedChannels ? -127.0 : 0.0;
        values[7] = signedChannels ? 127.0 : 255.0;
    }
    BcPalette result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (signedChannels) {
            const auto clamped = std::clamp(values[index], -127.0, 127.0);
            result.values[index] = roundByte((clamped / 127.0 * 0.5 + 0.5) * 255.0);
        } else {
            result.values[index] = roundByte(values[index]);
        }
    }
    return result;
}

[[nodiscard]] std::uint8_t bcIndex(std::span<const std::uint8_t> bytes, std::size_t offset,
                                   std::size_t pixel) noexcept {
    const auto bit = pixel * 3u;
    const auto source = offset + 2u + (bit >> 3u);
    const auto packed = static_cast<std::uint32_t>(bytes[source]) |
                        (source + 1u < offset + 8u
                             ? static_cast<std::uint32_t>(bytes[source + 1]) << 8u
                             : 0u);
    return static_cast<std::uint8_t>((packed >> (bit & 7u)) & 7u);
}

[[nodiscard]] std::uint8_t colorIndex(std::span<const std::uint8_t> bytes, std::size_t offset,
                                      std::size_t pixel) noexcept {
    const auto bit = pixel * 2u;
    const auto packed = static_cast<std::uint32_t>(bytes[offset + 4]) |
                        (static_cast<std::uint32_t>(bytes[offset + 5]) << 8u) |
                        (static_cast<std::uint32_t>(bytes[offset + 6]) << 16u) |
                        (static_cast<std::uint32_t>(bytes[offset + 7]) << 24u);
    return static_cast<std::uint8_t>((packed >> bit) & 3u);
}

void validateDescriptor(const DdsDescriptor& descriptor, std::span<const std::uint8_t> bytes,
                        std::string_view source, const apex::core::ParseLimits& limits) {
    if (bytes.size() > limits.maxInputBytes)
        throw ddsError(source, 0, "INPUT_TOO_LARGE", "input exceeds configured size limit");
    if (descriptor.width == 0 || descriptor.height == 0 || descriptor.width > MAX_DIMENSION ||
        descriptor.height > MAX_DIMENSION || descriptor.mipCount == 0 ||
        descriptor.mipCount > maximumMipCount(descriptor.width, descriptor.height)) {
        throw ddsError(source, 0, "INVALID_DIMENSIONS", "DDS dimensions or mip count are invalid");
    }
    if (descriptor.dataOffset > bytes.size())
        throw ddsError(source, descriptor.dataOffset, "TRUNCATED", "DDS data offset exceeds input");
    if (!descriptor.compressed && (!isRaw(descriptor.format) || descriptor.bitsPerPixel == 0 ||
                                   descriptor.bitsPerPixel % 8u != 0u)) {
        throw ddsError(source, descriptor.dataOffset, "UNSUPPORTED_FORMAT", "DDS raw format is unsupported");
    }
    if (descriptor.compressed && (!isKnownCompressed(descriptor.format) || descriptor.blockBytes == 0)) {
        throw ddsError(source, descriptor.dataOffset, "UNSUPPORTED_FORMAT", "DDS compressed format is unsupported");
    }

    std::size_t decodedBytes = 0;
    std::size_t offset = descriptor.dataOffset;
    std::uint32_t width = descriptor.width;
    std::uint32_t height = descriptor.height;
    for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
        const auto texels = apex::core::checkedMultiply(static_cast<std::size_t>(width),
                                                        static_cast<std::size_t>(height), "DDS",
                                                        source, offset, "decoded texels");
        const auto levelBytes = apex::core::checkedMultiply(texels, 4u, "DDS", source, offset,
                                                             "decoded pixels");
        const auto outputLimit = std::min(limits.maxOutputBytes, MAX_CPU_DECODED_BYTES);
        if (levelBytes > outputLimit - std::min(decodedBytes, outputLimit))
            throw ddsError(source, offset, "OUTPUT_TOO_LARGE", "decoded texture exceeds configured size limit");
        decodedBytes += levelBytes;

        std::size_t payload = 0;
        if (descriptor.compressed) {
            const auto blocksWide = (static_cast<std::size_t>(width) + 3u) / 4u;
            const auto blocksHigh = (static_cast<std::size_t>(height) + 3u) / 4u;
            const auto blocks = apex::core::checkedMultiply(blocksWide, blocksHigh, "DDS", source,
                                                            offset, "compressed blocks");
            payload = apex::core::checkedMultiply(blocks, descriptor.blockBytes, "DDS", source,
                                                  offset, "compressed mip");
        } else {
            const auto bytesPerPixel = static_cast<std::size_t>(descriptor.bitsPerPixel / 8u);
            const auto minimumPitch = apex::core::checkedMultiply(static_cast<std::size_t>(width),
                                                                  bytesPerPixel, "DDS", source, offset,
                                                                  "raw row");
            // The top-level DDS pitch is authoritative whenever it is large
            // enough for one packed row. Do not cap legal driver alignment to
            // a guessed padding amount; that would shift every later row and
            // mip for textures with larger alignment.
            const auto rowPitch = (level == 0u && descriptor.pitch >= minimumPitch)
                                      ? static_cast<std::size_t>(descriptor.pitch)
                                      : minimumPitch;
            payload = apex::core::checkedMultiply(rowPitch, static_cast<std::size_t>(height), "DDS",
                                                  source, offset, "raw mip");
        }
        if (payload > bytes.size() - offset)
            throw ddsError(source, offset, "TRUNCATED", "DDS mip exceeds texture data");
        offset += payload;
        width = std::max(1u, width >> 1u);
        height = std::max(1u, height >> 1u);
    }
}

[[nodiscard]] std::vector<DdsLevel> decodeRaw(std::span<const std::uint8_t> bytes,
                                               const DdsDescriptor& descriptor) {
    std::vector<DdsLevel> levels;
    levels.reserve(descriptor.mipCount);
    std::size_t offset = descriptor.dataOffset;
    std::uint32_t width = descriptor.width;
    std::uint32_t height = descriptor.height;
    const auto bytesPerPixel = static_cast<std::size_t>(descriptor.bitsPerPixel / 8u);
    for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
        const auto minimumPitch = static_cast<std::size_t>(width) * bytesPerPixel;
        const auto rowPitch = (level == 0u && descriptor.pitch >= minimumPitch)
                                  ? static_cast<std::size_t>(descriptor.pitch)
                                  : minimumPitch;
        DdsLevel output{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4u)};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto source = offset + static_cast<std::size_t>(y) * rowPitch +
                                    static_cast<std::size_t>(x) * bytesPerPixel;
                std::uint32_t packed = 0;
                for (std::size_t byte = 0; byte < bytesPerPixel; ++byte)
                    packed |= static_cast<std::uint32_t>(bytes[source + byte]) << (byte * 8u);
                const auto target = (static_cast<std::size_t>(y) * width + x) * 4u;
                const auto red = maskByte(packed, descriptor.masks[0]);
                output.pixels[target] = red;
                output.pixels[target + 1] = descriptor.luminance || descriptor.masks[1] == 0
                                                 ? red
                                                 : maskByte(packed, descriptor.masks[1]);
                output.pixels[target + 2] = descriptor.luminance || descriptor.masks[2] == 0
                                                 ? red
                                                 : maskByte(packed, descriptor.masks[2]);
                output.pixels[target + 3] = descriptor.masks[3] == 0
                                                 ? 255
                                                 : maskByte(packed, descriptor.masks[3]);
            }
        }
        levels.push_back(std::move(output));
        offset += rowPitch * static_cast<std::size_t>(height);
        width = std::max(1u, width >> 1u);
        height = std::max(1u, height >> 1u);
    }
    return levels;
}

[[nodiscard]] std::vector<DdsLevel> decodeBc(std::span<const std::uint8_t> bytes,
                                             const DdsDescriptor& descriptor) {
    std::vector<DdsLevel> levels;
    levels.reserve(descriptor.mipCount);
    std::size_t offset = descriptor.dataOffset;
    std::uint32_t width = descriptor.width;
    std::uint32_t height = descriptor.height;
    const bool bcColor = descriptor.format == DdsFormat::BC1 || descriptor.format == DdsFormat::BC2 ||
                         descriptor.format == DdsFormat::BC3;
    const bool bc5 = descriptor.format == DdsFormat::BC5Unorm || descriptor.format == DdsFormat::BC5Snorm;
    for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
        const auto blocksWide = (static_cast<std::size_t>(width) + 3u) / 4u;
        const auto blocksHigh = (static_cast<std::size_t>(height) + 3u) / 4u;
        const auto payload = blocksWide * blocksHigh * descriptor.blockBytes;
        DdsLevel output{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4u)};
        for (std::size_t blockY = 0; blockY < blocksHigh; ++blockY) {
            for (std::size_t blockX = 0; blockX < blocksWide; ++blockX) {
                const auto block = offset + (blockY * blocksWide + blockX) * descriptor.blockBytes;
                std::array<std::uint8_t, 3> colorPalette[4]{};
                BcPalette redPalette{}, greenPalette{};
                if (bcColor) {
                    const auto colorOffset = descriptor.format == DdsFormat::BC1 ? block : block + 8u;
                    const auto first = static_cast<std::uint16_t>(bytes[colorOffset] |
                                                                   (static_cast<std::uint16_t>(bytes[colorOffset + 1]) << 8u));
                    const auto second = static_cast<std::uint16_t>(bytes[colorOffset + 2] |
                                                                    (static_cast<std::uint16_t>(bytes[colorOffset + 3]) << 8u));
                    colorPalette[0] = rgb565(first);
                    colorPalette[1] = rgb565(second);
                    const bool fourColor = descriptor.format != DdsFormat::BC1 || first > second;
                    colorPalette[2] = fourColor ? mixRgb(colorPalette[0], colorPalette[1], 2, 3)
                                                : mixRgb(colorPalette[0], colorPalette[1], 1, 2);
                    colorPalette[3] = fourColor ? mixRgb(colorPalette[0], colorPalette[1], 1, 3)
                                                : std::array<std::uint8_t, 3>{0, 0, 0};
                    if (descriptor.format == DdsFormat::BC3)
                        redPalette = bcPalette(bytes, block, false);
                } else {
                    redPalette = bcPalette(bytes, block, descriptor.signedChannels);
                    if (bc5) greenPalette = bcPalette(bytes, block + 8u, descriptor.signedChannels);
                }
                for (std::size_t y = 0; y < 4; ++y) {
                    for (std::size_t x = 0; x < 4; ++x) {
                        const auto px = blockX * 4u + x;
                        const auto py = blockY * 4u + y;
                        if (px >= width || py >= height) continue;
                        const auto pixel = y * 4u + x;
                        std::uint8_t red = 0, green = 0, blue = 0, alpha = 255;
                        if (bcColor) {
                            const auto index = colorIndex(bytes, descriptor.format == DdsFormat::BC1 ? block : block + 8u, pixel);
                            const auto color = colorPalette[index];
                            red = color[0]; green = color[1]; blue = color[2];
                            if (descriptor.format == DdsFormat::BC1) {
                                const auto first = static_cast<std::uint16_t>(bytes[block] |
                                                                               (static_cast<std::uint16_t>(bytes[block + 1]) << 8u));
                                const auto second = static_cast<std::uint16_t>(bytes[block + 2] |
                                                                                (static_cast<std::uint16_t>(bytes[block + 3]) << 8u));
                                if (first <= second && index == 3u) alpha = 0;
                            } else if (descriptor.format == DdsFormat::BC2) {
                                const auto nibble =
                                    (static_cast<std::uint32_t>(bytes[block + pixel / 2u]) >>
                                     ((pixel & 1u) * 4u)) &
                                    0xfu;
                                alpha = static_cast<std::uint8_t>(nibble * 17u);
                            } else {
                                alpha = redPalette.values[bcIndex(bytes, block, pixel)];
                            }
                            if (descriptor.premultiplied && alpha > 0u && alpha < 255u) {
                                red = static_cast<std::uint8_t>(std::min(255.0, std::floor(static_cast<double>(red) * 255.0 / alpha + 0.5)));
                                green = static_cast<std::uint8_t>(std::min(255.0, std::floor(static_cast<double>(green) * 255.0 / alpha + 0.5)));
                                blue = static_cast<std::uint8_t>(std::min(255.0, std::floor(static_cast<double>(blue) * 255.0 / alpha + 0.5)));
                            }
                        } else {
                            red = redPalette.values[bcIndex(bytes, block, pixel)];
                            if (bc5) {
                                green = greenPalette.values[bcIndex(bytes, block + 8u, pixel)];
                                const auto nx = static_cast<double>(red) / 127.5 - 1.0;
                                const auto ny = static_cast<double>(green) / 127.5 - 1.0;
                                blue = roundByte((std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny)) * 0.5 + 0.5) * 255.0);
                            } else {
                                green = red; blue = red;
                            }
                        }
                        const auto target = (py * static_cast<std::size_t>(width) + px) * 4u;
                        output.pixels[target] = red;
                        output.pixels[target + 1] = green;
                        output.pixels[target + 2] = blue;
                        output.pixels[target + 3] = alpha;
                    }
                }
            }
        }
        levels.push_back(std::move(output));
        offset += payload;
        width = std::max(1u, width >> 1u);
        height = std::max(1u, height >> 1u);
    }
    return levels;
}

} // namespace

const char* ddsFormatName(DdsFormat format) noexcept {
    switch (format) {
    case DdsFormat::Raw8: return "RAW_8";
    case DdsFormat::Raw16: return "RAW_16";
    case DdsFormat::Raw24: return "RAW_24";
    case DdsFormat::Raw32: return "RAW_32";
    case DdsFormat::BC1: return "BC1";
    case DdsFormat::BC2: return "BC2";
    case DdsFormat::BC3: return "BC3";
    case DdsFormat::BC4Unorm: return "BC4_UNORM";
    case DdsFormat::BC4Snorm: return "BC4_SNORM";
    case DdsFormat::BC5Unorm: return "BC5_UNORM";
    case DdsFormat::BC5Snorm: return "BC5_SNORM";
    case DdsFormat::BC6HUf16: return "BC6H_UF16";
    case DdsFormat::BC6HSf16: return "BC6H_SF16";
    case DdsFormat::BC7: return "BC7";
    case DdsFormat::BC7Srgb: return "BC7_SRGB";
    default: return "UNKNOWN";
    }
}

std::optional<DdsDescriptor> inspectDds(std::span<const std::uint8_t> bytes, std::string source,
                                        apex::core::ParseLimits limits) {
    (void)source;
    if (bytes.size() > limits.maxInputBytes || bytes.size() < 128u) return std::nullopt;
    if (u32(bytes, 0) != DDS_MAGIC || u32(bytes, 4) != 124u || u32(bytes, 76) != 32u) return std::nullopt;
    const auto width = u32(bytes, 16);
    const auto height = u32(bytes, 12);
    if (width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION) return std::nullopt;
    const auto suppliedMips = u32(bytes, 28);
    const auto mipCount = std::max(1u, suppliedMips);
    if (mipCount > maximumMipCount(width, height)) return std::nullopt;
    DdsDescriptor descriptor;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.mipCount = mipCount;
    descriptor.pitch = u32(bytes, 20);
    const auto flags = u32(bytes, 80);
    descriptor.fourCC = fourCC(bytes, 84);
    if ((flags & DDPF_FOURCC) != 0u) {
        descriptor.compressed = true;
        descriptor.dataOffset = 128u;
        if (descriptor.fourCC == "DX10") {
            if (bytes.size() < 148u) return std::nullopt;
            descriptor.dataOffset = 148u;
            descriptor.dxgi = u32(bytes, 128);
            descriptor.resourceDimension = u32(bytes, 132);
            descriptor.miscFlags = u32(bytes, 136);
            descriptor.arraySize = u32(bytes, 140);
            // D3D10_RESOURCE_DIMENSION_TEXTURE1D/2D/3D are 2/3/4.
            // UNKNOWN (0), BUFFER (1), and future values are not valid for a
            // texture upload descriptor. An array size of zero is malformed.
            if (descriptor.resourceDimension < 2u || descriptor.resourceDimension > 4u ||
                descriptor.arraySize == 0u)
                return std::nullopt;
            switch (descriptor.dxgi) {
            case 28: case 29:
                descriptor.format = DdsFormat::Raw32; descriptor.compressed = false;
                descriptor.masks = {0xffu, 0xff00u, 0xff0000u, 0xff000000u}; break;
            case 61:
                descriptor.format = DdsFormat::Raw8; descriptor.compressed = false;
                descriptor.luminance = true; descriptor.masks = {0xffu, 0, 0, 0}; break;
            case 87: case 91:
                descriptor.format = DdsFormat::Raw32; descriptor.compressed = false;
                descriptor.masks = {0xff0000u, 0xff00u, 0xffu, 0xff000000u}; break;
            case 71: case 72: descriptor.format = DdsFormat::BC1; descriptor.blockBytes = 8; break;
            case 74: case 75: descriptor.format = DdsFormat::BC2; descriptor.blockBytes = 16; break;
            case 77: case 78: descriptor.format = DdsFormat::BC3; descriptor.blockBytes = 16; break;
            case 80: descriptor.format = DdsFormat::BC4Unorm; descriptor.blockBytes = 8; break;
            case 81: descriptor.format = DdsFormat::BC4Snorm; descriptor.blockBytes = 8; descriptor.signedChannels = true; break;
            case 83: descriptor.format = DdsFormat::BC5Unorm; descriptor.blockBytes = 16; break;
            case 84: descriptor.format = DdsFormat::BC5Snorm; descriptor.blockBytes = 16; descriptor.signedChannels = true; break;
            case 95: descriptor.format = DdsFormat::BC6HUf16; descriptor.blockBytes = 16; descriptor.gpuRequired = true; break;
            case 96: descriptor.format = DdsFormat::BC6HSf16; descriptor.blockBytes = 16; descriptor.gpuRequired = true; break;
            case 98: descriptor.format = DdsFormat::BC7; descriptor.blockBytes = 16; descriptor.gpuRequired = true; break;
            case 99: descriptor.format = DdsFormat::BC7Srgb; descriptor.blockBytes = 16; descriptor.gpuRequired = true; break;
            default: descriptor.format = DdsFormat::Unknown; descriptor.blockBytes = 0; descriptor.gpuRequired = true; break;
            }
        } else {
            if (descriptor.fourCC == "DXT1") { descriptor.format = DdsFormat::BC1; descriptor.blockBytes = 8; }
            else if (descriptor.fourCC == "DXT2") { descriptor.format = DdsFormat::BC2; descriptor.blockBytes = 16; descriptor.premultiplied = true; }
            else if (descriptor.fourCC == "DXT3") { descriptor.format = DdsFormat::BC2; descriptor.blockBytes = 16; }
            else if (descriptor.fourCC == "DXT4") { descriptor.format = DdsFormat::BC3; descriptor.blockBytes = 16; descriptor.premultiplied = true; }
            else if (descriptor.fourCC == "DXT5") { descriptor.format = DdsFormat::BC3; descriptor.blockBytes = 16; }
            else if (descriptor.fourCC == "ATI1" || descriptor.fourCC == "BC4U") { descriptor.format = DdsFormat::BC4Unorm; descriptor.blockBytes = 8; }
            else if (descriptor.fourCC == "BC4S") { descriptor.format = DdsFormat::BC4Snorm; descriptor.blockBytes = 8; descriptor.signedChannels = true; }
            else if (descriptor.fourCC == "ATI2" || descriptor.fourCC == "BC5U") { descriptor.format = DdsFormat::BC5Unorm; descriptor.blockBytes = 16; }
            else if (descriptor.fourCC == "BC5S") { descriptor.format = DdsFormat::BC5Snorm; descriptor.blockBytes = 16; descriptor.signedChannels = true; }
            else { descriptor.format = DdsFormat::Unknown; descriptor.gpuRequired = true; }
        }
    } else {
        descriptor.dataOffset = 128u;
        descriptor.bitsPerPixel = u32(bytes, 88);
        if (descriptor.bitsPerPixel != 8u && descriptor.bitsPerPixel != 16u &&
            descriptor.bitsPerPixel != 24u && descriptor.bitsPerPixel != 32u) return std::nullopt;
        descriptor.format = descriptor.bitsPerPixel == 8u ? DdsFormat::Raw8
                          : descriptor.bitsPerPixel == 16u ? DdsFormat::Raw16
                          : descriptor.bitsPerPixel == 24u ? DdsFormat::Raw24 : DdsFormat::Raw32;
        descriptor.compressed = false;
        descriptor.luminance = (flags & DDPF_LUMINANCE) != 0u;
        for (std::size_t index = 0; index < descriptor.masks.size(); ++index)
            descriptor.masks[index] = u32(bytes, 92u + index * 4u);
    }
    if (!descriptor.compressed) descriptor.bitsPerPixel = bitsForRaw(descriptor.format);
    if (descriptor.compressed && descriptor.blockBytes == 0u && descriptor.format != DdsFormat::Unknown)
        descriptor.blockBytes = blockBytesFor(descriptor.format);
    return descriptor;
}

bool webglCompressedMipChainSafe(const DdsDescriptor& descriptor) noexcept {
    if (!descriptor.compressed || descriptor.width == 0 || descriptor.height == 0 || descriptor.mipCount == 0 ||
        descriptor.width > MAX_DIMENSION || descriptor.height > MAX_DIMENSION ||
        descriptor.mipCount > maximumMipCount(descriptor.width, descriptor.height)) return false;
    auto width = descriptor.width;
    auto height = descriptor.height;
    for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
        if ((width % 4u) != 0u || (height % 4u) != 0u) return false;
        width = std::max(1u, width >> 1u);
        height = std::max(1u, height >> 1u);
    }
    return true;
}

std::vector<DdsLevel> decodeDdsRgba(std::span<const std::uint8_t> bytes,
                                    const DdsDescriptor& descriptor, std::string source,
                                    apex::core::ParseLimits limits) {
    validateDescriptor(descriptor, bytes, source, limits);
    if (descriptor.gpuRequired || isGpuOnly(descriptor.format))
        throw ddsError(source, descriptor.dataOffset, "GPU_REQUIRED",
                       std::string("DDS ") + ddsFormatName(descriptor.format) + " requires a GPU compressed-texture path");
    if (!descriptor.compressed) return decodeRaw(bytes, descriptor);
    return decodeBc(bytes, descriptor);
}

std::vector<DdsLevel> decodeDdsRgba(std::span<const std::uint8_t> bytes, std::string source,
                                    apex::core::ParseLimits limits) {
    const auto descriptor = inspectDds(bytes, source, limits);
    if (!descriptor) throw ddsError(source, 0, "INVALID_HEADER", "not a supported DDS header");
    return decodeDdsRgba(bytes, *descriptor, std::move(source), limits);
}

} // namespace apex::formats
