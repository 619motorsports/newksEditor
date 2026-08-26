#include "apex/authoring/ai_spline_session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace apex::authoring {
namespace {

void fail(AiSplineSessionResult& result, AiSplineWaypointStatus status,
          std::string code, std::string path, std::string message) {
    result.status = status;
    result.diagnostics.push_back(
        {std::move(code), std::move(path), std::move(message)});
}

[[nodiscard]] AiSplineWaypointStatus
statusForWriteError(const formats::AiSplineWriteError& error) noexcept {
    if (error.code() == "UNSUPPORTED_VERSION")
        return AiSplineWaypointStatus::unsupported;
    if (error.code() == "ALLOCATION_FAILED")
        return AiSplineWaypointStatus::failed;
    return AiSplineWaypointStatus::invalid;
}

[[nodiscard]] AiSplineWaypointStatus
statusForGridError(const formats::AiSplineGridBuildError& error) noexcept {
    if (error.code() == "UNSUPPORTED_VERSION")
        return AiSplineWaypointStatus::unsupported;
    if (error.code() == "ALLOCATION_FAILED")
        return AiSplineWaypointStatus::failed;
    return AiSplineWaypointStatus::invalid;
}

[[nodiscard]] bool checkedCharge(std::size_t count, std::size_t elementBytes,
                                 std::size_t limit,
                                 std::size_t& used) noexcept {
    if (count != 0U &&
        elementBytes > std::numeric_limits<std::size_t>::max() / count)
        return false;
    const auto bytes = count * elementBytes;
    if (bytes > std::numeric_limits<std::size_t>::max() - used ||
        used + bytes > limit)
        return false;
    used += bytes;
    return true;
}

[[nodiscard]] std::optional<std::size_t>
snapshotModelBytes(const formats::AiSpline& spline,
                   std::size_t limit) noexcept {
    std::size_t used = sizeof(formats::AiSpline);
    if (used > limit ||
        !checkedCharge(spline.source.size(), sizeof(char), limit, used) ||
        !checkedCharge(spline.points.size(), sizeof(formats::AiSplinePoint),
                       limit, used) ||
        !checkedCharge(spline.payloads.size(), sizeof(formats::AiSplinePayload),
                       limit, used) ||
        !checkedCharge(spline.legacyV2Records.size(),
                       sizeof(formats::AiSplineLegacyV2Record), limit, used) ||
        !checkedCharge(spline.nativeRetainedIndices.size(),
                       sizeof(std::uint32_t), limit, used) ||
        !checkedCharge(spline.nativeRetainedForwards.size(),
                       sizeof(std::array<float, 3>), limit, used))
        return std::nullopt;
    if (!spline.grid.has_value())
        return used;
    if (!checkedCharge(spline.grid->rows.size(),
                       sizeof(formats::AiSplineGridRow), limit, used))
        return std::nullopt;
    for (const auto& row : spline.grid->rows) {
        if (!checkedCharge(row.cells.size(), sizeof(formats::AiSplineGridCell),
                           limit, used))
            return std::nullopt;
        for (const auto& cell : row.cells) {
            if (!checkedCharge(cell.pointIndices.size(), sizeof(std::uint32_t),
                               limit, used))
                return std::nullopt;
        }
    }
    return used;
}

[[nodiscard]] bool samePosition(const std::array<float, 3>& left,
                                const std::array<float, 3>& right) noexcept {
    for (std::size_t component = 0U; component < left.size(); ++component) {
        if (std::bit_cast<std::uint32_t>(left[component]) !=
            std::bit_cast<std::uint32_t>(right[component]))
            return false;
    }
    return true;
}

[[nodiscard]] formats::AiSplineGridBuildLimits
sessionGridLimits(const AiSplineSessionLimits& limits) noexcept {
    auto result = limits.grid;
    result.maxPoints = std::min(result.maxPoints, limits.write.maxPoints);
    result.maxGridNeighbors =
        std::min(result.maxGridNeighbors, limits.write.maxGridNeighbors);
    result.maxGridRows = std::min(result.maxGridRows, limits.write.maxGridRows);
    result.maxGridCellsPerRow =
        std::min(result.maxGridCellsPerRow, limits.write.maxGridCellsPerRow);
    result.maxGridIndices =
        std::min(result.maxGridIndices, limits.write.maxGridIndices);
    return result;
}

} // namespace

AiSplineSession::AiSplineSession(formats::AiSpline baseline,
                                 AiSplineSessionLimits limits)
    : limits_(std::move(limits)) {
    auto bytes = formats::serializeAiSpline(baseline, limits_.write);
    baseline_ = makeSnapshot(std::move(baseline), std::move(bytes));
    current_ = baseline_;
}

