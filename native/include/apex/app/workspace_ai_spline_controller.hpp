#pragma once

#include "apex/app/workspace_viewport.hpp"
#include "apex/authoring/ai_spline_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::app {

struct WorkspaceAiSplineControllerConfiguration {
    WorkspaceAiSplineDisplayMode mode = WorkspaceAiSplineDisplayMode::raw;
    std::optional<WorkspaceAiSplineInterval> interval;
    bool showLeft = false;
    bool showRight = false;
    std::vector<std::uint32_t> selectedIndices;
    bool showCamber = false;
};

inline constexpr float workspace_ai_spline_manual_fixed_delta = 0.16F;
inline constexpr float workspace_ai_spline_manual_speed = 0.1F;
inline constexpr float workspace_ai_spline_manual_accelerated_speed = 1.0F;

enum class WorkspaceAiSplineManualKey : std::uint8_t {
    forward,
    backward,
    left,
    right,
    up,
    down,
    left_control,
    right_control,
    count,
};

struct WorkspaceAiSplineManualMovement {
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool accelerated = false;

    friend bool operator==(const WorkspaceAiSplineManualMovement&,
                           const WorkspaceAiSplineManualMovement&) = default;
};

// This state consumes portable key transitions. Repeated key-down events are
// idempotent. Focus loss clears held keys and blocks new key-down events.
class WorkspaceAiSplineManualInputState final {
public:
    [[nodiscard]] bool setPressed(WorkspaceAiSplineManualKey key,
                                  bool pressed) noexcept;
    void setFocused(bool focused) noexcept;
    void clear() noexcept;
    [[nodiscard]] WorkspaceAiSplineManualMovement movement() const noexcept;

private:
    std::array<bool, static_cast<std::size_t>(
                         WorkspaceAiSplineManualKey::count)>
        pressed_{};
    bool focused_ = true;
};

// Recover the fixed local movement applied by one installed-editor callback.
// The source passes 0.16 instead of the measured frame duration.
[[nodiscard]] std::array<float, 3U> workspaceAiSplineManualLocalDelta(
    const WorkspaceAiSplineManualMovement& movement) noexcept;

enum class WorkspaceAiSplineControllerStatus : std::uint8_t {
    ready,
    unchanged,
    stale_revision,
    stale_input,
    stale_state,
    invalid_edit,
    overlay_failed,
    viewport_failed,
    unsupported,
    allocation_failed,
};

// This snapshot identifies the controller state that produced one input
// event. The input epoch changes after selection, edit-mode, or visibility
// changes that can invalidate an earlier event.
struct WorkspaceAiSplineControllerInputSnapshot {
    WorkspaceViewportAiSplineGeneration generation;
    std::uint64_t inputEpoch = 0U;
    bool editing = false;

    [[nodiscard]] bool valid() const noexcept {
        return generation.valid();
    }

    friend bool operator==(
        const WorkspaceAiSplineControllerInputSnapshot&,
        const WorkspaceAiSplineControllerInputSnapshot&) = default;
};

struct WorkspaceAiSplineControllerResult {
    WorkspaceAiSplineControllerStatus status =
        WorkspaceAiSplineControllerStatus::invalid_edit;
    render::Diagnostic diagnostic;
    WorkspaceAiSplineControllerInputSnapshot resultingInput;
    std::uint64_t revision = 0U;
    std::size_t applied = 0U;
    std::size_t replacedPassCount = 0U;
    bool changed = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineControllerStatus::ready ||
               status == WorkspaceAiSplineControllerStatus::unchanged;
    }
};

// A caller creates this request after it resolves a viewport hit to one
// validated spline point. Screen-space hit testing is intentionally outside
// the controller. Capture the controller input snapshot with the event.
struct WorkspaceAiSplinePointSelectionRequest {
    std::uint32_t pointIndex = 0U;
    std::array<float, 3U> pickedPosition{};
    bool controlPressed = false;
    bool shiftPressed = false;
    WorkspaceAiSplineControllerInputSnapshot expected;
};

struct WorkspaceAiSplinePointSelectionResult {
    WorkspaceAiSplineControllerStatus status =
        WorkspaceAiSplineControllerStatus::invalid_edit;
    render::Diagnostic diagnostic;
    WorkspaceAiSplineControllerInputSnapshot resultingInput;
    std::uint64_t revision = 0U;
    std::size_t selectionCount = 0U;
    std::size_t temporaryPointCount = 0U;
    std::optional<std::uint32_t> lastSelectedIndex;
    std::optional<std::size_t> movableTemporaryPoint;
    std::optional<float> normalizedPosition;
    std::size_t replacedPassCount = 0U;
    bool changed = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineControllerStatus::ready ||
               status == WorkspaceAiSplineControllerStatus::unchanged;
    }
};

enum class WorkspaceAiSplineControllerSaveStatus : std::uint8_t {
    ready,
    stale_revision,
    invalid,
    unsupported,
    resource_limit,
    failed,
};

struct WorkspaceAiSplineControllerSaveResult {
    WorkspaceAiSplineControllerSaveStatus status =
        WorkspaceAiSplineControllerSaveStatus::failed;
    render::Diagnostic diagnostic;
    std::vector<std::uint8_t> bytes;
    std::uint64_t revision = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineControllerSaveStatus::ready;
    }
};

class WorkspaceAiSplineController;

