#pragma once

#include "apex/formats/ai_spline.hpp"
#include "apex/render/device.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace apex::app {

inline constexpr std::array<float, 3U> workspace_ai_spline_raw_color = {
    3.0F, 0.0F, 3.0F};

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
buildWorkspaceAiSplineGeometry(const formats::AiSpline& spline);

[[nodiscard]] const char*
workspace_ai_spline_status_name(WorkspaceAiSplineStatus status) noexcept;

} // namespace apex::app
