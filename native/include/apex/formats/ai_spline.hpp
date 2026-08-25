#pragma once

#include "apex/core/parse_error.hpp"
#include "apex/core/parse_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

// Assetto Corsa AI spline files are little-endian binary records.  This
// reader deliberately supports only the version-7 layout recovered from the
// editor.  Version 2 and older layouts have different records and are not
// guessed here.
struct AiSplinePoint {
    std::array<float, 3> position{};
    float length = 0.0f;
    std::int32_t tag = 0;
};

struct AiSplinePayload {
    float speed = 0.0f;
    float gas = 0.0f;
    float brake = 0.0f;
    float lateralG = 0.0f;
    float radius = 0.0f;
    float side0 = 0.0f;
    float side1 = 0.0f;
    float camber = 0.0f;
    float direction = 0.0f;
    std::array<float, 3> normal{};
    float length = 0.0f;
    std::array<float, 3> forward{};
    std::uint32_t reserved = 0;
    float grade = 0.0f;
};

struct AiSplineGridCell {
    std::vector<std::uint32_t> pointIndices;
};

struct AiSplineGridRow {
    std::vector<AiSplineGridCell> cells;
};

struct AiSplineGrid {
    std::array<float, 3> maximum{};
    std::array<float, 3> minimum{};
    std::uint32_t neighborCount = 0;
    float samplingDensity = 0.0f;
    std::vector<AiSplineGridRow> rows;
};

struct AiSplineParseLimits {
    apex::core::ParseLimits parse{};
    std::size_t maxPoints = 1'000'000;
    std::size_t maxPayloads = 1'000'000;
    std::size_t maxGridNeighbors = 1'000'000;
    std::size_t maxGridRows = 1'000'000;
    std::size_t maxGridCellsPerRow = 1'000'000;
    std::size_t maxGridIndicesPerCell = 1'000'000;
    std::size_t maxGridIndices = 10'000'000;
    std::size_t maxAggregateBytes = 512u * 1024u * 1024u;
};

using AiSplineError = apex::core::ParseError;

struct AiSpline {
    std::string source;
    std::uint32_t version = 7;
    std::uint32_t lapTime = 0;
    std::uint32_t reserved = 0;
    std::vector<AiSplinePoint> points;
    std::vector<AiSplinePayload> payloads;
    std::optional<AiSplineGrid> grid;
    std::size_t bytesRead = 0;
    std::size_t byteLength = 0;
};

[[nodiscard]] AiSpline parseAiSpline(
    std::span<const std::uint8_t> bytes, std::string source = "ideal_line.ai",
    AiSplineParseLimits limits = {});

inline AiSpline parse_ai_spline(
    std::span<const std::uint8_t> bytes, std::string source = "ideal_line.ai",
    AiSplineParseLimits limits = {}) {
    return parseAiSpline(bytes, std::move(source), std::move(limits));
}

}  // namespace apex::formats
