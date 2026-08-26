#include "apex/app/workspace_ai_spline_controller.hpp"

#include "apex/app/installed_editor_spline.hpp"
#include "apex/formats/ai_spline_write.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <unordered_set>
#include <utility>

namespace apex::app {
namespace {

[[nodiscard]] render::Diagnostic diagnostic(const char* code,
                                            const char* message) {
    return {code, message};
}

[[nodiscard]] WorkspaceAiSplineOverlayRequest overlayRequest(
    const WorkspaceAiSplineControllerConfiguration& configuration) noexcept {
    WorkspaceAiSplineOverlayRequest request;
    request.mode = configuration.mode;
    request.interval = configuration.interval;
    request.show_left = configuration.showLeft;
    request.show_right = configuration.showRight;
    request.selected_indices = configuration.selectedIndices;
    request.show_camber = configuration.showCamber;
    return request;
}

[[nodiscard]] WorkspaceAiSplineControllerStatus sessionFailureStatus(
    const authoring::AiSplineSessionResult& result) noexcept {
    if (result.status == authoring::AiSplineWaypointStatus::unsupported)
        return WorkspaceAiSplineControllerStatus::unsupported;
    if (!result.diagnostics.empty() &&
        result.diagnostics.front().code == "ALLOCATION_FAILED")
        return WorkspaceAiSplineControllerStatus::allocation_failed;
    return WorkspaceAiSplineControllerStatus::invalid_edit;
}

[[nodiscard]] WorkspaceAiSplineControllerResult sessionFailure(
    const authoring::AiSplineSessionResult& sessionResult,
    std::uint64_t currentRevision) {
    WorkspaceAiSplineControllerResult result;
    result.status = sessionFailureStatus(sessionResult);
    result.revision = currentRevision;
    result.applied = sessionResult.applied;
    if (!sessionResult.diagnostics.empty()) {
        result.diagnostic.code = sessionResult.diagnostics.front().code;
        result.diagnostic.message = sessionResult.diagnostics.front().message;
    } else {
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_session_failed",
            "AI spline session operation failed without a diagnostic");
    }
    return result;
}

[[nodiscard]] WorkspaceAiSplineControllerStatus overlayFailureStatus(
    WorkspaceAiSplineStatus status) noexcept {
    return status == WorkspaceAiSplineStatus::allocation_failed
               ? WorkspaceAiSplineControllerStatus::allocation_failed
               : WorkspaceAiSplineControllerStatus::overlay_failed;
}

[[nodiscard]] bool canonicalizeConfiguration(
    WorkspaceAiSplineControllerConfiguration& configuration,
    render::Diagnostic& outputDiagnostic) {
    if (configuration.selectedIndices.size() >
        authoring::aiSplineMaxSelectionEntries) {
        outputDiagnostic = diagnostic(
            "workspace_ai_spline_controller_selection_limit",
            "AI spline controller input exceeds the selection entry limit");
        return false;
    }
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(std::min(configuration.selectedIndices.size(),
                          workspace_ai_spline_max_selection_points));
    std::vector<std::uint32_t> unique;
    unique.reserve(configuration.selectedIndices.size());
    for (const std::uint32_t index : configuration.selectedIndices) {
        if (seen.insert(index).second) {
            if (unique.size() >=
                workspace_ai_spline_max_selection_points) {
                outputDiagnostic = diagnostic(
                    "workspace_ai_spline_controller_selection_limit",
                    "AI spline controller selection exceeds the marker "
                    "limit");
                return false;
            }
            unique.push_back(index);
        }
    }
    configuration.selectedIndices = std::move(unique);
    return true;
}

[[nodiscard]] bool buildMovementForwards(
    const formats::AiSpline& spline,
    std::vector<std::array<float, 2U>>& forwards,
    render::Diagnostic& outputDiagnostic) {
    forwards.reserve(spline.points.size());
    const bool closed =
        spline.points.size() >= 2U &&
        installedEditorSplinePointDistance(spline.points.back().position,
                                           spline.points.front().position) <=
            installed_editor_spline_closure_distance;
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        const std::size_t nextIndex =
            index + 1U < spline.points.size()
                ? index + 1U
                : (closed && !spline.points.empty() ? 0U : index);
        float forwardX = spline.points[nextIndex].position[0] -
                         spline.points[index].position[0];
        float forwardZ = spline.points[nextIndex].position[2] -
                         spline.points[index].position[2];
        const float length =
            std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (!std::isfinite(length)) {
            outputDiagnostic = diagnostic(
                "workspace_ai_spline_controller_forward_non_finite",
                "AI spline manual movement produced a non-finite forward "
                "direction");
            return false;
        }
        if (length != 0.0F) {
            forwardX /= length;
            forwardZ /= length;
        }
        if (!std::isfinite(forwardX) || !std::isfinite(forwardZ)) {
            outputDiagnostic = diagnostic(
                "workspace_ai_spline_controller_forward_non_finite",
                "AI spline manual movement produced a non-finite forward "
                "direction");
            return false;
        }
        forwards.push_back({forwardX, forwardZ});
    }
    return true;
}

} // namespace

