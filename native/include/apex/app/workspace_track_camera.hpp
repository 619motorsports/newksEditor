#pragma once

#include "apex/app/installed_editor_spline.hpp"
#include "apex/domain/track_data.hpp"
#include "apex/render/camera.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::app {

inline constexpr double workspace_track_camera_default_animation_seconds =
    15.0;
inline constexpr double workspace_track_camera_minimum_animation_seconds =
    0.001;
inline constexpr float installed_editor_track_camera_default_speed = 0.5F;
inline constexpr float installed_editor_track_camera_default_lookahead = 1.0F;
inline constexpr float installed_editor_track_camera_endpoint_factor =
    0.9999899864196777F;
inline constexpr float installed_editor_track_camera_closure_distance =
    installed_editor_spline_closure_distance;
inline constexpr float installed_editor_track_camera_length_step =
    installed_editor_spline_length_step;
inline constexpr std::size_t
    installed_editor_track_camera_max_spline_points = 65'536U;

enum class TrackCameraPreviewMode {
    webgl,
    installed_editor,
};

[[nodiscard]] const char* track_camera_preview_mode_name(
    TrackCameraPreviewMode mode) noexcept;

struct WorkspaceTrackCameraFrameRequest {
    const domain::CameraData* camera = nullptr;
    // Rotated spline offsets. An empty span selects the saved camera position.
    std::span<const std::array<double, 3U>> spline_points{};
    double spline_position = 0.0;
    float viewport_aspect = 1.0F;
    render::CameraClipSpace clip_space = render::CameraClipSpace::webgl;
};

// Build the production WebGL editor's saved-basis track-camera preview. A
// resolved spline changes only the eye position and selects MIN_FOV. This
// does not claim the installed editor or game's target-facing behavior.
[[nodiscard]] render::CameraFrameResult buildWorkspaceTrackCameraFrame(
    const WorkspaceTrackCameraFrameRequest& request);

using InstalledEditorTrackCameraSpline = InstalledEditorSpline;

struct InstalledEditorTrackCameraSplineResult {
    std::optional<InstalledEditorTrackCameraSpline> spline;
    std::string code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return spline.has_value(); }
};

// Convert bounded CSV points to the installed editor's inferred-topology,
// Catmull-Rom spline. The editor approximates each segment length at 0.001
// parameter intervals. This safe adapter rejects native divide-by-zero cases.
[[nodiscard]] InstalledEditorTrackCameraSplineResult
buildInstalledEditorTrackCameraSpline(
    std::span<const std::array<double, 3U>> points);

struct InstalledEditorTrackCameraFrameRequest {
    const domain::CameraData* camera = nullptr;
    const InstalledEditorTrackCameraSpline* spline = nullptr;
    float spline_position = 0.0F;
    float lookahead_world_units =
        installed_editor_track_camera_default_lookahead;
    float viewport_aspect = 1.0F;
    render::CameraClipSpace clip_space = render::CameraClipSpace::webgl;
};

// Build the installed editor's target-facing camera-spline preview. Spline
// points are absolute world positions. The path does not apply SPLINE_ROTATION
// or add the saved camera position.
[[nodiscard]] render::CameraFrameResult
buildInstalledEditorTrackCameraFrame(
    const InstalledEditorTrackCameraFrameRequest& request);

struct WorkspaceTrackCameraPlaybackRequest {
    double start_position = 0.0;
    double elapsed_seconds = 0.0;
    double animation_length_seconds = 0.0;
};

struct WorkspaceTrackCameraPlaybackResult {
    double position = 0.0;
    bool finished = false;
    std::string code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code.empty(); }
};

// Evaluate one-shot playback without owning a clock. A start position of one
// restarts at zero, like the production WebGL Play control.
[[nodiscard]] WorkspaceTrackCameraPlaybackResult
evaluateWorkspaceTrackCameraPlayback(
    const WorkspaceTrackCameraPlaybackRequest& request) noexcept;

struct InstalledEditorTrackCameraPlaybackRequest {
    float start_position = 0.0F;
    double elapsed_seconds = 0.0;
    float speed_multiplier = installed_editor_track_camera_default_speed;
    float spline_length = 0.0F;
};

// Evaluate the recovered installed-editor update without owning a clock. At
// the endpoint, the editor stops playback and resets the spline position.
[[nodiscard]] WorkspaceTrackCameraPlaybackResult
evaluateInstalledEditorTrackCameraPlayback(
    const InstalledEditorTrackCameraPlaybackRequest& request) noexcept;

} // namespace apex::app
