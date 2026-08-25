#include "apex/formats/ai_spline.hpp"

#include "apex/core/byte_reader.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::uint32_t kVersion7 = 7;
constexpr std::uint32_t kVersion2 = 2;
constexpr std::size_t kPointBytes = 20;
constexpr std::size_t kPayloadBytes = 72;
constexpr std::size_t kLegacyV2RecordBytes = 28;

[[nodiscard]] AiSplineError error(const std::string& source, std::size_t offset,
                                  const char* code, const char* message) {
    return AiSplineError("AI_SPLINE", source, offset, code, message);
}

struct Budget {
    std::size_t used = 0;
    std::size_t limit = 0;
    const std::string* source = nullptr;

    void charge(std::size_t bytes, std::size_t offset, const char* what) {
        if (bytes > std::numeric_limits<std::size_t>::max() - used ||
            used + bytes > limit) {
            throw error(*source, offset, "AGGREGATE_LIMIT", what);
        }
        used += bytes;
    }
};

[[nodiscard]] std::size_t storageBytes(std::size_t count, std::size_t elementSize,
                                       std::size_t offset, const std::string& source,
                                       const char* what) {
    return apex::core::checkedMultiply(count, elementSize, "AI_SPLINE", source,
                                       offset, what);
}

void requireCount(std::uint32_t count, std::size_t limit, std::size_t offset,
                  const std::string& source, const char* what) {
    if (static_cast<std::uint64_t>(count) > limit)
        throw error(source, offset, "COUNT_LIMIT", what);
}

void requireIndex(std::uint32_t index, std::size_t pointCount,
                  std::size_t offset, const std::string& source) {
    if (static_cast<std::uint64_t>(index) >= pointCount)
        throw error(source, offset, "INDEX_OUT_OF_RANGE", "AI spline grid index exceeds point count");
}

