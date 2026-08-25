#include "apex/render/camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace apex::render {

namespace {

using Matrix4 = apex::scene::Matrix4;
using Vector3 = apex::scene::Vector3;

constexpr float pi = 3.14159265358979323846F;
constexpr float minimum_basis_length = 1.0e-6F;
constexpr float native_degrees_to_radians = 0.01745299994945526F;

[[nodiscard]] bool finite_vector(const Vector3& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float component) {
        return std::isfinite(component);
    });
}

[[nodiscard]] Vector3 subtract(const Vector3& left, const Vector3& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] Vector3 add(const Vector3& left, const Vector3& right) noexcept {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

[[nodiscard]] Vector3 scale(const Vector3& value, float factor) noexcept {
    return {value[0] * factor, value[1] * factor, value[2] * factor};
}

[[nodiscard]] float dot(const Vector3& left, const Vector3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] Vector3 cross(const Vector3& left, const Vector3& right) noexcept {
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

[[nodiscard]] float length(const Vector3& value) noexcept {
    return std::sqrt(dot(value, value));
}

[[nodiscard]] Vector3 normalize(const Vector3& value, float value_length) noexcept {
    return {value[0] / value_length, value[1] / value_length, value[2] / value_length};
}

[[nodiscard]] bool finite_scalar(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool valid_native_basis(const NativeCameraPose& pose) noexcept {
    if (!finite_vector(pose.right) || !finite_vector(pose.up) ||
        !finite_vector(pose.backward) || !finite_vector(pose.position))
        return false;
    const float right_length = length(pose.right);
    const float up_length = length(pose.up);
    const float backward_length = length(pose.backward);
    if (!(right_length > minimum_basis_length && up_length > minimum_basis_length &&
          backward_length > minimum_basis_length))
        return false;
    constexpr float orthogonality_tolerance = 1.0e-3F;
    return std::abs(right_length - 1.0F) <= orthogonality_tolerance &&
           std::abs(up_length - 1.0F) <= orthogonality_tolerance &&
           std::abs(backward_length - 1.0F) <= orthogonality_tolerance &&
           std::abs(dot(pose.right, pose.up)) <= orthogonality_tolerance &&
           std::abs(dot(pose.right, pose.backward)) <= orthogonality_tolerance &&
           std::abs(dot(pose.up, pose.backward)) <= orthogonality_tolerance;
}

[[nodiscard]] Vector3 rotate_vector(const Vector3& value, const Vector3& unit_axis,
                                    float radians) noexcept {
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const Vector3 cross_axis = cross(unit_axis, value);
    const float axis_dot = dot(unit_axis, value);
    return add(add(scale(value, cosine), scale(cross_axis, sine)),
               scale(unit_axis, axis_dot * (1.0F - cosine)));
}

[[nodiscard]] bool finite_matrix(const Matrix4& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float component) {
        return std::isfinite(component);
    });
}

[[nodiscard]] bool valid_clip_space(CameraClipSpace value) noexcept {
    return value == CameraClipSpace::webgl || value == CameraClipSpace::vulkan ||
           value == CameraClipSpace::d3d12;
}

[[nodiscard]] Matrix4 look_at(const Vector3& eye, const Vector3& target,
                              const Vector3& authored_up, Vector3& forward,
                              Vector3& camera_up) noexcept {
    const Vector3 z = normalize(subtract(eye, target), length(subtract(eye, target)));
    const Vector3 x_source = cross(authored_up, z);
    const Vector3 x = normalize(x_source, length(x_source));
    const Vector3 y = cross(z, x);
    forward = {-z[0], -z[1], -z[2]};
    camera_up = y;
    return {
        x[0], y[0], z[0], 0.0F,
        x[1], y[1], z[1], 0.0F,
        x[2], y[2], z[2], 0.0F,
        -dot(x, eye), -dot(y, eye), -dot(z, eye), 1.0F,
    };
}

[[nodiscard]] Matrix4 perspective(const CameraFrameRequest& request) noexcept {
    const float f = 1.0F / std::tan(request.fov_radians * 0.5F);
    if (request.clip_space == CameraClipSpace::webgl) {
        const float nf = 1.0F / (request.near_plane - request.far_plane);
        return {
            f / request.aspect, 0.0F, 0.0F, 0.0F,
            0.0F, f, 0.0F, 0.0F,
            0.0F, 0.0F, (request.far_plane + request.near_plane) * nf, -1.0F,
            0.0F, 0.0F, 2.0F * request.far_plane * request.near_plane * nf, 0.0F,
        };
    }
    const float nf = 1.0F / (request.near_plane - request.far_plane);
    const float y = request.clip_space == CameraClipSpace::vulkan ? -f : f;
    return {
        f / request.aspect, 0.0F, 0.0F, 0.0F,
        0.0F, y, 0.0F, 0.0F,
        0.0F, 0.0F, request.far_plane * nf, -1.0F,
        0.0F, 0.0F, request.far_plane * request.near_plane * nf, 0.0F,
    };
}

[[nodiscard]] CameraFrameResult failure(std::string code, std::string message) {
    return {std::nullopt, std::move(code), std::move(message)};
}

} // namespace

