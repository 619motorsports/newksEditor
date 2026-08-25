#include "apex/app/workspace_ai_spline.hpp"
#include "apex/authoring/ai_spline_session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

apex::formats::AiSplinePoint point(float x, float y, float z) {
    apex::formats::AiSplinePoint result;
    result.position = {x, y, z};
    return result;
}

void convertsRawV7OpenPolyline() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(1.0F, 2.0F, 3.0F), point(4.0F, 5.0F, 6.0F),
                     point(7.0F, 8.0F, 9.0F)};
    const auto result = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(result.ok(), "three-point v7 spline must convert");
    require(result.geometry.source_point_count == 3U,
            "v7 source point count mismatch");
    require(result.geometry.vertices.size() == 4U,
            "v7 line-list vertex count mismatch");
    require(result.geometry.chunks.size() == 1U &&
                result.geometry.chunks[0].first_vertex == 0U &&
                result.geometry.chunks[0].vertex_count == 4U,
            "v7 chunk metadata mismatch");
    require(
        result.geometry.vertices[0].position == spline.points[0].position &&
            result.geometry.vertices[1].position == spline.points[1].position &&
            result.geometry.vertices[2].position == spline.points[1].position &&
            result.geometry.vertices[3].position == spline.points[2].position,
        "v7 open line-strip conversion mismatch");
    for (const auto& vertex : result.geometry.vertices)
        require(vertex.color == apex::app::workspace_ai_spline_raw_color,
                "raw spline color mismatch");
}

void preservesNativeV2RetentionChoice() {
    apex::formats::AiSpline spline;
    spline.version = 2U;
    for (std::uint32_t index = 0U; index < 7U; ++index) {
        apex::formats::AiSplineLegacyV2Record record;
        record.position = {static_cast<float>(index), 0.0F, 0.0F};
        spline.legacyV2Records.push_back(record);
    }
    spline.nativeRetainedIndices = {0U, 3U, 6U};
    const auto result = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(result.ok(), "retained v2 spline must convert");
    require(result.geometry.source_point_count == 3U,
            "v2 retained point count mismatch");
    require(result.geometry.vertices.size() == 4U,
            "v2 retained geometry size mismatch");
    require(result.geometry.vertices[0].position[0] == 0.0F &&
                result.geometry.vertices[1].position[0] == 3.0F &&
                result.geometry.vertices[2].position[0] == 3.0F &&
                result.geometry.vertices[3].position[0] == 6.0F,
            "v2 must use native retained source indices");
}

void preservesRecoveredSmallSplineEarlyReturn() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 0.0F, 0.0F), point(1.0F, 0.0F, 0.0F)};
    const auto result = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(result.ok(), "two-point spline must be accepted");
    require(result.geometry.source_point_count == 2U,
            "two-point source count mismatch");
    require(result.geometry.vertices.empty() && result.geometry.chunks.empty(),
            "recovered helper must not draw two-point splines");
}

void rejectsMalformedConstructedSources() {
    apex::formats::AiSpline non_finite;
    non_finite.version = 7U;
    non_finite.points = {
        point(0.0F, 0.0F, 0.0F), point(1.0F, 0.0F, 0.0F),
        point(std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F)};
    auto result = apex::app::buildWorkspaceAiSplineGeometry(non_finite);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::invalid_source,
            "non-finite source must be rejected");

    apex::formats::AiSpline bad_v2;
    bad_v2.version = 2U;
    bad_v2.legacyV2Records.resize(1U);
    bad_v2.nativeRetainedIndices = {1U};
    result = apex::app::buildWorkspaceAiSplineGeometry(bad_v2);
    require(!result.ok(), "out-of-range v2 retained index must be rejected");

    apex::formats::AiSpline unsupported;
    unsupported.version = 8U;
    result = apex::app::buildWorkspaceAiSplineGeometry(unsupported);
    require(!result.ok(), "unsupported AI spline version must be rejected");
}

