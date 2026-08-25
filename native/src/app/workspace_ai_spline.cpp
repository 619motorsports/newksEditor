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

const formats::AiSplinePayload* payload_for_point(
    const formats::AiSpline& spline,
    const formats::AiSplinePoint& point) noexcept {
    if (point.tag < 0 ||
        static_cast<std::size_t>(point.tag) >= spline.payloads.size())
        return nullptr;
    return &spline.payloads[static_cast<std::size_t>(point.tag)];
}

void build_line_list(WorkspaceAiSplineGeometry& geometry,
                     std::span<const std::array<float, 3U>> positions,
                     const std::array<float, 3U>& color) {
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
                {positions[segment], color});
            geometry.vertices.push_back(
                {positions[segment + 1U], color});
        }
        geometry.chunks.push_back(
            {static_cast<std::uint32_t>(first_vertex),
             static_cast<std::uint32_t>(current_segments * 2U)});
    }
}

void build_independent_line_chunks(WorkspaceAiSplineGeometry& geometry) {
    geometry.sample_point_count =
        static_cast<std::uint32_t>(geometry.vertices.size() / 2U);
    const std::size_t vertices_per_chunk =
        render::max_overlay_line_vertices -
        render::max_overlay_line_vertices % 2U;
    geometry.chunks.reserve(
        (geometry.vertices.size() + vertices_per_chunk - 1U) /
        vertices_per_chunk);
    for (std::size_t first_vertex = 0U;
         first_vertex < geometry.vertices.size();
         first_vertex += vertices_per_chunk) {
        const std::size_t vertex_count = std::min(
            vertices_per_chunk, geometry.vertices.size() - first_vertex);
        geometry.chunks.push_back(
            {static_cast<std::uint32_t>(first_vertex),
             static_cast<std::uint32_t>(vertex_count)});
    }
}

WorkspaceAiSplineResult build_geometry(
    const formats::AiSpline& spline, WorkspaceAiSplineDisplayMode mode,
    WorkspaceAiSplinePassKind pass,
    const std::optional<WorkspaceAiSplineInterval>& interval) {
    WorkspaceAiSplineResult result;
    try {
        if ((mode != WorkspaceAiSplineDisplayMode::raw &&
             mode != WorkspaceAiSplineDisplayMode::interpolated) ||
            (pass != WorkspaceAiSplinePassKind::primary &&
             pass != WorkspaceAiSplinePassKind::interval) ||
            (interval.has_value() !=
             (pass == WorkspaceAiSplinePassKind::interval))) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_mode_invalid",
                "AI spline display mode is invalid");
            return result;
        }
        if (interval.has_value() &&
            (!std::isfinite(interval->begin) ||
             !std::isfinite(interval->end) || interval->begin < 0.0F ||
             interval->end > 1.0F || interval->begin > interval->end)) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_interval_invalid",
                "AI spline interval must be finite, ordered, and from zero to one");
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
            mode == WorkspaceAiSplineDisplayMode::raw &&
                    pass == WorkspaceAiSplinePassKind::primary
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
        result.geometry.pass = pass;
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
            build_line_list(result.geometry, positions,
                            workspace_ai_spline_raw_color);
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
            const float begin = interval.has_value() ? interval->begin : 0.0F;
            const float end = interval.has_value() ? interval->end : 1.0F;
            for (float position = begin; position <= end;
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
                if (samples.size() >
                    workspace_ai_spline_interpolated_sample_count) {
                    result.status = WorkspaceAiSplineStatus::limit_exceeded;
                    result.diagnostic = diagnostic(
                        "workspace_ai_spline_interval_sample_limit",
                        "AI spline interval exceeds the recovered sample limit");
                    return result;
                }
            }
            if (!interval.has_value() && samples.size() !=
                workspace_ai_spline_interpolated_sample_count) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_interpolation_schedule_invalid",
                    "The platform did not reproduce the recovered float "
                    "sampling schedule");
                return result;
            }
            build_line_list(result.geometry, samples,
                            interval.has_value()
                                ? workspace_ai_spline_interval_color
                                : workspace_ai_spline_raw_color);
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

