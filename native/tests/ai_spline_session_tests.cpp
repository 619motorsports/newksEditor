#include "apex/authoring/ai_spline_session.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::authoring::AiSplineSession;
using apex::authoring::AiSplineSessionLimits;
using apex::authoring::AiSplineWaypointEdit;
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
    spline.source = "session-fixture.ai";
    spline.version = 7U;
    spline.lapTime = 321U;
    spline.points = {
        AiSplinePoint{{1.0F, 2.0F, 3.0F}, 4.0F, 2},
        AiSplinePoint{{5.0F, 6.0F, 7.0F}, 8.0F, 0},
        AiSplinePoint{{9.0F, 10.0F, 11.0F}, 12.0F, 3},
        AiSplinePoint{{13.0F, 14.0F, 15.0F}, 16.0F, 1},
    };
    for (std::uint32_t index = 0U; index < 4U; ++index) {
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
        payload.grade = base + 18.0F;
        spline.payloads.push_back(payload);
    }
    AiSplineGrid grid;
    grid.maximum = {13.0F, 14.0F, 15.0F};
    grid.minimum = {1.0F, 2.0F, 3.0F};
    grid.neighborCount = 2U;
    grid.samplingDensity = 0.5F;
    grid.rows = {AiSplineGridRow{{AiSplineGridCell{{0U, 3U}}}}};
    spline.grid = std::move(grid);
    return spline;
}

bool sameGrid(const AiSplineGrid& left, const AiSplineGrid& right) {
    if (left.maximum != right.maximum || left.minimum != right.minimum ||
        left.neighborCount != right.neighborCount ||
        left.samplingDensity != right.samplingDensity ||
        left.rows.size() != right.rows.size())
        return false;
    for (std::size_t row = 0U; row < left.rows.size(); ++row) {
        if (left.rows[row].cells.size() != right.rows[row].cells.size())
            return false;
        for (std::size_t cell = 0U; cell < left.rows[row].cells.size();
             ++cell) {
            if (left.rows[row].cells[cell].pointIndices !=
                right.rows[row].cells[cell].pointIndices)
                return false;
        }
    }
    return true;
}

void keepsIndependentLoadBaselineAndRevisions() {
    AiSplineSession session(fixture());
    const auto baselineBytes = session.currentBytes();
    AiSplineWaypointEdit edit;
    edit.additive.radius = 5.0F;
    const auto committed = session.commitWaypointEdit(0U, edit);
    require(committed.ok() && committed.changed && committed.applied == 1U &&
                committed.pointIndex == 0U && committed.payloadIndex == 2U &&
                committed.revision == 1U,
            "waypoint edit commits one tagged payload revision");
    require(session.current().payloads[2U].radius == 310.0F &&
                session.baseline().payloads[2U].radius == 305.0F &&
                session.currentBytes() != baselineBytes && session.canUndo() &&
                !session.canRedo(),
            "current changes while immutable load baseline remains intact");

    const auto undone = session.undo();
    require(undone.ok() && undone.changed && undone.revision == 2U &&
                session.currentBytes() == baselineBytes && session.canRedo(),
            "undo restores the owned baseline bytes");
    const auto redone = session.redo();
    require(redone.ok() && redone.revision == 3U &&
                session.current().payloads[2U].radius == 310.0F,
            "redo restores the committed payload");

    AiSplineWaypointEdit noChange;
    const auto noOp = session.commitWaypointEdit(0U, noChange);
    require(noOp.ok() && !noOp.changed && noOp.revision == 3U &&
                session.undoCount() == 1U,
            "byte-identical edit does not create history or a revision");
}

void invertsRawSelectionOrderAndSignedZero() {
    auto spline = fixture();
    spline.payloads[2U].camber = -0.0F;
    const std::array<std::uint32_t, 3U> selection{0U, 1U, 0U};
    const auto applied =
        apex::authoring::applyAiSplineCamberInversion(spline, selection);
    require(applied.ok() && applied.candidate.has_value() &&
                applied.applied == 3U && applied.changed,
            "raw inversion applies every selected entry");
    require(
        std::bit_cast<std::uint32_t>(applied.candidate->payloads[2U].camber) ==
                std::bit_cast<std::uint32_t>(-0.0F) &&
            applied.candidate->payloads[0U].camber == -0.1F,
        "duplicate point flips twice while the other tagged payload flips "
        "once");

    const std::array<std::uint32_t, 1U> signedZeroSelection{0U};
    const auto signedZero = apex::authoring::applyAiSplineCamberInversion(
        spline, signedZeroSelection);
    require(signedZero.ok() && signedZero.changed &&
                std::bit_cast<std::uint32_t>(
                    signedZero.candidate->payloads[2U].camber) ==
                    std::bit_cast<std::uint32_t>(0.0F),
            "signed-zero inversion is a persisted bit-level change");

    const std::array<std::uint32_t, 2U> duplicateOnly{1U, 1U};
    const auto duplicateResult =
        apex::authoring::applyAiSplineCamberInversion(spline, duplicateOnly);
    require(duplicateResult.ok() && !duplicateResult.changed &&
                duplicateResult.bytes ==
                    apex::formats::serializeAiSpline(spline),
            "two raw duplicate entries restore the original persisted value");

    const std::array<std::uint32_t, 0U> empty{};
    const auto emptyResult =
        apex::authoring::applyAiSplineCamberInversion(spline, empty);
    require(emptyResult.ok() && !emptyResult.changed &&
                emptyResult.applied == 0U,
            "empty native selection is a valid no-op");
}

