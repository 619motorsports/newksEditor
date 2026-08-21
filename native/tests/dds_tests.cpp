#include "apex/core/parse_error.hpp"
#include "apex/formats/dds.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using apex::core::ParseError;
using namespace apex::formats;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4u <= bytes.size(), "test header write out of bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16u);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24u);
}

std::vector<std::uint8_t> rawDds(std::uint32_t width, std::uint32_t height,
                                 std::uint32_t bits, std::array<std::uint32_t, 4> masks,
                                 std::vector<std::uint8_t> pixels, std::uint32_t flags = 0x40u,
                                 std::uint32_t pitch = 0) {
    const auto bytesPerPixel = bits / 8u;
    if (pitch == 0) pitch = width * bytesPerPixel;
    std::vector<std::uint8_t> bytes(128u + pixels.size(), 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 8, 0x100fu);
    put32(bytes, 12, height); put32(bytes, 16, width); put32(bytes, 20, pitch); put32(bytes, 28, 1u);
    put32(bytes, 76, 32u); put32(bytes, 80, flags); put32(bytes, 88, bits);
    for (std::size_t index = 0; index < masks.size(); ++index) put32(bytes, 92u + index * 4u, masks[index]);
    std::copy(pixels.begin(), pixels.end(), bytes.begin() + 128);
    return bytes;
}

std::vector<std::uint8_t> legacyBcDds(std::string_view tag, std::vector<std::uint8_t> block,
                                      std::uint32_t width = 4, std::uint32_t height = 4) {
    std::vector<std::uint8_t> bytes(128u + block.size(), 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 28, 1u); put32(bytes, 76, 32u); put32(bytes, 80, 4u);
    require(tag.size() == 4u, "fourCC test tag length");
    for (std::size_t index = 0; index < 4; ++index) bytes[84u + index] = static_cast<std::uint8_t>(tag[index]);
    std::copy(block.begin(), block.end(), bytes.begin() + 128);
    return bytes;
}

std::vector<std::uint8_t> dx10Dds(std::uint32_t width, std::uint32_t height,
                                  std::uint32_t dxgi, std::vector<std::uint8_t> payload,
                                  std::uint32_t resourceDimension = 3u,
                                  std::uint32_t miscFlags = 0u,
                                  std::uint32_t arraySize = 1u) {
    std::vector<std::uint8_t> bytes(148u + payload.size(), 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 28, 1u); put32(bytes, 76, 32u); put32(bytes, 80, 4u);
    bytes[84] = 'D'; bytes[85] = 'X'; bytes[86] = '1'; bytes[87] = '0'; put32(bytes, 128, dxgi);
    put32(bytes, 132, resourceDimension);
    put32(bytes, 136, miscFlags);
    put32(bytes, 140, arraySize);
    std::copy(payload.begin(), payload.end(), bytes.begin() + 148);
    return bytes;
}

template <typename Function>
void expectsParseError(Function&& function, std::string_view code) {
    bool caught = false;
    try {
        function();
    } catch (const ParseError& error) {
        caught = true;
        require(error.format() == "DDS", "unexpected parser format");
        if (error.code() != code)
            throw std::runtime_error("unexpected DDS parse error code: " + error.code() + " expected " +
                                     std::string(code));
    }
    require(caught, "expected DDS ParseError");
}

