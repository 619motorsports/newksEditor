#include "apex/app/workspace_ai_spline.hpp"

#include "apex/app/installed_editor_spline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
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

[[nodiscard]] float native_float(float value) noexcept {
    volatile float rounded = value;
    return rounded;
}

[[nodiscard]] float native_distance_squared(
    const std::array<float, 3U>& left,
    const std::array<float, 3U>& right) noexcept {
    const float dx = native_float(left[0] - right[0]);
    const float dy = native_float(left[1] - right[1]);
    const float dz = native_float(left[2] - right[2]);
    const float x2 = native_float(dx * dx);
    const float y2 = native_float(dy * dy);
    const float z2 = native_float(dz * dz);
    return native_float(native_float(x2 + y2) + z2);
}

[[nodiscard]] std::optional<std::size_t> native_grid_index(
    float coordinate, float minimum, float density) noexcept {
    const float relative = native_float(coordinate - minimum);
    const float scaled = native_float(relative / density);
    if (!std::isfinite(scaled) || scaled < 0.0F ||
        scaled >= 4'294'967'296.0F)
        return std::nullopt;
    return static_cast<std::size_t>(static_cast<std::uint32_t>(scaled));
}

[[nodiscard]] WorkspaceAiSplineClosestPointResult closest_failure(
    WorkspaceAiSplineStatus status, const char* code, const char* message) {
    WorkspaceAiSplineClosestPointResult result;
    result.status = status;
    result.diagnostic = diagnostic(code, message);
    return result;
}

[[nodiscard]] std::uint32_t scan_closest_points(
    const formats::AiSpline& spline,
    const std::array<float, 3U>& query,
    std::span<const std::uint32_t> candidates) noexcept {
    float best = std::bit_cast<float>(std::uint32_t{0x4B18967FU});
    std::uint32_t best_index = 0U;
    if (candidates.empty()) {
        for (std::size_t index = 0U; index < spline.points.size(); ++index) {
            const float distance = native_distance_squared(
                query, spline.points[index].position);
            if (distance < best) {
                best = distance;
                best_index = static_cast<std::uint32_t>(index);
            }
        }
        return best_index;
    }
    for (const std::uint32_t index : candidates) {
        const float distance = native_distance_squared(
            query, spline.points[static_cast<std::size_t>(index)].position);
        if (distance < best) {
            best = distance;
            best_index = index;
        }
    }
    return best_index;
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
    const formats::AiSpline& spline,
    std::span<const std::uint32_t> selected_indices) {
    WorkspaceAiSplineResult result;
    try {
        if (spline.version != 7U) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_version_unsupported",
                "AI spline selection markers require version-7 payloads");
            return result;
        }
        if (spline.points.size() != spline.payloads.size()) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_payload_count_invalid",
                "AI spline selection markers require one payload per point");
            return result;
        }
        if (spline.points.size() >
            workspace_ai_spline_max_interpolation_control_points) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_source_limit",
                "AI spline selection-marker source exceeds the bounded "
                "point limit");
            return result;
        }
        if (selected_indices.empty()) {
            result.status = WorkspaceAiSplineStatus::invalid_source;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_selection_empty",
                "AI spline selection geometry requires one selected index");
            return result;
        }
        const bool closed =
            spline.points.size() >= 2U &&
            installedEditorSplinePointDistance(
                spline.points.back().position,
                spline.points.front().position) <=
                installed_editor_spline_closure_distance;

        std::vector<std::uint8_t> seen(spline.points.size(), 0U);
        std::vector<std::uint32_t> unique_indices;
        unique_indices.reserve(std::min(
            selected_indices.size(),
            workspace_ai_spline_max_selection_points));
        std::size_t vertex_count = 0U;
        for (const std::uint32_t selected_index : selected_indices) {
            const std::size_t selected =
                static_cast<std::size_t>(selected_index);
            if (selected >= spline.points.size()) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_index_invalid",
                    "AI spline selected index is outside the retained points");
                return result;
            }
            if (seen[selected] != 0U) continue;
            if (unique_indices.size() >=
                workspace_ai_spline_max_selection_points) {
                result.status = WorkspaceAiSplineStatus::limit_exceeded;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_count_limit",
                    "AI spline selection exceeds the bounded marker limit");
                return result;
            }
            seen[selected] = 1U;
            unique_indices.push_back(selected_index);

            const auto& source_point = spline.points[selected];
            const auto* payload = payload_for_point(spline, source_point);
            if (payload == nullptr) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_payload_index_invalid",
                    "AI spline selected point tag is outside the payload array");
                return result;
            }
            const std::size_t next_index =
                selected + 1U < spline.points.size()
                    ? selected + 1U
                    : (closed ? 0U : selected);
            if (!finite_position(source_point.position) ||
                !finite_position(spline.points[next_index].position) ||
                !std::isfinite(payload->side0) ||
                !std::isfinite(payload->side1)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_source_non_finite",
                    "AI spline selection markers require finite points and widths");
                return result;
            }
            if (payload->side0 != 0.0F) {
                const float forward_x =
                    spline.points[next_index].position[0] -
                    source_point.position[0];
                const float forward_z =
                    spline.points[next_index].position[2] -
                    source_point.position[2];
                const float length = std::hypot(forward_x, forward_z);
                if (!std::isfinite(forward_x) ||
                    !std::isfinite(forward_z) || !std::isfinite(length)) {
                    result.status = WorkspaceAiSplineStatus::invalid_source;
                    result.diagnostic = diagnostic(
                        "workspace_ai_spline_selection_derived_non_finite",
                        "AI spline selection direction is not finite");
                    return result;
                }
            }
            const std::size_t marker_vertices =
                payload->side0 == 0.0F ? 2U : 6U;
            if (marker_vertices >
                render::max_overlay_line_total_vertices - vertex_count) {
                result.status = WorkspaceAiSplineStatus::limit_exceeded;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_vertex_limit",
                    "AI spline selection exceeds the bounded overlay vertex limit");
                return result;
            }
            vertex_count += marker_vertices;
        }

        result.geometry.source_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.selected_point_count =
            static_cast<std::uint32_t>(unique_indices.size());
        result.geometry.last_selected_index = unique_indices.back();
        result.geometry.mode = WorkspaceAiSplineDisplayMode::raw;
        result.geometry.pass = WorkspaceAiSplinePassKind::selection;
        result.geometry.topology =
            WorkspaceAiSplineTopology::independent_lines;
        result.geometry.vertices.reserve(vertex_count);

        for (const std::uint32_t selected_index : unique_indices) {
            const std::size_t selected =
                static_cast<std::size_t>(selected_index);
            const auto& source_point = spline.points[selected];
            const auto* payload = payload_for_point(spline, source_point);
            const auto& point = source_point.position;
            const std::size_t next_index =
                selected + 1U < spline.points.size()
                    ? selected + 1U
                    : (closed ? 0U : selected);
            const auto& next = spline.points[next_index].position;

            auto center_end = point;
            center_end[1] += workspace_ai_spline_selection_height;
            result.geometry.vertices.push_back(
                {point, workspace_ai_spline_selection_color});
            result.geometry.vertices.push_back(
                {center_end, workspace_ai_spline_selection_color});

            if (payload->side0 != 0.0F) {
                float forward_x = next[0] - point[0];
                float forward_z = next[2] - point[2];
                const float length = std::hypot(forward_x, forward_z);
                if (length != 0.0F) {
                    forward_x /= length;
                    forward_z /= length;
                }
                const std::array<float, 3U> lateral = {
                    -forward_z, 0.0F, forward_x};
                const auto append_side = [&](float width) {
                    std::array<float, 3U> begin = {
                        point[0] + lateral[0] * width,
                        point[1] -
                            workspace_ai_spline_selection_side_half_height,
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
        }
        for (const auto& vertex : result.geometry.vertices) {
            if (!finite_position(vertex.position)) {
                result.status = WorkspaceAiSplineStatus::invalid_source;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_selection_derived_non_finite",
                    "AI spline selection marker produced a non-finite point");
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

WorkspaceAiSplineClosestPointResult
resolveWorkspaceAiSplineClosestPoint(
    const formats::AiSpline& spline,
    const std::array<float, 3U>& query,
    const WorkspaceAiSplineClosestPointLimits& limits) {
    WorkspaceAiSplineClosestPointResult result;
    result.status = WorkspaceAiSplineStatus::ready;
    if (spline.points.empty() || !finite_position(query)) return result;
    if (limits.max_points == 0U || limits.max_grid_rows == 0U ||
        limits.max_grid_cells == 0U || limits.max_grid_indices == 0U ||
        limits.max_grid_neighbors == 0U ||
        spline.points.size() > limits.max_points ||
        spline.points.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return closest_failure(
            WorkspaceAiSplineStatus::limit_exceeded,
            "workspace_ai_spline_closest_limit_exceeded",
            "AI spline closest-point input exceeds its configured limit");
    }
    for (const formats::AiSplinePoint& point : spline.points) {
        if (!finite_position(point.position)) {
            return closest_failure(
                WorkspaceAiSplineStatus::invalid_source,
                "workspace_ai_spline_closest_point_non_finite",
                "AI spline closest-point input contains a non-finite point");
        }
    }
    if (!spline.grid.has_value()) {
        result.point_index = scan_closest_points(spline, query, {});
        return result;
    }

    const formats::AiSplineGrid& grid = *spline.grid;
    if (!finite_position(grid.minimum) || !finite_position(grid.maximum) ||
        !std::isfinite(grid.samplingDensity) ||
        !(grid.samplingDensity > 0.0F) ||
        grid.maximum[0] < grid.minimum[0] ||
        grid.maximum[2] < grid.minimum[2]) {
        return closest_failure(
            WorkspaceAiSplineStatus::invalid_source,
            "workspace_ai_spline_closest_grid_invalid",
            "AI spline closest-point grid metadata is invalid");
    }
    if (grid.neighborCount > limits.max_grid_neighbors ||
        grid.rows.size() > limits.max_grid_rows) {
        return closest_failure(
            WorkspaceAiSplineStatus::limit_exceeded,
            "workspace_ai_spline_closest_limit_exceeded",
            "AI spline closest-point grid exceeds its configured limit");
    }
    std::size_t cell_count = 0U;
    std::size_t candidate_count = 0U;
    for (const formats::AiSplineGridRow& row : grid.rows) {
        if (row.cells.size() > limits.max_grid_cells - cell_count) {
            return closest_failure(
                WorkspaceAiSplineStatus::limit_exceeded,
                "workspace_ai_spline_closest_limit_exceeded",
                "AI spline closest-point cells exceed their configured limit");
        }
        cell_count += row.cells.size();
        for (const formats::AiSplineGridCell& cell : row.cells) {
            if (cell.pointIndices.size() >
                limits.max_grid_indices - candidate_count) {
                return closest_failure(
                    WorkspaceAiSplineStatus::limit_exceeded,
                    "workspace_ai_spline_closest_limit_exceeded",
                    "AI spline closest-point candidates exceed their configured limit");
            }
            candidate_count += cell.pointIndices.size();
            for (const std::uint32_t index : cell.pointIndices) {
                if (static_cast<std::size_t>(index) >= spline.points.size()) {
                    return closest_failure(
                        WorkspaceAiSplineStatus::invalid_source,
                        "workspace_ai_spline_closest_grid_index_invalid",
                        "AI spline closest-point grid references a missing point");
                }
            }
        }
    }

    const auto row_index = native_grid_index(
        query[0], grid.minimum[0], grid.samplingDensity);
    if (!row_index.has_value() || *row_index >= grid.rows.size()) {
        result.point_index = scan_closest_points(spline, query, {});
        return result;
    }
    const formats::AiSplineGridRow& row = grid.rows[*row_index];
    const auto cell_index = native_grid_index(
        query[2], grid.minimum[2], grid.samplingDensity);
    if (!cell_index.has_value() || *cell_index >= row.cells.size()) {
        result.point_index = scan_closest_points(spline, query, {});
        return result;
    }
    result.used_grid = true;
    const auto& candidates = row.cells[*cell_index].pointIndices;
    if (candidates.empty()) return result;
    result.point_index = scan_closest_points(spline, query, candidates);
    return result;
}

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
    const formats::AiSpline& spline,
    std::span<const std::uint32_t> selected_indices) {
    return build_selection_geometry(spline, selected_indices);
}

WorkspaceAiSplineResult
buildWorkspaceAiSplineSelectionGeometry(const formats::AiSpline &spline,
                                        std::uint32_t selected_index) {
    const std::array selected_indices = {selected_index};
    return build_selection_geometry(spline, selected_indices);
}

WorkspaceAiSplineResult
buildWorkspaceAiSplineTemporaryInterpolationGeometry(
    const formats::AiSpline& spline,
    std::span<const std::uint32_t> selected_indices,
    std::span<const WorkspaceAiSplineTemporaryEditPoint> temporary_points) {
    WorkspaceAiSplineResult result;
    try {
        if (spline.points.size() >
                workspace_ai_spline_max_interpolation_control_points ||
            spline.points.size() > static_cast<std::size_t>(
                                       std::numeric_limits<std::uint32_t>::max())) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_source_limit",
                "Temporary AI spline source points exceed the safety limit");
            return result;
        }
        if (spline.version != 7U || selected_indices.size() < 2U ||
            temporary_points.size() < 5U) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_interpolation_state_invalid",
                "Temporary AI spline interpolation requires version 7, two selected endpoints, and five edit points");
            return result;
        }
        if (selected_indices.size() > spline.points.size()) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_selection_invalid",
                "Temporary AI spline selection exceeds the source point count");
            return result;
        }
        std::vector<std::uint8_t> seen(spline.points.size(), 0U);
        for (const std::uint32_t index : selected_indices) {
            if (static_cast<std::size_t>(index) >= spline.points.size() ||
                seen[index] != 0U) {
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_selection_invalid",
                    "Temporary AI spline selection requires unique in-range indices");
                return result;
            }
            seen[index] = 1U;
        }
        if (temporary_points.size() >
            workspace_ai_spline_max_temporary_edit_points) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_point_limit",
                "Temporary AI spline edit points exceed the safety limit");
            return result;
        }
        const std::size_t first = selected_indices.front();
        const std::size_t last = selected_indices.back();
        if (first >= spline.points.size() || last >= spline.points.size()) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_endpoint_invalid",
                "A temporary AI spline endpoint is outside the point array");
            return result;
        }

        InstalledEditorSpline interpolating;
        interpolating.points.reserve(temporary_points.size() + 2U);
        interpolating.points.push_back(spline.points[first].position);
        for (const auto& point : temporary_points) {
            if (!finite_position(point.position) ||
                !finite_position(point.forward)) {
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_point_non_finite",
                    "Temporary AI spline edit points require finite positions and forwards");
                return result;
            }
            interpolating.points.push_back(point.position);
        }
        interpolating.points.push_back(spline.points[last].position);
        interpolating.closed = false;
        if (!recomputeInstalledEditorSplineLengths(interpolating)) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_length_invalid",
                "Temporary AI spline interpolation requires a positive finite path");
            return result;
        }

        std::vector<std::array<float, 3U>> samples;
        samples.reserve(workspace_ai_spline_interpolated_sample_count);
        for (float position = 0.0F; position <= 1.0F;
             position += workspace_ai_spline_interpolation_step) {
            const auto sample =
                sampleInstalledEditorSpline(interpolating, position);
            if (!sample.has_value()) {
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_sample_invalid",
                    "Temporary AI spline interpolation produced an invalid sample");
                return result;
            }
            samples.push_back(*sample);
            if (samples.size() >
                workspace_ai_spline_interpolated_sample_count) {
                result.status = WorkspaceAiSplineStatus::limit_exceeded;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_sample_limit",
                    "Temporary AI spline interpolation exceeds the recovered sample limit");
                return result;
            }
        }
        if (samples.size() !=
            workspace_ai_spline_interpolated_sample_count) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_schedule_invalid",
                "Temporary AI spline interpolation did not match the recovered sample schedule");
            return result;
        }

        result.geometry.source_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.temporary_point_count =
            static_cast<std::uint32_t>(temporary_points.size());
        result.geometry.mode = WorkspaceAiSplineDisplayMode::interpolated;
        result.geometry.pass =
            WorkspaceAiSplinePassKind::temporary_interpolation;
        result.geometry.topology = WorkspaceAiSplineTopology::polyline;
        build_line_list(result.geometry, samples,
                        workspace_ai_spline_temporary_color);
        result.status = WorkspaceAiSplineStatus::ready;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceAiSplineStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_allocation_failed",
            "Temporary AI spline geometry exceeded available allocation capacity");
        return result;
    }
}

