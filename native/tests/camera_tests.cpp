#include "apex/render/camera.hpp"

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

} // namespace

int main() {
    try {
        matches_webgl_reference_formulas();
        maps_native_clip_depth_and_vulkan_y_explicitly();
        rejects_malformed_camera_inputs();
        std::cout << "camera tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "camera tests failed: " << error.what() << '\n';
        return 1;
    }
}
