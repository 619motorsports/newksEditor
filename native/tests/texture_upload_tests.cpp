#include "apex/core/parse_error.hpp"
#include "apex/formats/dds.hpp"
#include "apex/render/decoded_dds_texture.hpp"
#include "apex/render/texture_upload.hpp"
#include "png_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::vector<std::uint8_t> legacyFloat(std::uint32_t fourCC, std::uint32_t width,
                                      std::uint32_t height, std::size_t payloadBytes,
                                      std::uint32_t pitch = 0u) {
    std::vector<std::uint8_t> bytes(128u + payloadBytes, 0);
    put32(bytes, 0, 0x20534444u); put32(bytes, 4, 124u); put32(bytes, 12, height);
    put32(bytes, 16, width); put32(bytes, 20, pitch == 0u ? width * 4u : pitch);
    put32(bytes, 28, 1u); put32(bytes, 76, 32u); put32(bytes, 80, 4u);
    put32(bytes, 84, fourCC);
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

    const auto bc2 = apex::formats::inspectDds(dx10(4, 4, 74, 16));
    require(bc2.has_value(), "DX10 BC2 descriptor");
    const auto bc2Mapping = mapDdsTextureFormat(*bc2);
    require(bc2Mapping.ok() && bc2Mapping.mapping->format == TextureFormat::bc2_unorm &&
                bc2Mapping.mapping->vulkanFormat == 135u && bc2Mapping.mapping->dxgiFormat == 74u,
            "BC2 mapping");

    const auto bc2Srgb = apex::formats::inspectDds(dx10(4, 4, 75, 16));
    require(bc2Srgb.has_value(), "DX10 BC2 sRGB descriptor");
    const auto bc2SrgbMapping = mapDdsTextureFormat(*bc2Srgb);
    require(bc2SrgbMapping.ok() && bc2SrgbMapping.mapping->format == TextureFormat::bc2_srgb &&
                bc2SrgbMapping.mapping->vulkanFormat == 136u &&
                bc2SrgbMapping.mapping->dxgiFormat == 75u && bc2SrgbMapping.mapping->srgb,
            "BC2 sRGB mapping");

    const auto bc5Snorm = apex::formats::inspectDds(dx10(4, 4, 84, 16));
    require(bc5Snorm.has_value(), "DX10 BC5 SNORM descriptor");
    const auto bc5SnormMapping = mapDdsTextureFormat(*bc5Snorm);
    require(bc5SnormMapping.ok() &&
                bc5SnormMapping.mapping->format == TextureFormat::bc5_snorm &&
                bc5SnormMapping.mapping->vulkanFormat == 142U &&
                bc5SnormMapping.mapping->dxgiFormat == 84U &&
                bc5SnormMapping.mapping->signedChannels,
            "BC5 SNORM mapping");

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

void plansLegacyR32FWithoutApproximation() {
    auto r32f = legacyFloat(114u, 5u, 3u, 72u, 24u);
    // Keep non-finite bit patterns byte-for-byte. The upload planner validates
    // layout only and must not invent a scalar sanitization policy.
    r32f[128u] = 0x00u; r32f[129u] = 0x00u;
    r32f[130u] = 0x80u; r32f[131u] = 0x7fu;
    const auto descriptor = apex::formats::inspectDds(r32f);
    require(descriptor.has_value() && descriptor->format == DdsFormat::LegacyFloat &&
                descriptor->legacyFourCC == 114u && descriptor->bitsPerPixel == 32u &&
                descriptor->gpuRequired,
            "legacy R32F inspection");
    const auto mapping = mapDdsTextureFormat(*descriptor);
    require(mapping.ok() && mapping.mapping->format == TextureFormat::r32_sfloat &&
                mapping.mapping->vulkanFormat == 100u &&
                mapping.mapping->dxgiFormat == 41u &&
                mapping.mapping->signedChannels && !mapping.mapping->cpuConversion,
            "legacy R32F exact native mapping");
    const auto plan = buildDdsUploadPlan(r32f, *descriptor, "legacy-r32f.dds");
    require(plan.ok() && plan.plan->subresources.size() == 1u &&
                plan.plan->subresources.front().offset == 128u &&
                plan.plan->subresources.front().rowPitch == 24u &&
                plan.plan->subresources.front().rowCount == 3u &&
                plan.plan->subresources.front().size == 72u &&
                plan.plan->payloadBytes == 72u && plan.plan->convertedPayload.empty(),
            "legacy R32F padded-row upload plan");

    const auto truncated = legacyFloat(114u, 5u, 3u, 71u, 24u);
    const auto truncatedPlan = buildDdsUploadPlan(truncated, "truncated-r32f.dds");
    require(!truncatedPlan.ok() && !truncatedPlan.plan.has_value() &&
                truncatedPlan.status == TextureUploadStatus::invalid &&
                truncatedPlan.diagnostic.code == "truncated_payload" &&
                truncatedPlan.diagnostic.offset == 128u &&
                truncatedPlan.diagnostic.source == "truncated-r32f.dds",
            "truncated legacy R32F payload rejection");

    for (const std::uint32_t unsupportedFourCC : {111u, 112u, 113u, 115u, 116u}) {
        const auto other = apex::formats::inspectDds(
            legacyFloat(unsupportedFourCC, 1u, 1u, 16u));
        require(other.has_value() && !mapDdsTextureFormat(*other).ok(),
                "neighboring legacy float layout remains unsupported");
    }
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

    const auto bc2Edge = dx10(5, 3, 74, 32);
    const auto bc2EdgePlan = buildDdsUploadPlan(bc2Edge, "bc2-edge.dds");
    require(bc2EdgePlan.ok() && bc2EdgePlan.plan->mapping.format == TextureFormat::bc2_unorm &&
                bc2EdgePlan.plan->subresources.size() == 1U &&
                bc2EdgePlan.plan->subresources[0].blocksWide == 2U &&
                bc2EdgePlan.plan->subresources[0].blocksHigh == 1U &&
                bc2EdgePlan.plan->subresources[0].rowPitch == 32U &&
                bc2EdgePlan.plan->subresources[0].size == 32U,
            "BC2 edge pitches");

    // 8x8 -> 4x4 -> 2x2 has 4, 1, and 1 compressed blocks respectively.
    const auto chain = legacyBc("ATI2", 8, 8, 3, 64u + 16u + 16u);
    const auto chainPlan = buildDdsUploadPlan(chain);
    require(chainPlan.ok(), "BC5 mip plan");
    require(chainPlan.plan->subresources.size() == 3 && chainPlan.plan->subresources[0].offset == 128u &&
                chainPlan.plan->subresources[0].size == 64u && chainPlan.plan->subresources[0].rowPitch == 32u &&
                chainPlan.plan->subresources[0].rowCount == 2u && chainPlan.plan->subresources[1].offset == 192u &&
                chainPlan.plan->subresources[1].size == 16u && chainPlan.plan->subresources[2].offset == 208u &&
                chainPlan.plan->subresources[2].size == 16u, "BC5 mip offsets and pitches");

    const auto bc4Edge = dx10(5, 3, 80, 16);
    const auto bc4EdgePlan = buildDdsUploadPlan(bc4Edge, "bc4-edge.dds");
    require(bc4EdgePlan.ok() && bc4EdgePlan.plan->mapping.format == TextureFormat::bc4_unorm &&
                bc4EdgePlan.plan->subresources.size() == 1U &&
                bc4EdgePlan.plan->subresources[0].blocksWide == 2U &&
                bc4EdgePlan.plan->subresources[0].blocksHigh == 1U &&
                bc4EdgePlan.plan->subresources[0].rowPitch == 16U &&
                bc4EdgePlan.plan->subresources[0].size == 16U,
            "BC4 edge pitches");

    const auto bc5SnormEdge = dx10(5, 3, 84, 32);
    const auto bc5SnormEdgePlan =
        buildDdsUploadPlan(bc5SnormEdge, "bc5-snorm-edge.dds");
    require(bc5SnormEdgePlan.ok() &&
                bc5SnormEdgePlan.plan->mapping.format == TextureFormat::bc5_snorm &&
                bc5SnormEdgePlan.plan->subresources.size() == 1U &&
                bc5SnormEdgePlan.plan->subresources[0].blocksWide == 2U &&
                bc5SnormEdgePlan.plan->subresources[0].blocksHigh == 1U &&
                bc5SnormEdgePlan.plan->subresources[0].rowPitch == 32U &&
                bc5SnormEdgePlan.plan->subresources[0].size == 32U,
            "BC5 SNORM edge pitches");

    const auto bc6Edge = dx10(5, 3, 95, 32);
    const auto bc6EdgePlan = buildDdsUploadPlan(bc6Edge, "bc6h-edge.dds");
    require(bc6EdgePlan.ok() && bc6EdgePlan.plan->mapping.format == TextureFormat::bc6h_ufloat &&
                bc6EdgePlan.plan->subresources.size() == 1U &&
                bc6EdgePlan.plan->subresources[0].blocksWide == 2U &&
                bc6EdgePlan.plan->subresources[0].blocksHigh == 1U &&
                bc6EdgePlan.plan->subresources[0].rowPitch == 32U &&
                bc6EdgePlan.plan->subresources[0].size == 32U,
            "BC6H edge pitches");
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
    const auto shortBc2 = dx10(5, 3, 74, 16);
    const auto shortBc2Plan = buildDdsUploadPlan(shortBc2, "bc2-short.dds");
    require(!shortBc2Plan.ok() && shortBc2Plan.status == TextureUploadStatus::invalid &&
                shortBc2Plan.diagnostic.code == "truncated_payload" &&
                shortBc2Plan.diagnostic.offset == 148U,
            "truncated BC2 edge payload");
    const auto shortBc5 = dx10(5, 3, 83, 16);
    const auto shortBc5Plan = buildDdsUploadPlan(shortBc5, "bc5-short.dds");
    require(!shortBc5Plan.ok() && shortBc5Plan.status == TextureUploadStatus::invalid &&
                shortBc5Plan.diagnostic.code == "truncated_payload" &&
                shortBc5Plan.diagnostic.offset == 148U,
            "truncated BC5 edge payload");
    const auto shortBc5Snorm = dx10(5, 3, 84, 16);
    const auto shortBc5SnormPlan =
        buildDdsUploadPlan(shortBc5Snorm, "bc5-snorm-short.dds");
    require(!shortBc5SnormPlan.ok() &&
                shortBc5SnormPlan.status == TextureUploadStatus::invalid &&
                shortBc5SnormPlan.diagnostic.code == "truncated_payload" &&
                shortBc5SnormPlan.diagnostic.offset == 148U,
            "truncated BC5 SNORM edge payload");
    const auto shortBc4 = dx10(5, 3, 80, 8);
    const auto shortBc4Plan = buildDdsUploadPlan(shortBc4, "bc4-short.dds");
    require(!shortBc4Plan.ok() && shortBc4Plan.status == TextureUploadStatus::invalid &&
                shortBc4Plan.diagnostic.code == "truncated_payload" &&
                shortBc4Plan.diagnostic.offset == 148U,
            "truncated BC4 edge payload");
    const auto shortBc6 = dx10(5, 3, 95, 16);
    const auto shortBc6Plan = buildDdsUploadPlan(shortBc6, "bc6h-short.dds");
    require(!shortBc6Plan.ok() && shortBc6Plan.status == TextureUploadStatus::invalid &&
                shortBc6Plan.diagnostic.code == "truncated_payload" &&
                shortBc6Plan.diagnostic.offset == 148U,
            "truncated BC6H edge payload");
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
    require(bc6.has_value(), "BC6H descriptor for direct upload");
    const auto bc6Plan = buildDdsUploadPlan(dx10(4, 4, 95, 16u), *bc6, "bc6h.dds");
    require(bc6Plan.ok() && bc6Plan.plan->mapping.format == TextureFormat::bc6h_ufloat &&
                bc6Plan.plan->subresources.size() == 1U &&
                bc6Plan.plan->subresources.front().rowPitch == 16U &&
                bc6Plan.plan->subresources.front().size == 16U,
            "BC6H direct upload plan");
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

std::vector<std::uint8_t> readRepositoryJpeg() {
    constexpr std::array<std::string_view, 3> roots = {
        "test/content", "../test/content", "../../test/content"};
    for (const auto root : roots) {
        std::ifstream file(std::string(root) +
                               "/cars/619_gen6_arca_base/skins/default/preview.jpg",
                           std::ios::binary | std::ios::ate);
        if (!file) continue;
        const auto end = file.tellg();
        if (end < 0) continue;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (file.good() || file.eof()) return bytes;
    }
    return {};
}

void plansPortableJpegResources() {
    const auto jpeg = readRepositoryJpeg();
    if (jpeg.empty()) return;
    const auto decoded = plan_decoded_texture_payload(jpeg, "preview.jpg");
    if (!decoded.ok()) {
        require(decoded.status == TextureUploadStatus::unsupported,
                "JPEG decode failure is explicit when the optional decoder is unavailable");
        return;
    }
    require(decoded.plan.description.width == 1022U &&
                decoded.plan.description.height == 575U &&
                decoded.plan.levels.size() == 1U &&
                decoded.plan.levels.front().pixels.size() == 1022U * 575U * 4U,
            "restored LFS JPEG decodes to bounded RGBA8 pixels");

    for (const auto length : {std::size_t{0U}, std::size_t{1U}, std::size_t{2U},
                              std::size_t{3U}, jpeg.size() - 1U}) {
        const auto truncated = std::span<const std::uint8_t>(jpeg.data(), length);
        const auto result = plan_decoded_texture_payload(truncated, "truncated.jpg");
        require(!result.ok() &&
                    (result.status == TextureUploadStatus::invalid ||
                     result.status == TextureUploadStatus::unsupported),
                "truncated JPEG is rejected without a partial plan");
    }
    apex::core::ParseLimits limits;
    limits.maxOutputBytes = 1024U;
    const auto limited = plan_decoded_texture_payload(jpeg, "limited.jpg", limits);
    require(!limited.ok() && limited.diagnostic.code == "output_too_large",
            "JPEG decoded-output budget is enforced before allocation");
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

void validatesTextureAccessPolicies() {
    constexpr TextureUsage render_then_sample_usage =
        TextureUsage::sampled | TextureUsage::color_attachment |
        TextureUsage::transfer_source;
    TextureDescription render_then_sample;
    render_then_sample.width = 4U;
    render_then_sample.height = 4U;
    render_then_sample.format = TextureFormat::rgba8_unorm;
    render_then_sample.usage = render_then_sample_usage;
    render_then_sample.mutability = TextureMutability::mutable_data;
    render_then_sample.access_policy = TextureAccessPolicy::render_then_sample;

    Diagnostic diagnostic;
    require(validate_texture_description(render_then_sample, {}, diagnostic) ==
                TextureStatus::ready,
            "render-then-sample accepts its exact mutable uncompressed contract");
    require(validate_texture_upload_plan(render_then_sample, {}, diagnostic) ==
                TextureStatus::ready,
            "render-then-sample upload validation accepts its exact contract");
    require(validate_texture_mip_generation_description(
                render_then_sample, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_mip_generation_levels_invalid",
            "mip generation rejects a one-level render target");
    TextureDescription generated_chain = render_then_sample;
    generated_chain.mip_levels = 3U;
    require(validate_texture_mip_generation_description(
                generated_chain, diagnostic) == TextureStatus::ready,
            "mip generation accepts a complete four-pixel chain");
    generated_chain.mip_levels = 4U;
    require(validate_texture_mip_generation_description(
                generated_chain, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code ==
                    "texture_mip_generation_levels_excessive",
            "mip generation rejects levels beyond the full dimension chain");
    generated_chain.mip_levels = 3U;
    generated_chain.access_policy = TextureAccessPolicy::fixed_usage;
    require(validate_texture_mip_generation_description(
                generated_chain, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code ==
                    "texture_mip_generation_access_policy_invalid",
            "mip generation rejects fixed-state textures");

    const TextureFormatInfo rgba16 =
        texture_format_info(TextureFormat::rgba16_sfloat);
    require(rgba16.classification == TextureFormatClass::uncompressed &&
                rgba16.bytes_per_pixel == 8U && !rgba16.srgb &&
                rgba16.signed_channels &&
                texture_format_bytes_per_pixel(TextureFormat::rgba16_sfloat) == 8U &&
                texture_format_cpu_upload_supported(TextureFormat::rgba16_sfloat) &&
                textureFormatName(TextureFormat::rgba16_sfloat) ==
                    std::string_view("RGBA16_SFLOAT"),
            "RGBA16F has an exact eight-byte signed-float texture contract");
    TextureDescription rgba16_target = render_then_sample;
    rgba16_target.format = TextureFormat::rgba16_sfloat;
    require(validate_texture_description(rgba16_target, {}, diagnostic) ==
                TextureStatus::ready,
            "render-then-sample accepts the recovered RGBA16F capture format");
    const std::array<std::byte, 128U> rgba16_pixels{};
    TextureUploadPlan rgba16_upload;
    rgba16_upload.subresources.push_back(
        {0U, 0U, 4U, 4U, 32U, rgba16_pixels});
    require(validate_texture_upload_plan(rgba16_target, rgba16_upload,
                                         diagnostic) == TextureStatus::ready,
            "RGBA16F upload validation uses an eight-byte texel pitch");
    rgba16_upload.subresources.front().data =
        std::span<const std::byte>(rgba16_pixels.data(),
                                  rgba16_pixels.size() - 1U);
    require(validate_texture_upload_plan(rgba16_target, rgba16_upload,
                                         diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_upload_truncated",
            "a truncated RGBA16F upload is rejected without partial acceptance");

    TextureDescription legacy{
        4U, 4U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    require(legacy.access_policy == TextureAccessPolicy::fixed_usage &&
                validate_texture_description(legacy, {}, diagnostic) ==
                    TextureStatus::ready,
            "the appended access policy preserves the fixed-usage default");

    TextureDescription unknown_policy = render_then_sample;
    unknown_policy.access_policy =
        static_cast<TextureAccessPolicy>(0xffU);
    require(validate_texture_description(unknown_policy, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_unknown",
            "an unknown texture access policy is rejected with a stable diagnostic");
    require(validate_texture_upload_plan(unknown_policy, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_unknown",
            "upload validation rejects an unknown texture access policy");

    TextureDescription invalid_usage = render_then_sample;
    invalid_usage.usage = TextureUsage::sampled | TextureUsage::color_attachment;
    require(validate_texture_description(invalid_usage, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_usage_invalid",
            "render-then-sample rejects missing transfer-source usage");
    invalid_usage.usage = render_then_sample_usage | TextureUsage::storage;
    require(validate_texture_upload_plan(invalid_usage, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_usage_invalid",
            "render-then-sample rejects extra usage bits");

    TextureDescription invalid_mutability = render_then_sample;
    invalid_mutability.mutability = TextureMutability::immutable;
    require(validate_texture_description(invalid_mutability, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_mutability_invalid",
            "render-then-sample requires mutable data");

    TextureDescription invalid_samples = render_then_sample;
    invalid_samples.samples = 4U;
    require(validate_texture_upload_plan(invalid_samples, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_samples_invalid",
            "render-then-sample requires one sample");

    TextureDescription compressed = render_then_sample;
    compressed.format = TextureFormat::bc1_unorm;
    require(validate_texture_description(compressed, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_format_invalid",
            "render-then-sample rejects compressed formats");

    TextureDescription unknown_format = render_then_sample;
    unknown_format.format = TextureFormat::unknown;
    require(validate_texture_upload_plan(unknown_format, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_access_policy_format_invalid",
            "render-then-sample rejects unknown formats");
}

void validatesExplicitCubeTextureContracts() {
    constexpr std::array<CubeFace, texture_cube_face_count> faces = {
        CubeFace::positive_x, CubeFace::negative_x, CubeFace::positive_y,
        CubeFace::negative_y, CubeFace::positive_z, CubeFace::negative_z};
    const std::array<std::byte, 16U> rgba = {};

    TextureDescription cube;
    cube.width = 2U;
    cube.height = 2U;
    cube.format = TextureFormat::rgba8_unorm;
    cube.usage = TextureUsage::sampled;
    cube.shape = TextureShape::texture_cube;
    TextureUploadPlan uploads;
    for (const CubeFace face : faces)
        uploads.subresources.push_back({0U, 0U, 2U, 2U, 8U, rgba, face});

    Diagnostic diagnostic;
    require(validate_texture_description(cube, uploads, diagnostic) ==
                TextureStatus::ready &&
                texture_physical_array_layers(cube) == 6U &&
                texture_default_sample_view_kind(cube) ==
                    TextureSampleViewKind::texture_cube &&
                texture_upload_physical_array_layer(
                    cube, uploads.subresources.back()) == 5U,
            "an explicit six-face cube maps to six physical layers");

    TextureDescription ordinary_array = cube;
    ordinary_array.shape = TextureShape::texture_2d;
    ordinary_array.array_layers = 6U;
    require(texture_physical_array_layers(ordinary_array) == 6U &&
                texture_default_sample_view_kind(ordinary_array) ==
                    TextureSampleViewKind::texture_2d_array,
            "a six-layer 2D array is not inferred to be a cube");

    TextureDescription cube_array = cube;
    cube_array.array_layers = 2U;
    require(texture_physical_array_layers(cube_array) == 12U &&
                texture_default_sample_view_kind(cube_array) ==
                    TextureSampleViewKind::texture_cube_array,
            "a logical cube array selects a cube-array sampling view");

    TextureUploadPlan missing_face = uploads;
    missing_face.subresources.front().cube_face = CubeFace::none;
    require(validate_texture_upload_plan(cube, missing_face, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_cube_face_missing",
            "cube uploads require an explicit face");

    TextureUploadPlan unexpected_face;
    unexpected_face.subresources.push_back(
        {0U, 0U, 2U, 2U, 8U, rgba, CubeFace::positive_x});
    TextureDescription ordinary;
    ordinary.width = 2U;
    ordinary.height = 2U;
    ordinary.format = TextureFormat::rgba8_unorm;
    ordinary.usage = TextureUsage::sampled;
    require(validate_texture_upload_plan(ordinary, unexpected_face, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_cube_face_unexpected",
            "2D uploads reject cube-face metadata");

    TextureUploadPlan duplicate = uploads;
    duplicate.subresources.push_back(duplicate.subresources.front());
    require(validate_texture_upload_plan(cube, duplicate, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_subresource_duplicate",
            "cube duplicate detection includes the face");

    TextureDescription non_square = cube;
    non_square.height = 1U;
    require(validate_texture_description(non_square, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_cube_dimensions_invalid",
            "non-square cubes are rejected before allocation");

    TextureDescription multisampled = cube;
    multisampled.samples = 4U;
    require(validate_texture_description(multisampled, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_cube_samples_invalid",
            "multisampled cubes are rejected before allocation");

    TextureDescription oversized_cube_array = cube;
    oversized_cube_array.array_layers = 342U;
    require(validate_texture_description(oversized_cube_array, {}, diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_layer_limit",
            "logical cube arrays obey the physical layer limit");

    const std::array<std::byte, 8U> bc1_block = {};
    TextureDescription compressed_cube = cube;
    compressed_cube.width = 4U;
    compressed_cube.height = 4U;
    compressed_cube.format = TextureFormat::bc1_unorm;
    TextureUploadPlan compressed_uploads;
    for (const CubeFace face : faces)
        compressed_uploads.subresources.push_back(
            {0U, 0U, 4U, 4U, 8U, bc1_block, face});
    require(validate_texture_description(compressed_cube, compressed_uploads,
                                         diagnostic) == TextureStatus::ready,
            "compressed cubes require and accept all six faces");
    compressed_uploads.subresources.back().data =
        std::span<const std::byte>(bc1_block.data(), bc1_block.size() - 1U);
    require(validate_texture_description(compressed_cube, compressed_uploads,
                                         diagnostic) ==
                TextureStatus::invalid_description &&
                diagnostic.code == "texture_upload_truncated",
            "a truncated sixth compressed face has no valid upload plan");
}

} // namespace

int main() {
    try {
        mapsPortableAndSdkFormats();
        plansLegacyR32FWithoutApproximation();
        plansCompressedEdgesAndMips();
        plansRawPitches();
        rejectsMalformedPayloadAndLimits();
        rejectsUnsupportedDx10LayoutsAndInvalidBits();
        rejectsUnsupportedWithoutApproximation();
        plansPortableDecodedTextureResources();
        plansPortableJpegResources();
        rejectsOversizedUploadSubresourceLists();
        validatesTextureAccessPolicies();
        validatesExplicitCubeTextureContracts();
        std::cout << "texture upload tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "texture upload tests failed: " << error.what() << '\n';
        return 1;
    }
}
