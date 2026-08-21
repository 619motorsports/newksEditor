#include "apex/core/parse_error.hpp"
#include "apex/formats/dds.hpp"
#include "apex/render/texture_upload.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using apex::formats::DdsDescriptor;
using apex::formats::DdsFormat;
using apex::render::DdsUploadPlanResult;
using apex::render::TextureFormat;
using apex::render::TextureUploadStatus;
using namespace apex::render;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4u <= bytes.size(), "test write out of bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16u);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24u);
}

std::vector<std::uint8_t> legacyBc(std::string_view tag, std::uint32_t width,
                                   std::uint32_t height, std::uint32_t mipCount,
                                   std::size_t payloadBytes) {
    std::vector<std::uint8_t> bytes(128u + payloadBytes, 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 28, mipCount); put32(bytes, 76, 32u); put32(bytes, 80, 4u);
    require(tag.size() == 4u, "fourCC length");
    for (std::size_t index = 0; index < 4; ++index) bytes[84u + index] = static_cast<std::uint8_t>(tag[index]);
    return bytes;
}

std::vector<std::uint8_t> raw(std::uint32_t width, std::uint32_t height,
                              std::uint32_t mipCount, std::uint32_t bits,
                              std::array<std::uint32_t, 4> masks, std::size_t payloadBytes,
                              std::uint32_t pitch = 0) {
    std::vector<std::uint8_t> bytes(128u + payloadBytes, 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 20, pitch == 0 ? width * bits / 8u : pitch);
    put32(bytes, 28, mipCount); put32(bytes, 76, 32u); put32(bytes, 80, 0x40u); put32(bytes, 88, bits);
    for (std::size_t index = 0; index < masks.size(); ++index) put32(bytes, 92u + index * 4u, masks[index]);
    return bytes;
}

std::vector<std::uint8_t> dx10(std::uint32_t width, std::uint32_t height,
                               std::uint32_t dxgi, std::size_t payloadBytes) {
    std::vector<std::uint8_t> bytes(148u + payloadBytes, 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 28, 1u); put32(bytes, 76, 32u); put32(bytes, 80, 4u);
    bytes[84] = 'D'; bytes[85] = 'X'; bytes[86] = '1'; bytes[87] = '0';
    put32(bytes, 128, dxgi);       // DXGI format
    put32(bytes, 132, 3u);         // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    put32(bytes, 136, 0u);         // misc flags
    put32(bytes, 140, 1u);         // array size
    return bytes;
}

void mapsPortableAndSdkFormats() {
    const auto rgba = apex::formats::inspectDds(dx10(4, 4, 29, 64));
    require(rgba.has_value(), "DX10 RGBA8 descriptor");
    const auto rgbaMapping = mapDdsTextureFormat(*rgba);
    require(rgbaMapping.ok() && rgbaMapping.mapping->format == TextureFormat::rgba8_srgb &&
                rgbaMapping.mapping->vulkanFormat == 43u && rgbaMapping.mapping->dxgiFormat == 29u &&
                rgbaMapping.mapping->srgb, "RGBA8 sRGB mapping");

    const auto bc = apex::formats::inspectDds(dx10(4, 4, 72, 8));
    require(bc.has_value(), "DX10 BC1 descriptor");
    const auto bcMapping = mapDdsTextureFormat(*bc);
    require(bcMapping.ok() && bcMapping.mapping->format == TextureFormat::bc1_srgb &&
                bcMapping.mapping->vulkanFormat == 134u && bcMapping.mapping->dxgiFormat == 72u,
            "BC1 sRGB mapping");

    const auto bc6 = apex::formats::inspectDds(dx10(4, 4, 96, 16));
    require(bc6.has_value(), "DX10 BC6H descriptor");
    const auto bc6Mapping = mapDdsTextureFormat(*bc6);
    require(bc6Mapping.ok() && bc6Mapping.mapping->format == TextureFormat::bc6h_sfloat &&
                bc6Mapping.mapping->vulkanFormat == 144u && bc6Mapping.mapping->dxgiFormat == 96u,
            "BC6H SF16 mapping");

    const auto rgb565 = apex::formats::inspectDds(raw(4, 4, 1, 16, {0xf800u, 0x07e0u, 0x001fu, 0}, 32));
    require(rgb565.has_value(), "RGB565 descriptor");
    const auto rgb565Mapping = mapDdsTextureFormat(*rgb565);
    require(rgb565Mapping.ok() && rgb565Mapping.mapping->format == TextureFormat::r5g6b5_unorm &&
                rgb565Mapping.mapping->vulkanFormat == 4u && rgb565Mapping.mapping->dxgiFormat == 85u,
            "RGB565 mapping");

    const auto rgb24 = apex::formats::inspectDds(raw(4, 4, 1, 24, {0xff0000u, 0xff00u, 0xffu, 0}, 48));
    require(rgb24.has_value(), "RGB24 descriptor");
    require(!mapDdsTextureFormat(*rgb24).ok(), "unsupported RGB24 mapping");
}

