#include "apex/authoring/ai_spline_session.hpp"

#include <array>
#include <exception>
#include <limits>
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
    return error.code() == "UNSUPPORTED_VERSION"
               ? AiSplineWaypointStatus::unsupported
               : AiSplineWaypointStatus::invalid;
}

} // namespace

AiSplineSession::AiSplineSession(formats::AiSpline baseline,
                                 AiSplineSessionLimits limits)
    : limits_(std::move(limits)) {
    auto bytes = formats::serializeAiSpline(baseline, limits_.write);
    baseline_ = std::make_shared<const Snapshot>(
        Snapshot{std::move(baseline), std::move(bytes)});
    current_ = baseline_;
}

const formats::AiSpline& AiSplineSession::baseline() const noexcept {
    return baseline_->spline;
}

const formats::AiSpline& AiSplineSession::current() const noexcept {
    return current_->spline;
}

const std::vector<std::uint8_t>&
AiSplineSession::currentBytes() const noexcept {
    return current_->bytes;
}

bool AiSplineSession::stagePush(const SnapshotPtr& snapshot,
                                std::deque<SnapshotPtr>& history,
                                std::size_t& historyBytes,
                                AiSplineSessionResult& result) const {
    if (limits_.maxHistory == 0U) {
        history.clear();
        historyBytes = 0U;
        return true;
    }
    if (snapshot->bytes.size() > limits_.maxHistoryBytes) {
        fail(result, AiSplineWaypointStatus::invalid, "HISTORY_BYTE_LIMIT",
             current_->spline.source,
             "AI spline snapshot exceeds the session history byte limit");
        return false;
    }
    while (!history.empty() &&
           (history.size() >= limits_.maxHistory ||
            historyBytes > limits_.maxHistoryBytes - snapshot->bytes.size())) {
        historyBytes -= history.front()->bytes.size();
        history.pop_front();
    }
    history.push_back(snapshot);
    historyBytes += snapshot->bytes.size();
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
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(result, AiSplineWaypointStatus::failed, "REVISION_OVERFLOW",
             current_->spline.source,
             "AI spline session revision cannot be incremented");
        return result;
    }

    try {
        auto stagedUndo = undo_;
        auto stagedUndoBytes = undoBytes_;
        if (!stagePush(current_, stagedUndo, stagedUndoBytes, result))
            return result;

        current_ = std::move(next);
        undo_.swap(stagedUndo);
        undoBytes_ = stagedUndoBytes;
        redo_.clear();
        redoBytes_ = 0U;
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
        auto next = std::make_shared<const Snapshot>(
            Snapshot{std::move(*applied.candidate), std::move(applied.bytes)});
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
        auto next = std::make_shared<const Snapshot>(
            Snapshot{std::move(*applied.candidate), std::move(applied.bytes)});
        return commitSnapshot(std::move(next), applied.applied);
    } catch (const std::exception& error) {
        AiSplineSessionResult result;
        result.revision = revision_;
        fail(result, AiSplineWaypointStatus::failed, "SESSION_COMMIT_FAILED",
             current_->spline.source, error.what());
        return result;
    }
}

AiSplineSessionResult AiSplineSession::restoreSelectedDefaults(
    std::span<const std::uint32_t> selectedPointIndices) {
    AiSplineSessionResult result;
    result.revision = revision_;
    try {
        if (selectedPointIndices.size() > limits_.maxSelectionEntries) {
            fail(
                result, AiSplineWaypointStatus::invalid, "SELECTION_LIMIT",
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
        for (const auto pointIndex : selectedPointIndices) {
            candidate.points[pointIndex] = baseline_->spline.points[pointIndex];
            const auto payloadIndex =
                static_cast<std::size_t>(candidate.points[pointIndex].tag);
            candidate.payloads[payloadIndex] =
                baseline_->spline.payloads[payloadIndex];
        }
        auto bytes = formats::serializeAiSpline(candidate, limits_.write);
        auto next = std::make_shared<const Snapshot>(
            Snapshot{std::move(candidate), std::move(bytes)});
        return commitSnapshot(std::move(next), selectedPointIndices.size());
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
        auto& stagedSource = undoDirection ? stagedUndo : stagedRedo;
        auto& stagedSourceBytes =
            undoDirection ? stagedUndoBytes : stagedRedoBytes;
        auto& stagedDestination = undoDirection ? stagedRedo : stagedUndo;
        auto& stagedDestinationBytes =
            undoDirection ? stagedRedoBytes : stagedUndoBytes;
        const auto next = stagedSource.back();
        stagedSourceBytes -= next->bytes.size();
        stagedSource.pop_back();
        if (!stagePush(current_, stagedDestination, stagedDestinationBytes,
                       result))
            return result;

        current_ = next;
        undo_.swap(stagedUndo);
        redo_.swap(stagedRedo);
        undoBytes_ = stagedUndoBytes;
        redoBytes_ = stagedRedoBytes;
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
