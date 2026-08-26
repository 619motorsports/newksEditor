#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/render/camera.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace apex::render {

inline constexpr float installed_editor_pick_initial_distance = 1.0e11F;
inline constexpr std::size_t default_pick_max_vertices = 10'000'000U;
inline constexpr std::size_t default_pick_max_indices = 30'000'000U;
inline constexpr std::size_t default_pick_max_nodes = 1'000'000U;
inline constexpr std::size_t default_pick_max_depth = 1'024U;
inline constexpr std::size_t default_pick_max_triangles = 50'000'000U;

// A ray parameterized as origin + direction * distance. Screen rays use a
// unit direction. An inverse mesh transform can change its length, but it
// keeps the same distance parameter as the world ray.
struct PickRay {
    apex::scene::Vector3 origin{};
    apex::scene::Vector3 direction = {0.0F, 0.0F, -1.0F};
    float max_distance = std::numeric_limits<float>::infinity();
};

struct PickDiagnostic {
    std::string code;
    std::string message;
};

struct ScreenRayResult {
    std::optional<PickRay> ray;
    PickDiagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return ray.has_value(); }
};

// Build the recovered ksNet screen ray. It starts at the camera and has no
// near-plane or far-plane clipping. Pixel coordinates have their origin at
// the upper-left of the viewport.
[[nodiscard]] ScreenRayResult build_screen_ray(
    const CameraFrame& camera, float pixel_x, float pixel_y,
    std::uint32_t viewport_width, std::uint32_t viewport_height);

struct PickHit {
    float distance = 0.0F;
    std::uint32_t triangle_index = 0U;
    std::array<std::uint32_t, 3U> indices{};
    // position is world-space. mesh_position retains the local point that the
    // installed RayPicker passes to SplineEditor.
    apex::scene::Vector3 position{};
    apex::scene::Vector3 mesh_position{};
    // Barycentric order is vertex 0, vertex 1, vertex 2.
    apex::scene::Vector3 barycentric{};
};

enum class PickStatus : std::uint8_t {
    hit,
    miss,
    invalid_request,
};

struct TrianglePickResult {
    PickStatus status = PickStatus::miss;
    PickDiagnostic diagnostic;
    std::optional<PickHit> hit;

    [[nodiscard]] bool ok() const noexcept {
        return status == PickStatus::hit && hit.has_value();
    }
};

// The triangle test is inclusive on barycentric boundaries. It rejects an
// absolute determinant below 1e-5 and requires a distance greater than 1e-5.
[[nodiscard]] TrianglePickResult intersect_pick_triangle(
    const PickRay& ray, const apex::scene::Vector3& first,
    const apex::scene::Vector3& second,
    const apex::scene::Vector3& third);

// KN5 meshes use 16-bit indices, while converted/imported meshes can retain
// 32-bit indices. Exactly one index span must be supplied. The input spans
// are borrowed only for this synchronous call.
struct PickMeshView {
    std::span<const float> vertices{};
    std::size_t vertex_stride = 3U;
    std::span<const std::uint16_t> indices16{};
    std::span<const std::uint32_t> indices32{};
};

struct PickMeshLimits {
    std::size_t max_vertices = default_pick_max_vertices;
    std::size_t max_indices = default_pick_max_indices;
};

// Pick triangles in index-buffer order. The mesh transform maps local
// vertices to world space. All indices and referenced vertex ranges are
// validated before any triangle traversal.
[[nodiscard]] TrianglePickResult pick_mesh(
    const PickRay& world_ray, const PickMeshView& mesh,
    const apex::scene::Matrix4& mesh_to_world =
        apex::scene::identity_matrix,
    const PickMeshLimits& limits = {});

struct Kn5ScenePickLimits {
    std::size_t max_nodes = default_pick_max_nodes;
    std::size_t max_depth = default_pick_max_depth;
    std::size_t max_triangles = default_pick_max_triangles;
    PickMeshLimits mesh{};
};

struct Kn5ScenePickHit {
    PickHit triangle;
    std::size_t node_preorder_index = 0U;
    // This is the recovered callback input. The original editor does not
    // transform this mesh-local point back to world space.
    apex::scene::Vector3 callback_position{};
};

struct Kn5ScenePickResult {
    PickStatus status = PickStatus::miss;
    PickDiagnostic diagnostic;
    std::optional<Kn5ScenePickHit> hit;
    bool skipped_skinned_mesh = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == PickStatus::hit && hit.has_value();
    }
};

// Traverse active KN5 nodes in depth-first source order. Type-3 skinned
// meshes are skipped because native RayPicker only casts nodes to Mesh.
[[nodiscard]] Kn5ScenePickResult pick_kn5_scene(
    const PickRay& world_ray, const formats::Kn5Node& root,
    const Kn5ScenePickLimits& limits = {});

[[nodiscard]] const char* pick_status_name(PickStatus status) noexcept;

}  // namespace apex::render
