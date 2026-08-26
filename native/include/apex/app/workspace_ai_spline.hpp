#pragma once

#include "apex/formats/ai_spline.hpp"
#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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
inline constexpr std::array<float, 3U>
    workspace_ai_spline_selection_color = {3.0F, 3.0F, 0.0F};
inline constexpr std::array<float, 3U>
    workspace_ai_spline_selection_side_color = {0.0F, 3.0F, 3.0F};
inline constexpr std::array<float, 3U>
    workspace_ai_spline_temporary_color = {0.0F, 3.0F, 0.0F};
inline constexpr std::array<float, 3U>
    workspace_ai_spline_temporary_forward_color = {3.0F, 0.0F, 0.0F};
inline constexpr float workspace_ai_spline_camber_height_scale = 1'000.0F;
inline constexpr float workspace_ai_spline_selection_height = 40.0F;
inline constexpr float workspace_ai_spline_selection_side_half_height = 20.0F;
// The installed temporary marker is an owned graphics object. The portable
// line ABI represents it with one short vertical line at the picked point.
inline constexpr float workspace_ai_spline_temporary_marker_height = 2.0F;
inline constexpr std::size_t workspace_ai_spline_pass_count = 8U;
inline constexpr float workspace_ai_spline_interpolation_step = 0.0002F;
inline constexpr std::uint32_t
    workspace_ai_spline_interpolated_sample_count = 5'001U;
inline constexpr std::size_t
    workspace_ai_spline_max_interpolation_control_points = 65'536U;
inline constexpr std::size_t workspace_ai_spline_max_selection_points =
    render::max_overlay_line_total_vertices / 2U;
// This safety bound limits hostile click streams and the O(control-count)
// length work for every temporary spline segment.
inline constexpr std::size_t workspace_ai_spline_max_temporary_edit_points =
    1'024U;
inline constexpr float workspace_ai_spline_closest_initial_distance_squared =
    9'999'999.0F;

enum class WorkspaceAiSplineDisplayMode : std::uint8_t {
    raw,
    interpolated,
};

enum class WorkspaceAiSplinePassKind : std::uint8_t {
    primary,
    interval,
    left_side,
    right_side,
    selection,
    temporary_interpolation,
    temporary_markers,
    camber,
};

enum class WorkspaceAiSplineTopology : std::uint8_t {
    polyline,
    independent_lines,
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
    std::uint32_t selected_point_count = 0U;
    std::uint32_t temporary_point_count = 0U;
    std::optional<std::uint32_t> last_selected_index;
    WorkspaceAiSplineDisplayMode mode = WorkspaceAiSplineDisplayMode::raw;
    WorkspaceAiSplinePassKind pass = WorkspaceAiSplinePassKind::primary;
    WorkspaceAiSplineTopology topology = WorkspaceAiSplineTopology::polyline;
    std::vector<render::OverlayLineVertex> vertices;
    std::vector<WorkspaceAiSplineChunk> chunks;
};

struct WorkspaceAiSplineTemporaryEditPoint {
    std::array<float, 3U> forward{};
    std::array<float, 3U> position{};

    friend bool operator==(const WorkspaceAiSplineTemporaryEditPoint&,
                           const WorkspaceAiSplineTemporaryEditPoint&) =
        default;
};

struct WorkspaceAiSplineResult {
    WorkspaceAiSplineStatus status = WorkspaceAiSplineStatus::invalid_source;
    render::Diagnostic diagnostic;
    WorkspaceAiSplineGeometry geometry;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineStatus::ready;
    }
};

struct WorkspaceAiSplineOverlayRequest {
    WorkspaceAiSplineDisplayMode mode = WorkspaceAiSplineDisplayMode::raw;
    std::optional<WorkspaceAiSplineInterval> interval;
    bool show_left = false;
    bool show_right = false;
    std::span<const std::uint32_t> selected_indices{};
    std::span<const WorkspaceAiSplineTemporaryEditPoint>
        temporary_edit_points{};
    std::optional<std::size_t> movable_temporary_point;
    bool show_camber = false;
};

