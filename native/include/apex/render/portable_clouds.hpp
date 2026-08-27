#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apex::render {

// These values are part of the portable preview contract.  The vertex record
// mirrors the WebGL cloud buffer assembled in public/app.js: corner position,
// billboard center, UV, and angular speed.
inline constexpr std::uint32_t portable_cloud_max_count = 512U;
inline constexpr std::uint32_t portable_cloud_texture_count = 7U;
inline constexpr std::uint32_t portable_cloud_vertices_per_billboard = 6U;
inline constexpr std::uint32_t portable_cloud_vertex_float_count = 8U;
inline constexpr std::size_t portable_cloud_vertex_stride_bytes =
    portable_cloud_vertex_float_count * sizeof(float);
inline constexpr std::size_t portable_cloud_max_vertex_count =
    static_cast<std::size_t>(portable_cloud_max_count) *
    portable_cloud_vertices_per_billboard;
inline constexpr std::size_t portable_cloud_max_vertex_bytes =
    portable_cloud_max_vertex_count * portable_cloud_vertex_stride_bytes;

struct PortableCloudSettings {
    float width = 4.0F;
    float height = 2.0F;
    float radius = 4.0F;
    float base_speed = 0.01F;
    std::uint32_t count = 100U;
};

struct PortableCloudBuildOptions {
    float world_detail = 5.0F;
    std::uint32_t texture_count = portable_cloud_texture_count;
    std::uint32_t seed = 1U;
};

enum class PortableCloudStatus : std::uint8_t {
    ready,
    empty,
    invalid_request,
    resource_limit,
};

struct PortableCloudValidationResult {
    PortableCloudStatus status = PortableCloudStatus::ready;
    std::string code;

    [[nodiscard]] bool accepted() const noexcept {
        return status == PortableCloudStatus::ready;
    }
};

// One six-vertex billboard.  The position is the camera-relative sphere
// center; corner coordinates are emitted separately in the vertex stream.
struct PortableCloudBillboard {
    std::uint32_t index = 0U;
    std::array<float, 3U> position{};
    float phi = 0.0F;
    float theta = 0.0F;
    float radius = 0.0F;
    float speed = 0.0F;
    std::uint32_t texture = 0U;

    friend bool operator==(const PortableCloudBillboard&, const PortableCloudBillboard&) =
        default;
};

// Must remain eight contiguous float32 values (32 bytes), matching the
// WebGL buffer layout: corner.xy, center.xyz, uv, speed.
struct PortableCloudVertex {
    float corner_x = 0.0F;
    float corner_y = 0.0F;
    float center_x = 0.0F;
    float center_y = 0.0F;
    float center_z = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    float speed = 0.0F;

    friend bool operator==(const PortableCloudVertex&, const PortableCloudVertex&) = default;
};
static_assert(sizeof(PortableCloudVertex) == portable_cloud_vertex_stride_bytes);
static_assert(alignof(PortableCloudVertex) == alignof(float));

struct PortableCloudTextureRun {
    std::uint32_t texture = 0U;
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
    std::uint32_t cloud_count = 0U;

    friend bool operator==(const PortableCloudTextureRun&, const PortableCloudTextureRun&) =
        default;
};

struct PortableCloudBuildResult {
    PortableCloudStatus status = PortableCloudStatus::empty;
    std::string code;
    std::vector<PortableCloudBillboard> billboards;
    std::vector<PortableCloudVertex> vertices;
    std::vector<PortableCloudTextureRun> texture_runs;

    [[nodiscard]] bool ready() const noexcept {
        return status == PortableCloudStatus::ready;
    }
};

[[nodiscard]] PortableCloudValidationResult validatePortableCloudRequest(
    const PortableCloudSettings& settings,
    const PortableCloudBuildOptions& options = {}) noexcept;

// Builds a deterministic, source-evidenced preview approximation.  Seed 1 is
// the stable editor preview seed; the production editor itself uses process
// global rand() state, so this function does not claim native frame parity.
[[nodiscard]] PortableCloudBuildResult buildPortableCloudLayout(
    const PortableCloudSettings& settings,
    const PortableCloudBuildOptions& options = {});

} // namespace apex::render