bool NativeCameraPose::move_forward(float distance) noexcept {
    if (!finite_scalar(distance) || !valid_native_basis(*this)) return false;
    const Vector3 next = add(position, scale(backward, -distance));
    if (!finite_vector(next)) return false;
    position = next;
    return true;
}

bool NativeCameraPose::move_right(float distance) noexcept {
    if (!finite_scalar(distance) || !valid_native_basis(*this)) return false;
    const Vector3 next = add(position, scale(right, distance));
    if (!finite_vector(next)) return false;
    position = next;
    return true;
}

bool NativeCameraPose::move_up_world(float distance) noexcept {
    if (!finite_scalar(distance) || !valid_native_basis(*this)) return false;
    Vector3 next = position;
    next[1] += distance;
    if (!finite_vector(next)) return false;
    position = next;
    return true;
}

bool NativeCameraPose::rotate_on_axis(const Vector3& axis, float radians) noexcept {
    if (!finite_vector(axis) || !finite_scalar(radians) || !valid_native_basis(*this))
        return false;
    const float axis_length = length(axis);
    if (!(axis_length > minimum_basis_length)) return false;
    const Vector3 unit_axis = normalize(axis, axis_length);
    const Vector3 next_right = rotate_vector(right, unit_axis, radians);
    const Vector3 next_up = rotate_vector(up, unit_axis, radians);
    const Vector3 next_backward = rotate_vector(backward, unit_axis, radians);
    NativeCameraPose next = *this;
    next.right = next_right;
    next.up = next_up;
    next.backward = next_backward;
    if (!valid_native_basis(next)) return false;
    right = next_right;
    up = next_up;
    backward = next_backward;
    return true;
}

bool NativeCameraPose::rotate_pitch(float radians) noexcept {
    return rotate_on_axis({1.0F, 0.0F, 0.0F}, radians);
}

Matrix4 multiply_camera_matrices(const Matrix4& left, const Matrix4& right) noexcept {
    Matrix4 output{};
    for (std::size_t column = 0; column < 4U; ++column) {
        for (std::size_t row = 0; row < 4U; ++row) {
            float value = 0.0F;
            for (std::size_t component = 0; component < 4U; ++component) {
                value += left[component * 4U + row] * right[column * 4U + component];
            }
            output[column * 4U + row] = value;
        }
    }
    return output;
}