struct WorkspaceAiSplineClosestPointLimits {
    std::size_t max_points = 1'000'000U;
    std::size_t max_grid_rows = 1'000'000U;
    std::size_t max_grid_cells = 1'000'000U;
    std::size_t max_grid_indices = 10'000'000U;
    std::size_t max_grid_neighbors = 1'000'000U;
};

struct WorkspaceAiSplineClosestPointResult {
    WorkspaceAiSplineStatus status = WorkspaceAiSplineStatus::invalid_source;
    render::Diagnostic diagnostic;
    std::uint32_t point_index = 0U;
    bool used_grid = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineStatus::ready;
    }
};

// Resolve the recovered InterpolatingSpline::closestPointIndex result. Empty
// splines and non-finite queries return index zero. A valid in-range grid cell
// restricts the 3D scan to its stored candidate order.
[[nodiscard]] WorkspaceAiSplineClosestPointResult
resolveWorkspaceAiSplineClosestPoint(
    const formats::AiSpline& spline,
    const std::array<float, 3U>& query,
    const WorkspaceAiSplineClosestPointLimits& limits = {});

// One owned generation of every enabled spline pass. Building this set from
// one model snapshot prevents derived passes from observing mixed revisions.
struct WorkspaceAiSplineOverlaySet {
    WorkspaceAiSplineGeometry primary;
    std::optional<WorkspaceAiSplineGeometry> interval;
    std::optional<WorkspaceAiSplineGeometry> left;
    std::optional<WorkspaceAiSplineGeometry> right;
    std::optional<WorkspaceAiSplineGeometry> selection;
    std::optional<WorkspaceAiSplineGeometry> temporaryInterpolation;
    std::optional<WorkspaceAiSplineGeometry> temporaryMarkers;
    std::optional<WorkspaceAiSplineGeometry> camber;
};

struct WorkspaceAiSplineOverlayResult {
    WorkspaceAiSplineStatus status = WorkspaceAiSplineStatus::invalid_source;
    render::Diagnostic diagnostic;
    WorkspaceAiSplineOverlaySet overlays;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineStatus::ready;
    }
};

[[nodiscard]] WorkspaceAiSplineOverlayResult
buildWorkspaceAiSplineOverlays(
    const formats::AiSpline& spline,
    const WorkspaceAiSplineOverlayRequest& request);

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

// Build the recovered markers for an ordered selected-index vector. Repeated
// indices are ignored, as in SplineEditor::addIndex. Each selected point emits
// a yellow center line and optional cyan side-width lines. The final unique
// index supplies the native normalized pick/UI return.
[[nodiscard]] WorkspaceAiSplineResult buildWorkspaceAiSplineSelectionGeometry(
    const formats::AiSpline& spline,
    std::span<const std::uint32_t> selected_indices);

// Keep the original one-index entry point for callers that do not manage a
// selected-index vector.
[[nodiscard]] WorkspaceAiSplineResult buildWorkspaceAiSplineSelectionGeometry(
    const formats::AiSpline& spline, std::uint32_t selected_index);

// Build the source-matched green temporary spline after five edit records.
// The first and final selected entries are the committed endpoint controls.
[[nodiscard]] WorkspaceAiSplineResult
buildWorkspaceAiSplineTemporaryInterpolationGeometry(
    const formats::AiSpline& spline,
    std::span<const std::uint32_t> selected_indices,
    std::span<const WorkspaceAiSplineTemporaryEditPoint> temporary_points);

// Represent each native temporary indicator object with a bounded green line.
// This is a labeled portable translation for the shared Vulkan/D3D12 ABI.
[[nodiscard]] WorkspaceAiSplineResult
buildWorkspaceAiSplineTemporaryMarkerGeometry(
    const formats::AiSpline& spline,
    std::span<const WorkspaceAiSplineTemporaryEditPoint> temporary_points,
    std::optional<std::size_t> movable_point = std::nullopt);

[[nodiscard]] const char* workspace_ai_spline_display_mode_name(
    WorkspaceAiSplineDisplayMode mode) noexcept;

[[nodiscard]] const char*
workspace_ai_spline_status_name(WorkspaceAiSplineStatus status) noexcept;

} // namespace apex::app
