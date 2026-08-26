#include "apex/app/ai_spline_legacy_conversion.hpp"
#include "apex/app/installed_editor_spline.hpp"
#include "apex/formats/ai_spline.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using apex::app::AiSplineLegacyConversionLimits;
using apex::app::AiSplineLegacyConversionStatus;
using apex::app::convertAiSplineV2ToV7File;
using apex::formats::AiSpline;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void appendF32(std::vector<std::uint8_t>& output, float value) {
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

struct Record {
    std::array<float, 3U> position{};
    std::uint32_t legacyWord = 0U;
    float speed = 0.0F;
    float gas = 0.0F;
    float lateralG = 0.0F;
};

std::vector<std::uint8_t> legacyBytes(const std::vector<Record>& records,
                                      std::uint32_t lap_time = 456U) {
    std::vector<std::uint8_t> bytes;
    appendU32(bytes, 2U);
    appendU32(bytes, static_cast<std::uint32_t>(records.size()));
    appendU32(bytes, lap_time);
    for (const auto& record : records) {
        for (const float value : record.position) appendF32(bytes, value);
        appendU32(bytes, record.legacyWord);
        appendF32(bytes, record.speed);
        appendF32(bytes, record.gas);
        appendF32(bytes, record.lateralG);
    }
    return bytes;
}

AiSpline parsedLegacy(const std::vector<Record>& records) {
    return apex::formats::parseAiSpline(legacyBytes(records),
                                        "legacy-source.ai");
}

void convertsRecoveredFieldsAndRoundTrips() {
    const std::vector<Record> records = {
        {{0.0F, 0.0F, 0.0F}, 10U, 10.0F, 0.1F, 1.0F},
        {{10.0F, 0.0F, 0.0F}, 11U, 20.0F, 0.2F, 2.0F},
        {{20.0F, 0.0F, 0.0F}, 12U, 30.0F, 0.3F, 3.0F},
        {{100.0F, 0.0F, 0.0F}, 13U, 20.0F, 0.4F, 4.0F},
        {{110.0F, 0.0F, 0.0F}, 14U, 25.0F, 0.5F, 5.0F},
        {{120.0F, 0.0F, 0.0F}, 15U, 40.0F, 0.6F, 6.0F},
        {{200.0F, 0.0F, 0.0F}, 16U, 50.0F, 0.7F, 7.0F},
        {{210.0F, 0.0F, 0.0F}, 17U, 55.0F, 0.8F, 8.0F},
        {{220.0F, 0.0F, 0.0F}, 18U, 70.0F, 0.85F, 9.0F},
        {{400.0F, 0.0F, 0.0F}, 19U, 60.0F, 0.9F, 10.0F},
    };
    const AiSpline source = parsedLegacy(records);
    const auto converted = convertAiSplineV2ToV7File(source);
    require(converted.ok(), "valid legacy spline must convert");
    require(converted.status == AiSplineLegacyConversionStatus::converted,
            "successful legacy conversion status mismatch");
    const auto spline = apex::formats::parseAiSpline(
        converted.bytes, "converted-v7.ai");
    require(spline.version == 7U && spline.lapTime == 456U &&
                spline.reserved == 0U,
            "converted header mismatch");
    require(spline.points.size() == 4U && spline.payloads.size() == 4U,
            "converted retained count mismatch");
    require(spline.legacyV2Records.empty() &&
                spline.nativeRetainedIndices.empty() &&
                spline.nativeRetainedForwards.empty(),
            "converted spline retained legacy-only state");
    require(spline.grid.has_value(), "converted nonempty spline needs a grid");

    const std::array<std::size_t, 4U> source_indices = {0U, 3U, 6U, 9U};
    for (std::size_t index = 0U; index < source_indices.size(); ++index) {
        const auto source_index = source_indices[index];
        require(spline.points[index].position == records[source_index].position,
                "converted point position mismatch");
        require(spline.points[index].tag == static_cast<std::int32_t>(index),
                "converted point tag must use the retained ordinal");
        const auto& payload = spline.payloads[index];
        require(payload.speed == records[source_index].speed &&
                    payload.lateralG == records[source_index].lateralG,
                "converted serialized v2 payload fields mismatch");
        require(payload.radius == 0.0F && payload.side0 == 0.0F &&
                    payload.side1 == 0.0F && payload.camber == 0.0F &&
                    payload.direction == 1.0F &&
                    payload.normal == std::array<float, 3U>{} &&
                    payload.length == 0.0F && payload.reserved == 0U &&
                    payload.grade == 0.0F,
                "converted payload defaults mismatch");
        require(payload.forward == source.nativeRetainedForwards[index],
                "converted forward mismatch");
    }
    require(spline.payloads[0U].gas == 1.0F &&
                spline.payloads[0U].brake == 0.0F,
            "positive native acceleration must force full gas");
    require(spline.payloads[1U].gas == 0.4F &&
                spline.payloads[1U].brake == 1.0F,
            "negative native acceleration must preserve gas and clamp brake");
    require(spline.payloads[2U].gas == 1.0F &&
                spline.payloads[2U].brake == 0.0F,
            "later positive native acceleration mismatch");
    require(spline.payloads[3U].gas == 0.9F &&
                spline.payloads[3U].brake == 1.0F,
            "later negative native acceleration mismatch");

    apex::app::InstalledEditorSpline expected_lengths;
    for (const auto index : source_indices)
        expected_lengths.points.push_back(records[index].position);
    expected_lengths.closed = apex::app::installedEditorSplineIsClosed(
        expected_lengths.points);
    require(apex::app::recomputeInstalledEditorSplineLengths(
                expected_lengths),
            "expected native point lengths must compute");
    const std::array<std::uint32_t, 4U> expected_length_bits = {
        0x00000000U, 0x42c7ff86U, 0x4347ff67U, 0x43c7ff94U};
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        require(std::bit_cast<std::uint32_t>(
                    expected_lengths.cumulative_lengths[index]) ==
                    expected_length_bits[index],
                "recovered scalar length arithmetic changed");
        require(std::bit_cast<std::uint32_t>(spline.points[index].length) ==
                    std::bit_cast<std::uint32_t>(
                        expected_lengths.cumulative_lengths[index]),
                "converted point length mismatch");
    }

    require(spline.version == 7U && spline.points.size() == 4U &&
                spline.payloads.size() == 4U && spline.grid.has_value() &&
                converted.pointCount == 4U && converted.gridBuilt,
            "converted bytes must parse as a complete version-7 spline");
}