void decodesMaskedRawFormats() {
    const auto bgr = rawDds(2, 1, 24, {0xff0000u, 0xff00u, 0xffu, 0}, {30, 20, 10, 60, 50, 40});
    const auto bgrDescriptor = inspectDds(bgr);
    require(bgrDescriptor.has_value() && bgrDescriptor->format == DdsFormat::Raw24, "24-bit descriptor");
    require(decodeDdsRgba(bgr, *bgrDescriptor)[0].pixels ==
                std::vector<std::uint8_t>{10, 20, 30, 255, 40, 50, 60, 255}, "24-bit BGR decode");

    const auto luminance = rawDds(2, 1, 16, {0xffu, 0, 0, 0xff00u}, {32, 64, 128, 255}, 0x20001u);
    const auto luminancePixels = decodeDdsRgba(luminance)[0].pixels;
    require(luminancePixels == std::vector<std::uint8_t>{32, 32, 32, 64, 128, 128, 128, 255}, "luminance decode");

    const auto rgb565 = rawDds(2, 1, 16, {0xf800u, 0x07e0u, 0x001fu, 0}, {0, 0xf8u, 0xe0u, 0x07u});
    require(decodeDdsRgba(rgb565)[0].pixels ==
                std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255}, "RGB565 decode");

    const auto rgba = dx10Dds(1, 1, 28, {10, 20, 30, 40});
    const auto rgbaDescriptor = inspectDds(rgba);
    require(rgbaDescriptor.has_value() && rgbaDescriptor->format == DdsFormat::Raw32 &&
                !rgbaDescriptor->compressed && rgbaDescriptor->resourceDimension == 3u &&
                rgbaDescriptor->miscFlags == 0u && rgbaDescriptor->arraySize == 1u,
            "DX10 RGBA8 descriptor and extension metadata");
    require(decodeDdsRgba(rgba, *rgbaDescriptor)[0].pixels == std::vector<std::uint8_t>{10, 20, 30, 40},
            "DX10 RGBA8 decode");

    std::vector<std::uint8_t> paddedPixels(64u, 0);
    paddedPixels[0] = 30u; paddedPixels[1] = 20u; paddedPixels[2] = 10u; paddedPixels[3] = 255u;
    paddedPixels[32] = 60u; paddedPixels[33] = 50u; paddedPixels[34] = 40u; paddedPixels[35] = 255u;
    const auto padded = rawDds(4, 2, 32, {0xff0000u, 0xff00u, 0xffu, 0xff000000u},
                               paddedPixels, 0x40u, 32u);
    const auto paddedDescriptor = inspectDds(padded);
    require(paddedDescriptor.has_value(), "large-pitch raw descriptor");
    const auto paddedLevels = decodeDdsRgba(padded, *paddedDescriptor);
    require(paddedLevels[0].pixels[0] == 10u && paddedLevels[0].pixels[16u] == 40u,
            "large raw pitch decode uses authoritative row stride");

    // Match src/dds.js: a pitch more than 16 bytes above the packed row is
    // ignored for CPU decode instead of shifting subsequent rows.
    std::vector<std::uint8_t> excessivePitch(64u, 0xeeu);
    excessivePitch[0] = 10u; excessivePitch[1] = 20u; excessivePitch[2] = 30u; excessivePitch[3] = 255u;
    excessivePitch[8] = 40u; excessivePitch[9] = 50u; excessivePitch[10] = 60u; excessivePitch[11] = 255u;
    const auto ignoredPitch = rawDds(2, 2, 32, {0xffu, 0xff00u, 0xff0000u, 0xff000000u},
                                      excessivePitch, 0x40u, 32u);
    const auto ignoredPitchPixels = decodeDdsRgba(ignoredPitch)[0].pixels;
    require(ignoredPitchPixels[0] == 10u && ignoredPitchPixels[8u] == 40u,
            "excessive raw pitch follows JavaScript packed-row rule");
}

void decodesBcColorFormats() {
    const auto opaque = legacyBcDds("DXT1", {0, 0xf8u, 0xe0u, 0x07u, 0xe4u, 0, 0, 0});
    const auto pixels = decodeDdsRgba(opaque)[0].pixels;
    require(std::vector<std::uint8_t>(pixels.begin(), pixels.begin() + 16) ==
                std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 170, 85, 0, 255, 85, 170, 0, 255},
            "BC1 palette decode");
    const auto transparent = legacyBcDds("DXT1", {0, 0, 0xffu, 0xffu, 3, 0, 0, 0});
    require(decodeDdsRgba(transparent)[0].pixels[3] == 0, "BC1 transparent entry");

}

