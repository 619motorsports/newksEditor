#include "apex/app/workspace_ai_spline.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

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
    } catch (const std::exception& error) {
        std::cerr << "workspace_ai_spline_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "workspace_ai_spline_tests: ok\n";
    return 0;
}