void restoresSelectedRecordsFromLoadBaseline() {
    AiSplineSession session(fixture());
    AiSplineWaypointEdit edit;
    edit.replacement.radius = 10.0F;
    edit.additive.grade = 2.0F;
    require(session.commitWaypointEdit(0U, edit).ok(),
            "selected restore setup edit commits");
    const std::array<std::uint32_t, 1U> otherSelection{1U};
    require(session.invertSelectedCamber(otherSelection).ok(),
            "selected restore setup inversion commits");

    const auto gridBefore = *session.current().grid;
    const std::array<std::uint32_t, 2U> resetSelection{0U, 0U};
    const auto reset = session.restoreSelectedDefaults(resetSelection);
    require(reset.ok() && reset.changed && reset.applied == 2U &&
                session.current().payloads[2U].radius ==
                    session.baseline().payloads[2U].radius &&
                session.current().payloads[2U].grade ==
                    session.baseline().payloads[2U].grade,
            "reset copies the complete tagged payload from load baseline");
    require(session.current().payloads[0U].camber == -0.1F &&
                sameGrid(*session.current().grid, gridBefore),
            "reset leaves other payloads and the payload-only grid unchanged");

    const auto reparsed =
        apex::formats::parseAiSpline(session.currentBytes(), "reset-output.ai");
    require(reparsed.payloads[2U].radius == 305.0F &&
                reparsed.grid.has_value() &&
                sameGrid(*reparsed.grid, gridBefore),
            "selected reset bytes reparse with the complete grid preserved");

    const auto resetUndone = session.undo();
    require(resetUndone.ok() &&
                session.current().payloads[2U].radius == 10.0F &&
                session.current().payloads[2U].grade == 320.0F,
            "undo restores the complete state from before selected reset");
    const auto resetRedone = session.redo();
    require(resetRedone.ok() && session.current().payloads[2U].radius == 305.0F,
            "redo reapplies the selected reset");

    const auto restored = session.restoreBaseline();
    require(restored.ok() && restored.changed &&
                session.current().payloads[0U].camber == 0.1F &&
                session.baseline().payloads[0U].camber == 0.1F,
            "whole-session restore returns to the immutable load snapshot");
}

void keepsFailuresAtomic() {
    AiSplineSession session(fixture());
    const auto before = session.currentBytes();
    const auto revision = session.revision();
    const std::array<std::uint32_t, 1U> outside{99U};
    const auto invalidReset = session.restoreSelectedDefaults(outside);
    require(!invalidReset.ok() && invalidReset.applied == 0U &&
                session.currentBytes() == before &&
                session.revision() == revision && !session.canUndo(),
            "invalid reset does not mutate state or history");

    auto nonFinite = fixture();
    nonFinite.payloads[0U].camber = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try {
        AiSplineSession invalid(std::move(nonFinite));
        (void)invalid;
    } catch (const apex::formats::AiSplineWriteError& error) {
        rejected = error.code() == "NON_FINITE";
    }
    require(rejected, "session construction validates malformed payloads");

    auto malformedGrid = fixture();
    malformedGrid.grid->rows[0].cells[0].pointIndices.push_back(99U);
    rejected = false;
    try {
        AiSplineSession invalid(std::move(malformedGrid));
        (void)invalid;
    } catch (const apex::formats::AiSplineWriteError& error) {
        rejected = error.code() == "GRID_INDEX_INVALID";
    }
    require(rejected, "session construction validates retained grid indices");

    auto invalidTag = fixture();
    invalidTag.points[0U].tag = -1;
    const std::array<std::uint32_t, 1U> selected{0U};
    const auto invalidInversion =
        apex::authoring::applyAiSplineCamberInversion(invalidTag, selected);
    require(!invalidInversion.ok() && !invalidInversion.candidate.has_value() &&
                invalidInversion.bytes.empty() &&
                invalidInversion.diagnostics.back().code ==
                    "PAYLOAD_INDEX_INVALID",
            "camber inversion rejects an invalid selected tag atomically");

    auto invalidGrid = fixture();
    invalidGrid.grid->rows[0].cells[0].pointIndices.push_back(99U);
    const auto invalidGridInversion =
        apex::authoring::applyAiSplineCamberInversion(invalidGrid, selected);
    require(!invalidGridInversion.ok() &&
                invalidGridInversion.diagnostics.back().code ==
                    "GRID_INDEX_INVALID",
            "camber inversion applies final writer validation to the grid");

    auto writeLimits = apex::formats::AiSplineWriteLimits{};
    writeLimits.maxOutputBytes = 16U;
    const auto limitedInversion = apex::authoring::applyAiSplineCamberInversion(
        fixture(), selected, writeLimits);
    require(!limitedInversion.ok() &&
                limitedInversion.diagnostics.back().code == "OUTPUT_LIMIT",
            "camber inversion applies the output byte limit atomically");

    auto selectionLimits = apex::formats::AiSplineWriteLimits{};
    selectionLimits.maxPoints = 4U;
    const std::array<std::uint32_t, 5U> oversizedSelection{0U, 0U, 0U, 0U, 0U};
    const auto oversizedInversion =
        apex::authoring::applyAiSplineCamberInversion(
            fixture(), oversizedSelection, selectionLimits, 4U);
    require(!oversizedInversion.ok() &&
                oversizedInversion.diagnostics.back().code == "SELECTION_LIMIT",
            "camber inversion bounds hostile duplicate selection input");

    AiSplineSessionLimits sessionSelectionLimits;
    sessionSelectionLimits.maxSelectionEntries = 4U;
    AiSplineSession selectionLimited(fixture(), sessionSelectionLimits);
    const auto oversizedReset =
        selectionLimited.restoreSelectedDefaults(oversizedSelection);
    require(!oversizedReset.ok() &&
                oversizedReset.diagnostics.back().code == "SELECTION_LIMIT" &&
                selectionLimited.revision() == 0U,
            "selected reset bounds hostile duplicate selection input");

    auto legacy = fixture();
    legacy.version = 2U;
    rejected = false;
    try {
        AiSplineSession invalid(std::move(legacy));
        (void)invalid;
    } catch (const apex::formats::AiSplineWriteError& error) {
        rejected = error.code() == "UNSUPPORTED_VERSION";
    }
    require(rejected, "session construction rejects legacy version 2");
}