bool WorkspaceAiSplineManualInputState::setPressed(
    WorkspaceAiSplineManualKey key, bool pressed) noexcept {
    const auto index = static_cast<std::size_t>(key);
    if (index >= pressed_.size() || (pressed && !focused_)) return false;
    const bool changed = pressed_[index] != pressed;
    pressed_[index] = pressed;
    return changed;
}

void WorkspaceAiSplineManualInputState::setFocused(bool focused) noexcept {
    focused_ = focused;
    if (!focused_) clear();
}

void WorkspaceAiSplineManualInputState::clear() noexcept {
    pressed_.fill(false);
}

WorkspaceAiSplineManualMovement
WorkspaceAiSplineManualInputState::movement() const noexcept {
    WorkspaceAiSplineManualMovement result;
    result.forward = pressed_[static_cast<std::size_t>(
        WorkspaceAiSplineManualKey::forward)];
    result.backward = pressed_[static_cast<std::size_t>(
        WorkspaceAiSplineManualKey::backward)];
    result.left = pressed_[static_cast<std::size_t>(
        WorkspaceAiSplineManualKey::left)];
    result.right = pressed_[static_cast<std::size_t>(
        WorkspaceAiSplineManualKey::right)];
    result.up = pressed_[static_cast<std::size_t>(
        WorkspaceAiSplineManualKey::up)];
    result.down = pressed_[static_cast<std::size_t>(
        WorkspaceAiSplineManualKey::down)];
    result.accelerated =
        pressed_[static_cast<std::size_t>(
            WorkspaceAiSplineManualKey::left_control)] ||
        pressed_[static_cast<std::size_t>(
            WorkspaceAiSplineManualKey::right_control)];
    return result;
}

std::array<float, 3U> workspaceAiSplineManualLocalDelta(
    const WorkspaceAiSplineManualMovement& movement) noexcept {
    const float amount =
        (movement.accelerated
             ? workspace_ai_spline_manual_accelerated_speed
             : workspace_ai_spline_manual_speed) *
        workspace_ai_spline_manual_fixed_delta;
    std::array<float, 3U> result{};
    if (movement.forward) result[2] -= amount;
    if (movement.backward) result[2] += amount;
    if (movement.left) result[0] -= amount;
    if (movement.right) result[0] += amount;
    if (movement.up) result[1] += amount;
    if (movement.down) result[1] -= amount;
    return result;
}

struct WorkspaceAiSplineController::State {
    authoring::AiSplineSession session;
    WorkspaceAiSplineControllerConfiguration configuration;
    WorkspaceAiSplineOverlaySet overlays;
    std::vector<std::array<float, 2U>> movementForwards;
    WorkspaceViewportAiSplineGeneration generation;
    bool editing = false;