void chunksWithoutDroppingPortableSegments() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points.reserve(2'050U);
    for (std::uint32_t index = 0U; index < 2'050U; ++index)
        spline.points.push_back(point(static_cast<float>(index), 0.0F, 0.0F));
    const auto result = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(result.ok(), "chunk-boundary spline must convert");
    require(result.geometry.vertices.size() == 4'098U,
            "all source segments must be retained");
    require(result.geometry.chunks.size() == 2U,
            "chunk-boundary spline must use two draws");
    require(result.geometry.chunks[0].first_vertex == 0U &&
                result.geometry.chunks[0].vertex_count == 4'096U &&
                result.geometry.chunks[1].first_vertex == 4'096U &&
                result.geometry.chunks[1].vertex_count == 2U,
            "chunk ranges mismatch");
    require(result.geometry.vertices[4'095U].position[0] == 2'048.0F &&
                result.geometry.vertices[4'096U].position[0] == 2'048.0F &&
                result.geometry.vertices[4'097U].position[0] == 2'049.0F,
            "portable chunks must not drop their boundary segment");
}

void rejectsVisualizationBudgetOverflow() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points.resize(apex::render::max_overlay_line_total_vertices / 2U +
                         2U);
    const auto result = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::limit_exceeded,
            "render-budget overflow must be rejected");
}

void require_near(float actual, float expected, const char* message,
                  float tolerance = 0.001F) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

const apex::render::OverlayLineVertex&
sampled_vertex(const apex::app::WorkspaceAiSplineGeometry& geometry,
               std::size_t sample_index) {
    if (sample_index == 0U) return geometry.vertices.front();
    return geometry.vertices[sample_index * 2U - 1U];
}

void convertsRecoveredInterpolatedSpline() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 0.0F, 0.0F),
                     point(100.0F, 0.0F, 0.0F),
                     point(200.0F, 100.0F, 0.0F),
                     point(300.0F, 100.0F, 0.0F)};
    // load() calls computeSplineLength() after it reads either supported
    // version. These serialized values must not drive the display curve.
    spline.points[0].length = 400.0F;
    spline.points[1].length = -20.0F;
    spline.points[2].length = 1.0e20F;
    spline.points[3].length = 0.0F;

    const auto result = apex::app::buildWorkspaceAiSplineGeometry(
        spline, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(result.ok(), "interpolated v7 spline must convert");
    require(result.geometry.mode ==
                    apex::app::WorkspaceAiSplineDisplayMode::interpolated &&
                result.geometry.source_point_count == 4U &&
                result.geometry.sample_point_count == 5'001U,
            "interpolated spline metadata mismatch");
    require(result.geometry.vertices.size() == 10'000U &&
                result.geometry.chunks.size() == 3U,
            "interpolated spline line-list size mismatch");
    require(result.geometry.chunks[0].first_vertex == 0U &&
                result.geometry.chunks[0].vertex_count == 4'096U &&
                result.geometry.chunks[1].first_vertex == 4'096U &&
                result.geometry.chunks[1].vertex_count == 4'096U &&
                result.geometry.chunks[2].first_vertex == 8'192U &&
                result.geometry.chunks[2].vertex_count == 1'808U,
            "interpolated spline chunk ranges mismatch");

    const auto& first = sampled_vertex(result.geometry, 0U).position;
    const auto& quarter = sampled_vertex(result.geometry, 1'250U).position;
    const auto& middle = sampled_vertex(result.geometry, 2'500U).position;
    const auto& three_quarters =
        sampled_vertex(result.geometry, 3'750U).position;
    const auto& last = sampled_vertex(result.geometry, 5'000U).position;
    require(first == std::array<float, 3U>{0.0F, 0.0F, 0.0F},
            "interpolated spline must start at normalized zero");
    require_near(quarter[0], 84.090866F,
                 "interpolated quarter-sample x mismatch");
    require_near(quarter[1], -5.408871F,
                 "interpolated quarter-sample y mismatch");
    require_near(middle[0], 150.002029F,
                 "interpolated midpoint x mismatch");
    require_near(middle[1], 50.002514F,
                 "interpolated midpoint y mismatch");
    require_near(three_quarters[0], 215.901169F,
                 "interpolated three-quarter x mismatch");
    require_near(three_quarters[1], 105.407196F,
                 "interpolated three-quarter y mismatch");
    require_near(last[0], 299.989990F,
                 "float loop must stop before normalized one");
    require_near(last[1], 99.999985F,
                 "interpolated final sample y mismatch");
}

void preservesInterpolatedV2RetentionChoice() {
    apex::formats::AiSpline v7;
    v7.version = 7U;
    v7.points = {point(0.0F, 0.0F, 0.0F),
                 point(100.0F, 0.0F, 0.0F),
                 point(200.0F, 100.0F, 0.0F),
                 point(300.0F, 100.0F, 0.0F)};
    const auto expected = apex::app::buildWorkspaceAiSplineGeometry(
        v7, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(expected.ok(), "interpolated v7 comparison spline converts");

    apex::formats::AiSpline v2;
    v2.version = 2U;
    v2.legacyV2Records.resize(10U);
    for (auto& record : v2.legacyV2Records)
        record.position = {9'999.0F, 9'999.0F, 9'999.0F};
    v2.legacyV2Records[0U].position = v7.points[0U].position;
    v2.legacyV2Records[3U].position = v7.points[1U].position;
    v2.legacyV2Records[6U].position = v7.points[2U].position;
    v2.legacyV2Records[9U].position = v7.points[3U].position;
    v2.nativeRetainedIndices = {0U, 3U, 6U, 9U};
    const auto actual = apex::app::buildWorkspaceAiSplineGeometry(
        v2, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(actual.ok(), "interpolated retained v2 spline converts");
    require(actual.geometry.vertices.size() ==
                expected.geometry.vertices.size(),
            "interpolated v2 output size mismatch");
    for (std::size_t index = 0U;
         index < actual.geometry.vertices.size(); ++index) {
        require(actual.geometry.vertices[index].position ==
                        expected.geometry.vertices[index].position &&
                    actual.geometry.vertices[index].color ==
                        expected.geometry.vertices[index].color,
                "interpolated v2 must use only native retained source indices");
    }
}

void rejectsUnsafeInterpolatedSources() {
    apex::formats::AiSpline short_spline;
    short_spline.version = 7U;
    short_spline.points = {point(0.0F, 0.0F, 0.0F),
                           point(1.0F, 0.0F, 0.0F),
                           point(2.0F, 0.0F, 0.0F)};
    auto result = apex::app::buildWorkspaceAiSplineGeometry(
        short_spline, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_interpolation_too_short",
            "short interpolated spline must be rejected safely");

    apex::formats::AiSpline repeated;
    repeated.version = 7U;
    repeated.points.resize(4U);
    result = apex::app::buildWorkspaceAiSplineGeometry(
        repeated, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_interpolation_length_invalid",
            "zero-length interpolated spline must be rejected safely");

    apex::formats::AiSpline exact_endpoint;
    exact_endpoint.version = 7U;
    exact_endpoint.points = {
        point(0.0F, 0.0F, 0.0F), point(10.0F, 0.0F, 0.0F),
        point(10.0F, 10.0F, 0.0F), point(0.0F, 0.0F, 0.0F)};
    result = apex::app::buildWorkspaceAiSplineGeometry(
        exact_endpoint, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(!result.ok(),
            "zero closed endpoint chord must not reach native division by zero");

    apex::formats::AiSpline oversized;
    oversized.version = 7U;
    oversized.points.resize(
        apex::app::workspace_ai_spline_max_interpolation_control_points + 1U);
    result = apex::app::buildWorkspaceAiSplineGeometry(
        oversized, apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::limit_exceeded,
            "interpolated control-point budget must be checked before work");

    apex::formats::AiSpline valid;
    valid.version = 7U;
    valid.points = {point(0.0F, 0.0F, 0.0F),
                    point(100.0F, 0.0F, 0.0F),
                    point(200.0F, 100.0F, 0.0F),
                    point(300.0F, 100.0F, 0.0F)};
    result = apex::app::buildWorkspaceAiSplineGeometry(
        valid, static_cast<apex::app::WorkspaceAiSplineDisplayMode>(255U));
    require(!result.ok() &&
                result.diagnostic.code == "workspace_ai_spline_mode_invalid",
            "unknown AI spline display mode must be rejected");
}

void buildsRecoveredInterpolatedInterval() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 0.0F, 0.0F),
                     point(100.0F, 0.0F, 0.0F),
                     point(200.0F, 100.0F, 0.0F),
                     point(300.0F, 100.0F, 0.0F)};

    const auto full = apex::app::buildWorkspaceAiSplineIntervalGeometry(
        spline, {0.0F, 1.0F});
    require(full.ok() &&
                full.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::interval &&
                full.geometry.mode ==
                    apex::app::WorkspaceAiSplineDisplayMode::interpolated &&
                full.geometry.sample_point_count == 5'001U &&
                full.geometry.vertices.size() == 10'000U &&
                full.geometry.chunks.size() == 3U,
            "full recovered interval metadata mismatch");
    for (const auto& vertex : full.geometry.vertices)
        require(vertex.color == apex::app::workspace_ai_spline_interval_color,
                "recovered interval must use the native blue color");
    require(sampled_vertex(full.geometry, 0U).position ==
                std::array<float, 3U>{0.0F, 0.0F, 0.0F},
            "full interval must begin at normalized zero");

    const auto partial = apex::app::buildWorkspaceAiSplineIntervalGeometry(
        spline, {0.25F, 0.2504F});
    require(partial.ok() && partial.geometry.sample_point_count == 3U &&
                partial.geometry.vertices.size() == 4U,
            "partial interval must preserve recovered float accumulation");
    require_near(sampled_vertex(partial.geometry, 0U).position[0], 84.089714F,
                 "partial interval start sample mismatch");

    const auto point_interval =
        apex::app::buildWorkspaceAiSplineIntervalGeometry(
            spline, {0.5F, 0.5F});
    require(point_interval.ok() &&
                point_interval.geometry.sample_point_count == 1U &&
                point_interval.geometry.vertices.empty() &&
                point_interval.geometry.chunks.empty(),
            "equal interval endpoints must reproduce the native no-draw pass");
}

void rejectsUnsafeInterpolatedIntervals() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 0.0F, 0.0F),
                     point(100.0F, 0.0F, 0.0F),
                     point(200.0F, 100.0F, 0.0F),
                     point(300.0F, 100.0F, 0.0F)};
    const std::array<apex::app::WorkspaceAiSplineInterval, 5U> invalid = {{
        {-0.1F, 0.5F},
        {0.0F, 1.1F},
        {0.75F, 0.25F},
        {std::numeric_limits<float>::quiet_NaN(), 1.0F},
        {0.0F, std::numeric_limits<float>::infinity()},
    }};
    for (const auto interval : invalid) {
        const auto result =
            apex::app::buildWorkspaceAiSplineIntervalGeometry(spline,
                                                                interval);
        require(!result.ok() &&
                    result.diagnostic.code ==
                        "workspace_ai_spline_interval_invalid",
                "unsafe interval must be rejected before sampling");
    }
}

void buildsRecoveredSideSplineGeometry() {
    const auto side_fixture = [](bool closed) {
        apex::formats::AiSpline spline;
        spline.version = 7U;
        spline.points = {
            point(0.0F, 0.0F, 0.0F), point(10.0F, 0.0F, 0.0F),
            point(10.0F, 0.0F, 10.0F),
            point(closed ? 20.0F : 100.0F, 0.0F, 10.0F)};
        spline.payloads.resize(spline.points.size());
        for (std::size_t index = 0U; index < spline.payloads.size(); ++index) {
            spline.points[index].tag = static_cast<std::int32_t>(index);
            auto& payload = spline.payloads[index];
            payload.side0 = 1.0F;
            payload.side1 = 2.0F;
        }
        return spline;
    };
    const auto require_position = [](const auto& geometry,
                                     std::size_t sample,
                                     std::array<float, 3U> expected,
                                     const char* message) {
        const auto& actual = sampled_vertex(geometry, sample).position;
        for (std::size_t axis = 0U; axis < expected.size(); ++axis)
            require_near(actual[axis], expected[axis], message);
    };

    const auto open = side_fixture(false);
    const auto left = apex::app::buildWorkspaceAiSplineSideGeometry(
        open, apex::app::WorkspaceAiSplineSide::left);
    require(left.ok() &&
                left.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::left_side &&
                left.geometry.sample_point_count == 4U &&
                left.geometry.vertices.size() == 6U &&
                left.geometry.chunks.size() == 1U &&
                left.geometry.chunks[0].vertex_count == 6U,
            "open left side metadata mismatch");
    require_position(left.geometry, 0U, {0.0F, 0.0F, -1.0F},
                     "open left first offset mismatch");
    require_position(left.geometry, 1U, {11.0F, 0.0F, 0.0F},
                     "open left turn offset mismatch");
    require_position(left.geometry, 2U, {10.0F, 0.0F, 9.0F},
                     "open left second turn offset mismatch");
    require_position(left.geometry, 3U, {100.0F, 0.0F, 10.0F},
                     "open left final clamp mismatch");
    for (const auto& vertex : left.geometry.vertices)
        require(vertex.color == apex::app::workspace_ai_spline_side_color,
                "left side color mismatch");

    const auto right = apex::app::buildWorkspaceAiSplineSideGeometry(
        open, apex::app::WorkspaceAiSplineSide::right);
    require(right.ok() &&
                right.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::right_side &&
                right.geometry.sample_point_count == 4U &&
                right.geometry.vertices.size() == 6U,
            "open right side metadata mismatch");
    require_position(right.geometry, 0U, {0.0F, 0.0F, 2.0F},
                     "open right first offset mismatch");
    require_position(right.geometry, 1U, {8.0F, 0.0F, 0.0F},
                     "open right turn offset mismatch");
    require_position(right.geometry, 2U, {10.0F, 0.0F, 12.0F},
                     "open right second turn offset mismatch");
    require_position(right.geometry, 3U, {100.0F, 0.0F, 10.0F},
                     "open right final clamp mismatch");

    auto tagged = open;
    tagged.points[0U].tag = 1;
    tagged.points[1U].tag = 0;
    tagged.payloads[1U].side0 = 3.0F;
    const auto tagged_left =
        apex::app::buildWorkspaceAiSplineSideGeometry(
            tagged, apex::app::WorkspaceAiSplineSide::left);
    require(tagged_left.ok(), "tagged side geometry must convert");
    require_position(tagged_left.geometry, 0U, {0.0F, 0.0F, -3.0F},
                     "side payload tag mapping mismatch");
    require_position(tagged_left.geometry, 1U, {11.0F, 0.0F, 0.0F},
                     "side payload remap mismatch");

    auto skipped = open;
    skipped.payloads[2U].side0 = 0.0F;
    skipped.payloads[2U].side1 = 99.0F;
    const auto skipped_left =
        apex::app::buildWorkspaceAiSplineSideGeometry(
            skipped, apex::app::WorkspaceAiSplineSide::left);
    const auto skipped_right =
        apex::app::buildWorkspaceAiSplineSideGeometry(
            skipped, apex::app::WorkspaceAiSplineSide::right);
    require(skipped_left.ok() && skipped_right.ok() &&
                skipped_left.geometry.sample_point_count == 3U &&
                skipped_right.geometry.sample_point_count == 3U &&
                skipped_left.geometry.vertices.size() == 4U &&
                skipped_right.geometry.vertices.size() == 4U,
            "zero side0 must skip both side points");
    require_position(skipped_left.geometry, 0U, {0.0F, 0.0F, -1.0F},
                     "skipped left first offset mismatch");
    require_position(skipped_left.geometry, 1U, {11.0F, 0.0F, 0.0F},
                     "skipped left turn offset mismatch");
    require_position(skipped_left.geometry, 2U, {100.0F, 0.0F, 10.0F},
                     "skipped left final offset mismatch");
    require_position(skipped_right.geometry, 0U, {0.0F, 0.0F, 2.0F},
                     "skipped right first offset mismatch");
    require_position(skipped_right.geometry, 1U, {8.0F, 0.0F, 0.0F},
                     "skipped right turn offset mismatch");
    require_position(skipped_right.geometry, 2U, {100.0F, 0.0F, 10.0F},
                     "skipped right final offset mismatch");

    const auto closed = side_fixture(true);
    const auto closed_left = apex::app::buildWorkspaceAiSplineSideGeometry(
        closed, apex::app::WorkspaceAiSplineSide::left);
    const auto closed_right = apex::app::buildWorkspaceAiSplineSideGeometry(
        closed, apex::app::WorkspaceAiSplineSide::right);
    require(closed_left.ok() && closed_right.ok() &&
                closed_left.geometry.sample_point_count == 4U &&
                closed_right.geometry.sample_point_count == 4U,
            "closed side geometry metadata mismatch");
    require_position(closed_left.geometry, 3U,
                     {19.552786F, 0.0F, 10.894427F},
                     "closed left final wrap mismatch");
    require_position(closed_right.geometry, 3U,
                     {20.894428F, 0.0F, 8.211146F},
                     "closed right final wrap mismatch");
}

void rejectsUnsafeSideSplineSources() {
    apex::formats::AiSpline v2;
    v2.version = 2U;
    v2.legacyV2Records.resize(3U);
    v2.nativeRetainedIndices = {0U, 1U, 2U};
    auto result = apex::app::buildWorkspaceAiSplineSideGeometry(
        v2, apex::app::WorkspaceAiSplineSide::left);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_side_version_unsupported",
            "version-2 side geometry must be rejected");

    apex::formats::AiSpline missing_payload;
    missing_payload.version = 7U;
    missing_payload.points = {point(0.0F, 0.0F, 0.0F),
                              point(1.0F, 0.0F, 0.0F),
                              point(2.0F, 0.0F, 0.0F)};
    missing_payload.payloads.resize(2U);
    result = apex::app::buildWorkspaceAiSplineSideGeometry(
        missing_payload, apex::app::WorkspaceAiSplineSide::right);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_side_payload_count_invalid",
            "side geometry must validate payload count");

    auto invalid_tag = missing_payload;
    invalid_tag.payloads.resize(invalid_tag.points.size());
    invalid_tag.points[0U].tag = -1;
    result = apex::app::buildWorkspaceAiSplineSideGeometry(
        invalid_tag, apex::app::WorkspaceAiSplineSide::left);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_side_payload_index_invalid",
            "side geometry must validate payload tags");

    apex::formats::AiSpline overflow;
    overflow.version = 7U;
    overflow.points = {point(3.0e38F, 0.0F, 0.0F),
                       point(3.0e38F, 0.0F, 10.0F),
                       point(3.0e38F, 0.0F, 20.0F)};
    overflow.payloads.resize(overflow.points.size());
    overflow.payloads[0U].side0 = std::numeric_limits<float>::max();
    result = apex::app::buildWorkspaceAiSplineSideGeometry(
        overflow, apex::app::WorkspaceAiSplineSide::left);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_side_derived_non_finite",
            "non-finite derived side point must be rejected");

    apex::formats::AiSpline invalid_side;
    invalid_side.version = 7U;
    result = apex::app::buildWorkspaceAiSplineSideGeometry(
        invalid_side, static_cast<apex::app::WorkspaceAiSplineSide>(255U));
    require(!result.ok() &&
                result.diagnostic.code == "workspace_ai_spline_side_invalid",
            "unknown side selection must be rejected");

    apex::formats::AiSpline oversized;
    oversized.version = 7U;
    oversized.points.resize(
        apex::render::max_overlay_line_total_vertices / 2U + 2U);
    oversized.payloads.resize(oversized.points.size());
    result = apex::app::buildWorkspaceAiSplineSideGeometry(
        oversized, apex::app::WorkspaceAiSplineSide::left);
    require(!result.ok() &&
                result.status == apex::app::WorkspaceAiSplineStatus::limit_exceeded &&
                result.diagnostic.code ==
                    "workspace_ai_spline_side_vertex_limit",
            "side geometry source bound must be enforced");
}