void convertsEmptyAndSinglePointInputs() {
    apex::app::InstalledEditorSpline invalid_empty_topology;
    invalid_empty_topology.closed = true;
    require(!apex::app::recomputeInstalledEditorSplineLengths(
                invalid_empty_topology),
            "closed empty spline topology must be rejected safely");

    apex::app::InstalledEditorSpline overflowed_length;
    overflowed_length.points = {
        {std::numeric_limits<float>::max(), 0.0F, 0.0F},
        {-std::numeric_limits<float>::max(), 0.0F, 0.0F}};
    overflowed_length.cumulative_lengths = {12.0F};
    overflowed_length.closing_length = 13.0F;
    overflowed_length.length = 14.0F;
    require(!apex::app::recomputeInstalledEditorSplineLengths(
                overflowed_length) &&
                overflowed_length.cumulative_lengths ==
                    std::vector<float>{12.0F} &&
                overflowed_length.closing_length == 13.0F &&
                overflowed_length.length == 14.0F,
            "failed native length calculation must preserve prior lengths");

    const auto empty = convertAiSplineV2ToV7File(parsedLegacy({}));
    require(empty.ok() && empty.pointCount == 0U && !empty.gridBuilt,
            "empty legacy spline must become an empty gridless v7 spline");
    const auto empty_roundtrip =
        apex::formats::parseAiSpline(empty.bytes, "empty-converted.ai");
    require(empty_roundtrip.version == 7U && empty_roundtrip.points.empty(),
            "empty converted bytes must parse");

    const std::vector<Record> one_record = {
        {{1.0F, 2.0F, 3.0F}, 99U, 4.0F, 0.25F, 6.0F},
    };
    const auto one = convertAiSplineV2ToV7File(parsedLegacy(one_record));
    const auto one_spline =
        apex::formats::parseAiSpline(one.bytes, "one-converted.ai");
    require(one.ok() && one_spline.points.size() == 1U &&
                one_spline.points[0U].length == 0.0F &&
                one_spline.grid.has_value(),
            "one retained point must keep zero length and build a grid");
    require(one_spline.payloads[0U].gas == 1.0F &&
                one_spline.payloads[0U].brake == 0.0F,
            "single positive-speed payload mapping mismatch");

    const std::vector<Record> zero_chord_records = {
        {{0.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F},
        {{10.0F, 0.0F, 0.0F}, 0U, 2.0F, 0.0F, 0.0F},
        {{20.0F, 0.0F, 0.0F}, 0U, 3.0F, 0.0F, 0.0F},
        {{100.0F, 0.0F, 0.0F}, 0U, 4.0F, 0.0F, 0.0F},
        {{80.0F, 0.0F, 0.0F}, 0U, 5.0F, 0.0F, 0.0F},
        {{40.0F, 0.0F, 0.0F}, 0U, 6.0F, 0.0F, 0.0F},
        {{0.0F, 0.0F, 0.0F}, 0U, 7.0F, 0.0F, 0.0F},
    };
    const auto zero_chord =
        convertAiSplineV2ToV7File(parsedLegacy(zero_chord_records));
    const auto zero_chord_spline = apex::formats::parseAiSpline(
        zero_chord.bytes, "zero-chord-converted.ai");
    require(zero_chord.ok() && zero_chord_spline.points.size() == 3U &&
                zero_chord_spline.points[1U].length > 0.0F &&
                zero_chord_spline.points[2U].length >
                    zero_chord_spline.points[1U].length,
            "native zero endpoint chord must keep finite cumulative lengths");
}

void rejectsInconsistentAndNonFiniteObjects() {
    AiSpline version7;
    version7.version = 7U;
    auto result = convertAiSplineV2ToV7File(version7);
    require(result.status == AiSplineLegacyConversionStatus::unsupported &&
                result.bytes.empty(),
            "version-7 conversion must be unsupported and atomic");

    AiSpline mixed = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F}});
    mixed.points.push_back({});
    result = convertAiSplineV2ToV7File(mixed);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_STATE_INVALID",
            "mixed legacy and v7 state must be rejected");

    AiSpline bad_retention = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F},
         {{2.0F, 0.0F, 0.0F}, 0U, 2.0F, 0.0F, 0.0F},
         {{3.0F, 0.0F, 0.0F}, 0U, 3.0F, 0.0F, 0.0F},
         {{4.0F, 0.0F, 0.0F}, 0U, 4.0F, 0.0F, 0.0F}});
    bad_retention.nativeRetainedIndices[1U] = 2U;
    result = convertAiSplineV2ToV7File(bad_retention);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_RETENTION_INVALID",
            "non-native retained stride must be rejected");

    AiSpline bad_forward = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F}});
    bad_forward.nativeRetainedForwards[0U][0U] = -1.0F;
    result = convertAiSplineV2ToV7File(bad_forward);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_FORWARD_INVALID",
            "inconsistent retained forward must be rejected");

    AiSpline non_finite = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F}});
    non_finite.legacyV2Records[0U].gas =
        std::numeric_limits<float>::infinity();
    result = convertAiSplineV2ToV7File(non_finite);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_NON_FINITE",
            "direct non-finite legacy field must be rejected");

    const float maximum = std::numeric_limits<float>::max();
    AiSpline overflowed_forward = parsedLegacy(
        {{{0.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F},
         {{1.0F, 0.0F, 0.0F}, 0U, 2.0F, 0.0F, 0.0F},
         {{-maximum, 0.0F, 0.0F}, 0U, 3.0F, 0.0F, 0.0F},
         {{maximum, 0.0F, 0.0F}, 0U, 4.0F, 0.0F, 0.0F}});
    result = convertAiSplineV2ToV7File(overflowed_forward);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_FORWARD_NON_FINITE",
            "overflowed retained forward must be rejected");

    AiSpline equal_speed = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.2F, 0.0F},
         {{2.0F, 0.0F, 0.0F}, 0U, 2.0F, 0.3F, 0.0F},
         {{3.0F, 0.0F, 0.0F}, 0U, 5.0F, 0.4F, 0.0F},
         {{4.0F, 0.0F, 0.0F}, 0U, 5.0F, 0.5F, 0.0F}});
    result = convertAiSplineV2ToV7File(equal_speed);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_ACCELERATION_NON_FINITE" &&
                result.bytes.empty(),
            "native equal-speed NaN must fail before publication");
}

