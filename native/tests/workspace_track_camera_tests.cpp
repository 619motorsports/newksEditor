#include "apex/app/workspace_track_camera.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void require_near(float actual, float expected, std::string_view message) {
    if (std::abs(actual - expected) > 0.0001F)
        throw std::runtime_error(std::string(message));
}

apex::domain::CameraData camera_fixture() {
    apex::domain::CameraData camera;
    camera.name = "Spline camera";
    camera.position = {10.0, 2.0, 3.0};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.min_fov = 20.0;
    camera.max_fov = 60.0;
    camera.near_plane = 0.25;
    camera.far_plane = 2'000.0;
    camera.spline = "follow.csv";
    camera.spline_animation_length = 4.0;
    return camera;
}

void builds_saved_and_spline_camera_frames() {
    const auto camera = camera_fixture();
    apex::app::WorkspaceTrackCameraFrameRequest request;
    request.camera = &camera;
    request.viewport_aspect = 2.0F;
    request.clip_space = apex::render::CameraClipSpace::vulkan;

    auto saved = apex::app::buildWorkspaceTrackCameraFrame(request);
    require(saved.ok(), "saved track-camera frame builds");
    require_near(saved.frame->position[0], 10.0F,
                 "saved camera keeps its base position");
    require_near(saved.frame->fov_radians,
                 40.0F * 3.14159265358979323846F / 180.0F,
                 "saved camera uses the midpoint FOV");
    require_near(saved.frame->near_plane, 0.25F,
                 "saved camera keeps its near plane");
    require_near(saved.frame->far_plane, 2'000.0F,
                 "saved camera keeps its far plane");
    require(saved.frame->clip_space ==
                apex::render::CameraClipSpace::vulkan,
            "saved camera keeps the requested backend clip space");

    const std::array<std::array<double, 3U>, 3U> spline = {{
        {0.0, 0.0, 0.0},
        {4.0, 0.0, 0.0},
        {8.0, 0.0, 0.0},
    }};
    request.spline_points = spline;
    request.spline_position = 0.5;
    auto sampled = apex::app::buildWorkspaceTrackCameraFrame(request);
    require(sampled.ok(), "sampled track-camera frame builds");
    const auto expected_offset =
        apex::domain::sample_camera_spline(spline, 0.5);
    require_near(sampled.frame->position[0],
                 static_cast<float>(10.0 + expected_offset[0]),
                 "spline changes only the saved eye position");
    require_near(sampled.frame->position[1], 2.0F,
                 "spline keeps the saved vertical position");
    require_near(sampled.frame->forward[2], -1.0F,
                 "spline keeps the saved forward basis");
    require_near(sampled.frame->fov_radians,
                 20.0F * 3.14159265358979323846F / 180.0F,
                 "spline camera uses MIN_FOV");

    request.spline_position = 1.0;
    auto endpoint = apex::app::buildWorkspaceTrackCameraFrame(request);
    require(endpoint.ok(), "spline endpoint track-camera frame builds");
    const auto expected_endpoint =
        apex::domain::sample_camera_spline(spline, 1.0);
    require_near(endpoint.frame->position[0],
                 static_cast<float>(10.0 + expected_endpoint[0]),
                 "spline endpoint keeps the recovered sampling factor");
}

void clamps_production_fov_and_rejects_unsafe_values() {
    auto camera = camera_fixture();
    camera.min_fov = -20.0;
    camera.max_fov = 400.0;
    apex::app::WorkspaceTrackCameraFrameRequest request;
    request.camera = &camera;
    auto clamped = apex::app::buildWorkspaceTrackCameraFrame(request);
    require(clamped.ok(), "WebGL FOV clamp produces a valid camera");
    require_near(clamped.frame->fov_radians,
                 160.0F * 3.14159265358979323846F / 180.0F,
                 "saved midpoint FOV is clamped to 160 degrees");

    camera.position[0] = std::numeric_limits<double>::max();
    auto overflow = apex::app::buildWorkspaceTrackCameraFrame(request);
    require(!overflow.ok() &&
                overflow.code ==
                    "workspace_track_camera_value_out_of_range",
            "double-to-float overflow is rejected");

    request.camera = nullptr;
    auto missing = apex::app::buildWorkspaceTrackCameraFrame(request);
    require(!missing.ok() &&
                missing.code == "workspace_track_camera_missing",
            "missing camera record is rejected");
}

