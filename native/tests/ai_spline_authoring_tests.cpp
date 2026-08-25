#include "apex/authoring/ai_spline.hpp"
#include "apex/formats/ai_spline.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::authoring::AiSplineWaypointEdit;
using apex::authoring::AiSplineWaypointInfo;
using apex::formats::AiSpline;
using apex::formats::AiSplineGrid;
using apex::formats::AiSplineGridCell;
using apex::formats::AiSplineGridRow;
using apex::formats::AiSplinePayload;
using apex::formats::AiSplinePoint;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

AiSpline fixture() {
    AiSpline spline;
    spline.source = "waypoint-fixture.ai";
    spline.version = 7U;
    spline.lapTime = 123U;
    spline.points = {
        AiSplinePoint{{1.0F, 2.0F, 3.0F}, 4.0F, 2},
        AiSplinePoint{{5.0F, 6.0F, 7.0F}, 8.0F, 0},
        AiSplinePoint{{9.0F, 10.0F, 11.0F}, 12.0F, 1},
    };
    for (std::uint32_t index = 0U; index < 3U; ++index) {
        AiSplinePayload payload;
        const float base = 100.0F * static_cast<float>(index + 1U);
        payload.speed = base + 1.0F;
        payload.gas = base + 2.0F;
        payload.brake = base + 3.0F;
        payload.lateralG = base + 4.0F;
        payload.radius = base + 5.0F;
        payload.side0 = base + 6.0F;
        payload.side1 = base + 7.0F;
        payload.camber = 0.1F * static_cast<float>(index + 1U);
        payload.direction = base + 9.0F;
        payload.normal = {base + 10.0F, base + 11.0F, base + 12.0F};
        payload.length = base + 13.0F;
        payload.forward = {base + 14.0F, base + 15.0F, base + 16.0F};
        payload.reserved = 0x12340000U + index;
        payload.grade = base + 18.0F;
        spline.payloads.push_back(payload);
    }
    AiSplineGrid grid;
    grid.maximum = {9.0F, 10.0F, 11.0F};
    grid.minimum = {1.0F, 2.0F, 3.0F};
    grid.neighborCount = 2U;
    grid.samplingDensity = 0.5F;
    grid.rows = {AiSplineGridRow{{AiSplineGridCell{{0U, 2U}}}}};
    spline.grid = grid;
    return spline;
}

bool samePayload(const AiSplinePayload& left, const AiSplinePayload& right) {
    return left.speed == right.speed && left.gas == right.gas &&
           left.brake == right.brake && left.lateralG == right.lateralG &&
           left.radius == right.radius && left.side0 == right.side0 &&
           left.side1 == right.side1 && left.camber == right.camber &&
           left.direction == right.direction && left.normal == right.normal &&
           left.length == right.length && left.forward == right.forward &&
           left.reserved == right.reserved && left.grade == right.grade;
}

void readsSixUiFieldsThroughPointTag() {
    const auto spline = fixture();
    const std::array<std::uint32_t, 1> selection{0U};
    const auto result =
        apex::authoring::readAiSplineWaypointInfo(spline, selection);
    require(result.ok() && result.info.has_value(), "single waypoint reads");
    require(result.pointIndex == 0U && result.payloadIndex == 2U,
            "point tag selects payload");
    require(result.info->radius == 305.0F && result.info->side0 == 306.0F &&
                result.info->side1 == 307.0F && result.info->length == 313.0F &&
                result.info->grade == 318.0F,
            "direct UI fields map to recovered payload offsets");
    require(
        result.info->camberDegrees ==
            0.3F * apex::authoring::aiSplineRadiansToDegrees,
        "camber getter converts radians to degrees with recovered constant");
}

void appliesReplacementThenAdditionAtomically() {
    const auto spline = fixture();
    const std::array<std::uint32_t, 1> selection{0U};
    AiSplineWaypointEdit edit;
    edit.replacement = {10.0F, 20.0F, 30.0F, 45.0F, 50.0F, 60.0F};
    edit.additive = {1.0F, 2.0F, 3.0F, -12.5F, 5.0F, 6.0F};
    const auto result =
        apex::authoring::applyAiSplineWaypointEdit(spline, selection, edit);
    require(result.ok() && result.candidate.has_value() &&
                !result.bytes.empty() && result.before.has_value() &&
                result.after.has_value() && result.changed,
            "valid waypoint edit returns a complete candidate and bytes");

    const auto& payload = result.candidate->payloads[2];
    require(payload.radius == 11.0F && payload.side0 == 22.0F &&
                payload.side1 == 33.0F && payload.length == 55.0F &&
                payload.grade == 66.0F,
            "addition follows replacement for direct fields");
    require(payload.camber ==
                45.0F * apex::authoring::aiSplineDegreesToRadians +
                    -12.5F * apex::authoring::aiSplineDegreesToRadians,
            "camber replacement and addition convert degrees separately");
    require(samePayload(spline.payloads[0], result.candidate->payloads[0]) &&
                samePayload(spline.payloads[1], result.candidate->payloads[1]),
            "unselected payloads stay unchanged");
    require(
        result.candidate->payloads[2].speed == spline.payloads[2].speed &&
            result.candidate->payloads[2].normal == spline.payloads[2].normal &&
            result.candidate->payloads[2].forward == spline.payloads[2].forward,
        "unexposed payload fields stay unchanged");
    require(spline.payloads[2].radius == 305.0F, "baseline is not mutated");

    const auto reparsed =
        apex::formats::parseAiSpline(result.bytes, "edited-waypoint.ai");
    require(reparsed.payloads[2].radius == 11.0F &&
                reparsed.payloads[2].camber == payload.camber &&
                reparsed.points[0].tag == 2 && reparsed.grid.has_value() &&
                reparsed.grid->rows[0].cells[0].pointIndices ==
                    std::vector<std::uint32_t>{0U, 2U},
            "serialized candidate round-trips with tag and grid preserved");
}

