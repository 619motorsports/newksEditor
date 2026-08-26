#include "apex/render/camera.hpp"
#include "apex/render/camera_mesh_filter.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void require_close(float actual, float expected, std::string_view message) {
    if (std::abs(actual - expected) > 1.0e-5F)
        throw std::runtime_error(std::string(message));
}

std::array<float, 4> transform(const apex::scene::Matrix4& matrix,
                               std::array<float, 4> value) {
    std::array<float, 4> output{};
    for (std::size_t row = 0; row < 4U; ++row) {
        for (std::size_t column = 0; column < 4U; ++column)
            output[row] += matrix[column * 4U + row] * value[column];
    }
    return output;
}

void matches_webgl_reference_formulas() {
    CameraFrameRequest request;
    request.eye = {0.0F, 0.0F, 5.0F};
    request.target = {0.0F, 0.0F, 0.0F};
    request.up = {0.0F, 1.0F, 0.0F};
    request.fov_radians = 0.7853981633974483F;
    request.aspect = 2.0F;
    request.near_plane = 0.1F;
    request.far_plane = 100.0F;
    request.clip_space = CameraClipSpace::webgl;
    const CameraFrameResult result = build_camera_frame(request);
    require(result.ok(), "valid WebGL camera accepted");
    const CameraFrame& frame = *result.frame;
    require_close(frame.view[0], 1.0F, "WebGL view X basis");
    require_close(frame.view[5], 1.0F, "WebGL view Y basis");
    require_close(frame.view[10], 1.0F, "WebGL view Z basis");
    require_close(frame.view[14], -5.0F, "WebGL view translation");
    const float f = 1.0F / std::tan(request.fov_radians * 0.5F);
    require_close(frame.projection[0], f / request.aspect, "WebGL perspective X");
    require_close(frame.projection[5], f, "WebGL perspective Y");
    require_close(frame.projection[11], -1.0F, "WebGL perspective W");
    require_close(frame.forward[2], -1.0F, "camera forward direction");

    const auto view_origin = transform(frame.view, {0.0F, 0.0F, 0.0F, 1.0F});
    require_close(view_origin[2], -5.0F, "view transforms target once");
    const auto clip_origin = transform(frame.view_projection, {0.0F, 0.0F, 0.0F, 1.0F});
    require_close(clip_origin[3], 5.0F, "view-projection multiplication order");
}

void maps_native_clip_depth_and_vulkan_y_explicitly() {
    CameraFrameRequest request;
    request.eye = {0.0F, 0.0F, 0.0F};
    request.target = {0.0F, 0.0F, -1.0F};
    request.near_plane = 0.25F;
    request.far_plane = 50.0F;
    request.clip_space = CameraClipSpace::d3d12;
    const CameraFrame d3d = *build_camera_frame(request).frame;
    const auto near_clip = transform(d3d.projection, {0.0F, 0.0F, -request.near_plane, 1.0F});
    const auto far_clip = transform(d3d.projection, {0.0F, 0.0F, -request.far_plane, 1.0F});
    require_close(near_clip[2] / near_clip[3], 0.0F, "D3D12 near depth is zero");
    require_close(far_clip[2] / far_clip[3], 1.0F, "D3D12 far depth is one");

    request.clip_space = CameraClipSpace::vulkan;
    const CameraFrame vulkan = *build_camera_frame(request).frame;
    require_close(vulkan.projection[5], -d3d.projection[5], "Vulkan projection flips framebuffer Y");
    require_close(vulkan.projection[10], d3d.projection[10], "native depth projection matches");
    require(std::string(camera_clip_space_name(CameraClipSpace::webgl)) == "webgl" &&
                std::string(camera_clip_space_name(static_cast<CameraClipSpace>(255))) == "unknown",
            "camera clip-space names are stable");
}