void buildsRecoveredCamberGeometry() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 10.0F, 1.0F),
                     point(2.0F, 20.0F, 3.0F),
                     point(4.0F, 30.0F, 5.0F)};
    spline.points[0U].tag = 2;
    spline.points[1U].tag = 0;
    spline.points[2U].tag = 1;
    spline.payloads.resize(3U);
    spline.payloads[0U].camber = 0.0F;
    spline.payloads[1U].camber = -0.25F;
    spline.payloads[2U].camber = 0.5F;

    const auto result =
        apex::app::buildWorkspaceAiSplineCamberGeometry(spline);
    require(result.ok() &&
                result.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::camber &&
                result.geometry.topology ==
                    apex::app::WorkspaceAiSplineTopology::independent_lines &&
                result.geometry.mode ==
                    apex::app::WorkspaceAiSplineDisplayMode::raw &&
                result.geometry.source_point_count == 3U &&
                result.geometry.sample_point_count == 3U &&
                result.geometry.vertices.size() == 6U &&
                result.geometry.chunks.size() == 1U &&
                result.geometry.chunks[0].vertex_count == 6U,
            "camber geometry metadata mismatch");
    const std::array<std::array<float, 3U>, 6U> expected_positions = {{
        {0.0F, 10.0F, 1.0F}, {0.0F, 510.0F, 1.0F},
        {2.0F, 20.0F, 3.0F}, {2.0F, 20.0F, 3.0F},
        {4.0F, 30.0F, 5.0F}, {4.0F, 280.0F, 5.0F},
    }};
    for (std::size_t index = 0U; index < expected_positions.size(); ++index)
        require(result.geometry.vertices[index].position ==
                    expected_positions[index],
                "camber line position mismatch");
    require(result.geometry.vertices[0U].color ==
                    apex::app::workspace_ai_spline_camber_positive_color &&
                result.geometry.vertices[1U].color ==
                    apex::app::workspace_ai_spline_camber_positive_color &&
                result.geometry.vertices[2U].color ==
                    apex::app::workspace_ai_spline_camber_nonpositive_color &&
                result.geometry.vertices[3U].color ==
                    apex::app::workspace_ai_spline_camber_nonpositive_color &&
                result.geometry.vertices[4U].color ==
                    apex::app::workspace_ai_spline_camber_nonpositive_color &&
                result.geometry.vertices[5U].color ==
                    apex::app::workspace_ai_spline_camber_nonpositive_color,
            "camber colors must follow the recovered sign split");

    apex::formats::AiSpline single;
    single.version = 7U;
    single.points = {point(1.0F, 2.0F, 3.0F)};
    single.payloads.resize(1U);
    single.payloads[0U].camber = 1.0F;
    const auto single_result =
        apex::app::buildWorkspaceAiSplineCamberGeometry(single);
    require(single_result.ok() &&
                single_result.geometry.vertices.size() == 2U &&
                single_result.geometry.chunks.size() == 1U,
            "one point must emit one immediate camber line");
}