AiSpline parseAiSplineImpl(std::span<const std::uint8_t> bytes,
                           const std::string& source,
                           AiSplineParseLimits limits) {
    apex::core::ByteReader reader(bytes, source, limits.parse, "AI_SPLINE");
    const auto versionOffset = reader.offset();
    const auto version = reader.u32("version");
    if (version == kVersion2) {
        const auto pointCountOffset = reader.offset();
        const auto pointCount = reader.u32("version-2 point count");
        requireCount(pointCount, limits.maxPoints, pointCountOffset,
                     source, "AI spline version-2 point count exceeds its limit");
        const auto lapTime = reader.u32("version-2 lap time");
        const auto recordBytes = storageBytes(
            pointCount, kLegacyV2RecordBytes, pointCountOffset, source,
            "AI spline version-2 record storage");
        if (recordBytes > reader.remaining()) {
            throw error(source, reader.offset(), "TRUNCATED",
                        "AI spline version-2 records are truncated");
        }
        const auto retainedCount = static_cast<std::size_t>(pointCount / 3U) +
                                   (pointCount % 3U == 0U ? 0U : 1U);
        requireCount(static_cast<std::uint32_t>(retainedCount), limits.maxRetainedPoints,
                     pointCountOffset,
                     source, "AI spline version-2 retained point count exceeds its limit");

        Budget budget{0, limits.maxAggregateBytes, &source};
        budget.charge(source.size(), 0U,
                      "AI spline source name exceeds aggregate limit");
        budget.charge(storageBytes(pointCount, sizeof(AiSplineLegacyV2Record),
                                   pointCountOffset, source,
                                   "AI spline version-2 record storage"),
                      pointCountOffset,
                      "AI spline version-2 record storage exceeds aggregate limit");
        budget.charge(storageBytes(retainedCount, sizeof(std::uint32_t),
                                   pointCountOffset, source,
                                   "AI spline version-2 retained-index storage"),
                      pointCountOffset,
                      "AI spline version-2 retained-index storage exceeds aggregate limit");

        AiSpline result;
        result.source = source;
        result.version = version;
        result.lapTime = lapTime;
        result.legacyV2Records.reserve(pointCount);
        result.nativeRetainedIndices.reserve(retainedCount);
        for (std::uint32_t index = 0; index < pointCount; ++index) {
            AiSplineLegacyV2Record record;
            for (auto& value : record.position)
                value = reader.f32("version-2 point position");
            record.legacyWord = reader.u32("version-2 legacy word");
            record.speed = reader.f32("version-2 speed");
            record.gas = reader.f32("version-2 gas");
            record.lateralG = reader.f32("version-2 lateral G");
            result.legacyV2Records.push_back(record);
            if (index % 3U == 0U) result.nativeRetainedIndices.push_back(index);
        }
        if (reader.remaining() != 0U)
            throw error(source, reader.offset(), "TRAILING_DATA",
                        "unexpected trailing version-2 AI spline data");
        result.bytesRead = reader.offset();
        result.byteLength = bytes.size();
        return result;
    }
    if (version != kVersion7)
        throw error(source, versionOffset, "UNSUPPORTED_VERSION",
                    "only AI spline versions 2 and 7 are supported");

    const auto pointCountOffset = reader.offset();
    const auto pointCount = reader.u32("spline point count");
    requireCount(pointCount, limits.maxPoints, pointCountOffset,
                 source, "AI spline point count exceeds its limit");
    const auto lapTime = reader.u32("lap time");
    const auto reserved = reader.u32("reserved header word");

    if (static_cast<std::size_t>(pointCount) > reader.remaining() / kPointBytes)
        throw error(source, reader.offset(), "TRUNCATED", "AI spline point array is truncated");

    Budget budget{0, limits.maxAggregateBytes, &source};
    budget.charge(source.size(), 0U,
                  "AI spline source name exceeds aggregate limit");
    budget.charge(storageBytes(pointCount, sizeof(AiSplinePoint), pointCountOffset,
                                source, "AI spline point storage"),
                  pointCountOffset, "AI spline point storage exceeds aggregate limit");
    AiSpline result;
    result.source = source;
    result.version = version;
    result.lapTime = lapTime;
    result.reserved = reserved;
    result.points.reserve(pointCount);
    for (std::uint32_t index = 0; index < pointCount; ++index) {
        AiSplinePoint point;
        for (auto& value : point.position) value = reader.f32("spline point position");
        point.length = reader.f32("spline point length");
        point.tag = std::bit_cast<std::int32_t>(reader.u32("spline point tag"));
        result.points.push_back(point);
    }

    const auto payloadCountOffset = reader.offset();
    const auto payloadCount = reader.u32("payload count");
    requireCount(payloadCount, limits.maxPayloads, payloadCountOffset,
                 source, "AI spline payload count exceeds its limit");
    if (payloadCount != pointCount)
        throw error(source, payloadCountOffset, "COUNT_MISMATCH",
                    "AI spline payload count must equal point count");
    if (static_cast<std::size_t>(payloadCount) > reader.remaining() / kPayloadBytes)
        throw error(source, reader.offset(), "TRUNCATED", "AI spline payload array is truncated");
    budget.charge(storageBytes(payloadCount, sizeof(AiSplinePayload), payloadCountOffset,
                                source, "AI spline payload storage"),
                  payloadCountOffset, "AI spline payload storage exceeds aggregate limit");
    result.payloads.reserve(payloadCount);
    for (std::uint32_t index = 0; index < payloadCount; ++index) {
        AiSplinePayload payload;
        payload.speed = reader.f32("payload speed");
        payload.gas = reader.f32("payload gas");
        payload.brake = reader.f32("payload brake");
        payload.lateralG = reader.f32("payload lateral G");
        payload.radius = reader.f32("payload radius");
        payload.side0 = reader.f32("payload side 0");
        payload.side1 = reader.f32("payload side 1");
        payload.camber = reader.f32("payload camber");
        payload.direction = reader.f32("payload direction");
        for (auto& value : payload.normal) value = reader.f32("payload normal");
        payload.length = reader.f32("payload length");
        for (auto& value : payload.forward) value = reader.f32("payload forward");
        payload.reserved = reader.u32("payload reserved word");
        payload.grade = reader.f32("payload grade");
        result.payloads.push_back(payload);
    }

    const auto gridFlagOffset = reader.offset();
    const auto gridFlag = reader.u32("grid presence flag");
    if (gridFlag > 1)
        throw error(source, gridFlagOffset, "INVALID_GRID_FLAG", "AI spline grid flag must be zero or one");
    if (gridFlag == 1) {
        AiSplineGrid grid;
        for (auto& value : grid.maximum) value = reader.f32("grid maximum");
        for (auto& value : grid.minimum) value = reader.f32("grid minimum");
        const auto neighborOffset = reader.offset();
        grid.neighborCount = reader.u32("grid neighbor count");
        requireCount(grid.neighborCount, limits.maxGridNeighbors, neighborOffset,
                     source, "AI spline grid neighbor count exceeds its limit");
        grid.samplingDensity = reader.f32("grid sampling density");
        const auto rowCountOffset = reader.offset();
        const auto rowCount = reader.u32("grid row count");
        requireCount(rowCount, limits.maxGridRows, rowCountOffset,
                     source, "AI spline grid row count exceeds its limit");
        if (static_cast<std::size_t>(rowCount) > reader.remaining() / sizeof(std::uint32_t))
            throw error(source, reader.offset(), "TRUNCATED", "AI spline grid rows are truncated");
        budget.charge(storageBytes(rowCount, sizeof(AiSplineGridRow), rowCountOffset,
                                   source, "AI spline grid row storage"),
                      rowCountOffset, "AI spline grid row storage exceeds aggregate limit");
        grid.rows.reserve(rowCount);
        std::size_t totalIndices = 0;
        for (std::uint32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            AiSplineGridRow row;
            const auto cellCountOffset = reader.offset();
            const auto cellCount = reader.u32("grid cell count");
            requireCount(cellCount, limits.maxGridCellsPerRow, cellCountOffset,
                         source, "AI spline grid cell count exceeds its limit");
            if (static_cast<std::size_t>(cellCount) >
                reader.remaining() / sizeof(std::uint32_t)) {
                throw error(source, reader.offset(), "TRUNCATED",
                            "AI spline grid cells are truncated");
            }
            budget.charge(storageBytes(cellCount, sizeof(AiSplineGridCell), cellCountOffset,
                                       source, "AI spline grid cell storage"),
                          cellCountOffset, "AI spline grid cell storage exceeds aggregate limit");
            row.cells.reserve(cellCount);
            for (std::uint32_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
                AiSplineGridCell cell;
                const auto indexCountOffset = reader.offset();
                const auto indexCount = reader.u32("grid index count");
                requireCount(indexCount, limits.maxGridIndicesPerCell, indexCountOffset,
                             source, "AI spline grid index count exceeds its limit");
                if (static_cast<std::size_t>(indexCount) > limits.maxGridIndices - totalIndices)
                    throw error(source, indexCountOffset, "COUNT_LIMIT",
                                "AI spline grid aggregate index count exceeds its limit");
                totalIndices += indexCount;
                budget.charge(storageBytes(indexCount, sizeof(std::uint32_t), indexCountOffset,
                                           source, "AI spline grid index storage"),
                              indexCountOffset, "AI spline grid index storage exceeds aggregate limit");
                if (static_cast<std::size_t>(indexCount) > reader.remaining() / sizeof(std::uint32_t))
                    throw error(source, reader.offset(), "TRUNCATED", "AI spline grid indices are truncated");
                cell.pointIndices.reserve(indexCount);
                for (std::uint32_t index = 0; index < indexCount; ++index) {
                    const auto indexOffset = reader.offset();
                    const auto pointIndex = reader.u32("grid point index");
                    requireIndex(pointIndex, result.points.size(), indexOffset, source);
                    cell.pointIndices.push_back(pointIndex);
                }
                row.cells.push_back(std::move(cell));
            }
            grid.rows.push_back(std::move(row));
        }
        result.grid = std::move(grid);
    }
    if (reader.remaining() != 0)
        throw error(source, reader.offset(), "TRAILING_DATA", "unexpected trailing AI spline data");
    result.bytesRead = reader.offset();
    result.byteLength = bytes.size();
    return result;
}

}  // namespace

AiSpline parseAiSpline(std::span<const std::uint8_t> bytes, std::string source,
                       AiSplineParseLimits limits) {
    try {
        return parseAiSplineImpl(bytes, source, std::move(limits));
    } catch (const AiSplineError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw error(source, 0U, "ALLOCATION_FAILED",
                    "AI spline allocation failed within configured limits");
    }
}

}  // namespace apex::formats
