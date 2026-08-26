#include "apex/render/camera_mesh_filter.hpp"

#include <algorithm>
#include <cmath>

namespace apex::render {
namespace {

using Matrix4 = apex::scene::Matrix4;
using Vector3 = apex::scene::Vector3;

[[nodiscard]] bool finite_vector(const Vector3& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float component) {
        return std::isfinite(component);
    });
}

[[nodiscard]] bool finite_matrix(const Matrix4& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float component) {
        return std::isfinite(component);
    });
}

[[nodiscard]] bool valid_sphere(const CameraFilterSphere& value) noexcept {
    return finite_vector(value.center) && std::isfinite(value.radius) &&
           value.radius >= 0.0F;
}

[[nodiscard]] float dot(const Vector3& left, const Vector3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] float basis_length(const Matrix4& matrix,
                                 std::size_t column) noexcept {
    const std::size_t offset = column * 4U;
    return std::sqrt(matrix[offset] * matrix[offset] +
                     matrix[offset + 1U] * matrix[offset + 1U] +
                     matrix[offset + 2U] * matrix[offset + 2U]);
}

[[nodiscard]] bool normalize_plane(CameraFilterPlane& plane) noexcept {
    const float length = std::sqrt(dot(plane.normal, plane.normal));
    if (!std::isfinite(length)) return false;
    if (length == 0.0F) {
        plane.normal = {0.0F, 1.0F, 0.0F};
        plane.distance = 0.0F;
        return true;
    }
    const float inverse = 1.0F / length;
    for (float& component : plane.normal) component *= inverse;
    plane.distance *= inverse;
    return finite_vector(plane.normal) && std::isfinite(plane.distance);
}

[[nodiscard]] CameraFilterPlane negate_sum(const std::array<float, 4>& left,
                                            const std::array<float, 4>& right) noexcept {
    return {{-(left[0] + right[0]), -(left[1] + right[1]),
             -(left[2] + right[2])},
            -(left[3] + right[3])};
}

[[nodiscard]] CameraFilterPlane subtract_row(const std::array<float, 4>& left,
                                             const std::array<float, 4>& right) noexcept {
    return {{left[0] - right[0], left[1] - right[1], left[2] - right[2]},
            left[3] - right[3]};
}

[[nodiscard]] std::array<float, 4> matrix_row(const Matrix4& matrix,
                                              std::size_t row) noexcept {
    return {matrix[row], matrix[4U + row], matrix[8U + row], matrix[12U + row]};
}

}  // namespace

bool build_camera_filter_frustum(const CameraFrame& camera,
                                 CameraFilterFrustum& output) noexcept {
    if (!finite_matrix(camera.view_projection) ||
        (camera.clip_space != CameraClipSpace::webgl &&
         camera.clip_space != CameraClipSpace::vulkan &&
         camera.clip_space != CameraClipSpace::d3d12)) {
        return false;
    }
    const auto row0 = matrix_row(camera.view_projection, 0U);
    const auto row1 = matrix_row(camera.view_projection, 1U);
    const auto row2 = matrix_row(camera.view_projection, 2U);
    const auto row3 = matrix_row(camera.view_projection, 3U);

    if (camera.clip_space == CameraClipSpace::webgl) {
        output.planes[0] = negate_sum(row2, row3);
    } else {
        output.planes[0] = {{-row2[0], -row2[1], -row2[2]}, -row2[3]};
    }
    output.planes[1] = subtract_row(row2, row3);
    output.planes[2] = negate_sum(row0, row3);
    output.planes[3] = subtract_row(row0, row3);
    output.planes[4] = subtract_row(row1, row3);
    output.planes[5] = negate_sum(row1, row3);
    for (CameraFilterPlane& plane : output.planes)
        if (!normalize_plane(plane)) return false;
    return true;
}

bool transform_camera_filter_sphere(const CameraFilterSphere& input,
                                    const Matrix4& world_matrix,
                                    CameraFilterSphere& output) noexcept {
    if (!valid_sphere(input) || !finite_matrix(world_matrix)) return false;
    output.center = {
        world_matrix[0] * input.center[0] + world_matrix[4] * input.center[1] +
            world_matrix[8] * input.center[2] + world_matrix[12],
        world_matrix[1] * input.center[0] + world_matrix[5] * input.center[1] +
            world_matrix[9] * input.center[2] + world_matrix[13],
        world_matrix[2] * input.center[0] + world_matrix[6] * input.center[1] +
            world_matrix[10] * input.center[2] + world_matrix[14],
    };
    const float scale_x = basis_length(world_matrix, 0U);
    const float scale_y = basis_length(world_matrix, 1U);
    const float scale_z = basis_length(world_matrix, 2U);
    if (!finite_vector(output.center) || !std::isfinite(scale_x) ||
        !std::isfinite(scale_y) || !std::isfinite(scale_z)) {
        return false;
    }
    const float radius_scale = scale_x == 1.0F || scale_y == 1.0F || scale_z == 1.0F
                                   ? 1.0F
                                   : std::max({scale_x, scale_y, scale_z});
    output.radius = input.radius * radius_scale;
    return valid_sphere(output);
}