const formats::AiSpline& AiSplineSession::baseline() const noexcept {
    return baseline_->spline;
}

const std::vector<std::uint8_t>&
AiSplineSession::baselineBytes() const noexcept {
    return baseline_->bytes;
}

const formats::AiSpline& AiSplineSession::current() const noexcept {
    return current_->spline;
}

const std::vector<std::uint8_t>&
AiSplineSession::currentBytes() const noexcept {
    return current_->bytes;
}

AiSplineSession::SnapshotPtr
AiSplineSession::makeSnapshot(formats::AiSpline spline,
                              std::vector<std::uint8_t> bytes) const {
    const auto modelBytes =
        snapshotModelBytes(spline, limits_.maxSnapshotModelBytes);
    if (!modelBytes.has_value())
        throw formats::AiSplineWriteError(
            "MODEL_BYTE_LIMIT",
            "AI spline snapshot model exceeds its byte limit");
    try {
        return std::make_shared<const Snapshot>(
            Snapshot{std::move(spline), std::move(bytes), *modelBytes});
    } catch (const std::bad_alloc&) {
        throw formats::AiSplineWriteError(
            "ALLOCATION_FAILED",
            "AI spline snapshot allocation failed within configured limits");
    }
}

bool AiSplineSession::stagePush(const SnapshotPtr& snapshot,
                                std::deque<SnapshotPtr>& history,
                                std::size_t& historyBytes,
                                std::size_t& historyModelBytes,
                                AiSplineSessionResult& result) const {
    if (limits_.maxHistory == 0U) {
        history.clear();
        historyBytes = 0U;
        historyModelBytes = 0U;
        return true;
    }
    if (snapshot->bytes.size() > limits_.maxHistoryBytes) {
        fail(result, AiSplineWaypointStatus::invalid, "HISTORY_BYTE_LIMIT",
             current_->spline.source,
             "AI spline snapshot exceeds the session history byte limit");
        return false;
    }
    if (snapshot->modelBytes > limits_.maxHistoryModelBytes) {
        fail(result, AiSplineWaypointStatus::invalid, "HISTORY_MODEL_LIMIT",
             current_->spline.source,
             "AI spline snapshot model exceeds the session history model "
             "limit");
        return false;
    }
    while (!history.empty() &&
           (history.size() >= limits_.maxHistory ||
            historyBytes > limits_.maxHistoryBytes - snapshot->bytes.size() ||
            historyModelBytes >
                limits_.maxHistoryModelBytes - snapshot->modelBytes)) {
        historyBytes -= history.front()->bytes.size();
        historyModelBytes -= history.front()->modelBytes;
        history.pop_front();
    }
    history.push_back(snapshot);
    historyBytes += snapshot->bytes.size();
    historyModelBytes += snapshot->modelBytes;
    return true;
}

AiSplineSessionResult
AiSplineSession::commitSnapshot(SnapshotPtr next, std::size_t applied,
                                std::optional<std::uint32_t> pointIndex,
                                std::optional<std::uint32_t> payloadIndex) {
    AiSplineSessionResult result;
    result.pointIndex = pointIndex;
    result.payloadIndex = payloadIndex;
    result.revision = revision_;
    if (next->bytes == current_->bytes) {
        result.applied = applied;
        result.status = AiSplineWaypointStatus::ok;
        return result;
    }
    if (limits_.maxHistory != 0U &&
        next->bytes.size() > limits_.maxHistoryBytes) {
        fail(result, AiSplineWaypointStatus::invalid, "HISTORY_BYTE_LIMIT",
             current_->spline.source,
             "AI spline candidate exceeds the session history byte limit");
        return result;
    }
    if (limits_.maxHistory != 0U &&
        next->modelBytes > limits_.maxHistoryModelBytes) {
        fail(result, AiSplineWaypointStatus::invalid, "HISTORY_MODEL_LIMIT",
             current_->spline.source,
             "AI spline candidate model exceeds the session history model "
             "limit");
        return result;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(result, AiSplineWaypointStatus::failed, "REVISION_OVERFLOW",
             current_->spline.source,
             "AI spline session revision cannot be incremented");
        return result;
    }

    try {
        auto stagedUndo = undo_;
        auto stagedUndoBytes = undoBytes_;
        auto stagedUndoModelBytes = undoModelBytes_;
        if (!stagePush(current_, stagedUndo, stagedUndoBytes,
                       stagedUndoModelBytes, result))
            return result;

        current_ = std::move(next);
        undo_.swap(stagedUndo);
        undoBytes_ = stagedUndoBytes;
        undoModelBytes_ = stagedUndoModelBytes;
        redo_.clear();
        redoBytes_ = 0U;
        redoModelBytes_ = 0U;
        ++revision_;
        result.revision = revision_;
        result.applied = applied;
        result.changed = true;
        result.status = AiSplineWaypointStatus::ok;
    } catch (const std::exception& error) {
        fail(result, AiSplineWaypointStatus::failed, "SESSION_COMMIT_FAILED",
             current_->spline.source, error.what());
    }
    return result;
}