void rebuildsCamberGeometryFromEditedSessionState() {
    apex::formats::AiSpline spline;
    spline.source = "camber-session.ai";
    spline.version = 7U;
    spline.points = {point(0.0F, 10.0F, 1.0F),
                     point(2.0F, 20.0F, 3.0F)};
    spline.points[0U].tag = 1;
    spline.points[1U].tag = 0;
    spline.payloads.resize(2U);
    spline.payloads[0U].camber = -0.25F;
    spline.payloads[1U].camber = 0.5F;
    apex::authoring::AiSplineSession session(std::move(spline));

    const auto before =
        apex::app::buildWorkspaceAiSplineCamberGeometry(session.current());
    const std::array<std::uint32_t, 1U> selection{0U};
    const auto edited = session.invertSelectedCamber(selection);
    const auto after =
        apex::app::buildWorkspaceAiSplineCamberGeometry(session.current());
    require(before.ok() && edited.ok() && edited.changed && after.ok(),
            "session camber edit rebuilds valid visible geometry");
    require(before.geometry.vertices[0U].color ==
                    apex::app::workspace_ai_spline_camber_positive_color &&
                after.geometry.vertices[0U].color ==
                    apex::app::workspace_ai_spline_camber_nonpositive_color &&
                before.geometry.vertices[1U].position ==
                    after.geometry.vertices[1U].position,
            "selected tagged camber changes sign color but preserves height");
    require(before.geometry.vertices[2U].color ==
                    after.geometry.vertices[2U].color &&
                before.geometry.vertices[3U].position ==
                    after.geometry.vertices[3U].position,
            "unselected camber geometry remains unchanged");
}