WorkspaceAiSplineResult buildWorkspaceAiSplineTemporaryMarkerGeometry(
    const formats::AiSpline& spline,
    std::span<const WorkspaceAiSplineTemporaryEditPoint> temporary_points,
    std::optional<std::size_t> movable_point) {
    WorkspaceAiSplineResult result;
    try {
        if (spline.points.size() >
                workspace_ai_spline_max_interpolation_control_points ||
            spline.points.size() > static_cast<std::size_t>(
                                       std::numeric_limits<std::uint32_t>::max())) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_source_limit",
                "Temporary AI spline source points exceed the safety limit");
            return result;
        }
        if (spline.version != 7U || temporary_points.empty()) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_marker_state_invalid",
                "Temporary AI spline markers require version 7 and one edit point");
            return result;
        }
        if (temporary_points.size() >
            workspace_ai_spline_max_temporary_edit_points) {
            result.status = WorkspaceAiSplineStatus::limit_exceeded;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_point_limit",
                "Temporary AI spline edit points exceed the safety limit");
            return result;
        }
        if (movable_point.has_value() &&
            *movable_point >= temporary_points.size()) {
            result.diagnostic = diagnostic(
                "workspace_ai_spline_temporary_movable_invalid",
                "The movable temporary AI spline point is outside the edit-point array");
            return result;
        }
        result.geometry.source_point_count =
            static_cast<std::uint32_t>(spline.points.size());
        result.geometry.temporary_point_count =
            static_cast<std::uint32_t>(temporary_points.size());
        result.geometry.mode = WorkspaceAiSplineDisplayMode::raw;
        result.geometry.pass = WorkspaceAiSplinePassKind::temporary_markers;
        result.geometry.topology =
            WorkspaceAiSplineTopology::independent_lines;
        result.geometry.vertices.reserve(
            temporary_points.size() * 2U +
            (movable_point.has_value() ? 4U : 0U));
        for (const auto& point : temporary_points) {
            if (!finite_position(point.position) ||
                !finite_position(point.forward)) {
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_point_non_finite",
                    "Temporary AI spline edit points require finite positions and forwards");
                result.geometry = {};
                return result;
            }
            auto end = point.position;
            end[1] += workspace_ai_spline_temporary_marker_height;
            if (!finite_position(end)) {
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_marker_non_finite",
                    "A temporary AI spline marker produced a non-finite point");
                result.geometry = {};
                return result;
            }
            result.geometry.vertices.push_back(
                {point.position, workspace_ai_spline_temporary_color});
            result.geometry.vertices.push_back(
                {end, workspace_ai_spline_temporary_color});
        }
        if (movable_point.has_value()) {
            const auto& point = temporary_points[*movable_point];
            const std::array<float, 3U> forwardEnd = {
                point.position[0U] + point.forward[0U] * 3.0F,
                point.position[1U] + point.forward[1U] * 3.0F,
                point.position[2U] + point.forward[2U] * 3.0F};
            const std::array<float, 3U> sideEnd = {
                point.position[0U] - point.forward[2U] * 3.0F,
                point.position[1U],
                point.position[2U] + point.forward[0U] * 3.0F};
            if (!finite_position(forwardEnd) || !finite_position(sideEnd)) {
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_temporary_axis_non_finite",
                    "The movable temporary AI spline axes produced a non-finite point");
                result.geometry = {};
                return result;
            }
            result.geometry.vertices.push_back(
                {point.position,
                 workspace_ai_spline_temporary_forward_color});
            result.geometry.vertices.push_back(
                {forwardEnd, workspace_ai_spline_temporary_forward_color});
            result.geometry.vertices.push_back(
                {point.position, workspace_ai_spline_temporary_color});
            result.geometry.vertices.push_back(
                {sideEnd, workspace_ai_spline_temporary_color});
        }
        build_independent_line_chunks(result.geometry);
        result.status = WorkspaceAiSplineStatus::ready;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceAiSplineStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_allocation_failed",
            "Temporary AI spline geometry exceeded available allocation capacity");
        result.geometry = {};
        return result;
    }
}