void preservesNativeZeroSentinels() {
    const auto spline = fixture();
    const std::array<std::uint32_t, 1> selection{1U};
    AiSplineWaypointEdit edit;
    edit.replacement.radius = -0.0F;
    edit.replacement.side0 = 0.0F;
    edit.additive.radius = 0.0F;
    const auto result =
        apex::authoring::applyAiSplineWaypointEdit(spline, selection, edit);
    require(result.ok() && result.candidate.has_value() && !result.changed,
            "positive and negative replacement zero mean unchanged");
    require(samePayload(spline.payloads[0], result.candidate->payloads[0]),
            "zero-sentinel edit preserves target payload");

    AiSplineWaypointEdit clearEdit;
    clearEdit.additive.radius = -105.0F;
    const auto clearResult = apex::authoring::applyAiSplineWaypointEdit(
        spline, selection, clearEdit);
    require(clearResult.ok() && clearResult.candidate.has_value() &&
                clearResult.changed &&
                clearResult.candidate->payloads[0].radius == 0.0F,
            "additive input can produce a zero field");
}

void rejectsInvalidSelectionVersionAndTags() {
    const auto spline = fixture();
    const std::array<std::uint32_t, 0> empty{};
    const std::array<std::uint32_t, 2> multiple{0U, 1U};
    const std::array<std::uint32_t, 1> outside{3U};
    require(!apex::authoring::readAiSplineWaypointInfo(spline, empty).ok(),
            "empty selection rejects");
    require(
        !apex::authoring::applyAiSplineWaypointEdit(spline, multiple, {}).ok(),
        "multiple selection rejects");
    require(
        !apex::authoring::applyAiSplineWaypointEdit(spline, outside, {}).ok(),
        "out-of-range point rejects");

    auto invalidTag = spline;
    invalidTag.points[0].tag = -1;
    const std::array<std::uint32_t, 1> selected{0U};
    const auto tagResult =
        apex::authoring::applyAiSplineWaypointEdit(invalidTag, selected, {});
    require(!tagResult.ok() && !tagResult.candidate.has_value() &&
                tagResult.bytes.empty() &&
                tagResult.diagnostics.back().code == "PAYLOAD_INDEX_INVALID",
            "negative point tag rejects without candidate bytes");

    auto mismatch = spline;
    mismatch.payloads.pop_back();
    require(!apex::authoring::applyAiSplineWaypointEdit(mismatch, selected, {})
                 .ok(),
            "point and payload mismatch rejects");

    auto legacy = spline;
    legacy.version = 2U;
    legacy.points.clear();
    legacy.payloads.clear();
    const auto legacyResult =
        apex::authoring::applyAiSplineWaypointEdit(legacy, selected, {});
    require(!legacyResult.ok() &&
                legacyResult.status ==
                    apex::authoring::AiSplineWaypointStatus::unsupported,
            "version 2 editing is explicitly unsupported");
}

void rejectsNonFiniteInputsAndResultsWithoutMutation() {
    const auto spline = fixture();
    const std::array<std::uint32_t, 1> selection{0U};
    AiSplineWaypointEdit nanEdit;
    nanEdit.replacement.side1 = std::numeric_limits<float>::quiet_NaN();
    const auto nanResult =
        apex::authoring::applyAiSplineWaypointEdit(spline, selection, nanEdit);
    require(!nanResult.ok() && !nanResult.candidate.has_value() &&
                nanResult.bytes.empty() && spline.payloads[2].side1 == 307.0F,
            "non-finite edit rejects without baseline mutation");

    auto overflowSpline = spline;
    overflowSpline.payloads[2].radius = std::numeric_limits<float>::max();
    AiSplineWaypointEdit overflowEdit;
    overflowEdit.additive.radius = std::numeric_limits<float>::max();
    const auto overflowResult = apex::authoring::applyAiSplineWaypointEdit(
        overflowSpline, selection, overflowEdit);
    require(!overflowResult.ok() && !overflowResult.candidate.has_value() &&
                overflowResult.bytes.empty() &&
                overflowSpline.payloads[2].radius ==
                    std::numeric_limits<float>::max(),
            "additive overflow rejects atomically");

    auto malformedGrid = spline;
    malformedGrid.grid->rows[0].cells[0].pointIndices.push_back(99U);
    const auto gridResult = apex::authoring::applyAiSplineWaypointEdit(
        malformedGrid, selection, {});
    require(!gridResult.ok() && !gridResult.candidate.has_value() &&
                gridResult.diagnostics.back().code == "GRID_INDEX_INVALID",
            "final writer validation rejects malformed retained grid");

    auto limits = apex::formats::AiSplineWriteLimits{};
    limits.maxOutputBytes = 16U;
    const auto limitedResult = apex::authoring::applyAiSplineWaypointEdit(
        spline, selection, {}, limits);
    require(!limitedResult.ok() && !limitedResult.candidate.has_value() &&
                limitedResult.bytes.empty() &&
                limitedResult.diagnostics.back().code == "OUTPUT_LIMIT",
            "output limit rejects without candidate bytes");
}

} // namespace

int main() {
    try {
        readsSixUiFieldsThroughPointTag();
        appliesReplacementThenAdditionAtomically();
        preservesNativeZeroSentinels();
        rejectsInvalidSelectionVersionAndTags();
        rejectsNonFiniteInputsAndResultsWithoutMutation();
        std::cout << "ai_spline_authoring_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ai_spline_authoring_tests: " << error.what() << '\n';
        return 1;
    }
}