AiSplineSessionResult
AiSplineSession::commitWaypointEdit(std::uint32_t pointIndex,
                                    const AiSplineWaypointEdit& edit) {
    const std::array<std::uint32_t, 1U> selection{pointIndex};
    auto applied = applyAiSplineWaypointEdit(current_->spline, selection, edit,
                                             limits_.write);
    if (!applied.ok()) {
        AiSplineSessionResult result;
        result.status = applied.status;
        result.diagnostics = std::move(applied.diagnostics);
        result.pointIndex = applied.pointIndex;
        result.payloadIndex = applied.payloadIndex;
        result.revision = revision_;
        return result;
    }

    try {
        auto next = makeSnapshot(std::move(*applied.candidate),
                                 std::move(applied.bytes));
        return commitSnapshot(std::move(next), 1U, applied.pointIndex,
                              applied.payloadIndex);
    } catch (const std::exception& error) {
        AiSplineSessionResult result;
        result.revision = revision_;
        fail(result, AiSplineWaypointStatus::failed, "SESSION_COMMIT_FAILED",
             current_->spline.source, error.what());
        return result;
    }
}

AiSplineSessionResult AiSplineSession::invertSelectedCamber(
    std::span<const std::uint32_t> selectedPointIndices) {
    auto applied = applyAiSplineCamberInversion(
        current_->spline, selectedPointIndices, limits_.write,
        limits_.maxSelectionEntries);
    if (!applied.ok()) {
        AiSplineSessionResult result;
        result.status = applied.status;
        result.diagnostics = std::move(applied.diagnostics);
        result.revision = revision_;
        return result;
    }

    try {
        auto next = makeSnapshot(std::move(*applied.candidate),
                                 std::move(applied.bytes));
        return commitSnapshot(std::move(next), applied.applied);
    } catch (const std::exception& error) {
        AiSplineSessionResult result;
        result.revision = revision_;
        fail(result, AiSplineWaypointStatus::failed, "SESSION_COMMIT_FAILED",
             current_->spline.source, error.what());
        return result;
    }
}

AiSplineSessionResult
AiSplineSession::setPointPosition(std::uint32_t pointIndex,
                                  const std::array<float, 3>& position) {
    const std::array<AiSplinePointPositionEdit, 1U> edits{
        AiSplinePointPositionEdit{pointIndex, position}};
    return setPointPositions(edits);
}

