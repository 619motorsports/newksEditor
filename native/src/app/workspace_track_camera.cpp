#include "apex/app/workspace_track_camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace apex::app {

namespace {

constexpr double pi = 3.14159265358979323846;

[[nodiscard]] render::CameraFrameResult failure(std::string code,
                                                std::string message) {
    return {std::nullopt, std::move(code), std::move(message)};
}

[[nodiscard]] bool checked_float(double value, float& output) noexcept {
    if (!std::isfinite(value) ||
        value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    output = static_cast<float>(value);
    return std::isfinite(output);
}

} // namespace

render::CameraFrameResult buildWorkspaceTrackCameraFrame(
    const WorkspaceTrackCameraFrameRequest& request) {
    if (request.camera == nullptr)
        return failure("workspace_track_camera_missing",
                       "Track-camera preview requires a camera record");

    const domain::CameraData& camera = *request.camera;
    const bool spline_active = !request.spline_points.empty();
    const auto offset = spline_active
                            ? domain::sample_camera_spline(
                                  request.spline_points,
                                  request.spline_position)
                            : std::array<double, 3U>{0.0, 0.0, 0.0};
    render::CameraFrameRequest camera_request;
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        if (!checked_float(camera.position[axis] + offset[axis],
                           camera_request.eye[axis]) ||
            !checked_float(camera.forward[axis],
                           camera_request.target[axis]) ||
            !checked_float(camera.up[axis], camera_request.up[axis])) {
            return failure(
                "workspace_track_camera_value_out_of_range",
                "Track-camera position, spline, and basis must fit finite float values");
        }
        camera_request.target[axis] += camera_request.eye[axis];
        if (!std::isfinite(camera_request.target[axis])) {
            return failure(
                "workspace_track_camera_value_out_of_range",
                "Track-camera target must fit finite float values");
        }
    }

    const double fov_degrees = std::clamp(
        spline_active ? camera.min_fov
                      : (camera.min_fov + camera.max_fov) * 0.5,
        1.0, 160.0);
    if (!checked_float(fov_degrees * pi / 180.0,
                       camera_request.fov_radians) ||
        !checked_float(camera.near_plane, camera_request.near_plane) ||
        !checked_float(camera.far_plane, camera_request.far_plane)) {
        return failure(
            "workspace_track_camera_value_out_of_range",
            "Track-camera optics must fit finite float values");
    }
    camera_request.aspect = request.viewport_aspect;
    camera_request.clip_space = request.clip_space;
    return render::build_camera_frame(camera_request);
}

WorkspaceTrackCameraPlaybackResult evaluateWorkspaceTrackCameraPlayback(
    const WorkspaceTrackCameraPlaybackRequest& request) noexcept {
    if (!std::isfinite(request.start_position) ||
        request.start_position < 0.0 || request.start_position > 1.0) {
        return {0.0, false,
                "workspace_track_camera_playback_position_invalid",
                "Track-camera playback start position must be from zero to one"};
    }
    if (!std::isfinite(request.elapsed_seconds) ||
        request.elapsed_seconds < 0.0) {
        return {0.0, false,
                "workspace_track_camera_playback_elapsed_invalid",
                "Track-camera playback elapsed time must be finite and nonnegative"};
    }
    if (!std::isfinite(request.animation_length_seconds)) {
        return {0.0, false,
                "workspace_track_camera_playback_length_invalid",
                "Track-camera animation length must be finite"};
    }

    const double start = request.start_position >= 1.0
                             ? 0.0
                             : request.start_position;
    const double configured_duration =
        request.animation_length_seconds == 0.0
            ? workspace_track_camera_default_animation_seconds
            : request.animation_length_seconds;
    const double duration = std::max(
        workspace_track_camera_minimum_animation_seconds,
        configured_duration);
    const double position = std::min(
        1.0, start + request.elapsed_seconds / duration);
    return {position, position >= 1.0, {}, {}};
}

} // namespace apex::app