    State(authoring::AiSplineSession candidateSession,
          WorkspaceAiSplineControllerConfiguration candidateConfiguration,
          WorkspaceAiSplineOverlaySet candidateOverlays,
          std::vector<std::array<float, 2U>> candidateMovementForwards,
          WorkspaceViewportAiSplineGeneration candidateGeneration,
          bool candidateEditing = false)
        : session(std::move(candidateSession)),
          configuration(std::move(candidateConfiguration)),
          overlays(std::move(candidateOverlays)),
          movementForwards(std::move(candidateMovementForwards)),
          generation(std::move(candidateGeneration)),
          editing(candidateEditing) {}
};

WorkspaceAiSplineController::WorkspaceAiSplineController(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

WorkspaceAiSplineController::WorkspaceAiSplineController(
    WorkspaceAiSplineController&&) noexcept = default;

WorkspaceAiSplineController& WorkspaceAiSplineController::operator=(
    WorkspaceAiSplineController&&) noexcept = default;

WorkspaceAiSplineController::~WorkspaceAiSplineController() = default;

WorkspaceAiSplineControllerCreateResult WorkspaceAiSplineController::create(
    formats::AiSpline baseline,
    WorkspaceAiSplineControllerConfiguration configuration,
    authoring::AiSplineSessionLimits limits) {
    WorkspaceAiSplineControllerCreateResult result;
    if (baseline.version != 7U) {
        result.status = WorkspaceAiSplineControllerStatus::unsupported;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_version_unsupported",
            "Live AI spline editing supports native file version 7 only");
        return result;
    }
    try {
        if (!canonicalizeConfiguration(configuration, result.diagnostic))
            return result;
        authoring::AiSplineSession session(std::move(baseline),
                                            std::move(limits));
        auto built = buildWorkspaceAiSplineOverlays(
            session.current(), overlayRequest(configuration));
        if (!built.ok()) {
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }
        std::vector<std::array<float, 2U>> movementForwards;
        if (!buildMovementForwards(session.current(), movementForwards,
                                   result.diagnostic))
            return result;
        WorkspaceViewportAiSplineGeneration generation;
        generation.owner =
            std::make_shared<WorkspaceViewportAiSplineGenerationOwner>();
        generation.revision = session.revision();
        auto state = std::make_unique<State>(
            std::move(session), std::move(configuration),
            std::move(built.overlays), std::move(movementForwards),
            std::move(generation));
        result.controller = std::unique_ptr<WorkspaceAiSplineController>(
            new WorkspaceAiSplineController(std::move(state)));
        result.status = WorkspaceAiSplineControllerStatus::ready;
        return result;
    } catch (const formats::AiSplineWriteError& error) {
        result.status =
            error.code() == "UNSUPPORTED_VERSION"
                ? WorkspaceAiSplineControllerStatus::unsupported
            : error.code() == "ALLOCATION_FAILED"
                ? WorkspaceAiSplineControllerStatus::allocation_failed
                : WorkspaceAiSplineControllerStatus::invalid_edit;
        result.diagnostic = {error.code(), error.what()};
        return result;
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline controller exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        result.diagnostic = {
            "workspace_ai_spline_controller_create_failed", error.what()};
        return result;
    }
}

const formats::AiSpline& WorkspaceAiSplineController::current() const noexcept {
    return state_->session.current();
}

const std::vector<std::uint8_t>&
WorkspaceAiSplineController::currentBytes() const noexcept {
    return state_->session.currentBytes();
}

const WorkspaceAiSplineOverlaySet&
WorkspaceAiSplineController::overlays() const noexcept {
    return state_->overlays;
}

const WorkspaceAiSplineControllerConfiguration&
WorkspaceAiSplineController::configuration() const noexcept {
    return state_->configuration;
}

const WorkspaceViewportAiSplineGeneration&
WorkspaceAiSplineController::generation() const noexcept {
    return state_->generation;
}

std::uint64_t WorkspaceAiSplineController::revision() const noexcept {
    return state_->session.revision();
}

bool WorkspaceAiSplineController::dirty() const noexcept {
    return state_->session.currentBytes() != state_->session.baselineBytes();
}

