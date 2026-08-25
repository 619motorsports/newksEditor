#pragma once

#include "apex/formats/ai_spline.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace apex::formats {

struct AiSplineGridBuildLimits {
    std::size_t maxPoints = 1'000'000U;
    std::size_t maxGridNeighbors = 10U;
    std::size_t maxGridRows = 1'000'000U;
    std::size_t maxGridCellsPerRow = 1'000'000U;
    std::size_t maxGridCells = 1'000'000U;
    std::size_t maxGridIndices = 10'000'000U;
    std::size_t maxDistanceEvaluations = 250'000'000U;
    std::size_t maxAggregateBytes = 512U * 1024U * 1024U;
};

class AiSplineGridBuildError final : public std::runtime_error {
  public:
    AiSplineGridBuildError(std::string code, std::string message);

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

// Build the version-7 spatial grid with the recovered native constants and
// arithmetic order. The native implementation has undefined behavior for
// fewer than ten points. This bounded implementation keeps all available
// points in each cell and leaves neighborCount at the native value of ten.
// The builder preserves the recovered Visual C++ 2013 sort order for equal
// distances so that native grids remain byte-identical.
[[nodiscard]] AiSplineGrid
buildAiSplineGrid(const AiSpline& spline, AiSplineGridBuildLimits limits = {});

[[nodiscard]] inline AiSplineGrid
build_ai_spline_grid(const AiSpline& spline,
                     AiSplineGridBuildLimits limits = {}) {
    return buildAiSplineGrid(spline, limits);
}

} // namespace apex::formats
