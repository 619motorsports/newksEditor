#pragma once

#include "apex/formats/ai_spline.hpp"
#include "apex/render/device.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace apex::app {

inline constexpr std::array<float, 3U> workspace_ai_spline_raw_color = {
    3.0F, 0.0F, 3.0F};
inline constexpr float workspace_ai_spline_interpolation_step = 0.0002F;
inline constexpr std::uint32_t
    workspace_ai_spline_interpolated_sample_count = 5'001U;
inline constexpr std::size_t
    workspace_ai_spline_max_interpolation_control_points = 65'536U;

enum class WorkspaceAiSplineDisplayMode : std::uint8_t {
    raw,
    interpolated,
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

[[nodiscard]] const char* workspace_ai_spline_display_mode_name(
    WorkspaceAiSplineDisplayMode mode) noexcept;

[[nodiscard]] const char*
workspace_ai_spline_status_name(WorkspaceAiSplineStatus status) noexcept;

} // namespace apex::app
