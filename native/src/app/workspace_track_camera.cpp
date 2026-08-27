#include "apex/app/workspace_track_camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace apex::app {

namespace {

constexpr double pi = 3.14159265358979323846;

using Point = std::array<float, 3U>;

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

[[nodiscard]] Point subtract(const Point& left, const Point& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] Point cross(const Point& left, const Point& right) noexcept {
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

[[nodiscard]] float dot(const Point& left, const Point& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] +
           left[2] * right[2];
}

[[nodiscard]] float point_length(const Point& point) noexcept {
    const float squared = dot(point, point);
    return squared == 0.0F ? 0.0F : std::sqrt(squared);
}

} // namespace

const char* track_camera_preview_mode_name(
    TrackCameraPreviewMode mode) noexcept {
    switch (mode) {
    case TrackCameraPreviewMode::webgl: return "webgl";
    case TrackCameraPreviewMode::installed_editor: return "installed-editor";
    }
    return "unknown";
}

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

InstalledEditorTrackCameraSplineResult
buildInstalledEditorTrackCameraSpline(
    std::span<const std::array<double, 3U>> points) {
    if (points.size() < 4U) {
        return {std::nullopt,
                "installed_editor_track_camera_spline_too_short",
                "Installed-editor Catmull-Rom preview requires at least four spline points"};
    }
    if (points.size() > installed_editor_track_camera_max_spline_points) {
        return {std::nullopt,
                "installed_editor_track_camera_spline_limit",
                "Installed-editor camera spline exceeds the workspace point limit"};
    }

    InstalledEditorTrackCameraSpline spline;
    spline.points.reserve(points.size());
    for (const auto& point : points) {
        Point converted{};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (!checked_float(point[axis], converted[axis])) {
                return {std::nullopt,
                        "installed_editor_track_camera_spline_non_finite",
                        "Installed-editor camera spline points must fit finite float values"};
            }
        }
        spline.points.push_back(converted);
    }
    spline.closed = installedEditorSplineIsClosed(spline.points);
    if (!recomputeInstalledEditorSplineLengths(spline) ||
        !validInstalledEditorSpline(spline)) {
        return {std::nullopt,
                "installed_editor_track_camera_spline_length_invalid",
                "Installed-editor camera spline must have a positive finite length"};
    }
    return {std::move(spline), {}, {}};
}

render::CameraFrameResult buildInstalledEditorTrackCameraFrame(
    const InstalledEditorTrackCameraFrameRequest& request) {
    if (request.camera == nullptr)
        return failure("installed_editor_track_camera_missing",
                       "Installed-editor preview requires a camera record");
    if (request.spline == nullptr)
        return failure("installed_editor_track_camera_spline_missing",
                       "Installed-editor preview requires a camera spline");
    if (!std::isfinite(request.spline_position) ||
        !std::isfinite(request.lookahead_world_units)) {
        return failure(
            "installed_editor_track_camera_value_invalid",
            "Installed-editor camera position and look-ahead must be finite");
    }
    const auto& spline = *request.spline;
    if (!validInstalledEditorSpline(spline)) {
        return failure(
            "installed_editor_track_camera_spline_invalid",
            "Installed-editor camera spline data is incomplete or invalid");
    }

    const float scaled = request.spline_position *
                         installed_editor_track_camera_endpoint_factor;
    const float position = std::clamp(scaled, 0.0F, 1.0F);
    const auto eye_sample = sampleInstalledEditorSpline(spline, position);
    const float target_position = std::clamp(
        position + request.lookahead_world_units / spline.length,
        0.0F, 1.0F);
    const auto target_sample =
        sampleInstalledEditorSpline(spline, target_position);
    if (!eye_sample.has_value() || !target_sample.has_value()) {
        return failure(
            "installed_editor_track_camera_spline_invalid",
            "Installed-editor camera spline data is incomplete or invalid");
    }
    const Point& eye = *eye_sample;
    const Point& target = *target_sample;
    const Point direction_source = subtract(target, eye);
    const float direction_length = point_length(direction_source);
    if (!(direction_length > 1.0e-6F) || !std::isfinite(direction_length)) {
        return failure(
            "installed_editor_track_camera_target_degenerate",
            "Installed-editor camera position and look-ahead target must differ");
    }
    const Point direction = {
        direction_source[0] / direction_length,
        direction_source[1] / direction_length,
        direction_source[2] / direction_length,
    };
    const Point world_up = {0.0F, 1.0F, 0.0F};
    const Point right_source = cross(direction, world_up);
    const float right_length = point_length(right_source);
    if (!(right_length > 1.0e-6F) || !std::isfinite(right_length)) {
        return failure(
            "installed_editor_track_camera_basis_degenerate",
            "Installed-editor camera direction must not be parallel to world up");
    }
    const Point right = {
        right_source[0] / right_length,
        right_source[1] / right_length,
        right_source[2] / right_length,
    };
    Point up = cross(right, direction);
    if (up[1] < 0.0F) {
        up[0] = -up[0];
        up[1] = -up[1];
        up[2] = -up[2];
    }

    render::CameraFrameRequest camera_request;
    camera_request.eye = eye;
    camera_request.target = target;
    camera_request.up = up;
    if (!checked_float(request.camera->min_fov * pi / 180.0,
                       camera_request.fov_radians) ||
        !checked_float(request.camera->near_plane,
                       camera_request.near_plane) ||
        !checked_float(request.camera->far_plane,
                       camera_request.far_plane)) {
        return failure(
            "installed_editor_track_camera_optics_out_of_range",
            "Installed-editor camera optics must fit finite float values");
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

WorkspaceTrackCameraPlaybackResult
evaluateInstalledEditorTrackCameraPlayback(
    const InstalledEditorTrackCameraPlaybackRequest& request) noexcept {
    if (!std::isfinite(request.start_position) ||
        request.start_position < 0.0F || request.start_position > 1.0F) {
        return {0.0, false,
                "installed_editor_track_camera_playback_position_invalid",
                "Installed-editor playback position must be from zero to one"};
    }
    if (!std::isfinite(request.elapsed_seconds) ||
        request.elapsed_seconds < 0.0) {
        return {0.0, false,
                "installed_editor_track_camera_playback_elapsed_invalid",
                "Installed-editor playback time must be finite and nonnegative"};
    }
    if (!std::isfinite(request.speed_multiplier)) {
        return {0.0, false,
                "installed_editor_track_camera_playback_speed_invalid",
                "Installed-editor playback speed must be finite"};
    }
    if (!(request.spline_length > 0.0F) ||
        !std::isfinite(request.spline_length)) {
        return {0.0, false,
                "installed_editor_track_camera_playback_length_invalid",
                "Installed-editor playback requires a positive finite spline length"};
    }
    const double position =
        static_cast<double>(request.start_position) +
        request.elapsed_seconds *
            static_cast<double>(request.speed_multiplier) /
            static_cast<double>(request.spline_length);
    if (!std::isfinite(position)) {
        return {0.0, false,
                "installed_editor_track_camera_playback_result_invalid",
                "Installed-editor playback position exceeded the finite range"};
    }
    if (position >= 1.0) return {0.0, true, {}, {}};
    return {position, false, {}, {}};
}

} // namespace apex::app
