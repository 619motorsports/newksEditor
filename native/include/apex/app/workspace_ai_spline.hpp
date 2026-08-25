#pragma once

#include "apex/formats/ai_spline.hpp"
#include "apex/render/device.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace apex::app {

inline constexpr std::array<float, 3U> workspace_ai_spline_raw_color = {
    3.0F, 0.0F, 3.0F};
inline constexpr std::array<float, 3U> workspace_ai_spline_interval_color = {
    0.0F, 0.0F, 3.0F};
inline constexpr std::array<float, 3U> workspace_ai_spline_side_color = {
    0.0F, 3.0F, 3.0F};
inline constexpr std::array<float, 3U>
    workspace_ai_spline_camber_positive_color = {0.0F, 3.0F, 0.0F};
inline constexpr std::array<float, 3U>
    workspace_ai_spline_camber_nonpositive_color = {3.0F, 0.0F, 0.0F};
inline constexpr float workspace_ai_spline_camber_height_scale = 1'000.0F;
inline constexpr std::size_t workspace_ai_spline_pass_count = 5U;
inline constexpr float workspace_ai_spline_interpolation_step = 0.0002F;
inline constexpr std::uint32_t
    workspace_ai_spline_interpolated_sample_count = 5'001U;
inline constexpr std::size_t
    workspace_ai_spline_max_interpolation_control_points = 65'536U;

enum class WorkspaceAiSplineDisplayMode : std::uint8_t {
    raw,
    interpolated,
};

enum class WorkspaceAiSplinePassKind : std::uint8_t {
    primary,
    interval,
    left_side,
    right_side,
    camber,
};

enum class WorkspaceAiSplineSide : std::uint8_t {
    left,
    right,
};

struct WorkspaceAiSplineInterval {
    float begin = 0.0F;
    float end = 1.0F;
};

enum class WorkspaceAiSplineStatus : std::uint8_t {
    ready,
    invalid_source,
    limit_exceeded,
    allocation_failed,
};

struct WorkspaceAiSplineChunk {
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
};

struct WorkspaceAiSplineGeometry {
    std::uint32_t source_point_count = 0U;
    std::uint32_t sample_point_count = 0U;
    WorkspaceAiSplineDisplayMode mode = WorkspaceAiSplineDisplayMode::raw;
    WorkspaceAiSplinePassKind pass = WorkspaceAiSplinePassKind::primary;
    std::vector<render::OverlayLineVertex> vertices;
    std::vector<WorkspaceAiSplineChunk> chunks;
};

struct WorkspaceAiSplineResult {
    WorkspaceAiSplineStatus status = WorkspaceAiSplineStatus::invalid_source;
    render::Diagnostic diagnostic;
    WorkspaceAiSplineGeometry geometry;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineStatus::ready;
    }
};

// Convert the recovered raw SplineEditor polyline to the backend-neutral
// line-list ABI. Version 2 uses the points retained by the native AISpline
// loader. The original <= 2 point early return and open-polyline behavior are
// preserved. Chunking is a labeled portable translation: it preserves every
// source segment while the recovered OpenGL helper used line strips.
[[nodiscard]] WorkspaceAiSplineResult
buildWorkspaceAiSplineGeometry(
    const formats::AiSpline& spline,
    WorkspaceAiSplineDisplayMode mode = WorkspaceAiSplineDisplayMode::raw);

// Build the recovered blue in/out interval pass. The installed editor always
// interpolates this pass, independently of the primary spline display mode.
// Values are normalized spline positions and must be finite, ordered, and in
// the inclusive range [0, 1].
[[nodiscard]] WorkspaceAiSplineResult buildWorkspaceAiSplineIntervalGeometry(
    const formats::AiSpline& spline, WorkspaceAiSplineInterval interval);

// Build one recovered version-7 side spline. The installed editor uses the
// horizontal cross(tangent, up) basis and skips both side points when the
// left width is zero. Side splines remain raw, even when the primary display
// uses interpolation.
[[nodiscard]] WorkspaceAiSplineResult buildWorkspaceAiSplineSideGeometry(
    const formats::AiSpline& spline, WorkspaceAiSplineSide side);

// Build the recovered vertical camber lines. Each version-7 source point
// emits one independent line. Positive camber is green. Zero and negative
// camber are red. Height is abs(camber) times 1,000.
[[nodiscard]] WorkspaceAiSplineResult buildWorkspaceAiSplineCamberGeometry(
    const formats::AiSpline& spline);

[[nodiscard]] const char* workspace_ai_spline_display_mode_name(
    WorkspaceAiSplineDisplayMode mode) noexcept;

[[nodiscard]] const char*
workspace_ai_spline_status_name(WorkspaceAiSplineStatus status) noexcept;

} // namespace apex::app