CameraFrameResult build_camera_frame(const CameraFrameRequest& request) {
    if (!valid_clip_space(request.clip_space)) {
        return failure("camera_clip_space_invalid", "Camera clip-space convention is unknown");
    }
    if (!finite_vector(request.eye) || !finite_vector(request.target) ||
        !finite_vector(request.up) || !std::isfinite(request.fov_radians) ||
        !std::isfinite(request.aspect) || !std::isfinite(request.near_plane) ||
        !std::isfinite(request.far_plane)) {
        return failure("camera_non_finite", "Camera inputs must contain only finite values");
    }
    if (!(request.fov_radians > 0.0F && request.fov_radians < pi)) {
        return failure("camera_fov_invalid", "Camera field of view must be between zero and pi radians");
    }
    if (!(request.aspect > 0.0F)) {
        return failure("camera_aspect_invalid", "Camera aspect ratio must be greater than zero");
    }
    if (!(request.near_plane > 0.0F && request.far_plane > request.near_plane)) {
        return failure("camera_clip_planes_invalid",
                       "Camera near plane must be positive and less than the far plane");
    }
    const Vector3 eye_to_target = subtract(request.target, request.eye);
    const float forward_length = length(eye_to_target);
    const float up_length = length(request.up);
    if (!(forward_length > minimum_basis_length)) {
        return failure("camera_direction_degenerate", "Camera eye and target positions must differ");
    }
    if (!(up_length > minimum_basis_length)) {
        return failure("camera_up_degenerate", "Camera up vector must have nonzero length");
    }
    const Vector3 backwards = {
        -eye_to_target[0] / forward_length,
        -eye_to_target[1] / forward_length,
        -eye_to_target[2] / forward_length,
    };
    if (!(length(cross(request.up, backwards)) > minimum_basis_length)) {
        return failure("camera_basis_degenerate",
                       "Camera direction and up vectors must not be parallel");
    }

    CameraFrame frame;
    frame.view = look_at(request.eye, request.target, request.up, frame.forward, frame.up);
    frame.projection = perspective(request);
    frame.view_projection = multiply_camera_matrices(frame.projection, frame.view);
    frame.position = request.eye;
    frame.fov_radians = request.fov_radians;
    frame.aspect = request.aspect;
    frame.near_plane = request.near_plane;
    frame.far_plane = request.far_plane;
    frame.clip_space = request.clip_space;
    if (!finite_matrix(frame.view) || !finite_matrix(frame.projection) ||
        !finite_matrix(frame.view_projection) || !finite_vector(frame.forward) ||
        !finite_vector(frame.up)) {
        return failure("camera_result_non_finite", "Camera matrix construction exceeded finite float range");
    }
    return {frame, {}, {}};
}

CameraFrameResult build_native_camera_frame(const NativeCameraPose& pose,
                                             float viewport_aspect,
                                             CameraClipSpace clip_space) {
    if (!valid_native_basis(pose) || !finite_scalar(pose.fov_degrees) ||
        !finite_scalar(pose.aspect_ratio) || !finite_scalar(pose.near_plane) ||
        !finite_scalar(pose.far_plane)) {
        return failure("camera_pose_invalid", "Native camera pose must contain finite orthonormal data");
    }
    if (!(pose.fov_degrees > 0.0F && pose.fov_degrees < 180.0F)) {
        return failure("camera_fov_invalid", "Native camera field of view must be between zero and 180 degrees");
    }
    float aspect = pose.aspect_ratio;
    if (aspect == -1.0F) aspect = viewport_aspect;
    if (!(aspect > 0.0F) || !finite_scalar(aspect)) {
        return failure("camera_aspect_invalid", "Native camera aspect ratio must be positive or -1");
    }
    CameraFrameRequest request;
    request.eye = pose.position;
    request.target = add(pose.position, scale(pose.backward, -1.0F));
    request.up = pose.up;
    request.fov_radians = pose.fov_degrees * native_degrees_to_radians;
    request.aspect = aspect;
    request.near_plane = pose.near_plane;
    request.far_plane = pose.far_plane;
    request.clip_space = clip_space;
    return build_camera_frame(request);
}

const char* camera_clip_space_name(CameraClipSpace clip_space) noexcept {
    switch (clip_space) {
    case CameraClipSpace::webgl: return "webgl";
    case CameraClipSpace::vulkan: return "vulkan";
    case CameraClipSpace::d3d12: return "d3d12";
    }
    return "unknown";
}

} // namespace apex::render