void decodesBcAlphaAndNormals() {
    const auto color = std::array<std::uint8_t, 8>{0, 0xf8u, 0xe0u, 0x07u, 0, 0, 0, 0};
    std::vector<std::uint8_t> bc2Block{0x0fu, 0, 0, 0, 0, 0, 0, 0};
    bc2Block.insert(bc2Block.end(), color.begin(), color.end());
    const auto bc2 = legacyBcDds("DXT3", bc2Block);
    require(decodeDdsRgba(bc2)[0].pixels[3] == 255, "BC2 alpha decode");
    std::vector<std::uint8_t> bc3Block{255, 0, 1, 0, 0, 0, 0, 0};
    bc3Block.insert(bc3Block.end(), color.begin(), color.end());
    const auto bc3 = legacyBcDds("DXT5", bc3Block);
    require(decodeDdsRgba(bc3)[0].pixels[3] == 0, "BC3 alpha index decode");

    const auto bc4 = legacyBcDds("ATI1", {128, 128, 0, 0, 0, 0, 0, 0});
    require(decodeDdsRgba(bc4)[0].pixels[0] == 128 && decodeDdsRgba(bc4)[0].pixels[1] == 128,
            "BC4 channel decode");
    const auto bc5 = legacyBcDds("ATI2", {128, 128, 0, 0, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0});
    const auto normal = decodeDdsRgba(bc5)[0].pixels;
    require(normal[0] == 128 && normal[1] == 128 && normal[2] == 255 && normal[3] == 255,
            "BC5 normal reconstruction");

    const auto signedEndpoints = legacyBcDds("BC4S", {0x80u, 0x7fu, 0u, 0u, 0u, 0u, 0u, 0u});
    const auto signedPixels = decodeDdsRgba(signedEndpoints)[0].pixels;
    require(signedPixels[0] == 0u, "BC4 SNORM clamps -128 endpoint to -127 like JavaScript");
}