WorkspaceAiSplineOverlayResult
buildWorkspaceAiSplineOverlays(const formats::AiSpline &spline,
                               const WorkspaceAiSplineOverlayRequest &request) {
    WorkspaceAiSplineOverlayResult result;
    auto primary = buildWorkspaceAiSplineGeometry(spline, request.mode);
    if (!primary.ok()) {
        result.status = primary.status;
        result.diagnostic = std::move(primary.diagnostic);
        return result;
    }
    result.overlays.primary = std::move(primary.geometry);

    if (request.interval.has_value()) {
        auto interval =
            buildWorkspaceAiSplineIntervalGeometry(spline, *request.interval);
        if (!interval.ok()) {
            result.status = interval.status;
            result.diagnostic = std::move(interval.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.interval = std::move(interval.geometry);
    }
    if (request.show_left) {
        auto left = buildWorkspaceAiSplineSideGeometry(
            spline, WorkspaceAiSplineSide::left);
        if (!left.ok()) {
            result.status = left.status;
            result.diagnostic = std::move(left.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.left = std::move(left.geometry);
    }
    if (request.show_right) {
        auto right = buildWorkspaceAiSplineSideGeometry(
            spline, WorkspaceAiSplineSide::right);
        if (!right.ok()) {
            result.status = right.status;
            result.diagnostic = std::move(right.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.right = std::move(right.geometry);
    }
    if (!request.selected_indices.empty()) {
        auto selection = buildWorkspaceAiSplineSelectionGeometry(
            spline, request.selected_indices);
        if (!selection.ok()) {
            result.status = selection.status;
            result.diagnostic = std::move(selection.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.selection = std::move(selection.geometry);
    }
    if (request.temporary_edit_points.size() >= 5U &&
        request.selected_indices.size() >= 2U) {
        auto temporary = buildWorkspaceAiSplineTemporaryInterpolationGeometry(
            spline, request.selected_indices, request.temporary_edit_points);
        if (!temporary.ok()) {
            result.status = temporary.status;
            result.diagnostic = std::move(temporary.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.temporaryInterpolation =
            std::move(temporary.geometry);
    }
    if (!request.temporary_edit_points.empty()) {
        auto markers = buildWorkspaceAiSplineTemporaryMarkerGeometry(
            spline, request.temporary_edit_points,
            request.movable_temporary_point);
        if (!markers.ok()) {
            result.status = markers.status;
            result.diagnostic = std::move(markers.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.temporaryMarkers = std::move(markers.geometry);
    }
    if (request.show_camber) {
        auto camber = buildWorkspaceAiSplineCamberGeometry(spline);
        if (!camber.ok()) {
            result.status = camber.status;
            result.diagnostic = std::move(camber.diagnostic);
            result.overlays = {};
            return result;
        }
        result.overlays.camber = std::move(camber.geometry);
    }
    result.status = WorkspaceAiSplineStatus::ready;
    return result;
}

const char *workspace_ai_spline_display_mode_name(
    WorkspaceAiSplineDisplayMode mode) noexcept {
    switch (mode) {
    case WorkspaceAiSplineDisplayMode::raw:
        return "raw";
    case WorkspaceAiSplineDisplayMode::interpolated:
        return "interpolated";
    }
    return "unknown";
}

const char *
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
