#include "apex/formats/ai_spline_grid.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace apex::formats {
namespace {

constexpr float kSamplingDensity = 10.0F;
constexpr float kBoundsMargin = 350.0F;
constexpr std::uint32_t kNeighborCount = 10U;

struct ComparablePoint {
    std::uint32_t index = 0U;
    float distance = 0.0F;
};

using ComparableIterator = std::vector<ComparablePoint>::iterator;

[[nodiscard]] bool pointLess(const ComparablePoint& left,
                             const ComparablePoint& right) noexcept {
    return left.distance < right.distance;
}

void insertionSort(ComparableIterator first, ComparableIterator last) {
    if (first == last)
        return;
    for (auto next = first; ++next != last;) {
        if (pointLess(*next, *first)) {
            auto after = next;
            std::rotate(first, next, ++after);
        } else {
            auto destination = next;
            for (auto previous = destination; pointLess(*next, *--previous);)
                destination = previous;
            if (destination != next) {
                auto after = next;
                std::rotate(destination, next, ++after);
            }
        }
    }
}

void medianOfThree(ComparableIterator first, ComparableIterator middle,
                   ComparableIterator last) {
    if (pointLess(*middle, *first))
        std::iter_swap(middle, first);
    if (pointLess(*last, *middle))
        std::iter_swap(last, middle);
    if (pointLess(*middle, *first))
        std::iter_swap(middle, first);
}

void selectMedian(ComparableIterator first, ComparableIterator middle,
                  ComparableIterator last) {
    if (last - first > 40) {
        const auto step = (last - first + 1) / 8;
        medianOfThree(first, first + step, first + 2 * step);
        medianOfThree(middle - step, middle, middle + step);
        medianOfThree(last - 2 * step, last - step, last);
        medianOfThree(first + step, middle, last - step);
    } else {
        medianOfThree(first, middle, last);
    }
}

[[nodiscard]] std::pair<ComparableIterator, ComparableIterator>
partitionAsMsvc2013(ComparableIterator first, ComparableIterator last) {
    auto middle = first + (last - first) / 2;
    selectMedian(first, middle, last - 1);
    auto pivotFirst = middle;
    auto pivotLast = pivotFirst + 1;
    while (first < pivotFirst && !pointLess(*(pivotFirst - 1), *pivotFirst) &&
           !pointLess(*pivotFirst, *(pivotFirst - 1)))
        --pivotFirst;
    while (pivotLast < last && !pointLess(*pivotLast, *pivotFirst) &&
           !pointLess(*pivotFirst, *pivotLast))
        ++pivotLast;

    auto greaterFirst = pivotLast;
    auto greaterLast = pivotFirst;
    for (;;) {
        for (; greaterFirst < last; ++greaterFirst) {
            if (pointLess(*pivotFirst, *greaterFirst)) {
                continue;
            }
            if (pointLess(*greaterFirst, *pivotFirst))
                break;
            std::iter_swap(pivotLast++, greaterFirst);
        }
        for (; first < greaterLast; --greaterLast) {
            if (pointLess(*(greaterLast - 1), *pivotFirst)) {
                continue;
            }
            if (pointLess(*pivotFirst, *(greaterLast - 1)))
                break;
            std::iter_swap(--pivotFirst, greaterLast - 1);
        }
        if (greaterLast == first && greaterFirst == last)
            return {pivotFirst, pivotLast};
        if (greaterLast == first) {
            if (pivotLast != greaterFirst)
                std::iter_swap(pivotFirst, pivotLast);
            ++pivotLast;
            std::iter_swap(pivotFirst++, greaterFirst++);
        } else if (greaterFirst == last) {
            if (--greaterLast != --pivotFirst)
                std::iter_swap(greaterLast, pivotFirst);
            std::iter_swap(pivotFirst, --pivotLast);
        } else {
            std::iter_swap(greaterFirst++, --greaterLast);
        }
    }
}

void pushHeap(ComparableIterator first, std::ptrdiff_t hole, std::ptrdiff_t top,
              ComparablePoint value) {
    for (auto parent = (hole - 1) / 2;
         top < hole && pointLess(*(first + parent), value);
         parent = (hole - 1) / 2) {
        *(first + hole) = *(first + parent);
        hole = parent;
    }
    *(first + hole) = value;
}

void adjustHeap(ComparableIterator first, std::ptrdiff_t hole,
                std::ptrdiff_t bottom, ComparablePoint value) {
    const auto top = hole;
    auto child = 2 * hole + 2;
    for (; child < bottom; child = 2 * child + 2) {
        if (pointLess(*(first + child), *(first + (child - 1))))
            --child;
        *(first + hole) = *(first + child);
        hole = child;
    }
    if (child == bottom) {
        *(first + hole) = *(first + (bottom - 1));
        hole = bottom - 1;
    }
    pushHeap(first, hole, top, value);
}

void makeHeap(ComparableIterator first, ComparableIterator last) {
    const auto bottom = last - first;
    for (auto hole = bottom / 2; hole > 0;) {
        --hole;
        adjustHeap(first, hole, bottom, *(first + hole));
    }
}

void popHeap(ComparableIterator first, ComparableIterator last) {
    const auto destination = last - 1;
    const auto value = *destination;
    *destination = *first;
    adjustHeap(first, 0, destination - first, value);
}

void sortHeap(ComparableIterator first, ComparableIterator last) {
    while (last - first > 1)
        popHeap(first, last--);
}

void sortAsMsvc2013(ComparableIterator first, ComparableIterator last,
                    std::ptrdiff_t ideal) {
    std::ptrdiff_t count = 0;
    while ((count = last - first) > 32 && ideal > 0) {
        const auto middle = partitionAsMsvc2013(first, last);
        ideal /= 2;
        ideal += ideal / 2;
        if (middle.first - first < last - middle.second) {
            sortAsMsvc2013(first, middle.first, ideal);
            first = middle.second;
        } else {
            sortAsMsvc2013(middle.second, last, ideal);
            last = middle.first;
        }
    }
    if (count > 32) {
        makeHeap(first, last);
        sortHeap(first, last);
    } else if (count > 1) {
        insertionSort(first, last);
    }
}

void sortComparablePointsAsNative(std::vector<ComparablePoint>& points) {
    sortAsMsvc2013(points.begin(), points.end(),
                   static_cast<std::ptrdiff_t>(points.size()));
}

[[noreturn]] void fail(const char* code, const char* message) {
    throw AiSplineGridBuildError(code, message);
}

[[nodiscard]] bool checkedMultiply(std::size_t left, std::size_t right,
                                   std::size_t& result) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

[[nodiscard]] bool checkedAdd(std::size_t left, std::size_t right,
                              std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

// This store preserves the scalar single-precision rounding points that occur
// in the recovered SSE instruction sequence.
[[nodiscard]] float roundedFloat(float value) noexcept {
    volatile float rounded = value;
    return rounded;
}

[[nodiscard]] std::size_t checkedDimension(float maximum, float minimum,
                                           std::size_t limit,
                                           const char* message) {
    const float width = roundedFloat(maximum - minimum);
    const float reciprocal = roundedFloat(1.0F / kSamplingDensity);
    const float scaled = roundedFloat(width * reciprocal);
    if (!std::isfinite(width) || !std::isfinite(scaled) || scaled < 1.0F ||
        scaled > static_cast<float>(std::numeric_limits<std::uint32_t>::max()))
        fail("GRID_DIMENSION_INVALID",
             "AI spline grid dimensions are not finite positive counts");
    const auto dimension = static_cast<std::uint32_t>(scaled);
    if (static_cast<std::size_t>(dimension) > limit)
        fail("COUNT_LIMIT", message);
    return static_cast<std::size_t>(dimension);
}

[[nodiscard]] float flatDistance(float queryX, float queryZ,
                                 const AiSplinePoint& point) noexcept {
    const float deltaX = roundedFloat(queryX - point.position[0]);
    const float deltaZ = roundedFloat(queryZ - point.position[2]);
    const float squaredX = roundedFloat(deltaX * deltaX);
    const float squaredZ = roundedFloat(deltaZ * deltaZ);
    const float squared =
        roundedFloat(roundedFloat(squaredX + 0.0F) + squaredZ);
    return roundedFloat(std::sqrt(squared));
}

void requireFinitePoints(const AiSpline& spline) {
    for (const auto& point : spline.points) {
        for (const float coordinate : point.position) {
            if (!std::isfinite(coordinate))
                fail("NON_FINITE",
                     "AI spline point coordinates must be finite");
        }
    }
}

} // namespace

AiSplineGridBuildError::AiSplineGridBuildError(std::string code,
                                               std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

AiSplineGrid buildAiSplineGrid(const AiSpline& spline,
                               AiSplineGridBuildLimits limits) {
    try {
        if (spline.version != 7U)
            fail("UNSUPPORTED_VERSION",
                 "AI spline grid generation supports version 7 only");
        if (spline.points.empty())
            fail("GRID_EMPTY", "AI spline grid generation needs a point");
        if (spline.points.size() > limits.maxPoints ||
            spline.points.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max()))
            fail("COUNT_LIMIT", "AI spline point count exceeds its limit");
        if (limits.maxGridNeighbors < kNeighborCount)
            fail("COUNT_LIMIT",
                 "AI spline grid neighbor count exceeds its limit");
        requireFinitePoints(spline);

        AiSplineGrid grid;
        grid.maximum = {std::bit_cast<float>(std::uint32_t{0x00800000U}), 0.0F,
                        std::bit_cast<float>(std::uint32_t{0x00800000U})};
        grid.minimum = {std::numeric_limits<float>::max(), 0.0F,
                        std::numeric_limits<float>::max()};
        grid.neighborCount = kNeighborCount;
        grid.samplingDensity = kSamplingDensity;

        for (const auto& point : spline.points) {
            grid.maximum[0] = std::max(grid.maximum[0], point.position[0]);
            grid.minimum[2] = std::min(grid.minimum[2], point.position[2]);
            grid.minimum[0] = std::min(grid.minimum[0], point.position[0]);
            grid.maximum[2] = std::max(grid.maximum[2], point.position[2]);
        }
        grid.minimum[0] = roundedFloat(grid.minimum[0] - kBoundsMargin);
        grid.minimum[2] = roundedFloat(grid.minimum[2] - kBoundsMargin);
        grid.maximum[0] = roundedFloat(grid.maximum[0] + kBoundsMargin);
        grid.maximum[2] = roundedFloat(grid.maximum[2] + kBoundsMargin);
        if (!std::isfinite(grid.minimum[0]) ||
            !std::isfinite(grid.minimum[2]) ||
            !std::isfinite(grid.maximum[0]) || !std::isfinite(grid.maximum[2]))
            fail("GRID_DIMENSION_INVALID",
                 "AI spline grid extents must be finite");

        const auto rowCount = checkedDimension(
            grid.maximum[0], grid.minimum[0], limits.maxGridRows,
            "AI spline grid row count exceeds its limit");
        const auto cellsPerRow = checkedDimension(
            grid.maximum[2], grid.minimum[2], limits.maxGridCellsPerRow,
            "AI spline grid cell count exceeds its per-row limit");

        std::size_t cellCount = 0U;
        if (!checkedMultiply(rowCount, cellsPerRow, cellCount) ||
            cellCount > limits.maxGridCells)
            fail("COUNT_LIMIT",
                 "AI spline grid aggregate cell count exceeds its limit");
        const auto effectiveNeighbors =
            std::min<std::size_t>(kNeighborCount, spline.points.size());
        std::size_t indexCount = 0U;
        if (!checkedMultiply(cellCount, effectiveNeighbors, indexCount) ||
            indexCount > limits.maxGridIndices)
            fail("COUNT_LIMIT",
                 "AI spline grid aggregate index count exceeds its limit");
        std::size_t distanceEvaluations = 0U;
        if (!checkedMultiply(cellCount, spline.points.size(),
                             distanceEvaluations) ||
            distanceEvaluations > limits.maxDistanceEvaluations)
            fail("WORK_LIMIT",
                 "AI spline grid distance work exceeds its limit");

        std::size_t rowBytes = 0U;
        std::size_t cellBytes = 0U;
        std::size_t indexBytes = 0U;
        std::size_t workBytes = 0U;
        std::size_t aggregateBytes = 0U;
        if (!checkedMultiply(rowCount, sizeof(AiSplineGridRow), rowBytes) ||
            !checkedMultiply(cellCount, sizeof(AiSplineGridCell), cellBytes) ||
            !checkedMultiply(indexCount, sizeof(std::uint32_t), indexBytes) ||
            !checkedMultiply(spline.points.size(), sizeof(ComparablePoint),
                             workBytes) ||
            !checkedAdd(rowBytes, cellBytes, aggregateBytes) ||
            !checkedAdd(aggregateBytes, indexBytes, aggregateBytes) ||
            !checkedAdd(aggregateBytes, workBytes, aggregateBytes) ||
            aggregateBytes > limits.maxAggregateBytes)
            fail("MEMORY_LIMIT",
                 "AI spline grid storage exceeds its memory limit");

        std::vector<ComparablePoint> nearest(spline.points.size());
        grid.rows.resize(rowCount);
        for (std::size_t rowIndex = 0U; rowIndex < rowCount; ++rowIndex) {
            auto& row = grid.rows[rowIndex];
            row.cells.resize(cellsPerRow);
            const float queryX =
                static_cast<float>(static_cast<double>(grid.minimum[0]) +
                                   (static_cast<double>(rowIndex) + 0.5) *
                                       static_cast<double>(kSamplingDensity));
            for (std::size_t cellIndex = 0U; cellIndex < cellsPerRow;
                 ++cellIndex) {
                const float queryZ = static_cast<float>(
                    static_cast<double>(grid.minimum[2]) +
                    (static_cast<double>(cellIndex) + 0.5) *
                        static_cast<double>(kSamplingDensity));
                for (std::size_t pointIndex = 0U;
                     pointIndex < spline.points.size(); ++pointIndex) {
                    nearest[pointIndex] = {
                        static_cast<std::uint32_t>(pointIndex),
                        flatDistance(queryX, queryZ,
                                     spline.points[pointIndex])};
                }
                sortComparablePointsAsNative(nearest);
                auto& indices = row.cells[cellIndex].pointIndices;
                indices.reserve(effectiveNeighbors);
                for (std::size_t neighbor = 0U; neighbor < effectiveNeighbors;
                     ++neighbor)
                    indices.push_back(nearest[neighbor].index);
            }
        }
        return grid;
    } catch (const AiSplineGridBuildError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw AiSplineGridBuildError(
            "ALLOCATION_FAILED",
            "AI spline grid allocation failed within configured limits");
    }
}

} // namespace apex::formats