AiSplineSessionResult AiSplineSession::setPointPositions(
    std::span<const AiSplinePointPositionEdit> edits) {
    AiSplineSessionResult result;
    result.revision = revision_;
    if (current_->spline.version != 7U) {
        fail(result, AiSplineWaypointStatus::unsupported, "UNSUPPORTED_VERSION",
             current_->spline.source,
             "AI spline point-position editing supports version 7 only");
        return result;
    }
    if (edits.size() > limits_.maxSelectionEntries) {
        fail(result, AiSplineWaypointStatus::invalid, "SELECTION_LIMIT",
             current_->spline.source,
             "AI spline point-position edit count exceeds its entry limit");
        return result;
    }
    if (edits.empty()) {
        result.status = AiSplineWaypointStatus::ok;
        return result;
    }

    try {
        constexpr std::size_t unseen = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> editForPoint(current_->spline.points.size(),
                                              unseen);
        std::vector<AiSplinePointPositionEdit> uniqueEdits;
        uniqueEdits.reserve(edits.size());
        for (const auto& edit : edits) {
            result.pointIndex = edit.pointIndex;
            const auto point = static_cast<std::size_t>(edit.pointIndex);
            if (point >= current_->spline.points.size()) {
                fail(result, AiSplineWaypointStatus::invalid,
                     "POINT_INDEX_INVALID", current_->spline.source,
                     "selected AI spline point is outside the point array");
                return result;
            }
            for (const float coordinate : edit.position) {
                if (!std::isfinite(coordinate)) {
                    fail(result, AiSplineWaypointStatus::invalid,
                         "NON_FINITE_POSITION", current_->spline.source,
                         "AI spline point position must contain finite values");
                    return result;
                }
            }
            const auto existing = editForPoint[point];
            if (existing == unseen) {
                editForPoint[point] = uniqueEdits.size();
                uniqueEdits.push_back(edit);
            } else if (!samePosition(uniqueEdits[existing].position,
                                     edit.position)) {
                fail(result, AiSplineWaypointStatus::invalid,
                     "POINT_EDIT_CONFLICT", current_->spline.source,
                     "duplicate AI spline point edits have different "
                     "positions");
                return result;
            }
        }
        result.pointIndex = uniqueEdits.back().pointIndex;
        const bool changed = std::any_of(
            uniqueEdits.begin(), uniqueEdits.end(), [&](const auto& edit) {
                return !samePosition(
                    current_->spline.points[edit.pointIndex].position,
                    edit.position);
            });
        if (!changed) {
            result.applied = uniqueEdits.size();
            result.status = AiSplineWaypointStatus::ok;
            return result;
        }

        const auto effectiveNeighbors =
            std::min<std::size_t>(10U, current_->spline.points.size());
        if (effectiveNeighbors > limits_.write.maxGridIndicesPerCell) {
            fail(result, AiSplineWaypointStatus::invalid, "COUNT_LIMIT",
                 current_->spline.source,
                 "AI spline grid cell index count exceeds the writer limit");
            return result;
        }
        auto candidate = current_->spline;
        for (const auto& edit : uniqueEdits)
            candidate.points[edit.pointIndex].position = edit.position;
        candidate.grid =
            formats::buildAiSplineGrid(candidate, sessionGridLimits(limits_));
        auto bytes = formats::serializeAiSpline(candidate, limits_.write);
        auto next = makeSnapshot(std::move(candidate), std::move(bytes));
        return commitSnapshot(std::move(next), uniqueEdits.size(),
                              uniqueEdits.back().pointIndex);
    } catch (const formats::AiSplineGridBuildError& error) {
        fail(result, statusForGridError(error), error.code(),
             current_->spline.source, error.what());
    } catch (const formats::AiSplineWriteError& error) {
        fail(result, statusForWriteError(error), error.code(),
             current_->spline.source, error.what());
    } catch (const std::exception& error) {
        fail(result, AiSplineWaypointStatus::failed, "POSITION_EDIT_FAILED",
             current_->spline.source, error.what());
    }
    return result;
}

AiSplineSessionResult AiSplineSession::restoreSelectedDefaults(
    std::span<const std::uint32_t> selectedPointIndices) {
    AiSplineSessionResult result;
    result.revision = revision_;
    try {
        if (selectedPointIndices.size() > limits_.maxSelectionEntries) {
            fail(result, AiSplineWaypointStatus::invalid, "SELECTION_LIMIT",
                 current_->spline.source,
                 "AI spline selection count exceeds its entry limit");
            return result;
        }
        if (current_->spline.points.size() != baseline_->spline.points.size()) {
            return commitSnapshot(baseline_, selectedPointIndices.size());
        }
        if (current_->spline.points.size() !=
                current_->spline.payloads.size() ||
            baseline_->spline.points.size() !=
                baseline_->spline.payloads.size()) {
            fail(result, AiSplineWaypointStatus::invalid, "COUNT_MISMATCH",
                 current_->spline.source,
                 "AI spline payload count must equal point count");
            return result;
        }

        for (const auto pointIndex : selectedPointIndices) {
            if (static_cast<std::size_t>(pointIndex) >=
                current_->spline.points.size()) {
                fail(result, AiSplineWaypointStatus::invalid,
                     "POINT_INDEX_INVALID", current_->spline.source,
                     "selected AI spline point is outside the point array");
                return result;
            }
            const auto baselineTag = baseline_->spline.points[pointIndex].tag;
            if (baselineTag < 0 ||
                static_cast<std::size_t>(baselineTag) >=
                    baseline_->spline.payloads.size() ||
                static_cast<std::size_t>(baselineTag) >=
                    current_->spline.payloads.size()) {
                fail(result, AiSplineWaypointStatus::invalid,
                     "PAYLOAD_INDEX_INVALID", current_->spline.source,
                     "load-time backup point tag is outside a payload array");
                return result;
            }
        }

        auto candidate = current_->spline;
        bool positionsChanged = false;
        for (const auto pointIndex : selectedPointIndices) {
            positionsChanged =
                positionsChanged ||
                !samePosition(current_->spline.points[pointIndex].position,
                              baseline_->spline.points[pointIndex].position);
            candidate.points[pointIndex] = baseline_->spline.points[pointIndex];
            const auto payloadIndex =
                static_cast<std::size_t>(candidate.points[pointIndex].tag);
            candidate.payloads[payloadIndex] =
                baseline_->spline.payloads[payloadIndex];
        }
        if (positionsChanged) {
            const auto effectiveNeighbors =
                std::min<std::size_t>(10U, candidate.points.size());
            if (effectiveNeighbors > limits_.write.maxGridIndicesPerCell) {
                fail(result, AiSplineWaypointStatus::invalid, "COUNT_LIMIT",
                     current_->spline.source,
                     "AI spline grid cell index count exceeds the writer "
                     "limit");
                return result;
            }
            candidate.grid = formats::buildAiSplineGrid(
                candidate, sessionGridLimits(limits_));
        }
        auto bytes = formats::serializeAiSpline(candidate, limits_.write);
        auto next = makeSnapshot(std::move(candidate), std::move(bytes));
        return commitSnapshot(std::move(next), selectedPointIndices.size());
    } catch (const formats::AiSplineGridBuildError& error) {
        fail(result, statusForGridError(error), error.code(),
             current_->spline.source, error.what());
    } catch (const formats::AiSplineWriteError& error) {
        fail(result, statusForWriteError(error), error.code(),
             current_->spline.source, error.what());
    } catch (const std::exception& error) {
        fail(result, AiSplineWaypointStatus::failed, "SESSION_RESET_FAILED",
             current_->spline.source, error.what());
    }
    return result;
}