void plansCompressedEdgesAndMips() {
    // 5x3 BC1 occupies two 4x4 blocks, including one clipped edge block.
    const auto edge = legacyBc("DXT1", 5, 3, 1, 16);
    const auto edgePlan = buildDdsUploadPlan(edge);
    require(edgePlan.ok(), "BC1 edge plan");
    require(edgePlan.plan->subresources.size() == 1 && edgePlan.plan->subresources[0].blocksWide == 2u &&
                edgePlan.plan->subresources[0].blocksHigh == 1u && edgePlan.plan->subresources[0].rowPitch == 16u &&
                edgePlan.plan->subresources[0].rowCount == 1u && edgePlan.plan->payloadBytes == 16u,
            "BC1 edge pitches");

    // 8x8 -> 4x4 -> 2x2 has 4, 1, and 1 compressed blocks respectively.
    const auto chain = legacyBc("ATI2", 8, 8, 3, 64u + 16u + 16u);
    const auto chainPlan = buildDdsUploadPlan(chain);
    require(chainPlan.ok(), "BC5 mip plan");
    require(chainPlan.plan->subresources.size() == 3 && chainPlan.plan->subresources[0].offset == 128u &&
                chainPlan.plan->subresources[0].size == 64u && chainPlan.plan->subresources[0].rowPitch == 32u &&
                chainPlan.plan->subresources[0].rowCount == 2u && chainPlan.plan->subresources[1].offset == 192u &&
                chainPlan.plan->subresources[1].size == 16u && chainPlan.plan->subresources[2].offset == 208u &&
                chainPlan.plan->subresources[2].size == 16u, "BC5 mip offsets and pitches");
}

void plansRawPitches() {
    // A legal level-0 pitch includes four bytes of row padding; later levels
    // use their tightly packed minimum pitch.
    auto bytes = raw(4, 2, 2, 32, {0xffu, 0xff00u, 0xff0000u, 0xff000000u}, 48, 20);
    const auto plan = buildDdsUploadPlan(bytes);
    require(plan.ok(), "raw mip plan");
    require(plan.plan->mapping.format == TextureFormat::rgba8_unorm && plan.plan->subresources[0].rowPitch == 20u &&
                plan.plan->subresources[0].rowCount == 2u && plan.plan->subresources[0].size == 40u &&
                plan.plan->subresources[1].width == 2u && plan.plan->subresources[1].height == 1u &&
                plan.plan->subresources[1].rowPitch == 8u && plan.plan->subresources[1].size == 8u,
            "raw pitches");

    // DDS permits more than the common 16-byte padding heuristic. The exact
    // top-level pitch must determine the next mip offset.
    auto largePadding = raw(4, 2, 2, 32, {0xffu, 0xff00u, 0xff0000u, 0xff000000u}, 72, 32);
    const auto paddedPlan = buildDdsUploadPlan(largePadding, "padded.dds");
    require(paddedPlan.ok() && paddedPlan.plan->subresources[0].rowPitch == 32u &&
                paddedPlan.plan->subresources[0].size == 64u &&
                paddedPlan.plan->subresources[1].offset == 192u,
            "large raw pitch controls mip offsets");

    const auto shortPitch = raw(4, 2, 1, 32,
                                {0xffu, 0xff00u, 0xff0000u, 0xff000000u}, 32, 15);
    const auto shortPitchPlan = buildDdsUploadPlan(shortPitch, "short-pitch.dds");
    require(shortPitchPlan.status == TextureUploadStatus::invalid &&
                shortPitchPlan.diagnostic.code == "row_pitch_mismatch",
            "raw pitch below packed row is rejected");
}

