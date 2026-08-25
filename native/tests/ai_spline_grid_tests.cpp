#include "apex/formats/ai_spline.hpp"
#include "apex/formats/ai_spline_grid.hpp"
#include "apex/formats/ai_spline_write.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace apex::formats;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename Function>
void expectsGridError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const AiSplineGridBuildError& error) {
        require(error.code() == code, "unexpected grid error code");
        return;
    }
    throw std::runtime_error("expected AI spline grid error");
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "AI spline fixture must be readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

AiSpline pointSpline(std::vector<std::array<float, 3>> positions) {
    AiSpline spline;
    spline.source = "grid-unit.ai";
    spline.points.reserve(positions.size());
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        spline.points.push_back(
            {positions[index], 0.0F, static_cast<std::int32_t>(index)});
    }
    return spline;
}

void buildsBoundedSmallSplines() {
    const auto onePoint = pointSpline({{{0.0F, 5000.0F, 0.0F}}});
    const auto grid = buildAiSplineGrid(onePoint);
    require(grid.maximum == std::array<float, 3>{350.0F, 0.0F, 350.0F} &&
                grid.minimum == std::array<float, 3>{-350.0F, 0.0F, -350.0F},
            "one-point grid bounds ignore Y");
    require(grid.neighborCount == 10U && grid.samplingDensity == 10.0F &&
                grid.rows.size() == 70U &&
                grid.rows.front().cells.size() == 70U,
            "one-point grid shape");
    for (const auto& row : grid.rows) {
        for (const auto& cell : row.cells)
            require(cell.pointIndices == std::vector<std::uint32_t>{0U},
                    "one-point grid uses its available neighbor");
    }

    const auto identical =
        pointSpline(std::vector<std::array<float, 3>>(11U, {0.0F, 0.0F, 0.0F}));
    const auto identicalGrid = buildAiSplineGrid(identical);
    require(
        identicalGrid.rows.front().cells.front().pointIndices ==
            std::vector<std::uint32_t>{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U},
        "equal-distance order matches the native small-range sort");

    const auto twoPoints =
        pointSpline({{{0.0F, 0.0F, 0.0F}}, {{10.0F, -9000.0F, 0.0F}}});
    const auto twoPointGrid = buildAiSplineGrid(twoPoints);
    require(twoPointGrid.rows.size() == 71U &&
                twoPointGrid.rows.front().cells.size() == 70U,
            "dimension conversion truncates after single-precision scaling");
}

void preservesRecoveredNegativeMaximumInitializer() {
    const auto spline =
        pointSpline({{{-1000.0F, 0.0F, -1000.0F}}, {{-900.0F, 0.0F, -800.0F}}});
    const auto grid = buildAiSplineGrid(spline);
    require(std::bit_cast<std::uint32_t>(grid.maximum[0]) ==
                    std::bit_cast<std::uint32_t>(350.0F) &&
                std::bit_cast<std::uint32_t>(grid.maximum[2]) ==
                    std::bit_cast<std::uint32_t>(350.0F),
            "negative-only bounds preserve the native positive initializer");
    require(grid.rows.size() == 170U && grid.rows.front().cells.size() == 170U,
            "negative-only native bounds affect grid dimensions");
}

void rejectsMalformedAndOverLimitInput() {
    AiSpline empty;
    expectsGridError([&] { (void)buildAiSplineGrid(empty); }, "GRID_EMPTY");

    auto legacy = pointSpline({{{0.0F, 0.0F, 0.0F}}});
    legacy.version = 2U;
    expectsGridError([&] { (void)buildAiSplineGrid(legacy); },
                     "UNSUPPORTED_VERSION");

    auto nonFinite = pointSpline({{{0.0F, 0.0F, 0.0F}}});
    nonFinite.points[0].position[2] = std::numeric_limits<float>::quiet_NaN();
    expectsGridError([&] { (void)buildAiSplineGrid(nonFinite); }, "NON_FINITE");

    auto extreme =
        pointSpline({{{std::numeric_limits<float>::max(), 0.0F, 0.0F}}});
    expectsGridError([&] { (void)buildAiSplineGrid(extreme); },
                     "GRID_DIMENSION_INVALID");

    const auto normal = pointSpline({{{0.0F, 0.0F, 0.0F}}});
    auto rowLimits = AiSplineGridBuildLimits{};
    rowLimits.maxGridRows = 69U;
    expectsGridError([&] { (void)buildAiSplineGrid(normal, rowLimits); },
                     "COUNT_LIMIT");

    auto cellLimits = AiSplineGridBuildLimits{};
    cellLimits.maxGridCells = 4'899U;
    expectsGridError([&] { (void)buildAiSplineGrid(normal, cellLimits); },
                     "COUNT_LIMIT");

    auto indexLimits = AiSplineGridBuildLimits{};
    indexLimits.maxGridIndices = 4'899U;
    expectsGridError([&] { (void)buildAiSplineGrid(normal, indexLimits); },
                     "COUNT_LIMIT");

    auto neighborLimits = AiSplineGridBuildLimits{};
    neighborLimits.maxGridNeighbors = 9U;
    expectsGridError([&] { (void)buildAiSplineGrid(normal, neighborLimits); },
                     "COUNT_LIMIT");

    auto workLimits = AiSplineGridBuildLimits{};
    workLimits.maxDistanceEvaluations = 4'899U;
    expectsGridError([&] { (void)buildAiSplineGrid(normal, workLimits); },
                     "WORK_LIMIT");

    auto memoryLimits = AiSplineGridBuildLimits{};
    memoryLimits.maxAggregateBytes = 1U;
    expectsGridError([&] { (void)buildAiSplineGrid(normal, memoryLimits); },
                     "MEMORY_LIMIT");
}

