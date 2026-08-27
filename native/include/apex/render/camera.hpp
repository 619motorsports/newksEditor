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

// The native ksNet Camera stores an orthonormal pose as right, up, backward,
// and position vectors.  This semantic representation avoids coupling the
// controller to the field ordering of the original mat44f implementation.
// `backward` points away from the view direction, as it does in the native
// Camera::getViewMatrix path.
struct NativeCameraPose {
    apex::scene::Vector3 right = {1.0F, 0.0F, 0.0F};
    apex::scene::Vector3 up = {0.0F, 1.0F, 0.0F};
    apex::scene::Vector3 backward = {0.0F, 0.0F, 1.0F};
    apex::scene::Vector3 position = {0.0F, 0.0F, 0.0F};
    // ksNet stores FOV in degrees and uses -1 as an aspect-ratio sentinel.
    float fov_degrees = 45.0F;
    float aspect_ratio = -1.0F;
    float near_plane = 0.01F;
    float far_plane = 100.0F;

    // These operations match Camera::moveForward, moveRight, moveUpWorld,
    // rotateOnAxis, and rotatePitch. False means the finite/basis guard
    // rejected the request without mutating the pose.
    [[nodiscard]] bool move_forward(float distance) noexcept;
    [[nodiscard]] bool move_right(float distance) noexcept;
    [[nodiscard]] bool move_up_world(float distance) noexcept;
    [[nodiscard]] bool rotate_on_axis(const apex::scene::Vector3& axis,
                                      float radians) noexcept;
    [[nodiscard]] bool rotate_pitch(float radians) noexcept;
};

// Matrices use the same 16-float column-major layout as public/app.js and
// SceneNode::transform. multiply_camera_matrices(a, b) returns a * b.
[[nodiscard]] apex::scene::Matrix4 multiply_camera_matrices(
    const apex::scene::Matrix4& left,
    const apex::scene::Matrix4& right) noexcept;

// Returns the inverse of a finite, well-conditioned column-major matrix.
// Singular and near-singular inputs are rejected instead of propagating
// non-finite values into camera or picking state.
[[nodiscard]] std::optional<apex::scene::Matrix4> invert_camera_matrix(
    const apex::scene::Matrix4& input) noexcept;

// Builds the source-evidenced WebGL look-at/perspective camera. Vulkan and
// D3D12 variants retain the same view math and explicitly remap clip space.
[[nodiscard]] CameraFrameResult build_camera_frame(const CameraFrameRequest& request);

// Builds an existing native pose for one backend. If aspect_ratio is -1, the
// supplied viewport aspect is used, matching ksNet's video-size fallback.
// Projection uses the existing explicit backend clip remapping; the native
// createPerspective internals are not claimed as fully recovered here.
[[nodiscard]] CameraFrameResult build_native_camera_frame(
    const NativeCameraPose& pose, float viewport_aspect,
    CameraClipSpace clip_space = CameraClipSpace::webgl);

[[nodiscard]] const char* camera_clip_space_name(CameraClipSpace clip_space) noexcept;

} // namespace apex::render
