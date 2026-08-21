#pragma once

#include "apex/core/parse_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::formats {

enum class VaoChannel : std::uint8_t {
    primary,
    secondary,
    normal,
};

struct VaoLighting {
    float opacity = 0.85f;
    float brightness = 1.1f;
    float gamma = 1.0f;
};

struct VaoRecord {
    std::string name;
    std::uint32_t type = 0;
    VaoChannel channel = VaoChannel::primary;
    bool alternate = false;
    std::array<float, 3> firstVertex{};
    std::uint32_t vertexCount = 0;
    std::vector<std::uint8_t> values;
    std::vector<float> normals;
    std::size_t offset = 0;
};

struct VaoEmbeddedExtraSamples {
    std::string entry;
    std::uint32_t version = 1;
    std::uint32_t samples = 0;
    std::size_t bytes = 0;
};

struct VaoData {
    std::uint32_t version = 4;
    std::vector<VaoRecord> records;
    std::size_t recordCount = 0;
    std::size_t byteLength = 0;
    std::size_t bytesRead = 0;
    std::optional<VaoEmbeddedExtraSamples> embeddedExtraSamples;
};

struct VaoSplitWing {
    std::uint32_t index = 0;
    std::string name;
    float exponent = 1.0f;
    std::vector<std::string> nodes;
};

struct VaoSplitAo {
    bool present = false;
    std::vector<std::string> cockpitHr;
    float doorExponent = 2.0f;
    std::vector<std::string> doorNodes;
    float headlightsExponent = 2.0f;
    std::vector<std::string> headlightsNodes;
    std::vector<std::string> steeringWheelNodes;
    std::vector<VaoSplitWing> wings;
    std::vector<std::string> warnings;
};

struct VaoArchiveEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
};

struct VaoPatch {
    std::string source;
    std::uint32_t version = 0;
    std::string entry;
    std::string configText;
    VaoLighting lighting;
    VaoSplitAo splitAo;
    VaoData data;
    std::vector<VaoArchiveEntry> archiveEntries;
    std::optional<VaoEmbeddedExtraSamples> extraSamples;
    std::optional<VaoArchiveEntry> treeSamples;
};

// Parse the binary Patch.data/Patch_v3/v4/v5 payload. The parser rejects
// unsupported versions, counts, non-finite first vertices/normals, and every
// truncated prefix before any untrusted allocation.
[[nodiscard]] VaoData parseVaoData(
    std::span<const std::uint8_t> bytes, std::uint32_t version = 4,
    VaoLighting lighting = {}, std::string source = "Patch_v4.data",
    apex::core::ParseLimits limits = {});

// Parse the ZIP VAO patch container. Deflate support is implemented in the
// source file with a bounded RFC 1951 decoder, so this public API has no zlib
// or minizip link/header dependency.
[[nodiscard]] VaoPatch parseVaoPatch(
    std::span<const std::uint8_t> bytes, std::string source = "VAO patch",
    apex::core::ParseLimits limits = {});

[[nodiscard]] bool vaoNormalOverrideNameEligible(std::string_view name) noexcept;

}
