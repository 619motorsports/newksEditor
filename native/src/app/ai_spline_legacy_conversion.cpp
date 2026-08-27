#include "apex/app/ai_spline_legacy_conversion.hpp"

#include "apex/app/installed_editor_spline.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace apex::app {
namespace {

constexpr std::size_t kLengthEvaluationsPerSegment = 1001U;

[[nodiscard]] AiSplineLegacyConversionModelResult failure(
    AiSplineLegacyConversionStatus status, std::string code,
    std::string message) {
    AiSplineLegacyConversionModelResult result;
    result.status = status;
    result.diagnostics.push_back(
        {std::move(code), std::move(message)});
    return result;
}

[[nodiscard]] bool checkedMultiply(std::size_t left, std::size_t right,
                                   std::size_t& result) noexcept {
    if (left != 0U &&
        right > std::numeric_limits<std::size_t>::max() / left)
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

[[nodiscard]] bool finite(const std::array<float, 3U>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](float component) {
                           return std::isfinite(component);
                       });
}

[[nodiscard]] std::array<float, 3U> normalizedDifference(
    const std::array<float, 3U>& current,
    const std::array<float, 3U>& previous) noexcept {
    const float x = current[0U] - previous[0U];
    const float y = current[1U] - previous[1U];
    const float z = current[2U] - previous[2U];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length == 0.0F)
        return {x, y, z};
    const float scale = 1.0F / length;
    return {scale * x, scale * y, scale * z};
}

[[nodiscard]] bool sameFloat(float left, float right) noexcept {
    return std::bit_cast<std::uint32_t>(left) ==
           std::bit_cast<std::uint32_t>(right);
}

[[nodiscard]] bool sameVector(const std::array<float, 3U>& left,
                              const std::array<float, 3U>& right) noexcept {
    return sameFloat(left[0U], right[0U]) &&
           sameFloat(left[1U], right[1U]) &&
           sameFloat(left[2U], right[2U]);
}

[[nodiscard]] float roundedFloat(float value) noexcept {
    volatile float rounded = value;
    return rounded;
}

[[nodiscard]] float saturate(float value) noexcept {
    if (value > 1.0F) return 1.0F;
    if (value < 0.0F) return 0.0F;
    return value;
}

[[nodiscard]] bool resourceLimitCode(const std::string& code) noexcept {
    return code == "COUNT_LIMIT" || code == "WORK_LIMIT" ||
           code == "SORT_WORK_LIMIT" || code == "MEMORY_LIMIT" ||
           code == "OUTPUT_LIMIT" || code == "AGGREGATE_LIMIT";
}

} // namespace

AiSplineLegacyConversionLimits aiSplineLegacyConversionLimitsForSession(
    const authoring::AiSplineSessionLimits& limits) noexcept {
    AiSplineLegacyConversionLimits result;
    result.write = limits.write;
    result.grid = limits.grid;
    result.grid.maxPoints =
        std::min(result.grid.maxPoints, limits.write.maxPoints);
    result.grid.maxGridNeighbors = std::min(
        result.grid.maxGridNeighbors, limits.write.maxGridNeighbors);
    result.grid.maxGridRows =
        std::min(result.grid.maxGridRows, limits.write.maxGridRows);
    result.grid.maxGridCellsPerRow = std::min(
        result.grid.maxGridCellsPerRow,
        limits.write.maxGridCellsPerRow);
    result.grid.maxGridIndices = std::min(
        result.grid.maxGridIndices, limits.write.maxGridIndices);

    const std::size_t model_budget =
        limits.maxSnapshotModelBytes > sizeof(formats::AiSpline)
            ? limits.maxSnapshotModelBytes - sizeof(formats::AiSpline)
            : 0U;
    result.maxAggregateBytes =
        std::min(result.maxAggregateBytes, model_budget);
    result.grid.maxAggregateBytes =
        std::min(result.grid.maxAggregateBytes, model_budget);
    result.maxSourceNameBytes =
        std::min(result.maxSourceNameBytes, model_budget);
    result.maxRetainedPoints = std::min(
        {result.maxRetainedPoints, result.grid.maxPoints,
         result.write.maxPoints, result.write.maxPayloads});
    const std::size_t maximum_records =
        result.maxRetainedPoints >
                std::numeric_limits<std::size_t>::max() / 3U
            ? std::numeric_limits<std::size_t>::max()
            : result.maxRetainedPoints * 3U;
    result.maxRecords = std::min(result.maxRecords, maximum_records);
    const std::size_t maximum_length_work =
        result.maxRetainedPoints >
                std::numeric_limits<std::size_t>::max() /
                    kLengthEvaluationsPerSegment
            ? std::numeric_limits<std::size_t>::max()
            : result.maxRetainedPoints * kLengthEvaluationsPerSegment;
    result.maxLengthSampleEvaluations = std::min(
        result.maxLengthSampleEvaluations, maximum_length_work);
    return result;
}