void composes_world_after_view_projection() {
    apex::scene::Matrix4 world = apex::scene::identity_matrix;
    world[12] = 2.0F;
    apex::scene::Matrix4 view_projection = apex::scene::identity_matrix;
    view_projection[0] = 3.0F;
    const auto clip = transform(multiply_camera_matrices(view_projection, world),
                                {0.0F, 0.0F, 0.0F, 1.0F});
    require_close(clip[0], 6.0F, "view-projection multiplies the world position");
    require_close(clip[3], 1.0F, "affine transform preserves homogeneous W");
}

void matches_native_pose_motion_and_rotation() {
    NativeCameraPose pose;
    require(pose.move_forward(2.0F), "native forward movement accepted");
    require_close(pose.position[2], -2.0F, "native forward uses negative backward basis");
    require(pose.move_right(3.0F), "native right movement accepted");
    require_close(pose.position[0], 3.0F, "native right uses first basis");
    require(pose.move_up_world(4.0F), "native world-up movement accepted");
    require_close(pose.position[1], 4.0F, "native world-up changes only Y");

    require(pose.rotate_pitch(1.5707963267948966F), "native pitch rotation accepted");
    require_close(pose.backward[1], -1.0F, "native pitch rotates backward basis");
    require_close(pose.up[2], 1.0F, "native pitch rotates up basis");
    require_close(pose.right[0], 1.0F, "native pitch preserves right basis");

    const CameraFrameResult result = build_native_camera_frame(pose, 2.0F);
    require(result.ok(), "native pose converts to a camera frame");
    require_close(result.frame->position[0], 3.0F, "native frame keeps pose X");
    require_close(result.frame->position[1], 4.0F, "native frame keeps pose Y");
    require_close(result.frame->position[2], -2.0F, "native frame keeps pose Z");
    require_close(result.frame->forward[1], 1.0F, "native frame forward is negative backward");
    require_close(result.frame->up[2], 1.0F, "native frame keeps rotated up basis");
}

void matches_native_fov_and_aspect_fallback() {
    NativeCameraPose pose;
    pose.fov_degrees = 90.0F;
    pose.aspect_ratio = -1.0F;
    const auto fallback_result = build_native_camera_frame(pose, 2.0F);
    require(fallback_result.ok(), "native viewport aspect fallback accepted");
    const CameraFrame fallback = *fallback_result.frame;
    require_close(fallback.aspect, 2.0F, "native -1 aspect uses viewport aspect");
    require_close(fallback.fov_radians, 1.570769995F, "native FOV converts degrees to radians");

    pose.aspect_ratio = 1.25F;
    const auto authored_result = build_native_camera_frame(pose, 4.0F);
    require(authored_result.ok(), "native authored aspect accepted");
    const CameraFrame authored = *authored_result.frame;
    require_close(authored.aspect, 1.25F, "native authored aspect overrides viewport");
}

void rejects_invalid_native_pose_operations() {
    NativeCameraPose pose;
    const auto original = pose.position;
    require(!pose.move_forward(std::numeric_limits<float>::quiet_NaN()),
            "native non-finite movement is rejected");
    require(pose.position == original, "rejected movement does not mutate pose");
    require(!pose.rotate_on_axis({0.0F, 0.0F, 0.0F}, 1.0F),
            "native zero rotation axis is rejected");
    require(!pose.rotate_pitch(std::numeric_limits<float>::infinity()),
            "native non-finite rotation is rejected");

    pose.aspect_ratio = 0.0F;
    require(build_native_camera_frame(pose, 1.0F).code == "camera_aspect_invalid",
            "native invalid aspect is rejected");
    pose = {};
    pose.fov_degrees = 180.0F;
    require(build_native_camera_frame(pose, 1.0F).code == "camera_fov_invalid",
            "native 180-degree FOV is rejected");
}