AiSplineSessionResult AiSplineSession::moveHistory(bool undoDirection) {
    AiSplineSessionResult result;
    result.revision = revision_;
    auto& source = undoDirection ? undo_ : redo_;
    const char* emptyCode =
        undoDirection ? "NOTHING_TO_UNDO" : "NOTHING_TO_REDO";
    if (source.empty()) {
        fail(result, AiSplineWaypointStatus::invalid, emptyCode,
             current_->spline.source,
             undoDirection ? "AI spline session has nothing to undo"
                           : "AI spline session has nothing to redo");
        return result;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(result, AiSplineWaypointStatus::failed, "REVISION_OVERFLOW",
             current_->spline.source,
             "AI spline session revision cannot be incremented");
        return result;
    }

    try {
        auto stagedUndo = undo_;
        auto stagedRedo = redo_;
        auto stagedUndoBytes = undoBytes_;
        auto stagedRedoBytes = redoBytes_;
        auto stagedUndoModelBytes = undoModelBytes_;
        auto stagedRedoModelBytes = redoModelBytes_;
        auto& stagedSource = undoDirection ? stagedUndo : stagedRedo;
        auto& stagedSourceBytes =
            undoDirection ? stagedUndoBytes : stagedRedoBytes;
        auto& stagedDestination = undoDirection ? stagedRedo : stagedUndo;
        auto& stagedDestinationBytes =
            undoDirection ? stagedRedoBytes : stagedUndoBytes;
        auto& stagedSourceModelBytes =
            undoDirection ? stagedUndoModelBytes : stagedRedoModelBytes;
        auto& stagedDestinationModelBytes =
            undoDirection ? stagedRedoModelBytes : stagedUndoModelBytes;
        const auto next = stagedSource.back();
        stagedSourceBytes -= next->bytes.size();
        stagedSourceModelBytes -= next->modelBytes;
        stagedSource.pop_back();
        if (!stagePush(current_, stagedDestination, stagedDestinationBytes,
                       stagedDestinationModelBytes, result))
            return result;

        current_ = next;
        undo_.swap(stagedUndo);
        redo_.swap(stagedRedo);
        undoBytes_ = stagedUndoBytes;
        redoBytes_ = stagedRedoBytes;
        undoModelBytes_ = stagedUndoModelBytes;
        redoModelBytes_ = stagedRedoModelBytes;
        ++revision_;
        result.revision = revision_;
        result.changed = true;
        result.status = AiSplineWaypointStatus::ok;
    } catch (const std::exception& error) {
        fail(result, AiSplineWaypointStatus::failed,
             undoDirection ? "SESSION_UNDO_FAILED" : "SESSION_REDO_FAILED",
             current_->spline.source, error.what());
    }
    return result;
}

AiSplineSessionResult AiSplineSession::undo() { return moveHistory(true); }

AiSplineSessionResult AiSplineSession::redo() { return moveHistory(false); }

AiSplineSessionResult AiSplineSession::restoreBaseline() {
    return commitSnapshot(baseline_, 0U);
}

} // namespace apex::authoring