AiSplineLegacyConversionModelResult convertAiSplineV2ToV7Model(
    const formats::AiSpline& source,
    AiSplineLegacyConversionLimits limits) {
    try {
        if (source.version != 2U) {
            return failure(
                AiSplineLegacyConversionStatus::unsupported,
                "AI_SPLINE_LEGACY_VERSION_UNSUPPORTED",
                "AI spline conversion requires a version-2 source");
        }
        if (!source.points.empty() || !source.payloads.empty() ||
            source.grid.has_value() || source.reserved != 0U) {
            return failure(
                AiSplineLegacyConversionStatus::invalid,
                "AI_SPLINE_LEGACY_STATE_INVALID",
                "Version-2 AI spline data contains version-7 state");
        }
        if (source.source.size() > limits.maxSourceNameBytes) {
            return failure(
                AiSplineLegacyConversionStatus::resource_limit,
                "AI_SPLINE_LEGACY_SOURCE_LIMIT",
                "AI spline source name exceeds the conversion limit");
        }

        const std::size_t record_count = source.legacyV2Records.size();
        if (record_count > limits.maxRecords) {
            return failure(
                AiSplineLegacyConversionStatus::resource_limit,
                "AI_SPLINE_LEGACY_RECORD_LIMIT",
                "Version-2 AI spline record count exceeds the conversion limit");
        }
        const std::size_t retained_count =
            record_count / 3U + (record_count % 3U == 0U ? 0U : 1U);
        if (retained_count > limits.maxRetainedPoints ||
            retained_count > limits.grid.maxPoints ||
            retained_count > limits.write.maxPoints ||
            retained_count > limits.write.maxPayloads ||
            retained_count > static_cast<std::size_t>(
                                 std::numeric_limits<std::int32_t>::max())) {
            return failure(
                AiSplineLegacyConversionStatus::resource_limit,
                "AI_SPLINE_LEGACY_RETAINED_LIMIT",
                "Retained AI spline point count exceeds the conversion limit");
        }
        if (source.nativeRetainedIndices.size() != retained_count ||
            source.nativeRetainedForwards.size() != retained_count) {
            return failure(
                AiSplineLegacyConversionStatus::invalid,
                "AI_SPLINE_LEGACY_RETENTION_INVALID",
                "Version-2 retained index and forward data is inconsistent");
        }

        std::size_t length_work = 0U;
        const std::size_t maximum_segments =
            retained_count >= 2U ? retained_count : 0U;
        if (!checkedMultiply(maximum_segments,
                             kLengthEvaluationsPerSegment, length_work) ||
            length_work > limits.maxLengthSampleEvaluations) {
            return failure(
                AiSplineLegacyConversionStatus::resource_limit,
                "AI_SPLINE_LEGACY_LENGTH_WORK_LIMIT",
                "AI spline length work exceeds the conversion limit");
        }

        std::size_t persistent_aggregate = source.source.size();
        for (const std::size_t element_size : {
                 sizeof(formats::AiSplinePoint),
                 sizeof(formats::AiSplinePayload)}) {
            std::size_t bytes = 0U;
            if (!checkedMultiply(retained_count, element_size, bytes) ||
                !checkedAdd(persistent_aggregate, bytes,
                            persistent_aggregate) ||
                persistent_aggregate > limits.maxAggregateBytes) {
                return failure(
                    AiSplineLegacyConversionStatus::resource_limit,
                    "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                    "AI spline conversion storage exceeds its aggregate limit");
            }
        }
        std::size_t length_aggregate = persistent_aggregate;
        for (const std::size_t element_size : {
                 sizeof(InstalledEditorSplinePoint), sizeof(float)}) {
            std::size_t bytes = 0U;
            if (!checkedMultiply(retained_count, element_size, bytes) ||
                !checkedAdd(length_aggregate, bytes, length_aggregate) ||
                length_aggregate > limits.maxAggregateBytes) {
                return failure(
                    AiSplineLegacyConversionStatus::resource_limit,
                    "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                    "AI spline conversion storage exceeds its aggregate limit");
            }
        }

        std::array<float, 3U> previous_position{};
        for (std::size_t index = 0U; index < record_count; ++index) {
            const auto& record = source.legacyV2Records[index];
            if (!finite(record.position) || !std::isfinite(record.speed) ||
                !std::isfinite(record.gas) ||
                !std::isfinite(record.lateralG)) {
                return failure(
                    AiSplineLegacyConversionStatus::invalid,
                    "AI_SPLINE_LEGACY_NON_FINITE",
                    "Version-2 AI spline records must contain finite values");
            }
            const auto forward =
                normalizedDifference(record.position, previous_position);
            if (!finite(forward)) {
                return failure(
                    AiSplineLegacyConversionStatus::invalid,
                    "AI_SPLINE_LEGACY_FORWARD_NON_FINITE",
                    "Version-2 AI spline forward calculation is not finite");
            }
            if (index % 3U == 0U) {
                const std::size_t retained_index = index / 3U;
                if (source.nativeRetainedIndices[retained_index] != index) {
                    return failure(
                        AiSplineLegacyConversionStatus::invalid,
                        "AI_SPLINE_LEGACY_RETENTION_INVALID",
                        "Version-2 retained indices do not use the native stride");
                }
                if (retained_index != 0U || retained_count <= 1U) {
                    if (!sameVector(
                            source.nativeRetainedForwards[retained_index],
                            forward)) {
                        return failure(
                            AiSplineLegacyConversionStatus::invalid,
                            "AI_SPLINE_LEGACY_FORWARD_INVALID",
                            "Version-2 retained forward data does not match the source records");
                    }
                }
            }
            previous_position = record.position;
        }
        if (retained_count > 1U) {
            const auto expected = normalizedDifference(
                source.legacyV2Records.front().position,
                source.legacyV2Records[
                    source.nativeRetainedIndices.back()].position);
            if (!finite(expected)) {
                return failure(
                    AiSplineLegacyConversionStatus::invalid,
                    "AI_SPLINE_LEGACY_FORWARD_NON_FINITE",
                    "Version-2 wraparound forward calculation is not finite");
            }
            if (!sameVector(source.nativeRetainedForwards.front(), expected)) {
                return failure(
                    AiSplineLegacyConversionStatus::invalid,
                    "AI_SPLINE_LEGACY_FORWARD_INVALID",
                    "Version-2 wraparound forward data does not match the source records");
            }
        }

        formats::AiSpline candidate;
        candidate.source = source.source;
        candidate.version = 7U;
        candidate.lapTime = source.lapTime;
        candidate.points.reserve(retained_count);
        candidate.payloads.reserve(retained_count);

        float previous_speed = 0.0F;
        for (std::size_t index = 0U; index < record_count; ++index) {
            const auto& record = source.legacyV2Records[index];
            formats::AiSplinePayload payload;
            payload.speed = record.speed;
            payload.gas = record.gas;
            payload.lateralG = record.lateralG;
            payload.direction = 1.0F;

            const float speed_delta =
                roundedFloat(payload.speed - previous_speed);
            const float time =
                roundedFloat(payload.length / payload.speed);
            const float acceleration = roundedFloat(speed_delta / time);
            if (acceleration >= 0.0F)
                payload.gas = 1.0F;
            else
                payload.brake = saturate(-acceleration);
            previous_speed = payload.speed;

            if (index % 3U != 0U) continue;
            const std::size_t retained_index = index / 3U;
            payload.forward =
                source.nativeRetainedForwards[retained_index];
            if (!std::isfinite(payload.gas) ||
                !std::isfinite(payload.brake)) {
                return failure(
                    AiSplineLegacyConversionStatus::invalid,
                    "AI_SPLINE_LEGACY_ACCELERATION_NON_FINITE",
                    "Native version-2 acceleration produced a non-finite payload");
            }
            candidate.points.push_back(formats::AiSplinePoint{
                record.position, 0.0F,
                static_cast<std::int32_t>(retained_index)});
            candidate.payloads.push_back(payload);
        }

        if (retained_count >= 2U) {
            InstalledEditorSpline interpolating;
            interpolating.points.reserve(retained_count);
            for (const auto& point : candidate.points)
                interpolating.points.push_back(point.position);
            interpolating.closed =
                installedEditorSplineIsClosed(interpolating.points);
            if (!recomputeInstalledEditorSplineLengths(interpolating) ||
                interpolating.cumulative_lengths.size() != retained_count) {
                return failure(
                    AiSplineLegacyConversionStatus::invalid,
                    "AI_SPLINE_LEGACY_LENGTH_INVALID",
                    "Native AI spline length calculation produced an invalid result");
            }
            for (std::size_t index = 0U; index < retained_count; ++index) {
                const float length = interpolating.cumulative_lengths[index];
                if (!std::isfinite(length)) {
                    return failure(
                        AiSplineLegacyConversionStatus::invalid,
                        "AI_SPLINE_LEGACY_LENGTH_INVALID",
                        "Native AI spline point length is not finite");
                }
                candidate.points[index].length = length;
            }
        }

        if (!candidate.points.empty()) {
            auto grid_limits = limits.grid;
            grid_limits.maxAggregateBytes = std::min(
                grid_limits.maxAggregateBytes,
                limits.maxAggregateBytes - persistent_aggregate);
            candidate.grid =
                formats::buildAiSplineGrid(candidate, grid_limits);
        }

        std::size_t serialized_aggregate = persistent_aggregate;
        if (candidate.grid.has_value()) {
            const auto& grid = *candidate.grid;
            std::size_t grid_bytes = 0U;
            std::size_t bytes = 0U;
            if (!checkedMultiply(grid.rows.size(),
                                 sizeof(formats::AiSplineGridRow), bytes) ||
                !checkedAdd(grid_bytes, bytes, grid_bytes)) {
                return failure(
                    AiSplineLegacyConversionStatus::resource_limit,
                    "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                    "AI spline grid storage exceeds the conversion limit");
            }
            for (const auto& row : grid.rows) {
                if (!checkedMultiply(row.cells.size(),
                                     sizeof(formats::AiSplineGridCell),
                                     bytes) ||
                    !checkedAdd(grid_bytes, bytes, grid_bytes)) {
                    return failure(
                        AiSplineLegacyConversionStatus::resource_limit,
                        "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                        "AI spline grid storage exceeds the conversion limit");
                }
                for (const auto& cell : row.cells) {
                    if (!checkedMultiply(cell.pointIndices.size(),
                                         sizeof(std::uint32_t), bytes) ||
                        !checkedAdd(grid_bytes, bytes, grid_bytes)) {
                        return failure(
                            AiSplineLegacyConversionStatus::resource_limit,
                            "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                            "AI spline grid storage exceeds the conversion limit");
                    }
                }
            }
            if (!checkedAdd(serialized_aggregate, grid_bytes,
                            serialized_aggregate) ||
                serialized_aggregate > limits.maxAggregateBytes) {
                return failure(
                    AiSplineLegacyConversionStatus::resource_limit,
                    "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                    "AI spline conversion storage exceeds its aggregate limit");
            }
        }
        AiSplineLegacyConversionModelResult result;
        result.status = AiSplineLegacyConversionStatus::converted;
        result.pointCount = candidate.points.size();
        result.gridBuilt = candidate.grid.has_value();
        result.aggregateBytes = serialized_aggregate;
        result.model = std::move(candidate);
        return result;
    } catch (const formats::AiSplineGridBuildError& error) {
        return failure(resourceLimitCode(error.code())
                           ? AiSplineLegacyConversionStatus::resource_limit
                           : AiSplineLegacyConversionStatus::invalid,
                       error.code(), error.what());
    } catch (const formats::AiSplineWriteError& error) {
        const bool allocation = error.code() == "ALLOCATION_FAILED";
        return failure(
            allocation
                ? AiSplineLegacyConversionStatus::allocation_failed
                : resourceLimitCode(error.code())
                      ? AiSplineLegacyConversionStatus::resource_limit
                      : AiSplineLegacyConversionStatus::invalid,
            error.code(), error.what());
    } catch (const std::bad_alloc&) {
        return failure(
            AiSplineLegacyConversionStatus::allocation_failed,
            "AI_SPLINE_LEGACY_ALLOCATION_FAILED",
            "AI spline conversion allocation failed within configured limits");
    }
}

AiSplineLegacyConversionResult convertAiSplineV2ToV7File(
    const formats::AiSpline& source,
    AiSplineLegacyConversionLimits limits) {
    AiSplineLegacyConversionResult result;
    const auto model = convertAiSplineV2ToV7Model(source, limits);
    result.status = model.status;
    result.pointCount = model.pointCount;
    result.gridBuilt = model.gridBuilt;
    result.diagnostics = model.diagnostics;
    if (!model.ok()) return result;

    try {
        auto write_limits = limits.write;
        if (model.aggregateBytes > limits.maxAggregateBytes) {
            result.status = AiSplineLegacyConversionStatus::resource_limit;
            result.diagnostics.push_back({
                "AI_SPLINE_LEGACY_AGGREGATE_LIMIT",
                "AI spline conversion storage exceeds its aggregate limit"});
            return result;
        }
        write_limits.maxOutputBytes = std::min(
            write_limits.maxOutputBytes,
            limits.maxAggregateBytes - model.aggregateBytes);
        result.bytes = formats::serializeAiSpline(*model.model, write_limits);
        result.status = AiSplineLegacyConversionStatus::converted;
        return result;
    } catch (const formats::AiSplineWriteError& error) {
        const bool allocation = error.code() == "ALLOCATION_FAILED";
        result.status = allocation
                            ? AiSplineLegacyConversionStatus::allocation_failed
                            : resourceLimitCode(error.code())
                                  ? AiSplineLegacyConversionStatus::resource_limit
                                  : AiSplineLegacyConversionStatus::invalid;
        result.diagnostics.push_back({error.code(), error.what()});
        return result;
    } catch (const std::bad_alloc&) {
        result.status = AiSplineLegacyConversionStatus::allocation_failed;
        result.diagnostics.push_back({
            "AI_SPLINE_LEGACY_ALLOCATION_FAILED",
            "AI spline conversion allocation failed within configured limits"});
        return result;
    }
}

const char* aiSplineLegacyConversionStatusName(
    AiSplineLegacyConversionStatus status) noexcept {
    switch (status) {
    case AiSplineLegacyConversionStatus::converted: return "converted";
    case AiSplineLegacyConversionStatus::unsupported: return "unsupported";
    case AiSplineLegacyConversionStatus::invalid: return "invalid";
    case AiSplineLegacyConversionStatus::resource_limit:
        return "resource_limit";
    case AiSplineLegacyConversionStatus::allocation_failed:
        return "allocation_failed";
    }
    return "invalid";
}

} // namespace apex::app
