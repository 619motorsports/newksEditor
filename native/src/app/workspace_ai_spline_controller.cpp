#include "apex/app/workspace_ai_spline_controller.hpp"

#include "apex/app/installed_editor_spline.hpp"
#include "apex/formats/ai_spline_write.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace apex::app {
namespace {

[[nodiscard]] render::Diagnostic diagnostic(const char* code,
                                            const char* message) {
    return {code, message};
}

[[nodiscard]] bool finitePosition(
    const std::array<float, 3U>& position) noexcept {
    return std::all_of(position.begin(), position.end(),
                       [](float value) { return std::isfinite(value); });
}

[[nodiscard]] WorkspaceAiSplineOverlayRequest overlayRequest(
    const WorkspaceAiSplineControllerConfiguration& configuration,
    std::span<const WorkspaceAiSplineTemporaryEditPoint> temporaryPoints = {},
    std::optional<std::size_t> movableTemporaryPoint = std::nullopt) noexcept {
    WorkspaceAiSplineOverlayRequest request;
    request.mode = configuration.mode;
    request.interval = configuration.interval;
    request.show_left = configuration.showLeft;
    request.show_right = configuration.showRight;
    request.selected_indices = configuration.selectedIndices;
    request.temporary_edit_points = temporaryPoints;
    request.movable_temporary_point = movableTemporaryPoint;
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

[[nodiscard]] bool appendUniqueSelectionIndex(
    std::vector<std::uint32_t>& selectedIndices,
    std::unordered_set<std::uint32_t>& seen, std::uint32_t index,
    render::Diagnostic& outputDiagnostic) {
    if (!seen.insert(index).second) return true;
    if (selectedIndices.size() >= authoring::aiSplineMaxSelectionEntries ||
        selectedIndices.size() >= workspace_ai_spline_max_selection_points) {
        outputDiagnostic = diagnostic(
            "workspace_ai_spline_controller_selection_limit",
            "AI spline point selection exceeds the marker limit");
        return false;
    }
    selectedIndices.push_back(index);
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
    std::vector<WorkspaceAiSplineTemporaryEditPoint> temporaryEditPoints;
    std::optional<std::size_t> movableTemporaryPoint;
    WorkspaceViewportAiSplineGeneration generation;
    std::uint64_t inputEpoch = 0U;
    bool editing = false;

    State(authoring::AiSplineSession candidateSession,
          WorkspaceAiSplineControllerConfiguration candidateConfiguration,
          WorkspaceAiSplineOverlaySet candidateOverlays,
          std::vector<std::array<float, 2U>> candidateMovementForwards,
          WorkspaceViewportAiSplineGeneration candidateGeneration,
          std::uint64_t candidateInputEpoch = 0U,
          bool candidateEditing = false,
          std::vector<WorkspaceAiSplineTemporaryEditPoint>
              candidateTemporaryEditPoints = {},
          std::optional<std::size_t> candidateMovableTemporaryPoint =
              std::nullopt)
        : session(std::move(candidateSession)),
          configuration(std::move(candidateConfiguration)),
          overlays(std::move(candidateOverlays)),
          movementForwards(std::move(candidateMovementForwards)),
          temporaryEditPoints(std::move(candidateTemporaryEditPoints)),
          movableTemporaryPoint(candidateMovableTemporaryPoint),
          generation(std::move(candidateGeneration)),
          inputEpoch(candidateInputEpoch),
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

const std::vector<WorkspaceAiSplineTemporaryEditPoint>&
WorkspaceAiSplineController::temporaryEditPoints() const noexcept {
    return state_->temporaryEditPoints;
}

std::optional<std::size_t>
WorkspaceAiSplineController::movableTemporaryPoint() const noexcept {
    return state_->movableTemporaryPoint;
}

WorkspaceAiSplineControllerInputSnapshot
WorkspaceAiSplineController::inputSnapshot() const noexcept {
    return {state_->generation, state_->inputEpoch, state_->editing};
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::currentResult() const {
    WorkspaceAiSplineControllerResult result;
    result.resultingInput = inputSnapshot();
    result.revision = state_->session.revision();
    return result;
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
    auto result = currentResult();
    result.status = WorkspaceAiSplineControllerStatus::stale_revision;
    result.diagnostic = diagnostic(
        "workspace_ai_spline_controller_revision_stale",
        "AI spline edit revision does not match the current revision");
    return result;
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::viewportBindingResult() const {
    auto result = currentResult();
    result.status = WorkspaceAiSplineControllerStatus::stale_state;
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

bool WorkspaceAiSplineController::inputMatches(
    const WorkspaceAiSplineControllerInputSnapshot& expected) const noexcept {
    return expected.valid() && expected == inputSnapshot();
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::publishCandidate(
    render::Device& device, WorkspaceViewport& viewport,
    authoring::AiSplineSession candidate,
    authoring::AiSplineSessionResult sessionResult) {
    if (!sessionResult.ok()) {
        auto result =
            sessionFailure(sessionResult, state_->session.revision());
        result.resultingInput = inputSnapshot();
        return result;
    }
    if (!sessionResult.changed) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unchanged;
        result.applied = sessionResult.applied;
        return result;
    }
    if (state_->generation.publication ==
        std::numeric_limits<std::uint64_t>::max()) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unsupported;
        result.applied = sessionResult.applied;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_publication_exhausted",
            "AI spline overlay publication cannot advance without overflow");
        return result;
    }

    try {
        auto built = buildWorkspaceAiSplineOverlays(
            candidate.current(),
            overlayRequest(state_->configuration,
                           state_->temporaryEditPoints,
                           state_->movableTemporaryPoint));
        if (!built.ok()) {
            auto result = currentResult();
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            result.applied = sessionResult.applied;
            return result;
        }
        auto nextGeneration = state_->generation;
        nextGeneration.revision = sessionResult.revision;
        ++nextGeneration.publication;
        auto nextState = std::make_unique<State>(
            std::move(candidate), state_->configuration,
            std::move(built.overlays), state_->movementForwards,
            std::move(nextGeneration), state_->inputEpoch, state_->editing,
            state_->temporaryEditPoints, state_->movableTemporaryPoint);
        const auto replaced =
            viewport.replaceAiSplineOverlays(
                device, nextState->overlays,
                WorkspaceViewportAiSplineGenerationTransition{
                    state_->generation, nextState->generation});
        if (!replaced.ok()) {
            auto result = currentResult();
            result.status =
                replaced.status ==
                        WorkspaceViewportAiSplineUpdateStatus::unsupported
                    ? WorkspaceAiSplineControllerStatus::unsupported
                : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                         allocation_failed
                    ? WorkspaceAiSplineControllerStatus::allocation_failed
                    : WorkspaceAiSplineControllerStatus::viewport_failed;
            result.diagnostic = replaced.diagnostic;
            result.applied = sessionResult.applied;
            return result;
        }

        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.revision = sessionResult.revision;
        result.applied = sessionResult.applied;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        result.resultingInput = inputSnapshot();
        return result;
    } catch (const std::bad_alloc&) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline edit exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        auto result = currentResult();
        result.diagnostic = {
            "workspace_ai_spline_controller_publish_failed", error.what()};
        return result;
    }
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::updateEditingState(
    render::Device& device, WorkspaceViewport& viewport,
    const WorkspaceAiSplineControllerInputSnapshot& expected, bool editing,
    bool clearSelection, bool clearTemporaryPoints) {
    if (!inputMatches(expected)) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::stale_input;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_input_stale",
            "AI spline input snapshot does not match the current "
            "controller state");
        return result;
    }
    if (!viewportMatches(viewport))
        return viewportBindingResult();

    const bool selectionChanged =
        clearSelection && !state_->configuration.selectedIndices.empty();
    const bool temporaryChanged =
        clearTemporaryPoints && !state_->temporaryEditPoints.empty();
    if (!selectionChanged && !temporaryChanged &&
        state_->editing == editing) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unchanged;
        return result;
    }
    if (state_->inputEpoch ==
        std::numeric_limits<std::uint64_t>::max()) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unsupported;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_input_epoch_exhausted",
            "AI spline input epoch cannot advance without overflow");
        return result;
    }
    const std::uint64_t nextInputEpoch = state_->inputEpoch + 1U;
    if (!selectionChanged && !temporaryChanged) {
        state_->editing = editing;
        state_->inputEpoch = nextInputEpoch;
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.changed = true;
        return result;
    }
    if (state_->generation.publication ==
        std::numeric_limits<std::uint64_t>::max()) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unsupported;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_publication_exhausted",
            "AI spline overlay publication cannot advance without overflow");
        return result;
    }

    try {
        auto candidateConfiguration = state_->configuration;
        if (clearSelection) candidateConfiguration.selectedIndices.clear();
        auto candidateTemporaryPoints = state_->temporaryEditPoints;
        if (clearTemporaryPoints) candidateTemporaryPoints.clear();
        auto built = buildWorkspaceAiSplineOverlays(
            state_->session.current(),
            overlayRequest(candidateConfiguration, candidateTemporaryPoints,
                           clearTemporaryPoints
                               ? std::nullopt
                               : state_->movableTemporaryPoint));
        if (!built.ok()) {
            auto result = currentResult();
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }
        auto nextState = std::make_unique<State>(
            state_->session, std::move(candidateConfiguration),
            std::move(built.overlays), state_->movementForwards,
            state_->generation, nextInputEpoch, editing,
            std::move(candidateTemporaryPoints),
            clearTemporaryPoints ? std::nullopt
                                 : state_->movableTemporaryPoint);
        ++nextState->generation.publication;
        const auto revision = state_->session.revision();
        const auto replaced = viewport.replaceAiSplineOverlays(
            device, nextState->overlays,
            WorkspaceViewportAiSplineGenerationTransition{
                state_->generation, nextState->generation});
        if (!replaced.ok()) {
            auto result = currentResult();
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

        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.revision = revision;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        result.resultingInput = inputSnapshot();
        return result;
    } catch (const std::bad_alloc&) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline edit lifecycle exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        auto result = currentResult();
        result.diagnostic = {
            "workspace_ai_spline_controller_lifecycle_failed", error.what()};
        return result;
    }
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::startEditing(
    render::Device& device, WorkspaceViewport& viewport,
    const WorkspaceAiSplineControllerInputSnapshot& expected) {
    return updateEditingState(device, viewport, expected, true, true, false);
}