void boundsHistoryAndClearsRedo() {
    const auto bytes = apex::formats::serializeAiSpline(fixture());
    AiSplineSessionLimits byteLimits;
    byteLimits.maxHistoryBytes = bytes.size() - 1U;
    AiSplineSession byteLimited(fixture(), byteLimits);
    AiSplineWaypointEdit edit;
    edit.additive.radius = 1.0F;
    const auto rejected = byteLimited.commitWaypointEdit(0U, edit);
    require(!rejected.ok() &&
                rejected.diagnostics.back().code == "HISTORY_BYTE_LIMIT" &&
                byteLimited.revision() == 0U && !byteLimited.canUndo(),
            "history byte limit rejects a commit atomically");

    AiSplineSessionLimits countLimits;
    countLimits.maxHistory = 1U;
    AiSplineSession countLimited(fixture(), countLimits);
    require(countLimited.commitWaypointEdit(0U, edit).ok(),
            "first bounded-history edit commits");
    require(countLimited.commitWaypointEdit(1U, edit).ok() &&
                countLimited.undoCount() == 1U,
            "history evicts its oldest snapshot at the count bound");
    require(countLimited.undo().ok() && !countLimited.undo().ok(),
            "evicted history cannot be undone");

    AiSplineSession redoSession(fixture());
    require(redoSession.commitWaypointEdit(0U, edit).ok() &&
                redoSession.undo().ok() && redoSession.canRedo(),
            "redo setup succeeds");
    require(redoSession.commitWaypointEdit(1U, edit).ok() &&
                !redoSession.canRedo(),
            "a new edit after undo clears redo history");

    AiSplineSession inversionHistory(fixture());
    const std::array<std::uint32_t, 1U> invertOnce{1U};
    require(inversionHistory.invertSelectedCamber(invertOnce).ok() &&
                inversionHistory.current().payloads[0U].camber == -0.1F &&
                inversionHistory.undo().ok() &&
                inversionHistory.current().payloads[0U].camber == 0.1F &&
                inversionHistory.redo().ok() &&
                inversionHistory.current().payloads[0U].camber == -0.1F,
            "camber inversion participates in undo and redo history");

    require(inversionHistory.undo().ok() && inversionHistory.canRedo(),
            "no-op inversion history setup succeeds");
    const std::array<std::uint32_t, 2U> invertTwice{1U, 1U};
    const auto noOpInversion =
        inversionHistory.invertSelectedCamber(invertTwice);
    require(noOpInversion.ok() && !noOpInversion.changed &&
                inversionHistory.canRedo(),
            "no-op raw inversion preserves redo history");
    require(inversionHistory.invertSelectedCamber(invertOnce).ok() &&
                !inversionHistory.canRedo(),
            "new camber inversion after undo clears redo history");

    AiSplineSessionLimits noHistoryLimits;
    noHistoryLimits.maxHistory = 0U;
    AiSplineSession noHistory(fixture(), noHistoryLimits);
    require(noHistory.commitWaypointEdit(0U, edit).ok() && !noHistory.canUndo(),
            "zero history retains successful current state only");
}

} // namespace

int main() {
    try {
        keepsIndependentLoadBaselineAndRevisions();
        invertsRawSelectionOrderAndSignedZero();
        restoresSelectedRecordsFromLoadBaseline();
        keepsFailuresAtomic();
        boundsHistoryAndClearsRedo();
        std::cout << "ai_spline_session_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ai_spline_session_tests: " << error.what() << '\n';
        return 1;
    }
}
