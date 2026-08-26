#include "apex/app/workspace_ai_spline_controller.hpp"

#include "apex/formats/ai_spline_write.hpp"

#include <algorithm>
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

} // namespace

struct WorkspaceAiSplineController::State {
    authoring::AiSplineSession session;
    WorkspaceAiSplineControllerConfiguration configuration;
    WorkspaceAiSplineOverlaySet overlays;

    State(authoring::AiSplineSession candidateSession,
          WorkspaceAiSplineControllerConfiguration candidateConfiguration,
          WorkspaceAiSplineOverlaySet candidateOverlays)
        : session(std::move(candidateSession)),
          configuration(std::move(candidateConfiguration)),
          overlays(std::move(candidateOverlays)) {}
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
        auto state = std::make_unique<State>(
            std::move(session), std::move(configuration),
            std::move(built.overlays));
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
        auto nextState = std::make_unique<State>(
            std::move(candidate), state_->configuration,
            std::move(built.overlays));
        const auto replaced =
            viewport.replaceAiSplineOverlays(
                device, nextState->overlays,
                WorkspaceViewportAiSplineGenerationTransition{
                    state_->session.revision(), sessionResult.revision});
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
WorkspaceAiSplineController::setPointPositions(
    render::Device& device, WorkspaceViewport& viewport,
    std::span<const authoring::AiSplinePointPositionEdit> edits,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (viewport.aiSplineGeneration() != state_->session.revision())
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

WorkspaceAiSplineControllerResult WorkspaceAiSplineController::undo(
    render::Device& device, WorkspaceViewport& viewport,
    std::uint64_t expectedRevision) {
    if (expectedRevision != state_->session.revision())
        return staleResult();
    if (viewport.aiSplineGeneration() != state_->session.revision())
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
    if (viewport.aiSplineGeneration() != state_->session.revision())
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
    if (viewport.aiSplineGeneration() != state_->session.revision())
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