void enforcesConversionLimits() {
    const AiSpline source = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F}});
    AiSplineLegacyConversionLimits limits;
    limits.maxRecords = 0U;
    auto result = convertAiSplineV2ToV7File(source, limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit,
            "legacy record limit must be enforced");

    limits = {};
    limits.maxRetainedPoints = 0U;
    result = convertAiSplineV2ToV7File(source, limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit,
            "retained point limit must be enforced");

    limits = {};
    limits.maxLengthSampleEvaluations = 0U;
    const AiSpline two_points = parsedLegacy(
        {{{1.0F, 0.0F, 0.0F}, 0U, 1.0F, 0.0F, 0.0F},
         {{2.0F, 0.0F, 0.0F}, 0U, 2.0F, 0.0F, 0.0F},
         {{3.0F, 0.0F, 0.0F}, 0U, 3.0F, 0.0F, 0.0F},
         {{100.0F, 0.0F, 0.0F}, 0U, 4.0F, 0.0F, 0.0F}});
    result = convertAiSplineV2ToV7File(two_points, limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit,
            "length work limit must be enforced");

    limits = {};
    limits.maxAggregateBytes = 1U;
    result = convertAiSplineV2ToV7File(source, limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit,
            "conversion aggregate limit must be enforced");

    limits = {};
    limits.maxSourceNameBytes = source.source.size() - 1U;
    result = convertAiSplineV2ToV7File(source, limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit &&
                result.diagnostics.back().code ==
                    "AI_SPLINE_LEGACY_SOURCE_LIMIT",
            "conversion source-name limit must be enforced");

    limits = {};
    limits.write.maxOutputBytes = 23U;
    result = convertAiSplineV2ToV7File(parsedLegacy({}), limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit &&
                result.diagnostics.back().code == "OUTPUT_LIMIT",
            "writer output limit must remain atomic");

    limits = {};
    limits.grid.maxGridCells = 0U;
    result = convertAiSplineV2ToV7File(source, limits);
    require(result.status == AiSplineLegacyConversionStatus::resource_limit &&
                result.bytes.empty(),
            "grid limit failure must remain atomic");

    const AiSpline extreme = parsedLegacy(
        {{{std::numeric_limits<float>::max(), 0.0F, 0.0F},
          0U, 1.0F, 0.0F, 0.0F}});
    result = convertAiSplineV2ToV7File(extreme);
    require(result.status == AiSplineLegacyConversionStatus::invalid &&
                result.diagnostics.back().code ==
                    "GRID_DIMENSION_INVALID" &&
                result.bytes.empty(),
            "invalid finite grid extents must fail before publication");
}

} // namespace

int main() {
    try {
        convertsRecoveredFieldsAndRoundTrips();
        convertsEmptyAndSinglePointInputs();
        rejectsInconsistentAndNonFiniteObjects();
        enforcesConversionLimits();
        std::cout << "ai_spline_legacy_conversion_tests: ok\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ai_spline_legacy_conversion_tests: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