void evaluates_one_shot_playback() {
    auto default_duration =
        apex::app::evaluateWorkspaceTrackCameraPlayback({0.25, 3.75, 0.0});
    require(default_duration.ok() &&
                std::abs(default_duration.position - 0.5) < 0.000001 &&
                !default_duration.finished,
            "zero animation length selects the 15-second fallback");

    auto configured =
        apex::app::evaluateWorkspaceTrackCameraPlayback({0.25, 3.0, 4.0});
    require(configured.ok() && configured.position == 1.0 &&
                configured.finished,
            "configured playback stops at one");

    auto restarted =
        apex::app::evaluateWorkspaceTrackCameraPlayback({1.0, 0.5, -4.0});
    require(restarted.ok() && restarted.position == 1.0 &&
                restarted.finished,
            "play at the endpoint restarts and negative duration clamps to one millisecond");

    auto invalid = apex::app::evaluateWorkspaceTrackCameraPlayback(
        {0.0, std::numeric_limits<double>::quiet_NaN(), 1.0});
    require(!invalid.ok() &&
                invalid.code ==
                    "workspace_track_camera_playback_elapsed_invalid",
            "non-finite playback time is rejected");
}

void builds_recovered_installed_editor_spline() {
    const std::array<std::array<double, 3U>, 4U> open_points = {{
        {0.0, 0.0, 0.0},
        {100.0, 0.0, 0.0},
        {200.0, 0.0, 0.0},
        {300.0, 0.0, 0.0},
    }};
    auto open = apex::app::buildInstalledEditorTrackCameraSpline(open_points);
    require(open.ok(), "installed-editor open spline builds");
    require(!open.spline->closed,
            "endpoints farther than 75 world units select open topology");
    require(open.spline->cumulative_lengths.size() == open_points.size(),
            "installed-editor spline stores one cumulative length per point");
    require(open.spline->length > 299.0F && open.spline->length < 301.0F,
            "installed-editor length uses the recovered Catmull-Rom path");

    const std::array<std::array<double, 3U>, 4U> closed_points = {{
        {0.0, 0.0, 0.0},
        {100.0, 0.0, 0.0},
        {100.0, 0.0, 100.0},
        {0.0, 0.0, 1.0},
    }};
    auto closed =
        apex::app::buildInstalledEditorTrackCameraSpline(closed_points);
    require(closed.ok() && closed.spline->closed,
            "endpoints within 75 world units select closed topology");
    require(closed.spline->closing_length == 1.0F,
            "closed spline keeps the endpoint chord used by native lookup");
    require(closed.spline->length >
                closed.spline->cumulative_lengths.back() +
                    closed.spline->closing_length,
            "closed spline length includes the wrapped curve and the native "
            "endpoint-chord addition");
}

void builds_installed_editor_target_camera_frame() {
    auto camera = camera_fixture();
    camera.position = {900.0, 900.0, 900.0};
    camera.forward = {0.0, 0.0, -1.0};
    camera.min_fov = 20.0;
    camera.max_fov = 120.0;
    camera.spline_rotation = 90.0;
    const std::array<std::array<double, 3U>, 4U> points = {{
        {0.0, 0.0, 0.0},
        {100.0, 0.0, 0.0},
        {200.0, 0.0, 0.0},
        {300.0, 0.0, 0.0},
    }};
    auto spline = apex::app::buildInstalledEditorTrackCameraSpline(points);
    require(spline.ok(), "installed-editor frame spline builds");

    apex::app::InstalledEditorTrackCameraFrameRequest request;
    request.camera = &camera;
    request.spline = &*spline.spline;
    request.spline_position = 0.5F;
    request.viewport_aspect = 2.0F;
    request.clip_space = apex::render::CameraClipSpace::d3d12;
    auto frame = apex::app::buildInstalledEditorTrackCameraFrame(request);
    require(frame.ok(), "installed-editor target-facing frame builds");
    require_near(frame.frame->position[0], 149.9987F,
                 "arc-length Catmull-Rom sampling uses the endpoint factor");
    require_near(frame.frame->position[1], 0.0F,
                 "installed-editor spline points are absolute positions");
    require_near(frame.frame->forward[0], 1.0F,
                 "installed-editor camera targets the future spline point");
    require_near(frame.frame->up[1], 1.0F,
                 "installed-editor target basis keeps upward world orientation");
    require_near(frame.frame->fov_radians,
                 20.0F * 3.14159265358979323846F / 180.0F,
                 "installed-editor spline preview uses MIN_FOV");
    require(frame.frame->clip_space == apex::render::CameraClipSpace::d3d12,
            "installed-editor frame keeps backend clip space");

    request.spline_position = 0.0F;
    auto first = apex::app::buildInstalledEditorTrackCameraFrame(request);
    require(first.ok(), "installed-editor zero-position frame builds");
    require_near(first.frame->position[0], 0.0F,
                 "zero position uses the recovered first-point path");
}