void rebuildsVisibleGeometryFromBatchPositionState() {
    apex::formats::AiSpline spline;
    spline.source = "position-session.ai";
    spline.version = 7U;
    spline.points = {point(0.0F, 10.0F, 0.0F), point(10.0F, 20.0F, 0.0F),
                     point(20.0F, 30.0F, 0.0F)};
    spline.payloads.resize(3U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].tag = static_cast<std::int32_t>(index);
        spline.payloads[index].side0 = 1.0F;
        spline.payloads[index].side1 = 2.0F;
    }
    apex::authoring::AiSplineSession session(std::move(spline));
    const auto before = apex::app::buildWorkspaceAiSplineGeometry(
        session.current(), apex::app::WorkspaceAiSplineDisplayMode::raw);
    const auto sideBefore = apex::app::buildWorkspaceAiSplineSideGeometry(
        session.current(), apex::app::WorkspaceAiSplineSide::right);
    const std::array<apex::authoring::AiSplinePointPositionEdit, 2U> edits{
        apex::authoring::AiSplinePointPositionEdit{0U, {1.0F, 11.0F, 2.0F}},
        apex::authoring::AiSplinePointPositionEdit{1U, {11.0F, 21.0F, 2.0F}},
    };
    const auto edited = session.setPointPositions(edits);
    const auto after = apex::app::buildWorkspaceAiSplineGeometry(
        session.current(), apex::app::WorkspaceAiSplineDisplayMode::raw);
    const std::array<std::uint32_t, 2U> selection{0U, 1U};
    const auto selected = apex::app::buildWorkspaceAiSplineSelectionGeometry(
        session.current(), selection);
    const auto sideAfter = apex::app::buildWorkspaceAiSplineSideGeometry(
        session.current(), apex::app::WorkspaceAiSplineSide::right);
    require(before.ok() && sideBefore.ok() && edited.ok() && edited.changed &&
                after.ok() && after.geometry.vertices.size() >= 2U &&
                selected.ok() && selected.geometry.vertices.size() == 12U &&
                sideAfter.ok() && !sideBefore.geometry.vertices.empty() &&
                !sideAfter.geometry.vertices.empty(),
            "batch position state rebuilds all visible spline geometry");
    require(after.geometry.vertices[0U].position == edits[0U].position &&
                after.geometry.vertices[1U].position == edits[1U].position &&
                selected.geometry.vertices[0U].position == edits[0U].position &&
                selected.geometry.vertices[6U].position == edits[1U].position,
            "primary and selected overlays use committed batch positions");
    require(sideBefore.geometry.vertices[0U].position !=
                    sideAfter.geometry.vertices[0U].position &&
                std::all_of(sideAfter.geometry.vertices.begin(),
                            sideAfter.geometry.vertices.end(),
                            [](const auto& vertex) {
                                return std::all_of(
                                    vertex.position.begin(),
                                    vertex.position.end(), [](float value) {
                                        return std::isfinite(value);
                                    });
                            }),
            "side overlay rebuild uses finite committed batch positions");
}

