#include "apex/core/parse_error.hpp"
#include "apex/formats/dds.hpp"
#include "apex/render/decoded_dds_texture.hpp"
#include "apex/render/texture_upload.hpp"
#include "png_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
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
                               std::uint32_t dxgi, std::size_t payloadBytes,
                               std::uint32_t mipCount = 1u,
                               std::uint32_t resourceDimension = 3u,
                               std::uint32_t miscFlags = 0u,
                               std::uint32_t arraySize = 1u) {
    std::vector<std::uint8_t> bytes(148u + payloadBytes, 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 28, mipCount); put32(bytes, 76, 32u); put32(bytes, 80, 4u);
    bytes[84] = 'D'; bytes[85] = 'X'; bytes[86] = '1'; bytes[87] = '0';
    put32(bytes, 128, dxgi);       // DXGI format
    put32(bytes, 132, resourceDimension);
    put32(bytes, 136, miscFlags);
    put32(bytes, 140, arraySize);
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
    const auto rgb24Mapping = mapDdsTextureFormat(*rgb24);
    require(rgb24Mapping.ok() && rgb24Mapping.mapping->format == TextureFormat::rgba8_unorm &&
                rgb24Mapping.mapping->cpuConversion && rgb24Mapping.mapping->sourceBytesPerPixel == 3u,
            "RGB24 conversion mapping");

    auto rgb24Bytes = raw(2, 1, 1, 24, {0xff0000u, 0xff00u, 0xffu, 0}, 6u);
    rgb24Bytes[128] = 30u; rgb24Bytes[129] = 20u; rgb24Bytes[130] = 10u;
    rgb24Bytes[131] = 60u; rgb24Bytes[132] = 50u; rgb24Bytes[133] = 40u;
    const auto rgb24Plan = buildDdsUploadPlan(rgb24Bytes, "rgb24.dds");
    require(rgb24Plan.ok() && rgb24Plan.plan->convertedPayload ==
                std::vector<std::uint8_t>{10u, 20u, 30u, 255u, 40u, 50u, 60u, 255u} &&
                rgb24Plan.plan->subresources[0].convertedRowPitch == 8u &&
                rgb24Plan.plan->subresources[0].convertedSize == 8u,
            "exact RGB24-to-RGBA8 conversion");
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

    // BC7 uses the same 4x4 block footprint with a 16-byte block. Keep a
    // clipped edge level in the planner contract so its row sizing is not
    // accidentally coupled to BC1/BC3.
    const auto bc7Edge = dx10(5, 3, 98, 32);
    const auto bc7EdgePlan = buildDdsUploadPlan(bc7Edge, "bc7-edge.dds");
    require(bc7EdgePlan.ok() && bc7EdgePlan.plan->mapping.format == TextureFormat::bc7_unorm &&
                bc7EdgePlan.plan->subresources.size() == 1U &&
                bc7EdgePlan.plan->subresources[0].blocksWide == 2U &&
                bc7EdgePlan.plan->subresources[0].blocksHigh == 1U &&
                bc7EdgePlan.plan->subresources[0].rowPitch == 32U &&
                bc7EdgePlan.plan->subresources[0].size == 32U,
            "BC7 edge pitches");

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

    auto excessivePadding = raw(4, 2, 2, 32, {0xffu, 0xff00u, 0xff0000u, 0xff000000u}, 56, 40);
    const auto packedPlan = buildDdsUploadPlan(excessivePadding, "packed-pitch.dds");
    require(packedPlan.ok() && packedPlan.plan->subresources[0].rowPitch == 16u &&
                packedPlan.plan->subresources[0].size == 32u &&
                packedPlan.plan->subresources[1].offset == 160u,
            "excessive raw pitch follows JavaScript packed-row rule");

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
    const auto shortBc7 = dx10(5, 3, 98, 16);
    const auto shortBc7Plan = buildDdsUploadPlan(shortBc7, "bc7-short.dds");
    require(!shortBc7Plan.ok() && shortBc7Plan.status == TextureUploadStatus::invalid &&
                shortBc7Plan.diagnostic.code == "truncated_payload" &&
                shortBc7Plan.diagnostic.offset == 148U,
            "truncated BC7 edge payload");
    const auto attributed = buildDdsUploadPlan(shortBc, "cars/body.dds");
    require(attributed.diagnostic.source == "cars/body.dds" && attributed.diagnostic.offset == 128u,
            "upload diagnostic source attribution");

    const auto shortRgb24 = raw(2, 1, 1, 24, {0xff0000u, 0xff00u, 0xffu, 0}, 5u);
    const auto shortRgb24Plan = buildDdsUploadPlan(shortRgb24, "short-rgb24.dds");
    require(!shortRgb24Plan.ok() && !shortRgb24Plan.plan.has_value() &&
                shortRgb24Plan.diagnostic.code == "truncated_payload" &&
                shortRgb24Plan.diagnostic.offset == 128u,
            "truncated RGB24 conversion has no partial plan");

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

    apex::core::ParseLimits convertedLimits;
    convertedLimits.maxOutputBytes = 15u;
    const auto convertedLimited = buildDdsUploadPlan(
        raw(2, 2, 1, 24, {0xff0000u, 0xff00u, 0xffu, 0}, 12u),
        "converted-limited.dds", convertedLimits);
    require(!convertedLimited.ok() && !convertedLimited.plan.has_value() &&
                convertedLimited.diagnostic.code == "payload_too_large",
            "converted payload aggregate limit has no partial plan");

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

    auto array = dx10(2, 1, 29, 24u, 2u, 3u, 0u, 2u);
    for (std::size_t index = 0; index < 24u; ++index) array[148u + index] = static_cast<std::uint8_t>(index);
    const auto arrayDescriptor = apex::formats::inspectDds(array);
    require(arrayDescriptor.has_value(), "valid DX10 array descriptor inspection");
    const auto arrayPlan = buildDdsUploadPlan(array, *arrayDescriptor, "array.dds");
    require(arrayPlan.ok() && arrayPlan.plan->subresources.size() == 4u &&
                arrayPlan.plan->subresources[0].arrayLayer == 0u &&
                arrayPlan.plan->subresources[0].mipLevel == 0u &&
                arrayPlan.plan->subresources[0].offset == 148u &&
                arrayPlan.plan->subresources[1].arrayLayer == 0u &&
                arrayPlan.plan->subresources[1].mipLevel == 1u &&
                arrayPlan.plan->subresources[1].offset == 156u &&
                arrayPlan.plan->subresources[2].arrayLayer == 1u &&
                arrayPlan.plan->subresources[2].mipLevel == 0u &&
                arrayPlan.plan->subresources[2].offset == 160u,
            "DX10 array subresource ordering and offsets");

    auto truncatedArray = dx10(2, 1, 29, 20u, 2u, 3u, 0u, 2u);
    const auto truncatedArrayDescriptor = apex::formats::inspectDds(truncatedArray);
    require(truncatedArrayDescriptor.has_value(), "truncated array descriptor inspection");
    const auto truncatedArrayPlan = buildDdsUploadPlan(truncatedArray, *truncatedArrayDescriptor,
                                                       "truncated-array.dds");
    require(!truncatedArrayPlan.ok() && !truncatedArrayPlan.plan.has_value() &&
                truncatedArrayPlan.diagnostic.code == "truncated_payload",
            "truncated DX10 array has no partial plan");

    auto volume = dx10(4, 4, 29, 64);
    put32(volume, 132, 4u); // TEXTURE3D
    const auto volumeDescriptor = apex::formats::inspectDds(volume);
    require(volumeDescriptor.has_value(), "valid DX10 volume descriptor inspection");
    const auto volumePlan = buildDdsUploadPlan(volume, *volumeDescriptor, "volume.dds");
    require(volumePlan.status == TextureUploadStatus::unsupported &&
                volumePlan.diagnostic.code == "unsupported_layout",
            "DX10 volume upload rejection");

    auto cube = dx10(1, 1, 29, 24u, 1u, 3u, 0x4u, 1u); // D3D11_RESOURCE_MISC_TEXTURECUBE
    for (std::size_t index = 0; index < 24u; ++index) cube[148u + index] = static_cast<std::uint8_t>(index);
    const auto cubeDescriptor = apex::formats::inspectDds(cube);
    require(cubeDescriptor.has_value(), "valid DX10 cubemap descriptor inspection");
    const auto cubePlan = buildDdsUploadPlan(cube, *cubeDescriptor, "cube.dds");
    require(cubePlan.ok() && cubePlan.plan->subresources.size() == 6u &&
                cubePlan.plan->subresources[0].cubeFace == 0u &&
                cubePlan.plan->subresources[0].offset == 148u &&
                cubePlan.plan->subresources[5].cubeFace == 5u &&
                cubePlan.plan->subresources[5].offset == 168u,
            "DX10 cubemap face subresources");

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

    const auto bc6 = apex::formats::inspectDds(dx10(4, 4, 95, 16u));
    require(bc6.has_value(), "BC6H descriptor for GPU rejection");
    const auto bc6Plan = buildDdsUploadPlan(dx10(4, 4, 95, 16u), *bc6, "bc6h.dds");
    require(bc6Plan.status == TextureUploadStatus::unsupported &&
                bc6Plan.diagnostic.code == "gpu_required" && !bc6Plan.plan.has_value(),
            "BC6H upload remains explicitly GPU-required");
}

