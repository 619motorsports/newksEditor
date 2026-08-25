#pragma once

#include "apex/domain/track_data.hpp"
#include "apex/render/camera.hpp"

#include <array>
#include <span>
#include <string>

namespace apex::app {

inline constexpr double workspace_track_camera_default_animation_seconds =
    15.0;
inline constexpr double workspace_track_camera_minimum_animation_seconds =
    0.001;

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

} // namespace apex::app