void buildsRecoveredCurrentIndexMarker() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 10.0F, 0.0F), point(10.0F, 20.0F, 0.0F),
                     point(20.0F, 30.0F, 0.0F)};
    spline.points[0U].tag = 2;
    spline.points[1U].tag = 0;
    spline.points[2U].tag = 1;
    spline.payloads.resize(3U);
    spline.payloads[2U].side0 = 1.0F;
    spline.payloads[2U].side1 = 2.0F;

    const auto result =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(result.ok() &&
                result.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::selection &&
                result.geometry.topology ==
                    apex::app::WorkspaceAiSplineTopology::independent_lines &&
                result.geometry.source_point_count == 3U &&
                result.geometry.selected_point_count == 1U &&
                result.geometry.last_selected_index == 0U &&
                result.geometry.sample_point_count == 3U &&
                result.geometry.vertices.size() == 6U &&
                result.geometry.chunks.size() == 1U &&
                result.geometry.chunks[0U].vertex_count == 6U,
            "current-index marker metadata mismatch");
    const std::array<std::array<float, 3U>, 6U> expected = {{
        {0.0F, 10.0F, 0.0F}, {0.0F, 50.0F, 0.0F},
        {0.0F, -10.0F, -1.0F}, {0.0F, 30.0F, -1.0F},
        {0.0F, -10.0F, 2.0F}, {0.0F, 30.0F, 2.0F},
    }};
    for (std::size_t index = 0U; index < expected.size(); ++index)
        require(result.geometry.vertices[index].position == expected[index],
                "current-index marker position mismatch");
    require(result.geometry.vertices[0U].color ==
                    apex::app::workspace_ai_spline_selection_color &&
                result.geometry.vertices[1U].color ==
                    apex::app::workspace_ai_spline_selection_color,
            "current-index center marker must be yellow");
    for (std::size_t index = 2U; index < 6U; ++index)
        require(result.geometry.vertices[index].color ==
                    apex::app::workspace_ai_spline_selection_side_color,
                "current-index side markers must be cyan");

    spline.payloads[2U].side0 = 0.0F;
    const auto center_only =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(center_only.ok() &&
                center_only.geometry.sample_point_count == 1U &&
                center_only.geometry.vertices.size() == 2U,
            "zero left width must skip both current-index side markers");

    spline.payloads[2U].side0 = 1.0F;
    spline.payloads[2U].side1 = 0.0F;
    const auto zero_right =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(zero_right.ok() && zero_right.geometry.vertices.size() == 6U &&
                zero_right.geometry.vertices[4U].position[2] == 0.0F &&
                zero_right.geometry.vertices[5U].position[2] == 0.0F,
            "zero right width must retain its native coincident marker");

    spline.points[1U].position = {100.0F, 20.0F, 0.0F};
    spline.points[2U].position = {200.0F, 30.0F, 0.0F};
    spline.payloads[1U].side0 = 1.0F;
    spline.payloads[1U].side1 = 2.0F;
    const auto open_final =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 2U);
    require(open_final.ok() &&
                open_final.geometry.vertices[2U].position[0] == 200.0F &&
                open_final.geometry.vertices[2U].position[2] == 0.0F &&
                open_final.geometry.vertices[4U].position[0] == 200.0F &&
                open_final.geometry.vertices[4U].position[2] == 0.0F,
            "open final current index must clamp its next point");

    spline.points[2U].position = {50.0F, 30.0F, 0.0F};
    const auto closed_final =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 2U);
    require(closed_final.ok() &&
                closed_final.geometry.vertices[2U].position[2] == 1.0F &&
                closed_final.geometry.vertices[4U].position[2] == -2.0F,
            "closed final current index must wrap to the first point");
}