void rejectsMalformedPayloadAndLimits() {
    const auto shortBc = legacyBc("DXT1", 5, 3, 1, 8);
    const auto shortPlan = buildDdsUploadPlan(shortBc);
    require(!shortPlan.ok() && shortPlan.status == TextureUploadStatus::invalid &&
                shortPlan.diagnostic.code == "truncated_payload" && shortPlan.diagnostic.offset == 128u,
            "truncated BC edge payload");
    const auto attributed = buildDdsUploadPlan(shortBc, "cars/body.dds");
    require(attributed.diagnostic.source == "cars/body.dds" && attributed.diagnostic.offset == 128u,
            "upload diagnostic source attribution");

    auto excessiveMips = legacyBc("DXT1", 4, 4, 99, 8);
    put32(excessiveMips, 28, 99u);
    const auto mipPlan = buildDdsUploadPlan(excessiveMips);
    require(!mipPlan.ok() && mipPlan.diagnostic.code == "invalid_header", "invalid mip count");

    auto huge = legacyBc("DXT1", 32768, 32768, 1, 8);
    const auto hugePlan = buildDdsUploadPlan(huge);
    require(!hugePlan.ok() && hugePlan.diagnostic.code == "truncated_payload", "bounded huge dimensions");

    apex::core::ParseLimits limits;
    limits.maxOutputBytes = 7u;
    const auto limited = buildDdsUploadPlan(legacyBc("DXT1", 4, 4, 1, 8), "limited.dds", limits);
    require(!limited.ok() && limited.diagnostic.code == "payload_too_large", "payload output limit");

    auto malformed = legacyBc("DXT1", 4, 4, 1, 8);
    malformed.resize(100);
    const auto malformedPlan = buildDdsUploadPlan(malformed);
    require(!malformedPlan.ok() && malformedPlan.diagnostic.code == "invalid_header", "truncated DDS header");
}

