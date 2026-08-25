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

// Assetto Corsa AI spline files are little-endian binary records.  Version 7
// and the legacy version-2 layout are kept separate: the legacy records do
// not acquire fields which are only present in the version-7 representation.
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

// Version 2 stores every source sample as this 28-byte record.  The legacy
// word is read by the native loader but is not interpreted there.
struct AiSplineLegacyV2Record {
    std::array<float, 3> position{};
    std::uint32_t legacyWord = 0;
    float speed = 0.0f;
    float gas = 0.0f;
    float lateralG = 0.0f;
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
    std::size_t maxRetainedPoints = 1'000'000;
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
    // Populated only for version 2.  Version-7 points/payloads remain empty
    // for this representation because their missing fields are not inferred.
    std::vector<AiSplineLegacyV2Record> legacyV2Records;
    // Native version-2 loading retains source indices 0, 3, 6, ... .
    std::vector<std::uint32_t> nativeRetainedIndices;
    // Forward vectors for nativeRetainedIndices, in the same order. These
    // are derived from each retained record's immediately previous raw
    // record, before the three-to-one retention gate. The first vector is
    // replaced by retained-point wraparound when there are at least two
    // retained points.
    std::vector<std::array<float, 3>> nativeRetainedForwards;
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