void buildsRecoveredSelectedIndexMarkers() {
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points = {point(0.0F, 10.0F, 0.0F),
                     point(10.0F, 20.0F, 0.0F),
                     point(20.0F, 30.0F, 0.0F)};
    spline.points[0U].tag = 2;
    spline.points[1U].tag = 0;
    spline.points[2U].tag = 1;
    spline.payloads.resize(3U);
    spline.payloads[0U].side0 = 0.0F;
    spline.payloads[0U].side1 = 99.0F;
    spline.payloads[2U].side0 = 1.0F;
    spline.payloads[2U].side1 = 2.0F;

    const std::array<std::uint32_t, 3U> selected = {1U, 0U, 1U};
    const auto result =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, selected);
    require(result.ok() && result.geometry.selected_point_count == 2U &&
                result.geometry.last_selected_index == 0U &&
                result.geometry.sample_point_count == 4U &&
                result.geometry.vertices.size() == 8U &&
                result.geometry.chunks.size() == 1U,
            "ordered selected-index markers must deduplicate repeated indices");
    const std::array<std::array<float, 3U>, 8U> expected = {{
        {10.0F, 20.0F, 0.0F}, {10.0F, 60.0F, 0.0F},
        {0.0F, 10.0F, 0.0F}, {0.0F, 50.0F, 0.0F},
        {0.0F, -10.0F, -1.0F}, {0.0F, 30.0F, -1.0F},
        {0.0F, -10.0F, 2.0F}, {0.0F, 30.0F, 2.0F},
    }};
    for (std::size_t index = 0U; index < expected.size(); ++index)
        require(result.geometry.vertices[index].position == expected[index],
                "selected-index marker order or endpoint mismatch");
    require(result.geometry.vertices[2U].color ==
                    apex::app::workspace_ai_spline_selection_color &&
                result.geometry.vertices[4U].color ==
                    apex::app::workspace_ai_spline_selection_side_color,
            "each selected-index marker must start with its yellow center line");

    constexpr std::size_t selected_count = 700U;
    apex::formats::AiSpline many;
    many.version = 7U;
    many.points.resize(selected_count);
    many.payloads.resize(selected_count);
    std::vector<std::uint32_t> many_indices;
    many_indices.reserve(selected_count);
    for (std::size_t index = 0U; index < selected_count; ++index) {
        many.points[index].tag = static_cast<std::int32_t>(index);
        many.points[index].position[0] = static_cast<float>(index);
        many.payloads[index].side0 = 1.0F;
        many_indices.push_back(static_cast<std::uint32_t>(index));
    }
    const auto chunked =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(many,
                                                            many_indices);
    require(chunked.ok() && chunked.geometry.selected_point_count == 700U &&
                chunked.geometry.last_selected_index == 699U &&
                chunked.geometry.sample_point_count == 2'100U &&
                chunked.geometry.vertices.size() == 4'200U &&
                chunked.geometry.chunks.size() == 2U &&
                chunked.geometry.chunks[0U].vertex_count ==
                    apex::render::max_overlay_line_vertices &&
                chunked.geometry.chunks[1U].first_vertex ==
                    apex::render::max_overlay_line_vertices &&
                chunked.geometry.chunks[1U].vertex_count == 104U &&
                chunked.geometry.vertices.front().color ==
                    apex::app::workspace_ai_spline_selection_color &&
                chunked.geometry.vertices.back().color ==
                    apex::app::workspace_ai_spline_selection_side_color,
            "wide selected-index markers must cross chunks without dropped lines");

    apex::formats::AiSpline large_direction;
    large_direction.version = 7U;
    large_direction.points = {point(0.0F, 0.0F, 0.0F),
                              point(3.0e38F, 0.0F, 0.0F)};
    large_direction.payloads.resize(2U);
    large_direction.payloads[0U].side0 = 1.0F;
    const auto stable_direction =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(
            large_direction, 0U);
    require(stable_direction.ok() &&
                stable_direction.geometry.vertices[2U].position[2] == -1.0F,
            "finite large directions must not collapse during normalization");
}