struct WorkspaceAiSplineControllerCreateResult {
    WorkspaceAiSplineControllerStatus status =
        WorkspaceAiSplineControllerStatus::invalid_edit;
    render::Diagnostic diagnostic;
    std::unique_ptr<WorkspaceAiSplineController> controller;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineControllerStatus::ready &&
               controller != nullptr;
    }
};

// This controller owns the visible AI-spline model and overlay generation.
// Each operation stages a complete session copy and overlay set. It publishes
// the buffers first, then swaps the staged application state without throwing.
// Call update operations on the render thread, between viewport draws.
// Getter references remain valid until the next successful update. Reacquire
// these references after an update reports that it changed the controller.
class WorkspaceAiSplineController final {
public:
    WorkspaceAiSplineController(const WorkspaceAiSplineController&) = delete;
    WorkspaceAiSplineController&
    operator=(const WorkspaceAiSplineController&) = delete;
    WorkspaceAiSplineController(WorkspaceAiSplineController&&) noexcept;
    WorkspaceAiSplineController&
    operator=(WorkspaceAiSplineController&&) noexcept;
    ~WorkspaceAiSplineController();

    [[nodiscard]] static WorkspaceAiSplineControllerCreateResult create(
        formats::AiSpline baseline,
        WorkspaceAiSplineControllerConfiguration configuration,
        authoring::AiSplineSessionLimits limits = {});

    [[nodiscard]] const formats::AiSpline& current() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>&
    currentBytes() const noexcept;
    [[nodiscard]] const WorkspaceAiSplineOverlaySet& overlays() const noexcept;
    [[nodiscard]] const WorkspaceAiSplineControllerConfiguration&
    configuration() const noexcept;
    [[nodiscard]] const WorkspaceViewportAiSplineGeneration&
    generation() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool editing() const noexcept;
    [[nodiscard]] const std::vector<WorkspaceAiSplineTemporaryEditPoint>&
    temporaryEditPoints() const noexcept;
    [[nodiscard]] std::optional<std::size_t>
    movableTemporaryPoint() const noexcept;
    [[nodiscard]] WorkspaceAiSplineControllerInputSnapshot
    inputSnapshot() const noexcept;

    // Build recovered save bytes without changing visible or authoring state.
    [[nodiscard]] WorkspaceAiSplineControllerSaveResult
    buildSaveBytes(std::uint64_t expectedRevision) const;

    // Publish independent installed-editor side visibility without changing
    // the spline model, authoring history, selection, or edit state.
    [[nodiscard]] WorkspaceAiSplineControllerResult setSideVisibility(
        render::Device& device, WorkspaceViewport& viewport,
        bool showLeft, bool showRight,
        const WorkspaceAiSplineControllerInputSnapshot& expected);
    [[nodiscard]] WorkspaceAiSplineControllerResult setPointPositions(
        render::Device& device, WorkspaceViewport& viewport,
        std::span<const authoring::AiSplinePointPositionEdit> edits,
        std::uint64_t expectedRevision);
    [[nodiscard]] WorkspaceAiSplineControllerResult moveSelectedByManualInput(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplineManualMovement& movement,
        const WorkspaceAiSplineControllerInputSnapshot& expected);
    [[nodiscard]] WorkspaceAiSplineControllerResult startEditing(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplineControllerInputSnapshot& expected);
    [[nodiscard]] WorkspaceAiSplineControllerResult finishEditing(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplineControllerInputSnapshot& expected);
    [[nodiscard]] WorkspaceAiSplineControllerResult cancelEditing(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplineControllerInputSnapshot& expected);
    [[nodiscard]] WorkspaceAiSplinePointSelectionResult selectPoint(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplinePointSelectionRequest& request);
    [[nodiscard]] WorkspaceAiSplineControllerResult undo(
        render::Device& device, WorkspaceViewport& viewport,
        std::uint64_t expectedRevision);
    [[nodiscard]] WorkspaceAiSplineControllerResult redo(
        render::Device& device, WorkspaceViewport& viewport,
        std::uint64_t expectedRevision);
    [[nodiscard]] WorkspaceAiSplineControllerResult restoreBaseline(
        render::Device& device, WorkspaceViewport& viewport,
        std::uint64_t expectedRevision);

private:
    struct State;

    explicit WorkspaceAiSplineController(std::unique_ptr<State> state) noexcept;
    [[nodiscard]] WorkspaceAiSplineControllerResult publishCandidate(
        render::Device& device, WorkspaceViewport& viewport,
        authoring::AiSplineSession candidate,
        authoring::AiSplineSessionResult sessionResult);
    [[nodiscard]] WorkspaceAiSplineControllerResult currentResult() const;
    [[nodiscard]] WorkspaceAiSplineControllerResult staleResult() const;
    [[nodiscard]] WorkspaceAiSplineControllerResult
    viewportBindingResult() const;
    [[nodiscard]] bool viewportMatches(
        const WorkspaceViewport& viewport) const noexcept;
    [[nodiscard]] WorkspaceAiSplineControllerResult updateEditingState(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplineControllerInputSnapshot& expected,
        bool editing,
        bool clearSelection,
        bool clearTemporaryPoints);
    [[nodiscard]] bool inputMatches(
        const WorkspaceAiSplineControllerInputSnapshot& expected) const
        noexcept;

    std::unique_ptr<State> state_;
};

[[nodiscard]] const char* workspace_ai_spline_controller_status_name(
    WorkspaceAiSplineControllerStatus status) noexcept;

} // namespace apex::app