bool camera_filter_frustum_intersects(const CameraFilterFrustum& frustum,
                                      const CameraFilterSphere& sphere) noexcept {
    if (!valid_sphere(sphere)) return false;
    for (const CameraFilterPlane& plane : frustum.planes) {
        if (!finite_vector(plane.normal) || !std::isfinite(plane.distance)) return false;
        const float distance = dot(plane.normal, sphere.center) + plane.distance;
        if (!std::isfinite(distance) || distance > sphere.radius) return false;
    }
    return true;
}

CameraMeshFilterResult camera_mesh_filter_visible(
    const CameraMeshFilterRequest& request) noexcept {
    const CameraMeshRenderable& renderable = request.renderable;
    if (request.pass != CameraMeshPass::opaque &&
        request.pass != CameraMeshPass::transparent &&
        request.pass != CameraMeshPass::shadow) {
        return {CameraMeshFilterStatus::invalid_input};
    }
    if (request.max_layer < renderable.layer) return {CameraMeshFilterStatus::culled};
    if (request.pass == CameraMeshPass::shadow && !renderable.cast_shadows)
        return {CameraMeshFilterStatus::culled};
    if (renderable.transparent && request.pass != CameraMeshPass::transparent &&
        request.pass != CameraMeshPass::shadow) {
        return {CameraMeshFilterStatus::culled};
    }
    if (!renderable.transparent && request.pass == CameraMeshPass::transparent)
        return {CameraMeshFilterStatus::culled};
    if (request.pass != CameraMeshPass::shadow && !renderable.visible)
        return {CameraMeshFilterStatus::culled};
    if (renderable.no_cull || request.camera == nullptr)
        return {CameraMeshFilterStatus::visible};
    if (!std::isfinite(renderable.lod_in) || !std::isfinite(renderable.lod_out) ||
        !valid_sphere(renderable.bounding_sphere)) {
        return {CameraMeshFilterStatus::invalid_input};
    }

    CameraFilterSphere sphere = renderable.bounding_sphere;
    if (!renderable.is_static &&
        !transform_camera_filter_sphere(renderable.bounding_sphere,
                                        request.world_matrix, sphere)) {
        return {CameraMeshFilterStatus::invalid_input};
    }
    if (renderable.lod_in != 0.0F || renderable.lod_out != 0.0F) {
        if (!finite_vector(request.camera->position) ||
            !std::isfinite(request.camera->fov_radians)) {
            return {CameraMeshFilterStatus::invalid_input};
        }
        constexpr float radians_to_degrees = 57.29577951308232F;
        const float fov_degrees = request.camera->fov_radians * radians_to_degrees;
        const float scale = std::clamp(fov_degrees * 0.0125F, 0.0F, 1.0F);
        const float dx = request.camera->position[0] - sphere.center[0];
        const float dy = request.camera->position[1] - sphere.center[1];
        const float dz = request.camera->position[2] - sphere.center[2];
        const float scaled_distance_squared =
            (dx * dx + dy * dy + dz * dz) * scale * scale;
        const float near_squared = renderable.lod_in * renderable.lod_in;
        const float far = std::max(renderable.lod_out,
                                   renderable.bounding_sphere.radius);
        const float far_squared = far * far;
        if (!std::isfinite(scaled_distance_squared) ||
            !std::isfinite(near_squared) || !std::isfinite(far_squared)) {
            return {CameraMeshFilterStatus::invalid_input};
        }
        if (scaled_distance_squared < near_squared ||
            far_squared < scaled_distance_squared) {
            return {CameraMeshFilterStatus::culled};
        }
    }

    CameraFilterFrustum frustum;
    if (!build_camera_filter_frustum(*request.camera, frustum))
        return {CameraMeshFilterStatus::invalid_input};
    return {camera_filter_frustum_intersects(frustum, sphere)
                ? CameraMeshFilterStatus::visible
                : CameraMeshFilterStatus::culled};
}

}  // namespace apex::render