void rejectsUnsafeCurrentIndexMarkers() {
    apex::formats::AiSpline spline;
    spline.version = 2U;
    auto result =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_version_unsupported",
            "version-2 current-index markers must be rejected");

    spline.version = 7U;
    spline.points = {point(0.0F, 0.0F, 0.0F)};
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_payload_count_invalid",
            "current-index markers must validate payload count");

    spline.payloads.resize(1U);
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 1U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_index_invalid",
            "current-index markers must validate the selected index");

    spline.points[0U].tag = -1;
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_payload_index_invalid",
            "current-index markers must validate point tags");

    spline.points[0U].tag = 0;
    spline.payloads[0U].side0 =
        std::numeric_limits<float>::quiet_NaN();
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_source_non_finite",
            "current-index markers must reject non-finite widths");

    spline.points.push_back(point(
        1.0F, 0.0F, std::numeric_limits<float>::max()));
    spline.points[1U].tag = 1;
    spline.payloads.resize(2U);
    spline.payloads[0U].side0 = -std::numeric_limits<float>::max();
    spline.points[0U].position[2] = std::numeric_limits<float>::max();
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, 0U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_derived_non_finite",
            "current-index markers must reject non-finite derived points");

    apex::formats::AiSpline oversized;
    oversized.version = 7U;
    oversized.points.resize(
        apex::app::workspace_ai_spline_max_interpolation_control_points + 1U);
    oversized.payloads.resize(oversized.points.size());
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(oversized, 0U);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::limit_exceeded &&
                result.diagnostic.code ==
                    "workspace_ai_spline_selection_source_limit",
            "current-index markers must enforce the bounded point limit");

    const std::vector<std::uint32_t> empty_selection;
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(
        spline, empty_selection);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_empty",
            "empty selection geometry must be rejected");

    apex::formats::AiSpline too_many;
    too_many.version = 7U;
    too_many.points.resize(
        apex::app::workspace_ai_spline_max_selection_points + 1U);
    too_many.payloads.resize(too_many.points.size());
    std::vector<std::uint32_t> too_many_indices;
    too_many_indices.reserve(too_many.points.size());
    for (std::size_t index = 0U; index < too_many.points.size(); ++index) {
        too_many.points[index].tag = static_cast<std::int32_t>(index);
        too_many_indices.push_back(static_cast<std::uint32_t>(index));
    }
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(
        too_many, too_many_indices);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::limit_exceeded &&
                result.diagnostic.code ==
                    "workspace_ai_spline_selection_count_limit",
            "selection input must enforce its bounded unique-marker count");

    constexpr std::size_t wide_count =
        apex::render::max_overlay_line_total_vertices / 6U + 1U;
    apex::formats::AiSpline wide;
    wide.version = 7U;
    wide.points.resize(wide_count);
    wide.payloads.resize(wide_count);
    std::vector<std::uint32_t> wide_indices;
    wide_indices.reserve(wide_count);
    for (std::size_t index = 0U; index < wide_count; ++index) {
        wide.points[index].tag = static_cast<std::int32_t>(index);
        wide.points[index].position[0] = static_cast<float>(index);
        wide.payloads[index].side0 = 1.0F;
        wide_indices.push_back(static_cast<std::uint32_t>(index));
    }
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(
        wide, wide_indices);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::limit_exceeded &&
                result.diagnostic.code ==
                    "workspace_ai_spline_selection_vertex_limit",
            "selection markers must enforce the total vertex limit");

    apex::formats::AiSpline subtraction_overflow;
    subtraction_overflow.version = 7U;
    subtraction_overflow.points = {
        point(-std::numeric_limits<float>::max(), 0.0F, 0.0F),
        point(std::numeric_limits<float>::max(), 0.0F, 0.0F)};
    subtraction_overflow.payloads.resize(2U);
    subtraction_overflow.payloads[0U].side0 = 1.0F;
    result = apex::app::buildWorkspaceAiSplineSelectionGeometry(
        subtraction_overflow, 0U);
    require(!result.ok() && result.diagnostic.code ==
                "workspace_ai_spline_selection_derived_non_finite",
            "selection markers must reject an overflowing direction delta");
}

void rejectsUnsafeCamberSources() {
    apex::formats::AiSpline v2;
    v2.version = 2U;
    auto result = apex::app::buildWorkspaceAiSplineCamberGeometry(v2);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_camber_version_unsupported",
            "version-2 camber geometry must be rejected");

    apex::formats::AiSpline missing_payload;
    missing_payload.version = 7U;
    missing_payload.points = {point(0.0F, 0.0F, 0.0F)};
    result = apex::app::buildWorkspaceAiSplineCamberGeometry(missing_payload);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_camber_payload_count_invalid",
            "camber geometry must validate payload count");

    missing_payload.payloads.resize(1U);
    missing_payload.points[0U].tag = -1;
    result = apex::app::buildWorkspaceAiSplineCamberGeometry(missing_payload);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_camber_payload_index_invalid",
            "camber geometry must validate payload tags");

    missing_payload.points[0U].tag = 0;
    missing_payload.payloads[0U].camber =
        std::numeric_limits<float>::max();
    result = apex::app::buildWorkspaceAiSplineCamberGeometry(missing_payload);
    require(!result.ok() &&
                result.diagnostic.code ==
                    "workspace_ai_spline_camber_derived_non_finite",
            "non-finite derived camber point must be rejected");

    apex::formats::AiSpline oversized;
    oversized.version = 7U;
    oversized.points.resize(
        apex::render::max_overlay_line_total_vertices / 2U + 1U);
    oversized.payloads.resize(oversized.points.size());
    result = apex::app::buildWorkspaceAiSplineCamberGeometry(oversized);
    require(!result.ok() &&
                result.status ==
                    apex::app::WorkspaceAiSplineStatus::limit_exceeded &&
                result.diagnostic.code ==
                    "workspace_ai_spline_camber_vertex_limit",
            "camber geometry source bound must be enforced");
}

} // namespace

int main() {
    try {
        convertsRawV7OpenPolyline();
        preservesNativeV2RetentionChoice();
        preservesRecoveredSmallSplineEarlyReturn();
        rejectsMalformedConstructedSources();
        chunksWithoutDroppingPortableSegments();
        rejectsVisualizationBudgetOverflow();
        convertsRecoveredInterpolatedSpline();
        preservesInterpolatedV2RetentionChoice();
        rejectsUnsafeInterpolatedSources();
        buildsRecoveredInterpolatedInterval();
        rejectsUnsafeInterpolatedIntervals();
        buildsRecoveredSideSplineGeometry();
        rejectsUnsafeSideSplineSources();
        buildsRecoveredCamberGeometry();
        rebuildsCamberGeometryFromEditedSessionState();
        rebuildsVisibleGeometryFromBatchPositionState();
        rejectsUnsafeCamberSources();
        buildsRecoveredCurrentIndexMarker();
        buildsRecoveredSelectedIndexMarkers();
        rejectsUnsafeCurrentIndexMarkers();
    } catch (const std::exception& error) {
        std::cerr << "workspace_ai_spline_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "workspace_ai_spline_tests: ok\n";
    return 0;
}