void rejects_unsafe_installed_editor_splines() {
    const std::array<std::array<double, 3U>, 3U> short_points = {{
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
    }};
    auto short_spline =
        apex::app::buildInstalledEditorTrackCameraSpline(short_points);
    require(!short_spline.ok() &&
                short_spline.code ==
                    "installed_editor_track_camera_spline_too_short",
            "short Catmull-Rom spline is rejected safely");

    std::vector<std::array<double, 3U>> oversized(
        apex::app::installed_editor_track_camera_max_spline_points + 1U);
    auto point_limit =
        apex::app::buildInstalledEditorTrackCameraSpline(oversized);
    require(!point_limit.ok() &&
                point_limit.code ==
                    "installed_editor_track_camera_spline_limit",
            "installed-editor spline point budget is enforced before work");

    auto nonfinite_points = std::array<std::array<double, 3U>, 4U>{{
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
        {3.0, 0.0, 0.0},
    }};
    nonfinite_points[2][1] = std::numeric_limits<double>::infinity();
    auto nonfinite =
        apex::app::buildInstalledEditorTrackCameraSpline(nonfinite_points);
    require(!nonfinite.ok() &&
                nonfinite.code ==
                    "installed_editor_track_camera_spline_non_finite",
            "non-finite installed-editor spline point is rejected");

    const std::array<std::array<double, 3U>, 4U> repeated = {{
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
    }};
    auto zero_length =
        apex::app::buildInstalledEditorTrackCameraSpline(repeated);
    require(!zero_length.ok() &&
                zero_length.code ==
                    "installed_editor_track_camera_spline_length_invalid",
            "zero-length installed-editor spline is rejected");

    apex::app::InstalledEditorTrackCameraFrameRequest missing;
    auto missing_frame =
        apex::app::buildInstalledEditorTrackCameraFrame(missing);
    require(!missing_frame.ok() &&
                missing_frame.code ==
                    "installed_editor_track_camera_missing",
            "missing installed-editor camera record is rejected");
}

void evaluates_installed_editor_playback() {
    auto active = apex::app::evaluateInstalledEditorTrackCameraPlayback(
        {0.25F, 50.0, 0.5F, 100.0F});
    require(active.ok() && std::abs(active.position - 0.5) < 0.000001 &&
                !active.finished,
            "installed-editor playback advances by speed over spline length");

    auto stopped = apex::app::evaluateInstalledEditorTrackCameraPlayback(
        {0.25F, 150.0, 0.5F, 100.0F});
    require(stopped.ok() && stopped.position == 0.0 && stopped.finished,
            "installed-editor playback resets to zero at the endpoint");

    auto invalid = apex::app::evaluateInstalledEditorTrackCameraPlayback(
        {0.0F, 1.0, 0.5F, 0.0F});
    require(!invalid.ok() &&
                invalid.code ==
                    "installed_editor_track_camera_playback_length_invalid",
            "installed-editor playback rejects native divide-by-zero input");
}

} // namespace

int main() {
    try {
        builds_saved_and_spline_camera_frames();
        clamps_production_fov_and_rejects_unsafe_values();
        evaluates_one_shot_playback();
        builds_recovered_installed_editor_spline();
        builds_installed_editor_target_camera_frame();
        rejects_unsafe_installed_editor_splines();
        evaluates_installed_editor_playback();
        std::cout << "workspace track camera tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace track camera tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