WorkspaceAiSplineControllerResult
WorkspaceAiSplineController::setSideVisibility(
    render::Device& device, WorkspaceViewport& viewport, bool showLeft,
    bool showRight,
    const WorkspaceAiSplineControllerInputSnapshot& expected) {
    if (!inputMatches(expected)) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::stale_input;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_input_stale",
            "AI spline input snapshot does not match the current "
            "controller state");
        return result;
    }
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    if (state_->configuration.showLeft == showLeft &&
        state_->configuration.showRight == showRight) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unchanged;
        return result;
    }
    if (state_->generation.publication ==
            std::numeric_limits<std::uint64_t>::max() ||
        state_->inputEpoch == std::numeric_limits<std::uint64_t>::max()) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::unsupported;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_visibility_generation_exhausted",
            "AI spline side visibility cannot advance without overflow");
        return result;
    }

    try {
        auto candidateConfiguration = state_->configuration;
        candidateConfiguration.showLeft = showLeft;
        candidateConfiguration.showRight = showRight;
        auto built = buildWorkspaceAiSplineOverlays(
            state_->session.current(),
            overlayRequest(candidateConfiguration,
                           state_->temporaryEditPoints,
                           state_->movableTemporaryPoint));
        if (!built.ok()) {
            auto result = currentResult();
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }

        auto nextGeneration = state_->generation;
        ++nextGeneration.publication;
        auto nextState = std::make_unique<State>(
            state_->session, std::move(candidateConfiguration),
            std::move(built.overlays), state_->movementForwards,
            std::move(nextGeneration), state_->inputEpoch + 1U,
            state_->editing, state_->temporaryEditPoints,
            state_->movableTemporaryPoint);
        const auto replaced = viewport.replaceAiSplineOverlays(
            device, nextState->overlays,
            WorkspaceViewportAiSplineGenerationTransition{
                state_->generation, nextState->generation});
        if (!replaced.ok()) {
            auto result = currentResult();
            result.status =
                replaced.status ==
                        WorkspaceViewportAiSplineUpdateStatus::unsupported
                    ? WorkspaceAiSplineControllerStatus::unsupported
                : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                         allocation_failed
                    ? WorkspaceAiSplineControllerStatus::allocation_failed
                    : WorkspaceAiSplineControllerStatus::viewport_failed;
            result.diagnostic = replaced.diagnostic;
            return result;
        }

        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        result.resultingInput = inputSnapshot();
        return result;
    } catch (const std::bad_alloc&) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_visibility_allocation_failed",
            "AI spline side visibility exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        auto result = currentResult();
        result.diagnostic = {
            "workspace_ai_spline_controller_visibility_failed", error.what()};
        return result;
    }
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::finishEditing(
    render::Device& device, WorkspaceViewport& viewport,
    const WorkspaceAiSplineControllerInputSnapshot& expected) {
    if (!inputMatches(expected)) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::stale_input;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_input_stale",
            "AI spline input snapshot does not match the current controller state");
        return result;
    }
    if (!viewportMatches(viewport)) return viewportBindingResult();
    if (state_->temporaryEditPoints.size() < 5U ||
        state_->configuration.selectedIndices.size() < 2U ||
        state_->configuration.selectedIndices.front() >=
            state_->configuration.selectedIndices.back()) {
        return updateEditingState(device, viewport, expected, false, false,
                                  true);
    }

    try {
        const std::size_t first =
            state_->configuration.selectedIndices.front();
        const std::size_t last =
            state_->configuration.selectedIndices.back();
        if (last >= state_->session.current().points.size()) {
            auto result = currentResult();
            result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_controller_temporary_endpoint_invalid",
                "A temporary AI spline endpoint is outside the point array");
            return result;
        }
        InstalledEditorSpline interpolating;
        interpolating.points.reserve(state_->temporaryEditPoints.size() + 2U);
        interpolating.points.push_back(
            state_->session.current().points[first].position);
        for (const auto& point : state_->temporaryEditPoints)
            interpolating.points.push_back(point.position);
        interpolating.points.push_back(
            state_->session.current().points[last].position);
        interpolating.closed = false;
        if (!recomputeInstalledEditorSplineLengths(interpolating) ||
            !validInstalledEditorSpline(interpolating, 2U)) {
            auto result = currentResult();
            result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_controller_temporary_length_invalid",
                "Temporary AI spline interpolation requires a positive finite path");
            return result;
        }
        std::vector<authoring::AiSplinePointPositionEdit> edits;
        edits.reserve(last - first);
        const float denominator = static_cast<float>(last - first);
        for (std::size_t index = first; index < last; ++index) {
            const float position =
                static_cast<float>(index - first) / denominator;
            const auto sample =
                sampleInstalledEditorSpline(interpolating, position);
            if (!sample.has_value()) {
                auto result = currentResult();
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_temporary_sample_invalid",
                    "Temporary AI spline finish produced an invalid point");
                return result;
            }
            edits.push_back({static_cast<std::uint32_t>(index), *sample});
        }
        auto candidate = state_->session;
        auto sessionResult = candidate.setPointPositions(edits);
        if (!sessionResult.ok()) {
            auto result = sessionFailure(sessionResult,
                                         state_->session.revision());
            result.resultingInput = inputSnapshot();
            return result;
        }
        if (!sessionResult.changed)
            return updateEditingState(device, viewport, expected, false,
                                      false, true);

        auto built = buildWorkspaceAiSplineOverlays(
            candidate.current(), overlayRequest(state_->configuration));
        if (!built.ok()) {
            auto result = currentResult();
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }
        if (state_->generation.publication ==
                std::numeric_limits<std::uint64_t>::max() ||
            state_->inputEpoch == std::numeric_limits<std::uint64_t>::max()) {
            auto result = currentResult();
            result.status = WorkspaceAiSplineControllerStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_controller_counter_exhausted",
                "AI spline temporary finish cannot advance its state counters");
            return result;
        }
        auto nextGeneration = state_->generation;
        nextGeneration.revision = sessionResult.revision;
        ++nextGeneration.publication;
        auto nextState = std::make_unique<State>(
            std::move(candidate), state_->configuration,
            std::move(built.overlays), state_->movementForwards,
            std::move(nextGeneration), state_->inputEpoch + 1U, false);
        const auto replaced = viewport.replaceAiSplineOverlays(
            device, nextState->overlays,
            WorkspaceViewportAiSplineGenerationTransition{
                state_->generation, nextState->generation});
        if (!replaced.ok()) {
            auto result = currentResult();
            result.status =
                replaced.status ==
                        WorkspaceViewportAiSplineUpdateStatus::unsupported
                    ? WorkspaceAiSplineControllerStatus::unsupported
                : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                         allocation_failed
                    ? WorkspaceAiSplineControllerStatus::allocation_failed
                    : WorkspaceAiSplineControllerStatus::viewport_failed;
            result.diagnostic = replaced.diagnostic;
            return result;
        }
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.revision = sessionResult.revision;
        result.applied = sessionResult.applied;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        result.resultingInput = inputSnapshot();
        return result;
    } catch (const std::bad_alloc&) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "Temporary AI spline finish exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        auto result = currentResult();
        result.diagnostic = {
            "workspace_ai_spline_controller_temporary_finish_failed",
            error.what()};
        return result;
    }
}

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::cancelEditing(
    render::Device& device, WorkspaceViewport& viewport,
    const WorkspaceAiSplineControllerInputSnapshot& expected) {
    return updateEditingState(device, viewport, expected, false, true, true);
}