WorkspaceAiSplineResult build_side_geometry(
    const formats::AiSpline& spline, WorkspaceAiSplineSide side) {
    WorkspaceAiSplineResult result;
    try {
        if (side != WorkspaceAiSplineSide::left &&
            side != WorkspaceAiSplineSide::right) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_side_invalid",
                "AI spline side selection is invalid");
            return result;
        }
        if (spline.version != 7U) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_side_version_unsupported",
                "AI spline side overlays require version-7 payloads");
            return result;
        }
        if (spline.points.size() != spline.payloads.size()) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_side_payload_count_invalid",
                "AI spline side overlays require one payload per point");
            return result;
        }
        if (spline.points.size() >
            render::max_overlay_line_total_vertices / 2U + 1U) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_side_vertex_limit",
                "AI spline side geometry exceeds the bounded overlay vertex limit");
            return result;
        }

        result.geometry.source_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.mode = WorkspaceAiSplineDisplayMode::raw;
        result.geometry.pass = side == WorkspaceAiSplineSide::left
                                   ? WorkspaceAiSplinePassKind::left_side
                                   : WorkspaceAiSplinePassKind::right_side;

        std::vector<std::array<float, 3U>> positions;
        positions.reserve(spline.points.size());
        const bool closed =
            spline.points.size() >= 2U &&
            installedEditorSplinePointDistance(spline.points.back().position,
                                               spline.points.front().position) <=
                installed_editor_spline_closure_distance;
        for (std::size_t index = 0U; index < spline.points.size(); ++index) {
            const auto& source_point = spline.points[index];
            const auto& point = source_point.position;
            const auto* payload = payload_for_point(spline, source_point);
            if (payload == nullptr) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_side_payload_index_invalid",
                    "AI spline side point tag is outside the payload array");
                return result;
            }
            if (!finite_position(point) || !std::isfinite(payload->side0) ||
                !std::isfinite(payload->side1)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_side_source_non_finite",
                    "AI spline side overlays require finite points and widths");
                return result;
            }
            if (payload->side0 == 0.0F) continue;

            const std::size_t next_index =
                index + 1U < spline.points.size()
                    ? index + 1U
                    : (closed && !spline.points.empty() ? 0U : index);
            const auto& next = spline.points[next_index].position;
            if (!finite_position(next)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_side_source_non_finite",
                    "AI spline side overlays require finite points and widths");
                return result;
            }
            float delta_x = next[0] - point[0];
            float delta_z = next[2] - point[2];
            const float length =
                std::sqrt(delta_x * delta_x + delta_z * delta_z);
            if (length != 0.0F) {
                delta_x /= length;
                delta_z /= length;
            }
            const std::array<float, 3U> lateral = {-delta_z, 0.0F, delta_x};
            const float width = side == WorkspaceAiSplineSide::left
                                    ? -payload->side0
                                    : payload->side1;
            std::array<float, 3U> generated = {
                point[0] + lateral[0] * width,
                point[1] + lateral[1] * width,
                point[2] + lateral[2] * width,
            };
            if (!finite_position(generated)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_side_derived_non_finite",
                    "AI spline side offset produced a non-finite point");
                return result;
            }
            positions.push_back(generated);
        }
        build_line_list(result.geometry, positions,
                        workspace_ai_spline_side_color);
        if (result.geometry.chunks.size() > render::max_overlay_line_draws ||
            result.geometry.vertices.size() >
                render::max_overlay_line_total_vertices) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_side_draw_limit",
                "AI spline side geometry exceeds the bounded overlay draw limit");
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

WorkspaceAiSplineResult build_camber_geometry(
    const formats::AiSpline& spline) {
    WorkspaceAiSplineResult result;
    try {
        if (spline.version != 7U) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_camber_version_unsupported",
                "AI spline camber overlay requires version-7 payloads");
            return result;
        }
        if (spline.points.size() != spline.payloads.size()) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_camber_payload_count_invalid",
                "AI spline camber overlay requires one payload per point");
            return result;
        }
        if (spline.points.size() >
            render::max_overlay_line_total_vertices / 2U) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_camber_vertex_limit",
                "AI spline camber geometry exceeds the bounded overlay vertex limit");
            return result;
        }

        result.geometry.source_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.sample_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.mode = WorkspaceAiSplineDisplayMode::raw;
        result.geometry.pass = WorkspaceAiSplinePassKind::camber;
        result.geometry.topology =
            WorkspaceAiSplineTopology::independent_lines;
        result.geometry.vertices.reserve(spline.points.size() * 2U);
        for (std::size_t index = 0U; index < spline.points.size(); ++index) {
            const auto& source_point = spline.points[index];
            const auto* payload = payload_for_point(spline, source_point);
            if (payload == nullptr) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_camber_payload_index_invalid",
                    "AI spline camber point tag is outside the payload array");
                result.geometry = {};
                return result;
            }
            if (!finite_position(source_point.position) ||
                !std::isfinite(payload->camber)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_camber_source_non_finite",
                    "AI spline camber overlay requires finite points and values");
                result.geometry = {};
                return result;
            }
            const auto& color =
                payload->camber > 0.0F
                    ? workspace_ai_spline_camber_positive_color
                    : workspace_ai_spline_camber_nonpositive_color;
            const float height = std::abs(payload->camber) *
                                 workspace_ai_spline_camber_height_scale;
            auto endpoint = source_point.position;
            endpoint[1] += height;
            if (!std::isfinite(height) || !finite_position(endpoint)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_camber_derived_non_finite",
                    "AI spline camber height produced a non-finite point");
                result.geometry = {};
                return result;
            }
            result.geometry.vertices.push_back(
                {source_point.position, color});
            result.geometry.vertices.push_back({endpoint, color});
        }
        build_independent_line_chunks(result.geometry);
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

