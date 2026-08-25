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

} // namespace

int main() {
    try {
        convertsRawV7OpenPolyline();
        preservesNativeV2RetentionChoice();
        preservesRecoveredSmallSplineEarlyReturn();
        rejectsMalformedConstructedSources();
        chunksWithoutDroppingPortableSegments();
        rejectsVisualizationBudgetOverflow();
    } catch (const std::exception& error) {
        std::cerr << "workspace_ai_spline_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "workspace_ai_spline_tests: ok\n";
    return 0;
}
