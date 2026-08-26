#pragma once

#include "apex/render/camera.hpp"

#include <array>
#include <cstdint>

namespace apex::render {

struct CameraFilterSphere {
    apex::scene::Vector3 center{};
    float radius = 0.0F;
};

struct CameraFilterPlane {
    apex::scene::Vector3 normal{};
    float distance = 0.0F;
};

struct CameraFilterFrustum {
    // Native order: near, far, left, right, top, bottom.
    std::array<CameraFilterPlane, 6> planes{};
};

enum class CameraMeshPass : std::uint8_t {
    opaque = 0,
    transparent = 1,
    shadow = 2,
};

struct CameraMeshRenderable {
    // Dynamic renderables store a local sphere. Static renderables store the
    // world-space sphere that the native filter uses without a transform.
    CameraFilterSphere bounding_sphere{};
    float lod_in = 0.0F;
    float lod_out = 0.0F;
    std::uint32_t layer = 0;
    bool cast_shadows = true;
    bool visible = true;
    bool transparent = false;
    bool no_cull = false;
    bool is_static = false;
};

struct CameraMeshFilterRequest {
    CameraMeshRenderable renderable{};
    apex::scene::Matrix4 world_matrix = apex::scene::identity_matrix;
    // A null camera matches the native geometry-culling bypass. Pass, layer,
    // shadow, transparency, and visibility gates still apply.
    const CameraFrame* camera = nullptr;
    CameraMeshPass pass = CameraMeshPass::opaque;
    std::uint32_t max_layer = 5;
};

enum class CameraMeshFilterStatus : std::uint8_t {
    visible,
    culled,
    invalid_input,
};

struct CameraMeshFilterResult {
    CameraMeshFilterStatus status = CameraMeshFilterStatus::invalid_input;

    [[nodiscard]] bool visible() const noexcept {
        return status == CameraMeshFilterStatus::visible;
    }
};

/** Build the native-equivalent six planes for the frame's clip convention. */
[[nodiscard]] bool build_camera_filter_frustum(
    const CameraFrame& camera, CameraFilterFrustum& output) noexcept;

/** Apply the recovered native sphere transform, including its unit-scale rule. */
[[nodiscard]] bool transform_camera_filter_sphere(
    const CameraFilterSphere& input,
    const apex::scene::Matrix4& world_matrix,
    CameraFilterSphere& output) noexcept;

/** Apply the recovered native tangent-inclusive sphere/frustum predicate. */
[[nodiscard]] bool camera_filter_frustum_intersects(
    const CameraFilterFrustum& frustum,
    const CameraFilterSphere& sphere) noexcept;

/** Apply the recovered active path with guards for each consumed finite input. */
[[nodiscard]] CameraMeshFilterResult camera_mesh_filter_visible(
    const CameraMeshFilterRequest& request) noexcept;

}  // namespace apex::render
