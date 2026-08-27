#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

// This is the backend-neutral CPU preview contract.  It consumes triangles
// that have already passed CSP material/mesh selection; config parsing and
// texture sampling remain above the renderer boundary.
inline constexpr std::uint32_t portable_grass_default_atlas_columns = 16U;
inline constexpr std::uint32_t portable_grass_default_atlas_rows = 1U;
inline constexpr std::uint32_t portable_grass_max_atlas_dimension = 64U;
inline constexpr std::size_t portable_grass_max_atlas_tiles = 4096U;
inline constexpr std::size_t portable_grass_max_source_triangles = 1'000'000U;
inline constexpr std::size_t portable_grass_max_blades = 30'000U;
inline constexpr std::size_t portable_grass_max_candidates = 100'000U;
inline constexpr std::uint32_t portable_grass_vertices_per_blade = 6U;
inline constexpr std::uint32_t portable_grass_vertex_float_count = 14U;
inline constexpr std::size_t portable_grass_vertex_stride_bytes =
    portable_grass_vertex_float_count * sizeof(float);
inline constexpr std::size_t portable_grass_max_vertex_count =
    portable_grass_max_blades * portable_grass_vertices_per_blade;

struct PortableGrassSourceVertex {
    std::array<float, 3U> position{};
    std::array<float, 3U> normal{0.0F, 1.0F, 0.0F};
};

struct PortableGrassSourceTriangle {
    std::array<PortableGrassSourceVertex, 3U> vertices{};
    std::uint32_t source_id = 0U;
};

struct PortableGrassSettings {
    float density = 1.0F;
    float height = 1.0F;
    float width = 0.1F;
    float height_variation = 0.0F;
    float width_variation = 0.0F;
    float angle_degrees = 0.0F;
    std::array<float, 4U> color{0.24F, 0.42F, 0.08F, 1.0F};
    float color_factor = 1.0F;
    float wind = 0.0F;
    float minimum_upward = 0.25F;
    std::uint32_t atlas_columns = portable_grass_default_atlas_columns;
    std::uint32_t atlas_rows = portable_grass_default_atlas_rows;
    std::uint32_t atlas_tile = 0U;
};

struct PortableGrassBuildOptions {
    std::size_t max_blades = portable_grass_max_blades;
    std::size_t max_candidates = portable_grass_max_candidates;
    // A non-zero declaration lets the caller prove that an upstream span was
    // not truncated before it reached this renderer contract.
    std::size_t declared_triangle_count = 0U;
    std::uint32_t seed = 0U;
};

enum class PortableGrassStatus : std::uint8_t {
    ready,
    empty,
    invalid_request,
    resource_limit,
};

struct PortableGrassValidationResult {
    PortableGrassStatus status = PortableGrassStatus::ready;
    std::string code;

    [[nodiscard]] bool accepted() const noexcept {
        return status == PortableGrassStatus::ready;
    }
};

struct PortableGrassBlade {
    std::uint32_t index = 0U;
    std::uint32_t source_triangle = 0U;
    std::array<float, 3U> position{};
    std::array<float, 3U> normal{0.0F, 1.0F, 0.0F};
    float height = 0.0F;
    float width = 0.0F;
    float angle_degrees = 0.0F;
    float wind = 0.0F;
    std::uint32_t atlas_tile = 0U;

    friend bool operator==(const PortableGrassBlade&, const PortableGrassBlade&) =
        default;
};

// Six expanded vertices per blade.  The 14 float32 values are intentionally
// contiguous so Vulkan and D3D12 can use the same vertex-input description:
// world position, world normal, atlas UV, linear RGBA color, wind, tip weight.
struct PortableGrassVertex {
    float position_x = 0.0F;
    float position_y = 0.0F;
    float position_z = 0.0F;
    float normal_x = 0.0F;
    float normal_y = 1.0F;
    float normal_z = 0.0F;
    float u = 0.0F;
    float v = 1.0F;
    float color_r = 0.0F;
    float color_g = 0.0F;
    float color_b = 0.0F;
    float color_a = 1.0F;
    float wind = 0.0F;
    float tip_weight = 0.0F;

    friend bool operator==(const PortableGrassVertex&, const PortableGrassVertex&) =
        default;
};
static_assert(sizeof(PortableGrassVertex) == portable_grass_vertex_stride_bytes);
static_assert(alignof(PortableGrassVertex) == alignof(float));

struct PortableGrassBuildDiagnostics {
    std::size_t source_triangle_count = 0U;
    std::size_t accepted_triangle_count = 0U;
    std::size_t rejected_degenerate_triangles = 0U;
    std::size_t rejected_non_upward_triangles = 0U;
    std::size_t candidate_count = 0U;
    std::size_t blade_count = 0U;
    bool generation_truncated = false;
};

struct PortableGrassBuildResult {
    PortableGrassStatus status = PortableGrassStatus::empty;
    std::string code;
    std::vector<PortableGrassBlade> blades;
    std::vector<PortableGrassVertex> vertices;
    PortableGrassBuildDiagnostics diagnostics{};

    [[nodiscard]] bool ready() const noexcept {
        return status == PortableGrassStatus::ready;
    }
};

[[nodiscard]] PortableGrassValidationResult validatePortableGrassRequest(
    std::span<const PortableGrassSourceTriangle> triangles,
    const PortableGrassSettings& settings = {},
    const PortableGrassBuildOptions& options = {}) noexcept;

// Builds a deterministic, bounded portable approximation of the source
// triangle-stratified GrassFX path in src/grass-fx.js.  It intentionally does
// not claim native compute density, surface-map ownership, occluder, or wind
// field parity.  Input triangles must already be selected/evaluated upstream.
[[nodiscard]] PortableGrassBuildResult buildPortableGrassLayout(
    std::span<const PortableGrassSourceTriangle> triangles,
    const PortableGrassSettings& settings = {},
    const PortableGrassBuildOptions& options = {});

} // namespace apex::render