void rejectsUnsupportedDx10LayoutsAndInvalidBits() {
    auto oneDimensional = dx10(4, 4, 29, 64);
    put32(oneDimensional, 132, 2u); // TEXTURE1D
    const auto oneDimensionalDescriptor = apex::formats::inspectDds(oneDimensional);
    require(oneDimensionalDescriptor.has_value(), "valid DX10 1D descriptor inspection");
    const auto oneDimensionalPlan = buildDdsUploadPlan(oneDimensional, *oneDimensionalDescriptor,
                                                       "one-dimensional.dds");
    require(oneDimensionalPlan.status == TextureUploadStatus::unsupported &&
                oneDimensionalPlan.diagnostic.code == "unsupported_layout",
            "DX10 1D upload rejection");

    auto array = dx10(4, 4, 29, 64);
    put32(array, 140, 2u);
    const auto arrayDescriptor = apex::formats::inspectDds(array);
    require(arrayDescriptor.has_value(), "valid DX10 array descriptor inspection");
    const auto arrayPlan = buildDdsUploadPlan(array, *arrayDescriptor, "array.dds");
    require(arrayPlan.status == TextureUploadStatus::unsupported &&
                arrayPlan.diagnostic.code == "unsupported_layout",
            "DX10 array upload rejection");

    auto volume = dx10(4, 4, 29, 64);
    put32(volume, 132, 4u); // TEXTURE3D
    const auto volumeDescriptor = apex::formats::inspectDds(volume);
    require(volumeDescriptor.has_value(), "valid DX10 volume descriptor inspection");
    const auto volumePlan = buildDdsUploadPlan(volume, *volumeDescriptor, "volume.dds");
    require(volumePlan.status == TextureUploadStatus::unsupported &&
                volumePlan.diagnostic.code == "unsupported_layout",
            "DX10 volume upload rejection");

    auto cube = dx10(4, 4, 29, 64);
    put32(cube, 136, 0x4u); // D3D11_RESOURCE_MISC_TEXTURECUBE
    const auto cubeDescriptor = apex::formats::inspectDds(cube);
    require(cubeDescriptor.has_value(), "valid DX10 cubemap descriptor inspection");
    const auto cubePlan = buildDdsUploadPlan(cube, *cubeDescriptor, "cube.dds");
    require(cubePlan.status == TextureUploadStatus::unsupported &&
                cubePlan.diagnostic.code == "unsupported_layout",
            "DX10 cubemap upload rejection");

    auto malformedArray = dx10(4, 4, 29, 64);
    put32(malformedArray, 140, 0u);
    require(!apex::formats::inspectDds(malformedArray).has_value(), "zero DX10 array size rejection");

    DdsDescriptor invalidBits;
    invalidBits.format = DdsFormat::Raw32;
    invalidBits.width = 4;
    invalidBits.height = 4;
    invalidBits.mipCount = 1;
    invalidBits.dataOffset = 128;
    invalidBits.bitsPerPixel = 33;
    invalidBits.masks = {0xffu, 0xff00u, 0xff0000u, 0xff000000u};
    const std::vector<std::uint8_t> payload(128u + 64u, 0);
    const auto bitPlan = buildDdsUploadPlan(payload, invalidBits, "invalid-bits.dds");
    require(bitPlan.status == TextureUploadStatus::invalid &&
                bitPlan.diagnostic.code == "pixel_size_mismatch",
            "non-byte-aligned bit depth rejection");
}

void rejectsUnsupportedWithoutApproximation() {
    DdsDescriptor unsupported;
    unsupported.format = DdsFormat::Raw24;
    unsupported.width = 4; unsupported.height = 4; unsupported.mipCount = 1;
    unsupported.dataOffset = 128; unsupported.bitsPerPixel = 24;
    unsupported.masks = {0xff0000u, 0xff00u, 0xffu, 0};
    const auto result = mapDdsTextureFormat(unsupported);
    require(!result.ok() && result.diagnostic.code == "unsupported_format", "explicit unsupported diagnostic");

    auto bc7 = apex::formats::inspectDds(dx10(4, 4, 99, 16));
    require(bc7.has_value(), "BC7 descriptor");
    const auto plan = buildDdsUploadPlan(dx10(4, 4, 99, 16), *bc7);
    require(plan.ok() && plan.plan->mapping.format == TextureFormat::bc7_srgb &&
                plan.plan->mapping.vulkanFormat == 146u && plan.plan->mapping.dxgiFormat == 99u,
            "BC7 GPU plan without CPU approximation");
}

} // namespace

int main() {
    try {
        mapsPortableAndSdkFormats();
        plansCompressedEdgesAndMips();
        plansRawPitches();
        rejectsMalformedPayloadAndLimits();
        rejectsUnsupportedDx10LayoutsAndInvalidBits();
        rejectsUnsupportedWithoutApproximation();
        std::cout << "texture upload tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "texture upload tests failed: " << error.what() << '\n';
        return 1;
    }
}