WorkspaceAiSplinePointSelectionResult
WorkspaceAiSplineController::selectPoint(
    render::Device& device, WorkspaceViewport& viewport,
    const WorkspaceAiSplinePointSelectionRequest& request) {
    const auto baseResult = [&]() {
        WorkspaceAiSplinePointSelectionResult result;
        result.resultingInput = inputSnapshot();
        result.revision = state_->session.revision();
        result.selectionCount =
            state_->configuration.selectedIndices.size();
        result.temporaryPointCount = state_->temporaryEditPoints.size();
        result.movableTemporaryPoint = state_->movableTemporaryPoint;
        return result;
    };
    const auto staleResult = [&](const char* code, const char* message) {
        auto result = baseResult();
        result.status = WorkspaceAiSplineControllerStatus::stale_input;
        result.diagnostic = diagnostic(code, message);
        return result;
    };

    if (!inputMatches(request.expected)) {
        return staleResult(
            "workspace_ai_spline_controller_selection_input_stale",
            "AI spline point selection snapshot does not match the current "
            "controller state");
    }
    if (!viewportMatches(viewport)) {
        auto result = baseResult();
        result.status = WorkspaceAiSplineControllerStatus::stale_state;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_viewport_generation_mismatch",
            "AI spline viewport does not show the current controller "
            "revision");
        return result;
    }

    const std::size_t pointCount = state_->session.current().points.size();
    if (pointCount == 0U ||
        pointCount >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
        static_cast<std::size_t>(request.pointIndex) >= pointCount) {
        auto result = baseResult();
        result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_selection_index_invalid",
            "AI spline point selection index is outside the point array");
        return result;
    }
    if ((request.shiftPressed || !state_->temporaryEditPoints.empty()) &&
        !finitePosition(request.pickedPosition)) {
        auto result = baseResult();
        result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_pick_position_non_finite",
            "AI spline point selection requires a finite picked position");
        return result;
    }

    try {
        if (state_->editing && request.shiftPressed) {
            if (state_->configuration.selectedIndices.size() <= 1U) {
                auto result = baseResult();
                result.status = WorkspaceAiSplineControllerStatus::unchanged;
                return result;
            }
            if (state_->temporaryEditPoints.size() >=
                workspace_ai_spline_max_temporary_edit_points) {
                auto result = baseResult();
                result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_temporary_point_limit",
                    "Temporary AI spline edit points exceed the safety limit");
                return result;
            }
            if (state_->inputEpoch ==
                    std::numeric_limits<std::uint64_t>::max() ||
                state_->generation.publication ==
                    std::numeric_limits<std::uint64_t>::max()) {
                auto result = baseResult();
                result.status = WorkspaceAiSplineControllerStatus::unsupported;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_counter_exhausted",
                    "AI spline temporary selection cannot advance its state counters");
                return result;
            }
            auto candidateTemporaryPoints = state_->temporaryEditPoints;
            WorkspaceAiSplineTemporaryEditPoint point;
            point.forward = {
                state_->movementForwards[request.pointIndex][0U], 0.0F,
                state_->movementForwards[request.pointIndex][1U]};
            point.position = {
                request.pickedPosition[0U],
                state_->session.current().points[request.pointIndex]
                    .position[1U],
                request.pickedPosition[2U]};
            candidateTemporaryPoints.push_back(point);
            auto built = buildWorkspaceAiSplineOverlays(
                state_->session.current(),
                overlayRequest(state_->configuration,
                               candidateTemporaryPoints,
                               state_->movableTemporaryPoint));
            if (!built.ok()) {
                auto result = baseResult();
                result.status = overlayFailureStatus(built.status);
                result.diagnostic = std::move(built.diagnostic);
                return result;
            }
            auto nextState = std::make_unique<State>(
                state_->session, state_->configuration,
                std::move(built.overlays), state_->movementForwards,
                state_->generation, state_->inputEpoch + 1U, state_->editing,
                std::move(candidateTemporaryPoints),
                state_->movableTemporaryPoint);
            ++nextState->generation.publication;
            const auto replaced = viewport.replaceAiSplineOverlays(
                device, nextState->overlays,
                WorkspaceViewportAiSplineGenerationTransition{
                    state_->generation, nextState->generation});
            if (!replaced.ok()) {
                auto result = baseResult();
                result.status =
                    replaced.status ==
                            WorkspaceViewportAiSplineUpdateStatus::unsupported
                        ? WorkspaceAiSplineControllerStatus::unsupported
                    : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                             allocation_failed
                        ? WorkspaceAiSplineControllerStatus::allocation_failed
                        : WorkspaceAiSplineControllerStatus::viewport_failed;
                result.diagnostic = replaced.diagnostic;
                return result;
            }
            auto result = baseResult();
            result.status = WorkspaceAiSplineControllerStatus::ready;
            result.resultingInput = {nextState->generation,
                                     nextState->inputEpoch,
                                     nextState->editing};
            result.temporaryPointCount =
                nextState->temporaryEditPoints.size();
            result.replacedPassCount = replaced.replaced_pass_count;
            result.changed = true;
            state_.swap(nextState);
            return result;
        }

        if (state_->editing && !request.shiftPressed) {
            for (std::size_t index = 0U;
                 index < state_->temporaryEditPoints.size(); ++index) {
                if (installedEditorSplinePointDistance(
                        state_->temporaryEditPoints[index].position,
                        request.pickedPosition) < 1.0F) {
                    if (state_->movableTemporaryPoint == index) {
                        auto result = baseResult();
                        result.status =
                            WorkspaceAiSplineControllerStatus::unchanged;
                        return result;
                    }
                    if (state_->inputEpoch ==
                            std::numeric_limits<std::uint64_t>::max() ||
                        state_->generation.publication ==
                            std::numeric_limits<std::uint64_t>::max()) {
                        auto result = baseResult();
                        result.status =
                            WorkspaceAiSplineControllerStatus::unsupported;
                        result.diagnostic = diagnostic(
                            "workspace_ai_spline_controller_input_epoch_exhausted",
                            "AI spline input epoch cannot advance without overflow");
                        return result;
                    }
                    auto built = buildWorkspaceAiSplineOverlays(
                        state_->session.current(),
                        overlayRequest(state_->configuration,
                                       state_->temporaryEditPoints, index));
                    if (!built.ok()) {
                        auto result = baseResult();
                        result.status = overlayFailureStatus(built.status);
                        result.diagnostic = std::move(built.diagnostic);
                        return result;
                    }
                    auto nextState = std::make_unique<State>(
                        state_->session, state_->configuration,
                        std::move(built.overlays), state_->movementForwards,
                        state_->generation, state_->inputEpoch + 1U,
                        state_->editing, state_->temporaryEditPoints, index);
                    ++nextState->generation.publication;
                    const auto replaced = viewport.replaceAiSplineOverlays(
                        device, nextState->overlays,
                        WorkspaceViewportAiSplineGenerationTransition{
                            state_->generation, nextState->generation});
                    if (!replaced.ok()) {
                        auto result = baseResult();
                        result.status =
                            replaced.status ==
                                    WorkspaceViewportAiSplineUpdateStatus::
                                        unsupported
                                ? WorkspaceAiSplineControllerStatus::unsupported
                            : replaced.status ==
                                    WorkspaceViewportAiSplineUpdateStatus::
                                        allocation_failed
                                ? WorkspaceAiSplineControllerStatus::
                                      allocation_failed
                                : WorkspaceAiSplineControllerStatus::
                                      viewport_failed;
                        result.diagnostic = replaced.diagnostic;
                        return result;
                    }
                    auto result = baseResult();
                    result.status = WorkspaceAiSplineControllerStatus::ready;
                    result.changed = true;
                    result.resultingInput = {nextState->generation,
                                             nextState->inputEpoch,
                                             nextState->editing};
                    result.movableTemporaryPoint = index;
                    result.replacedPassCount = replaced.replaced_pass_count;
                    state_.swap(nextState);
                    return result;
                }
            }
        }

        auto candidateConfiguration = state_->configuration;
        auto& selected = candidateConfiguration.selectedIndices;
        std::unordered_set<std::uint32_t> seen;
        seen.reserve(std::min(selected.size(),
                              workspace_ai_spline_max_selection_points));
        seen.insert(selected.begin(), selected.end());
        render::Diagnostic selectionDiagnostic;
        const auto add = [&](std::uint32_t index) {
            return appendUniqueSelectionIndex(selected, seen, index,
                                              selectionDiagnostic);
        };

        if (state_->editing) {
            if (!add(request.pointIndex)) {
                auto result = baseResult();
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = std::move(selectionDiagnostic);
                return result;
            }
        } else if (!request.controlPressed || selected.empty()) {
            selected.clear();
            seen.clear();
            if (!add(request.pointIndex)) {
                auto result = baseResult();
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = std::move(selectionDiagnostic);
                return result;
            }
        } else {
            const std::uint32_t anchor = selected.back();
            if (static_cast<std::size_t>(anchor) >= pointCount) {
                auto result = baseResult();
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_selection_anchor_invalid",
                    "AI spline point selection anchor is outside the point "
                    "array");
                return result;
            }
            const std::size_t clicked = request.pointIndex;
            const std::size_t anchorIndex = anchor;
            const std::size_t forward =
                clicked >= anchorIndex
                    ? clicked - anchorIndex
                    : pointCount - (anchorIndex - clicked);
            const std::size_t reverse = pointCount - forward;
            std::size_t current =
                forward < reverse ? anchorIndex : clicked;
            const std::size_t stop =
                forward < reverse ? clicked : anchorIndex;
            while (current != stop) {
                if (!add(static_cast<std::uint32_t>(current))) {
                    auto result = baseResult();
                    result.status =
                        WorkspaceAiSplineControllerStatus::invalid_edit;
                    result.diagnostic = std::move(selectionDiagnostic);
                    return result;
                }
                current = current + 1U == pointCount ? 0U : current + 1U;
            }
            if (!add(request.pointIndex)) {
                auto result = baseResult();
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = std::move(selectionDiagnostic);
                return result;
            }
        }

        const std::uint32_t lastSelected = selected.back();
        const float normalized = static_cast<float>(lastSelected) /
                                 static_cast<float>(pointCount);
        if (selected == state_->configuration.selectedIndices &&
            !state_->movableTemporaryPoint.has_value()) {
            auto result = baseResult();
            result.status = WorkspaceAiSplineControllerStatus::unchanged;
            result.lastSelectedIndex = lastSelected;
            result.normalizedPosition = normalized;
            return result;
        }
        if (state_->inputEpoch ==
            std::numeric_limits<std::uint64_t>::max()) {
            auto result = baseResult();
            result.status = WorkspaceAiSplineControllerStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_controller_input_epoch_exhausted",
                "AI spline input epoch cannot advance without overflow");
            return result;
        }
        if (state_->generation.publication ==
            std::numeric_limits<std::uint64_t>::max()) {
            auto result = baseResult();
            result.status = WorkspaceAiSplineControllerStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_ai_spline_controller_publication_exhausted",
                "AI spline overlay publication cannot advance without "
                "overflow");
            return result;
        }
        const std::uint64_t nextInputEpoch = state_->inputEpoch + 1U;

        auto built = buildWorkspaceAiSplineOverlays(
            state_->session.current(),
            overlayRequest(candidateConfiguration,
                           state_->temporaryEditPoints));
        if (!built.ok()) {
            auto result = baseResult();
            result.status = overlayFailureStatus(built.status);
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }
        auto nextState = std::make_unique<State>(
            state_->session, std::move(candidateConfiguration),
            std::move(built.overlays), state_->movementForwards,
            state_->generation, nextInputEpoch, state_->editing,
            state_->temporaryEditPoints, std::nullopt);
        ++nextState->generation.publication;
        const auto replaced = viewport.replaceAiSplineOverlays(
            device, nextState->overlays,
            WorkspaceViewportAiSplineGenerationTransition{
                state_->generation, nextState->generation});
        if (!replaced.ok()) {
            auto result = baseResult();
            result.status =
                replaced.status ==
                        WorkspaceViewportAiSplineUpdateStatus::unsupported
                    ? WorkspaceAiSplineControllerStatus::unsupported
                : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                         allocation_failed
                    ? WorkspaceAiSplineControllerStatus::allocation_failed
                    : WorkspaceAiSplineControllerStatus::viewport_failed;
            result.diagnostic = replaced.diagnostic;
            return result;
        }

        auto result = baseResult();
        result.status = WorkspaceAiSplineControllerStatus::ready;
        result.resultingInput = {nextState->generation,
                                 nextState->inputEpoch,
                                 nextState->editing};
        result.selectionCount = nextState->configuration.selectedIndices.size();
        result.lastSelectedIndex = lastSelected;
        result.normalizedPosition = normalized;
        result.replacedPassCount = replaced.replaced_pass_count;
        result.changed = true;
        state_.swap(nextState);
        return result;
    } catch (const std::bad_alloc&) {
        auto result = baseResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_allocation_failed",
            "AI spline point selection exceeded available allocation "
            "capacity");
        return result;
    } catch (const std::exception& error) {
        auto result = baseResult();
        result.diagnostic = {
            "workspace_ai_spline_controller_selection_failed", error.what()};
        return result;
    }
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
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
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
    const WorkspaceAiSplineControllerInputSnapshot& expected) {
    if (!inputMatches(expected)) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::stale_input;
        result.diagnostic = diagnostic(
            "workspace_ai_spline_controller_input_stale",
            "AI spline input snapshot does not match the current "
            "controller state");
        return result;
    }
    if (!viewportMatches(viewport))
        return viewportBindingResult();
    try {
        const auto local = workspaceAiSplineManualLocalDelta(movement);
        if (local[0U] == 0.0F && local[1U] == 0.0F &&
            local[2U] == 0.0F) {
            auto result = currentResult();
            result.status = WorkspaceAiSplineControllerStatus::unchanged;
            return result;
        }
        if (state_->movableTemporaryPoint.has_value()) {
            const std::size_t index = *state_->movableTemporaryPoint;
            if (index >= state_->temporaryEditPoints.size()) {
                auto result = currentResult();
                result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_temporary_movable_invalid",
                    "The movable temporary AI spline point is outside the edit-point array");
                return result;
            }
            if (state_->inputEpoch ==
                    std::numeric_limits<std::uint64_t>::max() ||
                state_->generation.publication ==
                    std::numeric_limits<std::uint64_t>::max()) {
                auto result = currentResult();
                result.status = WorkspaceAiSplineControllerStatus::unsupported;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_counter_exhausted",
                    "Temporary AI spline movement cannot advance its state counters");
                return result;
            }
            const auto& point = state_->temporaryEditPoints[index];
            const float headingX = point.forward[0U];
            const float headingZ = point.forward[2U];
            const std::array<float, 3U> worldDelta = {
                -headingZ * local[0U] - headingX * local[2U], local[1U],
                headingX * local[0U] - headingZ * local[2U]};
            auto candidateTemporaryPoints = state_->temporaryEditPoints;
            for (std::size_t axis = 0U; axis < 3U; ++axis)
                candidateTemporaryPoints[index].position[axis] +=
                    worldDelta[axis];
            if (!finitePosition(candidateTemporaryPoints[index].position)) {
                auto result = currentResult();
                result.status = WorkspaceAiSplineControllerStatus::invalid_edit;
                result.diagnostic = diagnostic(
                    "workspace_ai_spline_controller_temporary_movement_non_finite",
                    "Temporary AI spline movement produced a non-finite point");
                return result;
            }
            auto built = buildWorkspaceAiSplineOverlays(
                state_->session.current(),
                overlayRequest(state_->configuration,
                               candidateTemporaryPoints, index));
            if (!built.ok()) {
                auto result = currentResult();
                result.status = overlayFailureStatus(built.status);
                result.diagnostic = std::move(built.diagnostic);
                return result;
            }
            auto nextState = std::make_unique<State>(
                state_->session, state_->configuration,
                std::move(built.overlays), state_->movementForwards,
                state_->generation, state_->inputEpoch + 1U, state_->editing,
                std::move(candidateTemporaryPoints), index);
            ++nextState->generation.publication;
            const auto replaced = viewport.replaceAiSplineOverlays(
                device, nextState->overlays,
                WorkspaceViewportAiSplineGenerationTransition{
                    state_->generation, nextState->generation});
            if (!replaced.ok()) {
                auto result = currentResult();
                result.status =
                    replaced.status ==
                            WorkspaceViewportAiSplineUpdateStatus::unsupported
                        ? WorkspaceAiSplineControllerStatus::unsupported
                    : replaced.status == WorkspaceViewportAiSplineUpdateStatus::
                                             allocation_failed
                        ? WorkspaceAiSplineControllerStatus::allocation_failed
                        : WorkspaceAiSplineControllerStatus::viewport_failed;
                result.diagnostic = replaced.diagnostic;
                return result;
            }
            auto result = currentResult();
            result.status = WorkspaceAiSplineControllerStatus::ready;
            result.replacedPassCount = replaced.replaced_pass_count;
            result.changed = true;
            state_.swap(nextState);
            result.resultingInput = inputSnapshot();
            return result;
        }
        if (state_->configuration.selectedIndices.empty()) {
            auto result = currentResult();
            result.status = WorkspaceAiSplineControllerStatus::unchanged;
            return result;
        }
        std::vector<authoring::AiSplinePointPositionEdit> edits;
        edits.reserve(state_->configuration.selectedIndices.size());
        for (const std::uint32_t selectedIndex :
             state_->configuration.selectedIndices) {
            const auto index = static_cast<std::size_t>(selectedIndex);
            if (index >= state_->session.current().points.size() ||
                index >= state_->movementForwards.size()) {
                auto result = currentResult();
                result.status =
                    WorkspaceAiSplineControllerStatus::invalid_edit;
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
        return setPointPositions(device, viewport, edits,
                                 expected.generation.revision);
    } catch (const std::bad_alloc&) {
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
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
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
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
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
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
        auto result = currentResult();
        result.status = WorkspaceAiSplineControllerStatus::allocation_failed;
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
    case WorkspaceAiSplineControllerStatus::stale_input:
        return "stale_input";
    case WorkspaceAiSplineControllerStatus::stale_state:
        return "stale_state";
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
