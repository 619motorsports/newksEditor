#include "apex/app/workspace_ai_spline.hpp"

#include "apex/app/installed_editor_spline.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <span>

namespace apex::app {
namespace {

render::Diagnostic diagnostic(const char* code, const char* message) {
    return {code, message};
}

bool finite_position(const std::array<float, 3U>& position) noexcept {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

void build_line_list(WorkspaceAiSplineGeometry& geometry,
                     std::span<const std::array<float, 3U>> positions) {
    geometry.sample_point_count =
        static_cast<std::uint32_t>(positions.size());
    // GLRenderer::spline returns without drawing for zero, one, or two
    // points. Keep that observable behavior even though two points could
    // form one line segment.
    if (positions.size() <= 2U) return;

    const std::size_t segment_count = positions.size() - 1U;
    const std::size_t vertex_count = segment_count * 2U;
    const std::size_t segments_per_chunk =
        render::max_overlay_line_vertices / 2U;
    const std::size_t chunk_count =
        (segment_count + segments_per_chunk - 1U) / segments_per_chunk;
    geometry.vertices.reserve(vertex_count);
    geometry.chunks.reserve(chunk_count);
    for (std::size_t first_segment = 0U; first_segment < segment_count;
         first_segment += segments_per_chunk) {
        const std::size_t current_segments =
            std::min(segments_per_chunk, segment_count - first_segment);
        const std::size_t first_vertex = geometry.vertices.size();
        for (std::size_t segment = first_segment;
             segment < first_segment + current_segments; ++segment) {
            geometry.vertices.push_back(
                {positions[segment], workspace_ai_spline_raw_color});
            geometry.vertices.push_back(
                {positions[segment + 1U], workspace_ai_spline_raw_color});
        }
        geometry.chunks.push_back(
            {static_cast<std::uint32_t>(first_vertex),
             static_cast<std::uint32_t>(current_segments * 2U)});
    }
}

} // namespace

WorkspaceAiSplineResult
buildWorkspaceAiSplineGeometry(const formats::AiSpline& spline,
                               WorkspaceAiSplineDisplayMode mode) {
    WorkspaceAiSplineResult result;
    try {
        if (mode != WorkspaceAiSplineDisplayMode::raw &&
            mode != WorkspaceAiSplineDisplayMode::interpolated) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_mode_invalid",
                "AI spline display mode is invalid");
            return result;
        }

        std::size_t source_point_count = 0U;
        if (spline.version == 7U) {
            source_point_count = spline.points.size();
        } else if (spline.version == 2U) {
            source_point_count = spline.nativeRetainedIndices.size();
        } else {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic =
                diagnostic("workspace_ai_spline_version_unsupported",
                           "AI spline rendering supports native file "
                           "versions 2 and 7");
            return result;
        }
        const std::size_t max_source_points =
            mode == WorkspaceAiSplineDisplayMode::raw
                ? render::max_overlay_line_total_vertices / 2U + 1U
                : workspace_ai_spline_max_interpolation_control_points;
        if (source_point_count > max_source_points) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_vertex_limit",
                mode == WorkspaceAiSplineDisplayMode::raw
                    ? "AI spline geometry exceeds the bounded overlay vertex "
                      "limit"
                    : "Interpolated AI spline exceeds the bounded "
                      "control-point limit");
            return result;
        }

        std::vector<std::array<float, 3U>> positions;
        if (spline.version == 7U) {
            positions.reserve(spline.points.size());
            for (const auto& point : spline.points)
                positions.push_back(point.position);
        } else {
            positions.reserve(spline.nativeRetainedIndices.size());
            for (const std::uint32_t source_index :
                 spline.nativeRetainedIndices) {
                if (static_cast<std::size_t>(source_index) >=
                    spline.legacyV2Records.size()) {
                    result.status = WorkspaceAiSplineStatus::invalid_source;
                    result.diagnostic =
                        diagnostic("workspace_ai_spline_v2_index_invalid",
                                   "A retained legacy AI spline point is "
                                   "outside the source records");
                    return result;
                }
                positions.push_back(
                    spline
                        .legacyV2Records[static_cast<std::size_t>(source_index)]
                        .position);
            }
        }

        result.geometry.source_point_count =
            static_cast<std::uint32_t>(positions.size());
        result.geometry.mode = mode;
        for (const auto& position : positions) {
            if (!finite_position(position)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_position_non_finite",
                    "AI spline rendering requires finite point coordinates");
                return result;
            }
        }

        if (mode == WorkspaceAiSplineDisplayMode::raw) {
            build_line_list(result.geometry, positions);
        } else {
            if (positions.size() < 4U) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_interpolation_too_short",
                    "Installed-editor AI spline interpolation requires at "
                    "least four control points");
                return result;
            }
            InstalledEditorSpline interpolating;
            interpolating.points = std::move(positions);
            interpolating.closed =
                installedEditorSplineIsClosed(interpolating.points);
            if (!recomputeInstalledEditorSplineLengths(interpolating)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_interpolation_length_invalid",
                    "Installed-editor AI spline interpolation requires a "
                    "positive finite path and endpoint chord");
                return result;
            }

            std::vector<std::array<float, 3U>> samples;
            samples.reserve(workspace_ai_spline_interpolated_sample_count);
            for (float position = 0.0F; position <= 1.0F;
                 position += workspace_ai_spline_interpolation_step) {
                const auto sample = sampleInstalledEditorSpline(
                    interpolating, position);
                if (!sample.has_value()) {
                    result.status = WorkspaceAiSplineStatus::invalid_source;
                    result.diagnostic = diagnostic(
                        "workspace_ai_spline_interpolation_sample_invalid",
                        "Installed-editor AI spline interpolation produced an "
                        "invalid sample");
                    return result;
                }
                samples.push_back(*sample);
            }
            if (samples.size() !=
                workspace_ai_spline_interpolated_sample_count) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_interpolation_schedule_invalid",
                    "The platform did not reproduce the recovered float "
                    "sampling schedule");
                return result;
            }
            build_line_list(result.geometry, samples);
        }
        if (result.geometry.chunks.size() >
                render::max_overlay_line_draws ||
            result.geometry.vertices.size() >
                render::max_overlay_line_total_vertices) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_draw_limit",
                "AI spline geometry exceeds the bounded overlay draw limit");
            result.geometry = {};
            return result;
        }
        result.status = WorkspaceAiSplineStatus::ready;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceAiSplineStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_allocation_failed",
            "AI spline geometry exceeded available allocation capacity");
        result.geometry = {};
        return result;
    }
}

const char* workspace_ai_spline_display_mode_name(
    WorkspaceAiSplineDisplayMode mode) noexcept {
    switch (mode) {
    case WorkspaceAiSplineDisplayMode::raw: return "raw";
    case WorkspaceAiSplineDisplayMode::interpolated: return "interpolated";
    }
    return "unknown";
}

const char*
workspace_ai_spline_status_name(WorkspaceAiSplineStatus status) noexcept {
    switch (status) {
    case WorkspaceAiSplineStatus::ready:
        return "ready";
    case WorkspaceAiSplineStatus::invalid_source:
        return "invalid_source";
    case WorkspaceAiSplineStatus::limit_exceeded:
        return "limit_exceeded";
    case WorkspaceAiSplineStatus::allocation_failed:
        return "allocation_failed";
    }
    return "invalid_source";
}

} // namespace apex::app
