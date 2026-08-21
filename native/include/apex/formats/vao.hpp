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

// A mesh view used by the bounded VAO binder. The first three floats of each
// vertex are the position, matching the KN5 mesh contract. The view is
// non-owning and must remain alive for the duration of bindVaoPatch().
struct VaoMeshTarget {
    std::string_view name;
    std::span<const float> vertices{};
    std::size_t vertex_stride_floats = 0;
};

struct VaoBindingLimits {
    std::size_t max_meshes = 1'000'000U;
    std::size_t max_vertices_per_mesh = 10'000'000U;
    std::size_t max_records = 1'000'000U;
    std::size_t max_string_bytes = 1U << 20U;
    std::size_t max_value_bytes = 512U * 1024U * 1024U;
    std::size_t max_diagnostics = 10'000U;
    std::size_t max_diagnostic_bytes = 1U << 20U;
};

enum class VaoBindingSeverity : std::uint8_t {
    warning,
    error,
};

struct VaoBindingDiagnostic {
    VaoBindingSeverity severity = VaoBindingSeverity::warning;
    std::string code;
    std::string message;
    std::size_t record_index = 0U;
    std::size_t mesh_index = 0U;
};

struct VaoMeshBinding {
    std::size_t mesh_index = 0U;
    std::optional<std::vector<std::uint8_t>> primary;
    std::optional<std::vector<std::uint8_t>> secondary;
    std::optional<std::vector<float>> normal;
};

struct VaoBindingResult {
    bool valid = true;
    bool split_ao_staged = false;
    std::size_t patch_record_count = 0U;
    std::size_t matched_records = 0U;
    std::size_t unmatched_records = 0U;
    std::size_t alternate_records = 0U;
    std::size_t normal_records = 0U;
    std::size_t matched_normal_records = 0U;
    std::size_t unmatched_normal_records = 0U;
    std::size_t matched_meshes = 0U;
    std::size_t primary_meshes = 0U;
    std::size_t secondary_meshes = 0U;
    std::size_t normal_meshes = 0U;
    std::size_t value_count = 0U;
    std::size_t normal_vertex_count = 0U;
    std::uint8_t minimum = 255U;
    std::uint8_t maximum = 255U;
    double mean = 255.0;
    std::vector<VaoMeshBinding> bindings;
    std::vector<std::string> unmatched;
    std::vector<std::string> alternate;
    std::vector<VaoBindingDiagnostic> diagnostics;
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

// Bind decoded VAO records to mesh views using the reference editor's exact
// name, vertex-count, and first-position gate. This operation never mutates
// the source patch or target meshes. Split-AO animation application remains
// staged and is reported explicitly when the patch contains split-AO config.
[[nodiscard]] VaoBindingResult bindVaoPatch(
    const VaoPatch& patch, std::span<const VaoMeshTarget> meshes,
    VaoBindingLimits limits = {});

inline VaoBindingResult bind_vao_patch(
    const VaoPatch& patch, std::span<const VaoMeshTarget> meshes,
    VaoBindingLimits limits = {}) {
    return bindVaoPatch(patch, meshes, limits);
}

}