void rejects_malformed_camera_inputs() {
    CameraFrameRequest request;
    request.eye[0] = std::numeric_limits<float>::quiet_NaN();
    require(!build_camera_frame(request).ok() &&
                build_camera_frame(request).code == "camera_non_finite",
            "non-finite camera rejected");
    request = {};
    request.fov_radians = 0.0F;
    require(build_camera_frame(request).code == "camera_fov_invalid", "zero FOV rejected");
    request = {};
    request.aspect = 0.0F;
    require(build_camera_frame(request).code == "camera_aspect_invalid", "zero aspect rejected");
    request = {};
    request.near_plane = 10.0F;
    request.far_plane = 1.0F;
    require(build_camera_frame(request).code == "camera_clip_planes_invalid", "reversed clips rejected");
    request = {};
    request.target = request.eye;
    require(build_camera_frame(request).code == "camera_direction_degenerate", "zero direction rejected");
    request = {};
    request.up = {0.0F, 0.0F, 0.0F};
    require(build_camera_frame(request).code == "camera_up_degenerate", "zero up rejected");
    request = {};
    request.up = {0.0F, 0.0F, 1.0F};
    require(build_camera_frame(request).code == "camera_basis_degenerate", "parallel basis rejected");
    request = {};
    request.clip_space = static_cast<CameraClipSpace>(255);
    require(build_camera_frame(request).code == "camera_clip_space_invalid", "unknown clip space rejected");
}

CameraFrame identity_filter_camera(CameraClipSpace clip_space) {
    CameraFrame camera;
    camera.view_projection = apex::scene::identity_matrix;
    camera.position = {0.0F, 0.0F, 0.0F};
    camera.fov_radians = 0.7853981633974483F;
    camera.aspect = 1.0F;
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    camera.clip_space = clip_space;
    return camera;
}

void recovers_native_frustum_planes_and_tangency() {
    CameraFilterFrustum frustum;
    const auto d3d = identity_filter_camera(CameraClipSpace::d3d12);
    require(build_camera_filter_frustum(d3d, frustum), "D3D filter frustum accepted");
    require(camera_filter_frustum_intersects(frustum, {{{0.0F, 0.0F, 0.5F}}, 0.0F}),
            "D3D point inside unit clip volume");
    require(!camera_filter_frustum_intersects(frustum, {{{0.0F, 0.0F, -0.001F}}, 0.0F}),
            "D3D point before zero-depth near plane rejected");
    require(!camera_filter_frustum_intersects(frustum, {{{0.0F, 0.0F, 1.001F}}, 0.0F}),
            "D3D point after far plane rejected");
    require(camera_filter_frustum_intersects(frustum, {{{1.25F, 0.0F, 0.5F}}, 0.25F}),
            "sphere tangent to right plane accepted");

    const auto webgl = identity_filter_camera(CameraClipSpace::webgl);
    require(build_camera_filter_frustum(webgl, frustum), "WebGL filter frustum accepted");
    require(camera_filter_frustum_intersects(frustum, {{{0.0F, 0.0F, -0.5F}}, 0.0F}),
            "WebGL negative clip depth accepted");
}

void recovers_native_sphere_transform_scale_rule() {
    CameraFilterSphere input{{1.0F, 2.0F, 3.0F}, 2.0F};
    CameraFilterSphere output;
    auto world = apex::scene::identity_matrix;
    world[0] = 2.0F;
    world[5] = 3.0F;
    world[10] = 1.0F;
    world[12] = 4.0F;
    require(transform_camera_filter_sphere(input, world, output),
            "native sphere transform accepted");
    require(output.center == apex::scene::Vector3{6.0F, 6.0F, 3.0F},
            "native sphere center uses column-major transpose equivalent");
    require_close(output.radius, 2.0F, "one exact unit scale preserves native radius");

    world[10] = 4.0F;
    require(transform_camera_filter_sphere(input, world, output),
            "non-unit native sphere transform accepted");
    require_close(output.radius, 8.0F, "all non-unit scales use maximum basis length");
}