WorkspaceAiSplineResult build_selection_geometry(
    const formats::AiSpline& spline, std::uint32_t selected_index) {
    WorkspaceAiSplineResult result;
    try {
        if (spline.version != 7U) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_version_unsupported",
                "AI spline current-index markers require version-7 payloads");
            return result;
        }
        if (spline.points.size() != spline.payloads.size()) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_payload_count_invalid",
                "AI spline current-index markers require one payload per point");
            return result;
        }
        if (spline.points.size() >
            workspace_ai_spline_max_interpolation_control_points) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_source_limit",
                "AI spline current-index marker source exceeds the bounded "
                "point limit");
            return result;
        }
        if (static_cast<std::size_t>(selected_index) >= spline.points.size()) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_index_invalid",
                "AI spline current index is outside the retained points");
            return result;
        }

        const auto& source_point =
            spline.points[static_cast<std::size_t>(selected_index)];
        const auto* payload = payload_for_point(spline, source_point);
        if (payload == nullptr) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_payload_index_invalid",
                "AI spline current point tag is outside the payload array");
            return result;
        }
        const auto& point = source_point.position;
        const bool closed =
            spline.points.size() >= 2U &&
            installedEditorSplinePointDistance(
                spline.points.back().position,
                spline.points.front().position) <=
                installed_editor_spline_closure_distance;
        const std::size_t selected =
            static_cast<std::size_t>(selected_index);
        const std::size_t next_index =
            selected + 1U < spline.points.size()
                ? selected + 1U
                : (closed ? 0U : selected);
        const auto& next = spline.points[next_index].position;
        if (!finite_position(point) || !finite_position(next) ||
            !std::isfinite(payload->side0) ||
            !std::isfinite(payload->side1)) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_source_non_finite",
                "AI spline current-index markers require finite points and "
                "widths");
            return result;
        }

        result.geometry.source_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.mode = WorkspaceAiSplineDisplayMode::raw;
        result.geometry.pass = WorkspaceAiSplinePassKind::selection;
        result.geometry.topology =
            WorkspaceAiSplineTopology::independent_lines;
        result.geometry.vertices.reserve(payload->side0 == 0.0F ? 2U : 6U);

        auto center_end = point;
        center_end[1] += workspace_ai_spline_selection_height;
        result.geometry.vertices.push_back(
            {point, workspace_ai_spline_selection_color});
        result.geometry.vertices.push_back(
            {center_end, workspace_ai_spline_selection_color});

        if (payload->side0 != 0.0F) {
            float forward_x = next[0] - point[0];
            float forward_z = next[2] - point[2];
            const float length = std::sqrt(
                forward_x * forward_x + forward_z * forward_z);
            if (length != 0.0F) {
                forward_x /= length;
                forward_z /= length;
            }
            const std::array<float, 3U> lateral = {
                -forward_z, 0.0F, forward_x};
            const auto append_side = [&](float width) {
                std::array<float, 3U> begin = {
                    point[0] + lateral[0] * width,
                    point[1] - workspace_ai_spline_selection_side_half_height,
                    point[2] + lateral[2] * width,
                };
                auto end = begin;
                end[1] += 2.0F *
                          workspace_ai_spline_selection_side_half_height;
                result.geometry.vertices.push_back(
                    {begin, workspace_ai_spline_selection_side_color});
                result.geometry.vertices.push_back(
                    {end, workspace_ai_spline_selection_side_color});
            };
            append_side(-payload->side0);
            append_side(payload->side1);
        }
        for (const auto& vertex : result.geometry.vertices) {
            if (!finite_position(vertex.position)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_derived_non_finite",
                    "AI spline current-index marker produced a non-finite "
                    "point");
                result.geometry = {};
                return result;
            }
        }
        build_independent_line_chunks(result.geometry);
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

} // namespace

WorkspaceAiSplineResult
buildWorkspaceAiSplineGeometry(const formats::AiSpline& spline,
                               WorkspaceAiSplineDisplayMode mode) {
    return build_geometry(spline, mode, WorkspaceAiSplinePassKind::primary,
                          std::nullopt);
}

WorkspaceAiSplineResult buildWorkspaceAiSplineIntervalGeometry(
    const formats::AiSpline& spline, WorkspaceAiSplineInterval interval) {
    return build_geometry(spline, WorkspaceAiSplineDisplayMode::interpolated,
                          WorkspaceAiSplinePassKind::interval, interval);
}

WorkspaceAiSplineResult buildWorkspaceAiSplineSideGeometry(
    const formats::AiSpline& spline, WorkspaceAiSplineSide side) {
    return build_side_geometry(spline, side);
}

WorkspaceAiSplineResult buildWorkspaceAiSplineCamberGeometry(
    const formats::AiSpline& spline) {
    return build_camber_geometry(spline);
}

WorkspaceAiSplineResult buildWorkspaceAiSplineSelectionGeometry(
    const formats::AiSpline& spline, std::uint32_t selected_index) {
    return build_selection_geometry(spline, selected_index);
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
