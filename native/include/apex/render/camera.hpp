#pragma once

#include "apex/scene/scene.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace apex::render {

// The WebGL reference and the native APIs use different clip-space depth and
// framebuffer-Y conventions. Callers must select one explicitly.
enum class CameraClipSpace : std::uint8_t {
    webgl,
    vulkan,
    d3d12,
};

struct CameraFrameRequest {
    apex::scene::Vector3 eye = {0.0F, 0.0F, 5.0F};
    apex::scene::Vector3 target = {0.0F, 0.0F, 0.0F};
    apex::scene::Vector3 up = {0.0F, 1.0F, 0.0F};
    float fov_radians = 0.7853981633974483F;
    float aspect = 1.0F;
    float near_plane = 0.01F;
    float far_plane = 100.0F;
    CameraClipSpace clip_space = CameraClipSpace::webgl;
};

struct CameraFrame {
    apex::scene::Matrix4 view = apex::scene::identity_matrix;
    apex::scene::Matrix4 projection = apex::scene::identity_matrix;
    apex::scene::Matrix4 view_projection = apex::scene::identity_matrix;
    apex::scene::Vector3 position = {0.0F, 0.0F, 0.0F};
    apex::scene::Vector3 forward = {0.0F, 0.0F, -1.0F};
    apex::scene::Vector3 up = {0.0F, 1.0F, 0.0F};
    float fov_radians = 0.0F;
    float aspect = 0.0F;
    float near_plane = 0.0F;
    float far_plane = 0.0F;
    CameraClipSpace clip_space = CameraClipSpace::webgl;
};

struct CameraFrameResult {
    std::optional<CameraFrame> frame;
    std::string code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return frame.has_value(); }
};

// Matrices use the same 16-float column-major layout as public/app.js and
// SceneNode::transform. multiply_camera_matrices(a, b) returns a * b.
[[nodiscard]] apex::scene::Matrix4 multiply_camera_matrices(
    const apex::scene::Matrix4& left,
    const apex::scene::Matrix4& right) noexcept;

// Builds the source-evidenced WebGL look-at/perspective camera. Vulkan and
// D3D12 variants retain the same view math and explicitly remap clip space.
[[nodiscard]] CameraFrameResult build_camera_frame(const CameraFrameRequest& request);

[[nodiscard]] const char* camera_clip_space_name(CameraClipSpace clip_space) noexcept;

} // namespace apex::render
