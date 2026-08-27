#pragma once

#include "apex/authoring/ai_spline.hpp"
#include "apex/formats/ai_spline_grid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::authoring {

struct AiSplineSessionLimits {
    std::size_t maxHistory = 64U;
    // This byte limit measures retained canonical serializations. The parsed
    // snapshot models use the separate model limits below.
    std::size_t maxHistoryBytes = 64U * 1024U * 1024U;
    std::size_t maxSnapshotModelBytes = 256U * 1024U * 1024U;
    std::size_t maxHistoryModelBytes = 256U * 1024U * 1024U;
    std::size_t maxSelectionEntries = aiSplineMaxSelectionEntries;
    formats::AiSplineGridBuildLimits grid{};
    formats::AiSplineWriteLimits write{};
};

struct AiSplinePointPositionEdit {
    std::uint32_t pointIndex = 0U;
    std::array<float, 3> position{};
};

struct AiSplineSessionResult {
    AiSplineWaypointStatus status = AiSplineWaypointStatus::failed;
    std::vector<AiSplineWaypointDiagnostic> diagnostics;
    std::optional<std::uint32_t> pointIndex;
    std::optional<std::uint32_t> payloadIndex;
    std::size_t applied = 0U;
    std::uint64_t revision = 0U;
    bool changed = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == AiSplineWaypointStatus::ok;
    }
};

enum class AiSplineSessionSaveStatus : std::uint8_t {
    ok,
    invalid,
    unsupported,
    resource_limit,
    failed,
};

struct AiSplineSessionSaveResult {
    AiSplineSessionSaveStatus status = AiSplineSessionSaveStatus::failed;
    std::vector<AiSplineWaypointDiagnostic> diagnostics;
    std::vector<std::uint8_t> bytes;
    std::uint64_t revision = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return status == AiSplineSessionSaveStatus::ok;
    }
};

// An AI spline editing session keeps the native editor's independent, immutable
// load-time backup. Each snapshot owns a parsed model and its validated,
// canonical writer bytes. Position edits rebuild the derived native grid
// before the candidate becomes visible.
// Construction throws AiSplineWriteError if the baseline is unsafe or cannot
// use the recovered version-7 writer.
class AiSplineSession final {
public:
    explicit AiSplineSession(formats::AiSpline baseline,
                             AiSplineSessionLimits limits = {});

    [[nodiscard]] const formats::AiSpline& baseline() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>&
    baselineBytes() const noexcept;
    [[nodiscard]] const formats::AiSpline& current() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>&
    currentBytes() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undoCount() const noexcept {
        return undo_.size();
    }
    [[nodiscard]] std::size_t redoCount() const noexcept {
        return redo_.size();
    }

    // Rebuild the recovered grid and serialize a complete save candidate.
    // This operation does not change the session, history, or baseline.
    [[nodiscard]] AiSplineSessionSaveResult buildSaveBytes() const;

    [[nodiscard]] AiSplineSessionResult
    commitWaypointEdit(std::uint32_t pointIndex,
                       const AiSplineWaypointEdit& edit);
    [[nodiscard]] AiSplineSessionResult
    invertSelectedCamber(std::span<const std::uint32_t> selectedPointIndices);
    // Set one absolute point position. This preserves point length, tag, and
    // every payload field, matching the recovered Spline::setPointAt scope.
    [[nodiscard]] AiSplineSessionResult
    setPointPosition(std::uint32_t pointIndex,
                     const std::array<float, 3>& position);
    // Set multiple absolute positions as one validated revision. Duplicate
    // indices with the same position are applied once in first-seen order.
    // Conflicting duplicates are rejected before the candidate is changed.
    [[nodiscard]] AiSplineSessionResult
    setPointPositions(std::span<const AiSplinePointPositionEdit> edits);
    // Restore selected records from the load-time backup in raw vector order.
    // A point-count mismatch restores the complete native backup instead.
    [[nodiscard]] AiSplineSessionResult restoreSelectedDefaults(
        std::span<const std::uint32_t> selectedPointIndices);
    [[nodiscard]] AiSplineSessionResult undo();
    [[nodiscard]] AiSplineSessionResult redo();
    [[nodiscard]] AiSplineSessionResult restoreBaseline();

private:
    struct Snapshot {
        formats::AiSpline spline;
        std::vector<std::uint8_t> bytes;
        std::size_t modelBytes = 0U;
    };
    using SnapshotPtr = std::shared_ptr<const Snapshot>;

    [[nodiscard]] AiSplineSessionResult
    commitSnapshot(SnapshotPtr next, std::size_t applied,
                   std::optional<std::uint32_t> pointIndex = std::nullopt,
                   std::optional<std::uint32_t> payloadIndex = std::nullopt);
    [[nodiscard]] SnapshotPtr
    makeSnapshot(formats::AiSpline spline,
                 std::vector<std::uint8_t> bytes) const;
    [[nodiscard]] bool stagePush(const SnapshotPtr& snapshot,
                                 std::deque<SnapshotPtr>& history,
                                 std::size_t& historyBytes,
                                 std::size_t& historyModelBytes,
                                 AiSplineSessionResult& result) const;
    [[nodiscard]] AiSplineSessionResult moveHistory(bool undoDirection);

    AiSplineSessionLimits limits_;
    SnapshotPtr baseline_;
    SnapshotPtr current_;
    std::deque<SnapshotPtr> undo_;
    std::deque<SnapshotPtr> redo_;
    std::size_t undoBytes_ = 0U;
    std::size_t redoBytes_ = 0U;
    std::size_t undoModelBytes_ = 0U;
    std::size_t redoModelBytes_ = 0U;
    std::uint64_t revision_ = 0U;
};

} // namespace apex::authoring
