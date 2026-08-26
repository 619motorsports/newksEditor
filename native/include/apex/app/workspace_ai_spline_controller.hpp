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
    invalid_edit,
    overlay_failed,
    viewport_failed,
    unsupported,
    allocation_failed,
};

struct WorkspaceAiSplineControllerResult {
    WorkspaceAiSplineControllerStatus status =
        WorkspaceAiSplineControllerStatus::invalid_edit;
    render::Diagnostic diagnostic;
    std::uint64_t revision = 0U;
    std::size_t applied = 0U;
    std::size_t replacedPassCount = 0U;
    bool changed = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceAiSplineControllerStatus::ready ||
               status == WorkspaceAiSplineControllerStatus::unchanged;
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
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;

    [[nodiscard]] WorkspaceAiSplineControllerResult setPointPositions(
        render::Device& device, WorkspaceViewport& viewport,
        std::span<const authoring::AiSplinePointPositionEdit> edits,
        std::uint64_t expectedRevision);
    [[nodiscard]] WorkspaceAiSplineControllerResult moveSelectedByManualInput(
        render::Device& device, WorkspaceViewport& viewport,
        const WorkspaceAiSplineManualMovement& movement,
        std::uint64_t expectedRevision);
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
    [[nodiscard]] WorkspaceAiSplineControllerResult staleResult() const;
    [[nodiscard]] WorkspaceAiSplineControllerResult
    viewportBindingResult() const;

    std::unique_ptr<State> state_;
};

[[nodiscard]] const char* workspace_ai_spline_controller_status_name(
    WorkspaceAiSplineControllerStatus status) noexcept;

} // namespace apex::app