void recognizesGpuOnlyFormatsWithoutCpuApproximation() {
    const auto bc6 = dx10Dds(4, 4, 95, std::vector<std::uint8_t>(16));
    const auto bc6Descriptor = inspectDds(bc6);
    require(bc6Descriptor.has_value() && bc6Descriptor->format == DdsFormat::BC6HUf16 &&
                bc6Descriptor->gpuRequired, "BC6H GPU capability");
    expectsParseError([&] { (void)decodeDdsRgba(bc6, *bc6Descriptor); }, "GPU_REQUIRED");
    const auto bc7 = dx10Dds(4, 4, 98, std::vector<std::uint8_t>{1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                                                                    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
    const auto bc7Descriptor = inspectDds(bc7);
    require(bc7Descriptor.has_value() && bc7Descriptor->format == DdsFormat::BC7 &&
                !bc7Descriptor->gpuRequired, "BC7 CPU capability");
    const auto bc7Pixels = decodeDdsRgba(bc7, *bc7Descriptor)[0].pixels;
    require(bc7Pixels.size() == 64u && bc7Pixels[0] == 0u && bc7Pixels[3] == 255u,
            "BC7 CPU decode");
    const auto bc7Srgb = dx10Dds(5, 3, 99, std::vector<std::uint8_t>(32u, 1u));
    const auto bc7SrgbDescriptor = inspectDds(bc7Srgb);
    require(bc7SrgbDescriptor.has_value() && bc7SrgbDescriptor->format == DdsFormat::BC7Srgb &&
                !bc7SrgbDescriptor->gpuRequired, "BC7 sRGB CPU capability");
    const auto bc7SrgbPixels = decodeDdsRgba(bc7Srgb, *bc7SrgbDescriptor)[0].pixels;
    require(bc7SrgbPixels.size() == 5u * 3u * 4u && bc7SrgbPixels[3] == 255u,
            "BC7 clipped edge decode");

    const auto rgbaSrgb = dx10Dds(1, 1, 29, {10, 20, 30, 40});
    const auto rgbaSrgbDescriptor = inspectDds(rgbaSrgb);
    require(rgbaSrgbDescriptor.has_value() && rgbaSrgbDescriptor->format == DdsFormat::Raw32 &&
                rgbaSrgbDescriptor->srgb && rgbaSrgbDescriptor->dxgi == 29u,
            "DX10 RGBA8 sRGB metadata preserved");
    for (const auto dxgi : {72u, 75u, 78u}) {
        const auto srgb = dx10Dds(4, 4, dxgi, std::vector<std::uint8_t>(16u, 1u));
        const auto descriptor = inspectDds(srgb);
        require(descriptor.has_value() && descriptor->srgb && descriptor->dxgi == dxgi,
                "DX10 BC sRGB metadata preserved");
    }
    const auto bgraSrgb = dx10Dds(1, 1, 91, {10, 20, 30, 40});
    const auto bgraDescriptor = inspectDds(bgraSrgb);
    require(bgraDescriptor.has_value() && bgraDescriptor->srgb && bgraDescriptor->dxgi == 91u,
            "DX10 BGRA8 sRGB metadata preserved");

    auto legacyFloat = std::vector<std::uint8_t>(144u, 0u);
    put32(legacyFloat, 0, 0x20534444u); put32(legacyFloat, 4, 124u); put32(legacyFloat, 12, 1u);
    put32(legacyFloat, 16, 1u); put32(legacyFloat, 28, 1u); put32(legacyFloat, 76, 32u);
    put32(legacyFloat, 80, 4u); put32(legacyFloat, 84, 116u);
    const auto legacyFloatDescriptor = inspectDds(legacyFloat);
    require(legacyFloatDescriptor.has_value() && legacyFloatDescriptor->format == DdsFormat::LegacyFloat &&
                !legacyFloatDescriptor->compressed && legacyFloatDescriptor->gpuRequired,
            "legacy float DDS remains explicit GPU_REQUIRED metadata");
    expectsParseError([&] { (void)decodeDdsRgba(legacyFloat, *legacyFloatDescriptor); }, "GPU_REQUIRED");
}

void rejectsMalformedAndTruncatedInputs() {
    const auto valid = legacyBcDds("DXT1", {0, 0xf8u, 0xe0u, 0x07u, 0, 0, 0, 0});
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::vector<std::uint8_t> prefix(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        if (length < 128u) {
            require(!inspectDds(prefix).has_value(), "truncated header accepted");
        } else {
            expectsParseError([&] { (void)decodeDdsRgba(prefix); }, "TRUNCATED");
        }
    }
    const auto validDx10 = dx10Dds(1, 1, 28, {1, 2, 3, 4});
    for (std::size_t length = 0; length < validDx10.size(); ++length) {
        const std::vector<std::uint8_t> prefix(validDx10.begin(),
                                                validDx10.begin() + static_cast<std::ptrdiff_t>(length));
        if (length < 148u) {
            require(!inspectDds(prefix).has_value(), "truncated DX10 header accepted");
        } else {
            expectsParseError([&] { (void)decodeDdsRgba(prefix); }, "TRUNCATED");
        }
    }
    auto badMipCount = valid;
    put32(badMipCount, 28, 99u);
    require(!inspectDds(badMipCount).has_value(), "excessive mip count accepted");
    auto badDimension = valid;
    put32(badDimension, 16, 0xffffffffu);
    require(!inspectDds(badDimension).has_value(), "excessive dimension accepted");
    auto badDx10Dimension = validDx10;
    put32(badDx10Dimension, 132, 1u); // D3D10_RESOURCE_DIMENSION_BUFFER
    require(!inspectDds(badDx10Dimension).has_value(), "invalid DX10 resource dimension accepted");
    for (const auto [dimension, misc, arraySize] : {
             std::array<std::uint32_t, 3>{2u, 0u, 1u}, // 1D
             std::array<std::uint32_t, 3>{3u, 0u, 2u}, // array
             std::array<std::uint32_t, 3>{3u, 0x4u, 1u}, // cube
             std::array<std::uint32_t, 3>{4u, 0u, 1u}, // 3D
         }) {
        const auto unsupportedLayout = dx10Dds(4, 4, 28, {1, 2, 3, 4}, dimension, misc, arraySize);
        const auto layoutDescriptor = inspectDds(unsupportedLayout);
        require(layoutDescriptor.has_value(), "unsupported DX10 layout remains inspectable");
        expectsParseError([&] { (void)decodeDdsRgba(unsupportedLayout, *layoutDescriptor); }, "UNSUPPORTED_LAYOUT");
    }
    auto rawShort = rawDds(4, 4, 32, {0xffu, 0xff00u, 0xff0000u, 0xff000000u}, {});
    expectsParseError([&] { (void)decodeDdsRgba(rawShort); }, "TRUNCATED");
    const auto validBc7 = dx10Dds(4, 4, 98, std::vector<std::uint8_t>{1u, 0u, 0u, 0u,
                                                                         0u, 0u, 0u, 0u,
                                                                         0u, 0u, 0u, 0u,
                                                                         0u, 0u, 0u, 0u});
    for (std::size_t length = 148u; length < validBc7.size(); ++length) {
        const std::vector<std::uint8_t> prefix(validBc7.begin(),
                                                validBc7.begin() + static_cast<std::ptrdiff_t>(length));
        expectsParseError([&] { (void)decodeDdsRgba(prefix); }, "TRUNCATED");
    }
    auto invalidBc7 = validBc7;
    invalidBc7[148u] = 0u;
    expectsParseError([&] { (void)decodeDdsRgba(invalidBc7); }, "INVALID_MODE");
    auto hugeDescriptor = *inspectDds(valid);
    hugeDescriptor.width = 32768; hugeDescriptor.height = 32768; hugeDescriptor.mipCount = 1;
    expectsParseError([&] { (void)decodeDdsRgba(valid, hugeDescriptor); }, "OUTPUT_TOO_LARGE");
    auto badRawDescriptor = *inspectDds(rawDds(1, 1, 32, {0xffu, 0xff00u, 0xff0000u, 0xff000000u}, {1, 2, 3, 4}));
    badRawDescriptor.bitsPerPixel = 16u;
    badRawDescriptor.dataOffset = 0u;
    expectsParseError([&] { (void)decodeDdsRgba(std::vector<std::uint8_t>{1, 2, 3, 4}, badRawDescriptor); },
                      "UNSUPPORTED_FORMAT");
    auto badBlockDescriptor = *inspectDds(validBc7);
    badBlockDescriptor.blockBytes = 8u;
    expectsParseError([&] { (void)decodeDdsRgba(validBc7, badBlockDescriptor); }, "UNSUPPORTED_FORMAT");
}

void checksWebglMipRule() {
    auto descriptor = *inspectDds(legacyBcDds("DXT1", std::vector<std::uint8_t>(8), 2048, 340));
    require(webglCompressedMipChainSafe(descriptor), "aligned compressed mip chain");
    descriptor.height = 337;
    require(!webglCompressedMipChainSafe(descriptor), "non-aligned compressed mip chain");
    descriptor.height = 4; descriptor.width = 4; descriptor.mipCount = 2;
    require(!webglCompressedMipChainSafe(descriptor), "partial final compressed mip chain");
}

} // namespace

int main() {
    try {
        decodesMaskedRawFormats();
        decodesBcColorFormats();
        decodesBcAlphaAndNormals();
        recognizesGpuOnlyFormatsWithoutCpuApproximation();
        rejectsMalformedAndTruncatedInputs();
        checksWebglMipRule();
        std::cout << "dds tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dds tests failed: " << error.what() << '\n';
        return 1;
    }
}