void rejectsUnsupportedWithoutApproximation() {
    DdsDescriptor unsupported;
    unsupported.format = DdsFormat::Raw24;
    unsupported.width = 4; unsupported.height = 4; unsupported.mipCount = 1;
    unsupported.dataOffset = 128; unsupported.bitsPerPixel = 24;
    unsupported.masks = {0xff0000u, 0xff00u, 0xffu, 0};
    const auto result = mapDdsTextureFormat(unsupported);
    require(result.ok() && result.mapping->cpuConversion && result.mapping->sourceBytesPerPixel == 3u,
            "raw 24-bit mapping uses an explicit conversion path");

    auto bc7 = apex::formats::inspectDds(dx10(4, 4, 99, 16));
    require(bc7.has_value(), "BC7 descriptor");
    const auto plan = buildDdsUploadPlan(dx10(4, 4, 99, 16), *bc7);
    require(plan.ok() && plan.plan->mapping.format == TextureFormat::bc7_srgb &&
                plan.plan->mapping.vulkanFormat == 146u && plan.plan->mapping.dxgiFormat == 99u,
            "BC7 GPU plan without CPU approximation");
}

void plansPortableDecodedTextureResources() {
    auto srgb = dx10(2u, 1u, 29u, 8u);
    const std::array<std::uint8_t, 8> pixels = {
        7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u};
    std::copy(pixels.begin(), pixels.end(), srgb.begin() + 148);
    const auto decoded = plan_decoded_dds_texture(srgb, "srgb.dds");
    require(decoded.ok() && decoded.plan.description.width == 2U &&
                decoded.plan.description.height == 1U &&
                decoded.plan.description.mip_levels == 1U &&
                decoded.plan.description.format == TextureFormat::rgba8_srgb &&
                decoded.plan.description.usage == TextureUsage::sampled &&
                decoded.plan.levels.front().pixels ==
                    std::vector<std::uint8_t>(pixels.begin(), pixels.end()),
            "decoded DDS plan preserves sRGB metadata and texels");
    const TextureUploadPlan uploads = decoded.plan.make_upload_plan();
    require(uploads.subresources.size() == 1U &&
                uploads.subresources.front().row_pitch == 8U &&
                uploads.subresources.front().data.size() == 8U,
            "decoded DDS plan exposes a borrowing texture upload");

    auto bc1 = legacyBc("DXT1", 4U, 4U, 1U, 8U);
    bc1[128] = 0x00U;
    bc1[129] = 0xf8U;
    const auto decoded_bc1 = plan_decoded_dds_texture(bc1, "bc1.dds");
    require(decoded_bc1.ok() &&
                decoded_bc1.plan.description.format == TextureFormat::rgba8_unorm &&
                decoded_bc1.plan.levels.front().pixels.size() == 64U &&
                decoded_bc1.plan.levels.front().pixels[0] == 255U,
            "BC1 uses the exact bounded RGBA8 CPU fallback");

    auto truncated = srgb;
    truncated.pop_back();
    const auto short_plan =
        plan_decoded_dds_texture(truncated, "truncated.dds");
    require(!short_plan.ok() &&
                short_plan.status == TextureUploadStatus::invalid &&
                short_plan.diagnostic.code == "truncated" &&
                short_plan.diagnostic.source == "truncated.dds",
            "decoded DDS plan rejects a truncated mip with source attribution");

    auto array = dx10(1U, 1U, 29U, 8U, 1U, 3U, 0U, 2U);
    const auto array_plan = plan_decoded_dds_texture(array, "array.dds");
    require(!array_plan.ok() &&
                array_plan.status == TextureUploadStatus::unsupported &&
                array_plan.diagnostic.code == "unsupported_layout",
            "decoded DDS plan rejects arrays without flattening them");

    const auto bc6 = plan_decoded_dds_texture(
        dx10(4U, 4U, 95U, 16U), "bc6h.dds");
    require(!bc6.ok() && bc6.status == TextureUploadStatus::unsupported &&
                bc6.diagnostic.code == "gpu_required",
            "decoded DDS plan preserves the explicit BC6H GPU boundary");

    const std::array<std::uint8_t, 4> png_pixel = {3U, 29U, 101U, 211U};
    auto png = apex::tests::rgba8PngFixture(png_pixel);
    const auto decoded_png =
        plan_decoded_texture_payload(png, "misleading.dds");
    require(decoded_png.ok() &&
                decoded_png.plan.description.width == 1U &&
                decoded_png.plan.description.height == 1U &&
                decoded_png.plan.description.mip_levels == 1U &&
                decoded_png.plan.description.format == TextureFormat::rgba8_unorm &&
                decoded_png.plan.levels.front().pixels ==
                    std::vector<std::uint8_t>(png_pixel.begin(), png_pixel.end()),
            "generic texture planner identifies PNG bytes and preserves RGBA texels");

    png.pop_back();
    const auto truncated_png =
        plan_decoded_texture_payload(png, "truncated.png");
    require(!truncated_png.ok() &&
                truncated_png.status == TextureUploadStatus::invalid &&
                truncated_png.diagnostic.code == "truncated" &&
                truncated_png.diagnostic.source == "truncated.png",
            "generic texture planner rejects truncated PNG before upload");

    apex::core::ParseLimits png_limits;
    png_limits.maxOutputBytes = 3U;
    const auto limited_png = plan_decoded_texture_payload(
        apex::tests::rgba8PngFixture(png_pixel), "limited.png", png_limits);
    require(!limited_png.ok() &&
                limited_png.diagnostic.code == "output_too_large",
            "generic texture planner applies the decoded-output limit to PNG");
}

void rejectsOversizedUploadSubresourceLists() {
    const std::array<std::byte, 4> pixel = {
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    TextureDescription description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    TextureUploadPlan uploads;
    uploads.subresources.resize(65536U);
    uploads.subresources.front() = {0U, 0U, 1U, 1U, 4U, pixel};
    Diagnostic diagnostic;
    require(validate_texture_upload_plan(description, uploads, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_upload_subresource_count",
            "an oversized upload list is rejected before iteration");
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
        plansPortableDecodedTextureResources();
        rejectsOversizedUploadSubresourceLists();
        std::cout << "texture upload tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "texture upload tests failed: " << error.what() << '\n';
        return 1;
    }
}