void applies_native_camera_mesh_filter_gates() {
    const auto camera = identity_filter_camera(CameraClipSpace::d3d12);
    CameraMeshFilterRequest request;
    request.camera = &camera;
    request.renderable.bounding_sphere = {{0.0F, 0.0F, 0.5F}, 0.0F};

    require(camera_mesh_filter_visible(request).visible(), "opaque mesh inside frustum visible");
    request.renderable.layer = 6U;
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::culled,
            "layer above native maximum rejected");
    request.renderable.layer = 0U;
    request.renderable.transparent = true;
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::culled,
            "transparent mesh rejected from opaque pass");
    request.pass = CameraMeshPass::transparent;
    require(camera_mesh_filter_visible(request).visible(),
            "transparent mesh accepted in transparent pass");
    request.pass = CameraMeshPass::shadow;
    request.renderable.cast_shadows = false;
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::culled,
            "non-caster rejected from shadow pass");
    request.renderable.cast_shadows = true;
    request.renderable.visible = false;
    require(camera_mesh_filter_visible(request).visible(),
            "shadow pass does not use ordinary visibility flag");
    request.pass = CameraMeshPass::transparent;
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::culled,
            "color pass uses ordinary visibility flag");

    request.renderable.visible = true;
    request.renderable.no_cull = true;
    request.renderable.bounding_sphere.center[0] = std::numeric_limits<float>::quiet_NaN();
    require(camera_mesh_filter_visible(request).visible(),
            "NO_CULL bypasses native LOD and frustum inputs");
}

void applies_native_dynamic_static_and_lod_rules() {
    auto camera = identity_filter_camera(CameraClipSpace::d3d12);
    CameraMeshFilterRequest request;
    request.camera = &camera;
    request.renderable.bounding_sphere = {{0.0F, 0.0F, 0.5F}, 0.0F};
    request.world_matrix[12] = 4.0F;
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::culled,
            "dynamic mesh sphere uses world transform");
    request.renderable.is_static = true;
    require(camera_mesh_filter_visible(request).visible(),
            "static mesh sphere ignores world transform");
    request.world_matrix[0] = std::numeric_limits<float>::quiet_NaN();
    require(camera_mesh_filter_visible(request).visible(),
            "static mesh does not consume world matrix");

    request = {};
    request.camera = &camera;
    request.renderable.bounding_sphere = {{0.0F, 0.0F, 0.5F}, 6.0F};
    request.renderable.lod_out = 1.0F;
    camera.position = {0.0F, 0.0F, 5.0F};
    require(camera_mesh_filter_visible(request).visible(),
            "original sphere radius raises native far LOD limit");
    request.renderable.bounding_sphere.radius = 0.0F;
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::culled,
            "native far LOD rejects distance beyond authored limit");
}

void rejects_malformed_camera_mesh_filter_inputs() {
    auto camera = identity_filter_camera(CameraClipSpace::d3d12);
    CameraMeshFilterRequest request;
    request.camera = &camera;
    request.renderable.bounding_sphere = {{0.0F, 0.0F, 0.5F}, 0.0F};
    request.world_matrix[0] = std::numeric_limits<float>::infinity();
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::invalid_input,
            "non-finite dynamic world matrix rejected");
    request.world_matrix = apex::scene::identity_matrix;
    camera.view_projection[0] = std::numeric_limits<float>::quiet_NaN();
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::invalid_input,
            "non-finite frustum matrix rejected");
    request.pass = static_cast<CameraMeshPass>(255);
    require(camera_mesh_filter_visible(request).status == CameraMeshFilterStatus::invalid_input,
            "unknown camera mesh pass rejected");
}

} // namespace

int main() {
    try {
        matches_webgl_reference_formulas();
        maps_native_clip_depth_and_vulkan_y_explicitly();
        composes_world_after_view_projection();
        matches_native_pose_motion_and_rotation();
        matches_native_fov_and_aspect_fallback();
        rejects_invalid_native_pose_operations();
        rejects_malformed_camera_inputs();
        recovers_native_frustum_planes_and_tangency();
        recovers_native_sphere_transform_scale_rule();
        applies_native_camera_mesh_filter_gates();
        applies_native_dynamic_static_and_lod_rules();
        rejects_malformed_camera_mesh_filter_inputs();
        std::cout << "camera tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "camera tests failed: " << error.what() << '\n';
        return 1;
    }
}
