#include "apex/formats/ai_spline_write.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace apex::formats {
namespace {

[[noreturn]] void fail(const char* code, const char* message) {
    throw AiSplineWriteError(code, message);
}

[[nodiscard]] std::uint32_t bounded_count(std::size_t count,
                                          std::size_t limit,
                                          const char* message) {
    if (count > limit ||
        count > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max()))
        fail("COUNT_LIMIT", message);
    return static_cast<std::uint32_t>(count);
}

void require_finite(float value, const char* message) {
    if (!std::isfinite(value)) fail("NON_FINITE", message);
}

template <std::size_t Size>
void require_finite(const std::array<float, Size>& values,
                    const char* message) {
    for (const float value : values) require_finite(value, message);
}

class Writer final {
public:
    explicit Writer(std::size_t limit) : limit_(limit) {
        bytes_.reserve(std::min(limit, std::size_t{1024U}));
    }

    void u32(std::uint32_t value) {
        ensure(4U);
        bytes_.push_back(static_cast<std::uint8_t>(value));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8U));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 16U));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 24U));
    }

    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }

    [[nodiscard]] std::vector<std::uint8_t>&& finish() {
        return std::move(bytes_);
    }

private:
    void ensure(std::size_t count) const {
        if (bytes_.size() > limit_ || count > limit_ - bytes_.size())
            fail("OUTPUT_LIMIT",
                 "AI spline output exceeds the configured byte limit");
    }

    std::size_t limit_ = 0U;
    std::vector<std::uint8_t> bytes_;
};

void write_point(Writer& writer, const AiSplinePoint& point,
                 std::size_t payload_count) {
    require_finite(point.position,
                   "AI spline point coordinates must be finite");
    require_finite(point.length, "AI spline point length must be finite");
    if (point.tag < 0 ||
        static_cast<std::size_t>(point.tag) >= payload_count)
        fail("PAYLOAD_INDEX_INVALID",
             "AI spline point tag is outside the payload array");
    for (const float value : point.position) writer.f32(value);
    writer.f32(point.length);
    writer.u32(std::bit_cast<std::uint32_t>(point.tag));
}

void write_payload(Writer& writer, const AiSplinePayload& payload) {
    require_finite(payload.speed, "AI spline payload speed must be finite");
    require_finite(payload.gas, "AI spline payload gas must be finite");
    require_finite(payload.brake, "AI spline payload brake must be finite");
    require_finite(payload.lateralG,
                   "AI spline payload lateral G must be finite");
    require_finite(payload.radius,
                   "AI spline payload radius must be finite");
    require_finite(payload.side0,
                   "AI spline payload left side must be finite");
    require_finite(payload.side1,
                   "AI spline payload right side must be finite");
    require_finite(payload.camber,
                   "AI spline payload camber must be finite");
    require_finite(payload.direction,
                   "AI spline payload direction must be finite");
    require_finite(payload.normal,
                   "AI spline payload normal must be finite");
    require_finite(payload.length,
                   "AI spline payload length must be finite");
    require_finite(payload.forward,
                   "AI spline payload forward vector must be finite");
    require_finite(payload.grade,
                   "AI spline payload grade must be finite");

    writer.f32(payload.speed);
    writer.f32(payload.gas);
    writer.f32(payload.brake);
    writer.f32(payload.lateralG);
    writer.f32(payload.radius);
    writer.f32(payload.side0);
    writer.f32(payload.side1);
    writer.f32(payload.camber);
    writer.f32(payload.direction);
    for (const float value : payload.normal) writer.f32(value);
    writer.f32(payload.length);
    for (const float value : payload.forward) writer.f32(value);
    writer.u32(0U);
    writer.f32(payload.grade);
}

void write_grid(Writer& writer, const AiSplineGrid& grid,
                std::size_t point_count,
                const AiSplineWriteLimits& limits) {
    require_finite(grid.maximum,
                   "AI spline grid maximum must be finite");
    require_finite(grid.minimum,
                   "AI spline grid minimum must be finite");
    require_finite(grid.samplingDensity,
                   "AI spline grid sampling density must be finite");
    if (static_cast<std::size_t>(grid.neighborCount) >
        limits.maxGridNeighbors)
        fail("COUNT_LIMIT",
             "AI spline grid neighbor count exceeds its limit");
    const std::uint32_t row_count = bounded_count(
        grid.rows.size(), limits.maxGridRows,
        "AI spline grid row count exceeds its limit");

    for (const float value : grid.maximum) writer.f32(value);
    for (const float value : grid.minimum) writer.f32(value);
    writer.u32(grid.neighborCount);
    writer.f32(grid.samplingDensity);
    writer.u32(row_count);

    std::size_t total_indices = 0U;
    for (const AiSplineGridRow& row : grid.rows) {
        writer.u32(bounded_count(
            row.cells.size(), limits.maxGridCellsPerRow,
            "AI spline grid cell count exceeds its limit"));
        for (const AiSplineGridCell& cell : row.cells) {
            const std::uint32_t index_count = bounded_count(
                cell.pointIndices.size(), limits.maxGridIndicesPerCell,
                "AI spline grid cell index count exceeds its limit");
            if (total_indices > limits.maxGridIndices ||
                cell.pointIndices.size() >
                    limits.maxGridIndices - total_indices)
                fail("COUNT_LIMIT",
                     "AI spline grid aggregate index count exceeds its limit");
            total_indices += cell.pointIndices.size();
            writer.u32(index_count);
            for (const std::uint32_t point_index : cell.pointIndices) {
                if (static_cast<std::size_t>(point_index) >= point_count)
                    fail("GRID_INDEX_INVALID",
                         "AI spline grid index is outside the point array");
                writer.u32(point_index);
            }
        }
    }
}

} // namespace

AiSplineWriteError::AiSplineWriteError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

std::vector<std::uint8_t> serializeAiSpline(
    const AiSpline& spline, AiSplineWriteLimits limits) {
    try {
        if (spline.version != 7U)
            fail("UNSUPPORTED_VERSION",
                 "AI spline writer supports version 7 only");
        const std::uint32_t point_count = bounded_count(
            spline.points.size(), limits.maxPoints,
            "AI spline point count exceeds its limit");
        const std::uint32_t payload_count = bounded_count(
            spline.payloads.size(), limits.maxPayloads,
            "AI spline payload count exceeds its limit");
        if (point_count != payload_count)
            fail("COUNT_MISMATCH",
                 "AI spline payload count must equal point count");

        Writer writer(limits.maxOutputBytes);
        writer.u32(7U);
        writer.u32(point_count);
        writer.u32(spline.lapTime);
        writer.u32(0U);
        for (const AiSplinePoint& point : spline.points)
            write_point(writer, point, spline.payloads.size());
        writer.u32(payload_count);
        for (const AiSplinePayload& payload : spline.payloads)
            write_payload(writer, payload);
        writer.u32(spline.grid.has_value() ? 1U : 0U);
        if (spline.grid.has_value())
            write_grid(writer, *spline.grid, spline.points.size(), limits);
        return writer.finish();
    } catch (const AiSplineWriteError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw AiSplineWriteError(
            "ALLOCATION_FAILED",
            "AI spline output allocation failed within configured limits");
    }
}

} // namespace apex::formats
