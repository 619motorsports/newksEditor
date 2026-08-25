#pragma once

#include "apex/app/workspace_session.hpp"
#include "apex/render/device.hpp"
#include "apex/render/stock_scene_execution.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace apex::app {

struct WorkspaceViewportPrepareResult;

enum class WorkspaceViewportStatus : std::uint8_t {
    ready,
    invalid,
    unsupported,
    allocation_failed,
};

enum class WorkspaceViewportFrameStatus : std::uint8_t {
    ready,
    invalid,
    unsupported,
    execution_failed,
};

struct WorkspaceViewportWorkspaceOptions {
    // LOD resolution needs the same production preview AABB center that the
    // browser controller supplies. The adapter does not invent one from the
    // scene radius.
    std::optional<apex::scene::Vector3> lod_bounds_center;
    std::optional<std::uint32_t> lod_index;
    float lod_fov_degrees = 45.0F;
    float lod_distance_divisor = 1.0F;
    bool lod_track_camera = false;

    std::optional<bool> cockpit_high_visible;
    std::optional<bool> blurred_rims_visible;
    bool driver_cockpit = false;
    std::span<const std::string> driver_hidden_names{};
};

struct WorkspaceViewportPrepareRequest {
    render::PresentationTargetDescription presentation{};
    // A presentation target is single-sample. This field is explicit so a
    // caller cannot accidentally request an unresolved multisample scene.
    std::uint32_t color_samples = 1U;
    std::span<const render::StockMaterialShaderModules> shader_modules{};
    std::span<const render::MaterialBindingOverrides> overrides_by_material{};
    render::RenderPlanOptions render{};
    render::DrawPacketOptions packets{};
    bool evaluate_damage_preview = false;
    std::optional<bool> damage_broken_visible;
    bool wireframe = false;
    bool directional_shadow_receiver = false;
    render::StockSceneExecutionLimits limits{};
    workspace::WorkspaceSceneLimits workspace_scene_limits{};
    WorkspaceViewportWorkspaceOptions workspace{};
};

struct WorkspaceViewportFrameRequest {
    render::CameraFrame camera{};
    bool load_color = false;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    bool clear_depth = true;
    float depth_clear_value = 1.0F;
    std::span<const render::DrawPacket> refreshed_packets{};
    bool apply_skinning = false;
    std::optional<render::KsPerPixelFrameConstants> frame_constants;
};

class WorkspaceViewport final {
public:
    WorkspaceViewport(const WorkspaceViewport&) = delete;
    WorkspaceViewport& operator=(const WorkspaceViewport&) = delete;
    WorkspaceViewport(WorkspaceViewport&&) = delete;
    WorkspaceViewport& operator=(WorkspaceViewport&&) = delete;
    ~WorkspaceViewport();

    [[nodiscard]] render::Backend backend() const noexcept { return backend_; }
    [[nodiscard]] const render::RenderPlan& renderPlan() const noexcept {
        return execution_->render_plan;
    }
    [[nodiscard]] const render::StockSceneExecutionResult& preparation() const noexcept {
        return *execution_;
    }

    // Draw is synchronous. If draw fails, present_texture is not called.
    // The target must have the dimensions and format supplied at preparation.
    [[nodiscard]] WorkspaceViewportFrameStatus drawAndPresent(
        render::Device& device, render::PresentationTarget& target,
        const WorkspaceViewportFrameRequest& request,
        render::Diagnostic& diagnostic);

private:
    WorkspaceViewport(
        render::Backend backend,
        render::PresentationTargetDescription presentation,
        std::unique_ptr<render::Texture> color,
        std::unique_ptr<render::DepthAttachment> depth,
        std::unique_ptr<render::StockSceneExecutionResult> execution);

    render::Backend backend_ = render::Backend::Vulkan;
    render::PresentationTargetDescription presentation_{};
    std::unique_ptr<render::Texture> color_;
    std::unique_ptr<render::DepthAttachment> depth_;
    std::unique_ptr<render::StockSceneExecutionResult> execution_;

    friend struct WorkspaceViewportPrepareResult;
    friend WorkspaceViewportPrepareResult prepareWorkspaceViewport(
        render::Device&, const WorkspaceSessionDocument&,
        const WorkspaceViewportPrepareRequest&);
};

struct WorkspaceViewportPrepareResult {
    WorkspaceViewportStatus status = WorkspaceViewportStatus::unsupported;
    render::Diagnostic diagnostic;
    std::unique_ptr<WorkspaceViewport> viewport;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceViewportStatus::ready && viewport != nullptr;
    }
};

[[nodiscard]] WorkspaceViewportPrepareResult prepareWorkspaceViewport(
    render::Device& device, const WorkspaceSessionDocument& document,
    const WorkspaceViewportPrepareRequest& request);

[[nodiscard]] const char* workspace_viewport_status_name(
    WorkspaceViewportStatus status) noexcept;

[[nodiscard]] const char* workspace_viewport_frame_status_name(
    WorkspaceViewportFrameStatus status) noexcept;

}  // namespace apex::app