bool WorkspaceAiSplineController::canUndo() const noexcept {
    return state_->session.canUndo();
}

bool WorkspaceAiSplineController::canRedo() const noexcept {
    return state_->session.canRedo();
}

bool WorkspaceAiSplineController::editing() const noexcept {
    return state_->editing;
}

WorkspaceAiSplineControllerSaveResult
WorkspaceAiSplineController::buildSaveBytes(
    std::uint64_t expectedRevision) const {
    if (expectedRevision != state_->session.revision()) {
        WorkspaceAiSplineControllerSaveResult result;
        result.status =
            WorkspaceAiSplineControllerSaveStatus::stale_revision;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_revision_stale",
            "AI spline save revision does not match the current revision");
        return result;
    }
    auto saved = state_->session.buildSaveBytes();
    WorkspaceAiSplineControllerSaveResult result;
    result.revision = saved.revision;
    if (saved.ok()) {
        result.status = WorkspaceAiSplineControllerSaveStatus::ready;
        result.bytes = std::move(saved.bytes);
        return result;
    }
    switch (saved.status) {
    case authoring::AiSplineSessionSaveStatus::ok:
        result.status = WorkspaceAiSplineControllerSaveStatus::ready;
        break;
    case authoring::AiSplineSessionSaveStatus::invalid:
        result.status = WorkspaceAiSplineControllerSaveStatus::invalid;
        break;
    case authoring::AiSplineSessionSaveStatus::unsupported:
        result.status = WorkspaceAiSplineControllerSaveStatus::unsupported;
        break;
    case authoring::AiSplineSessionSaveStatus::resource_limit:
        result.status =
            WorkspaceAiSplineControllerSaveStatus::resource_limit;
        break;
    case authoring::AiSplineSessionSaveStatus::failed:
        result.status = WorkspaceAiSplineControllerSaveStatus::failed;
        break;
    }
    if (saved.diagnostics.empty()) {
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_save_failed",
            "AI spline save failed without a diagnostic");
    } else {
        result.diagnostic.code = saved.diagnostics.front().code;
        result.diagnostic.message = saved.diagnostics.front().message;
    }
    return result;
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::staleResult() const {
    WorkspaceAiSplineControllerResult result;
    result.status = WorkspaceAiSplineControllerStatus::stale_revision;
    result.revision = state_->session.revision();
    result.diagnostic = diagnostic(
        "workspace_ai_spline_controller_revision_stale",
        "AI spline edit revision does not match the current revision");
    return result;
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::viewportBindingResult() const {
    WorkspaceAiSplineControllerResult result;
    result.status = WorkspaceAiSplineControllerStatus::stale_revision;
    result.revision = state_->session.revision();
    result.diagnostic = diagnostic(
        "workspace_ai_spline_controller_viewport_generation_mismatch",
        "AI spline viewport does not show the current controller revision");
    return result;
}

bool WorkspaceAiSplineController::viewportMatches(
    const WorkspaceViewport& viewport) const noexcept {
    return viewport.aiSplineGenerationIdentity().has_value() &&
           *viewport.aiSplineGenerationIdentity() == state_->generation;
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::publishCandidate(
    render::Device& device, WorkspaceViewport& viewport,
    authoring::AiSplineSession candidate,
    authoring::AiSplineSessionResult sessionResult) {
    if (!sessionResult.ok())
        return sessionFailure(sessionResult, state_->session.revision());
    if (!sessionResult.changed) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::unchanged;
        result.revision = state_->session.revision();
        result.applied = sessionResult.applied;
        return result;
    }

    try {
        auto built = buildWorkspaceAiSplineOverlays(
            candidate.current(), overlayRequest(state_->configuration));
        if (!built.ok()) {
            WorkspaceAiSplineControllerResult result;
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            result.revision = state_->session.revision();
            result.applied = sessionResult.applied;
            return result;
        }
        auto nextGeneration = state_->generation;
        nextGeneration.revision = sessionResult.revision;
        auto nextState = std::make_unique<State>(
            std::move(candidate), state_->configuration,
            std::move(built.overlays), state_->movementForwards,
            std::move(nextGeneration), state_->editing);
        const auto replaced =
            viewport.replaceAiSplineOverlays(
                device, nextState->overlays,
                WorkspaceViewportAiSplineGenerationTransition{
                    state_->generation, nextState->generation});
        if (!replaced.ok()) {
            WorkspaceAiSplineControllerResult result;
            result.status =
                replaced.status ==
                        WorkspaceViewportAiSplineUpdateStatus::unsupported
                    ? WorkspaceAiSplineControllerStatus::unsupported
                : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                         allocation_failed
                    ? WorkspaceAiSplineControllerStatus::allocation_failed
                    : WorkspaceAiSplineControllerStatus::viewport_failed;
            result.diagnostic = replaced.diagnostic;
            result.revision = state_->session.revision();
            result.applied = sessionResult.applied;
            return result;
        }

        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.revision = sessionResult.revision;
        result.applied = sessionResult.applied;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        return result;
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline edit exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        WorkspaceAiSplineControllerResult result;
        result.revision = state_->session.revision();
        result.diagnostic = {
            "workspace_ai_spline_controller_publish_failed", error.what()};
        return result;
    }
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::updateEditingState(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision, bool editing, bool clearSelection) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (!viewportMatches(viewport))
        return viewportBindingResult();

    const bool selectionChanged =
        clearSelection && !state_->configuration.selectedIndices.empty();
    if (!selectionChanged && state_->editing == editing) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::unchanged;
        result.revision = state_->session.revision();
        return result;
    }
    if (!selectionChanged) {
        state_->editing = editing;
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.revision = state_->session.revision();
        result.changed = true;
        return result;
    }

    try {
        auto candidateConfiguration = state_->configuration;
        candidateConfiguration.selectedIndices.clear();
        auto built = buildWorkspaceAiSplineOverlays(
            state_->session.current(), overlayRequest(candidateConfiguration));
        if (!built.ok()) {
            WorkspaceAiSplineControllerResult result;
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            result.revision = state_->session.revision();
            return result;
        }
        auto nextState = std::make_unique<State>(
            state_->session, std::move(candidateConfiguration),
            std::move(built.overlays), state_->movementForwards,
            state_->generation, editing);
        const auto revision = state_->session.revision();
        const auto replaced = viewport.replaceAiSplineOverlays(
            device, nextState->overlays,
            WorkspaceViewportAiSplineGenerationTransition{
                state_->generation, nextState->generation});
        if (!replaced.ok()) {
            WorkspaceAiSplineControllerResult result;
            result.status =
                replaced.status ==
                        WorkspaceViewportAiSplineUpdateStatus::unsupported
                    ? WorkspaceAiSplineControllerStatus::unsupported
                : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                         allocation_failed
                    ? WorkspaceAiSplineControllerStatus::allocation_failed
                    : WorkspaceAiSplineControllerStatus::viewport_failed;
            result.diagnostic = replaced.diagnostic;
            result.revision = revision;
            return result;
        }

        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.revision = revision;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        return result;
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline edit lifecycle exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        WorkspaceAiSplineControllerResult result;
        result.revision = state_->session.revision();
        result.diagnostic = {
            "workspace_ai_spline_controller_lifecycle_failed", error.what()};
        return result;
    }
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::startEditing(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    return updateEditingState(device, viewport, expectedRevision, true, true);
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::finishEditing(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    return updateEditingState(device, viewport, expectedRevision, false,
                              false);
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::cancelEditing(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    return updateEditingState(device, viewport, expectedRevision, false, true);
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::setPointPositions(
    render::Device& device, WorkspaceViewport& viewport,
    std::span<const authoring::AiSplinePointPositionEdit> edits,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    try {
        auto candidate = state_->session;
        auto sessionResult = candidate.setPointPositions(edits);
        return publishCandidate(device, viewport, std::move(candidate),
                                std::move(sessionResult));
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline edit exceeded available allocation capacity");
        return result;
    }
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::moveSelectedByManualInput(
    render::Device& device, WorkspaceViewport& viewport,
    const WorkspaceAiSplineManualMovement& movement,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    try {
        const auto local = workspaceAiSplineManualLocalDelta(movement);
        if (state_->configuration.selectedIndices.empty() ||
            (local[0U] == 0.0F && local[1U] == 0.0F &&
             local[2U] == 0.0F)) {
            WorkspaceAiSplineControllerResult result;
            result.status = WorkspaceAiSplineControllerStatus::unchanged;
            result.revision = state_->session.revision();
            return result;
        }
        std::vector<authoring::AiSplinePointPositionEdit> edits;
        edits.reserve(state_->configuration.selectedIndices.size());
        for (const std::uint32_t selectedIndex :
             state_->configuration.selectedIndices) {
            const auto index = static_cast<std::size_t>(selectedIndex);
            if (index >= state_->session.current().points.size() ||
                index >= state_->movementForwards.size()) {
                WorkspaceAiSplineControllerResult result;
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
                result.revision = state_->session.revision();
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_selection_invalid",
                    "AI spline manual movement selection is outside the "
                    "current model");
                return result;
            }
            const auto& position =
                state_->session.current().points[index].position;
            const float headingX = state_->movementForwards[index][0U];
            const float headingZ = state_->movementForwards[index][1U];
            const std::array<float, 3U> worldDelta = {
                -headingZ * local[0U] - headingX * local[2U],
                local[1U],
                headingX * local[0U] - headingZ * local[2U]};
            authoring::AiSplinePointPositionEdit edit;
            edit.pointIndex = selectedIndex;
            edit.position = {
                position[0U] + worldDelta[0U],
                position[1U] + worldDelta[1U],
                position[2U] + worldDelta[2U]};
            edits.push_back(edit);
        }
        return setPointPositions(device, viewport, edits, expectedRevision);
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline manual movement exceeded available allocation "
            "capacity");
        return result;
    }
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::undo(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    try {
        auto candidate = state_->session;
        auto sessionResult = candidate.undo();
        return publishCandidate(device, viewport, std::move(candidate),
                                std::move(sessionResult));
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline undo exceeded available allocation capacity");
        return result;
    }
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::redo(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    try {
        auto candidate = state_->session;
        auto sessionResult = candidate.redo();
        return publishCandidate(device, viewport, std::move(candidate),
                                std::move(sessionResult));
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline redo exceeded available allocation capacity");
        return result;
    }
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::restoreBaseline(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    try {
        auto candidate = state_->session;
        auto sessionResult = candidate.restoreBaseline();
        return publishCandidate(device, viewport, std::move(candidate),
                                std::move(sessionResult));
    } catch (const std::bad_alloc&) {
        WorkspaceAiSplineControllerResult result;
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.revision = state_->session.revision();
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline reset exceeded available allocation capacity");
        return result;
    }
}

const char* workspace_ai_spline_controller_status_name(
    WorkspaceAiSplineControllerStatus status) noexcept {
    switch (status) {
    case WorkspaceAiSplineControllerStatus::ready:
        return "ready";
    case WorkspaceAiSplineControllerStatus::unchanged:
        return "unchanged";
    case WorkspaceAiSplineControllerStatus::stale_revision:
        return "stale_revision";
    case WorkspaceAiSplineControllerStatus::invalid_edit:
        return "invalid_edit";
    case WorkspaceAiSplineControllerStatus::overlay_failed:
        return "overlay_failed";
    case WorkspaceAiSplineControllerStatus::viewport_failed:
        return "viewport_failed";
    case WorkspaceAiSplineControllerStatus::unsupported:
        return "unsupported";
    case WorkspaceAiSplineControllerStatus::allocation_failed:
        return "allocation_failed";
    }
    return "invalid_edit";
}

} // namespace apex::app