void reproducesNativeRepositoryFixture() {
    const auto root = std::filesystem::path(__FILE__)
                          .parent_path()
                          .parent_path()
                          .parent_path();
    const auto sourcePath = root / "test/content/tracks/sepang/ai/pit_lane.ai";
    const auto expectedPath =
        root / "test/content/tracks/sepang/ai/pit_lane_with_grid.ai";
    const auto sourceBytes = readFile(sourcePath);
    const auto expectedBytes = readFile(expectedPath);
    auto spline = parseAiSpline(sourceBytes, sourcePath.generic_string());
    require(!spline.grid.has_value() && spline.points.size() == 4'329U,
            "native fixture source has no grid");
    spline.grid = buildAiSplineGrid(spline);
    require(spline.grid->rows.size() == 185U &&
                spline.grid->rows.front().cells.size() == 163U,
            "native fixture grid shape");
    require(spline.grid->rows[0].cells[0].pointIndices ==
                std::vector<std::uint32_t>{1055U, 1056U, 1054U, 1057U, 1053U,
                                           1058U, 1052U, 1059U, 1051U, 1060U},
            "native fixture first cell ordering");
    const auto expectedSpline = parseAiSpline(expectedBytes);
    require(expectedSpline.grid.has_value(),
            "native fixture oracle contains a grid");
    for (std::size_t rowIndex = 0U; rowIndex < spline.grid->rows.size();
         ++rowIndex) {
        for (std::size_t cellIndex = 0U;
             cellIndex < spline.grid->rows[rowIndex].cells.size();
             ++cellIndex) {
            const auto& rebuilt =
                spline.grid->rows[rowIndex].cells[cellIndex].pointIndices;
            const auto& expected = expectedSpline.grid->rows[rowIndex]
                                       .cells[cellIndex]
                                       .pointIndices;
            for (std::size_t neighbor = 0U; neighbor < rebuilt.size();
                 ++neighbor) {
                if (rebuilt[neighbor] != expected[neighbor]) {
                    throw std::runtime_error(
                        "native fixture index differs at row " +
                        std::to_string(rowIndex) + ", cell " +
                        std::to_string(cellIndex) + ", neighbor " +
                        std::to_string(neighbor) + ": got " +
                        std::to_string(rebuilt[neighbor]) + ", expected " +
                        std::to_string(expected[neighbor]));
                }
            }
        }
    }
    const auto rebuiltBytes = serializeAiSpline(spline);
    require(rebuiltBytes.size() == expectedBytes.size(),
            "rebuilt native fixture size");
    for (std::size_t offset = 0U; offset < rebuiltBytes.size(); ++offset) {
        if (rebuiltBytes[offset] != expectedBytes[offset]) {
            throw std::runtime_error("rebuilt native fixture differs at byte " +
                                     std::to_string(offset));
        }
    }
    require(!parseAiSpline(sourceBytes).grid.has_value(),
            "grid generation does not mutate the source bytes");
}

} // namespace

int main() {
    try {
        buildsBoundedSmallSplines();
        preservesRecoveredNegativeMaximumInitializer();
        rejectsMalformedAndOverLimitInput();
        reproducesNativeRepositoryFixture();
        return 0;
    } catch (const std::exception& error) {
        return (std::fprintf(stderr, "AI spline grid tests failed: %s\n",
                             error.what()),
                1);
    }
}
