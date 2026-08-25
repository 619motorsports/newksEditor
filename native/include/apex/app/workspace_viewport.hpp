#pragma once

#include "apex/app/workspace_ai_spline.hpp"
#include "apex/app/workspace_session.hpp"
#include "apex/render/device.hpp"
#include "apex/render/directional_shadow.hpp"
#include "apex/render/selection_axis.hpp"
#include "apex/render/stock_scene_execution.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// Presence of this record enables the application shadow schedule. Programs
// remain explicit because the viewport cannot infer a depth ABI from material
// shader bytecode. Missing optional programs keep those caster branches staged.
struct WorkspaceViewportDirectionalShadowOptions {
    render::DirectionalShadowMapRequest maps{};
    std::optional<render::PipelineProgram> opaque_pipeline;
    std::optional<render::PipelineProgram> alpha_static_pipeline;
    std::optional<render::PipelineProgram> skinned_pipeline;
    render::DirectionalShadowReceiverConstantsLayout constants_layout =
        render::DirectionalShadowReceiverConstantsLayout::portable;
};

inline constexpr float workspace_viewport_default_sun_heading_degrees = 40.0F;
inline constexpr float workspace_viewport_default_sun_height_degrees = 55.0F;

struct WorkspaceViewportLightingRequest {
    // Empty selects the production default weather preset.
    std::string_view weather_id{};
    float sun_heading_degrees = workspace_viewport_default_sun_heading_degrees;
    float sun_height_degrees = workspace_viewport_default_sun_height_degrees;
};

enum class WorkspaceViewportLightingStatus : std::uint8_t {
    ready,
    invalid,
    allocation_failed,
};

struct WorkspaceViewportLightingResult {
    WorkspaceViewportLightingStatus status = WorkspaceViewportLightingStatus::invalid;
    render::Diagnostic diagnostic;
    render::EvaluatedLighting evaluated;
    render::KsPerPixelFrameConstants frame_constants{};

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceViewportLightingStatus::ready;
    }
};

[[nodiscard]] WorkspaceViewportLightingResult evaluateWorkspaceViewportLighting(
    const WorkspaceViewportLightingRequest& request);

struct WorkspaceViewportPrepareRequest {
    render::PresentationTargetDescription presentation{};
    // A presentation target is single-sample. Four-sample scenes resolve to
    // an owned single-sample texture before presentation.
    std::uint32_t color_samples = 1U;
    std::span<const render::StockMaterialShaderModules> shader_modules{};
    std::span<const render::MaterialBindingOverrides> overrides_by_material{};
    render::RenderPlanOptions render{};
    render::DrawPacketOptions packets{};
    bool evaluate_damage_preview = false;
    std::optional<bool> damage_broken_visible;
    bool wireframe = false;
    // The view axis, grid, and selected-node marker share this fixed
    // position/color line contract. Selection can omit its marker.
    std::optional<render::PipelineProgram> authoring_overlay_pipeline;
    // Optional recovered raw or interpolated SplineEditor pass. The geometry
    // is copied to an immutable buffer during preparation and is not retained
    // by reference.
    const WorkspaceAiSplineGeometry* ai_spline_geometry = nullptr;
    std::optional<render::PipelineProgram> ai_spline_pipeline;
    // Optional recovered magenta selected-mesh pass. The selected packet must
    // be static. This pipeline is independent of the line-overlay pipeline.
    std::optional<render::PipelineProgram> selected_mesh_pipeline;
    // Prepare and show the recovered 10 m authoring grid. The native default
    // is false. A true value requires authoring_overlay_pipeline.
    bool grid_visible = false;
    // Prepare and show the recovered one-meter world-origin view axis. The
    // native default is false. A true value requires the overlay pipeline.
    bool view_axis_visible = false;
    // Keep this explicit receiver-module selector for existing callers. A
    // true value requires directional_shadows so the viewport cannot prepare
    // a receiver that has no maps or caster schedule.
    bool directional_shadow_receiver = false;
    std::optional<WorkspaceViewportDirectionalShadowOptions> directional_shadows;
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
    // Optional stable prepared-packet visibility mask. See the static-scene
    // frame contract for count and value requirements. A nonempty mask is
    // authoritative. An empty mask lets a prepared car-LOD catalog derive
    // visibility from the frame camera.
    std::span<const std::uint8_t> packet_visibility{};
    bool apply_skinning = false;
    std::optional<render::KsPerPixelFrameConstants> frame_constants;
    // Override the prepared grid state. A true value requires grid resources
    // to have been requested during viewport preparation.
    std::optional<bool> grid_visible;
    // Override the prepared view-axis state. A true value requires view-axis
    // resources to have been requested during viewport preparation.
    std::optional<bool> view_axis_visible;
    // Override the prepared selected-node world transform for an animated
    // frame. Supplying this without a prepared selection axis is invalid.
    std::optional<apex::scene::Matrix4> selection_axis_world;
    // Override the elapsed selection time for deterministic playback/tests.
    // When absent, the viewport uses time since its preparation completed.
    std::optional<std::uint32_t> selected_mesh_elapsed_ms;
};

// The editor's browser viewport uses an orbit target, yaw/pitch, and distance
// state. This controller keeps that state in the application layer so SDL,
// Vulkan, and D3D12 code do not acquire camera-gesture semantics.
enum class WorkspaceViewportCameraGesture : std::uint8_t {
    begin_orbit,
    begin_pan,
    end_drag,
    drag,
    wheel,
};

struct WorkspaceViewportCameraInput {
    WorkspaceViewportCameraGesture gesture =
        WorkspaceViewportCameraGesture::end_drag;
    float x_delta = 0.0F;
    float y_delta = 0.0F;
};

enum class WorkspaceViewportCameraMove : std::uint8_t {
    forward,
    backward,
    left,
    right,
    up,
    down,
};

class WorkspaceViewportCameraController final {
public:
    // These defaults match the browser editor's initial orbit state.
    apex::scene::Vector3 target = {0.0F, 0.0F, 0.0F};
    float yaw = 0.7F;
    float pitch = 0.35F;
    float distance = 5.0F;

    [[nodiscard]] bool apply(const WorkspaceViewportCameraInput& input) noexcept;
    // Translate the orbit target and camera together. This keeps keyboard
    // motion independent of the graphics backend and preserves the current
    // orbit orientation and distance.
    [[nodiscard]] bool move(WorkspaceViewportCameraMove direction,
                            float distance = 0.25F) noexcept;
    [[nodiscard]] render::CameraFrameResult frame(
        float aspect, render::CameraClipSpace clip_space) const;

private:
    bool dragging_ = false;
    bool panning_ = false;
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
    struct LodCatalog {
        apex::scene::Vector3 bounds_center{};
        std::optional<std::uint32_t> selected_index;
        float fov_degrees = 45.0F;
        float distance_divisor = 1.0F;
        bool track_camera = false;
        std::vector<std::optional<workspace::CarLodManifest>> file_lods;
        std::vector<std::size_t> file_for_packet;
        std::vector<std::uint8_t> frame_visibility;
    };

    WorkspaceViewport(
        render::Backend backend,
        render::PresentationTargetDescription presentation,
        std::unique_ptr<render::Texture> color,
        std::unique_ptr<render::Texture> resolved_color,
        std::unique_ptr<render::DepthAttachment> depth,
        std::unique_ptr<render::StockSceneExecutionResult> execution,
        std::optional<render::PipelineProgram> authoring_overlay_pipeline,
        std::optional<render::PipelineProgram> ai_spline_pipeline,
        std::unique_ptr<render::Buffer> ai_spline_buffer,
        std::vector<WorkspaceAiSplineChunk> ai_spline_chunks,
        std::unique_ptr<render::Buffer> authoring_grid_buffer,
        bool grid_visible,
        std::unique_ptr<render::Buffer> view_axis_buffer,
        bool view_axis_visible,
        std::unique_ptr<render::Buffer> selection_axis_buffer,
        std::optional<apex::scene::Matrix4> selection_axis_world,
        std::optional<render::PipelineProgram> selected_mesh_pipeline,
        std::unique_ptr<render::Buffer> selected_mesh_color_buffer,
        std::unique_ptr<render::DirectionalShadowMapResources> shadow_maps,
        std::optional<WorkspaceViewportDirectionalShadowOptions> directional_shadows,
        std::optional<LodCatalog> lod_catalog);

    render::Backend backend_ = render::Backend::Vulkan;
    render::PresentationTargetDescription presentation_{};
    std::unique_ptr<render::Texture> color_;
    std::unique_ptr<render::Texture> resolved_color_;
    std::unique_ptr<render::DepthAttachment> depth_;
    std::unique_ptr<render::StockSceneExecutionResult> execution_;
    std::optional<render::PipelineProgram> authoring_overlay_pipeline_;
    std::optional<render::PipelineProgram> ai_spline_pipeline_;
    std::unique_ptr<render::Buffer> ai_spline_buffer_;
    std::vector<WorkspaceAiSplineChunk> ai_spline_chunks_;
    std::unique_ptr<render::Buffer> authoring_grid_buffer_;
    bool grid_visible_ = false;
    std::unique_ptr<render::Buffer> view_axis_buffer_;
    bool view_axis_visible_ = false;
    std::unique_ptr<render::Buffer> selection_axis_buffer_;
    std::optional<apex::scene::Matrix4> selection_axis_world_;
    std::optional<render::PipelineProgram> selected_mesh_pipeline_;
    std::unique_ptr<render::Buffer> selected_mesh_color_buffer_;
    std::chrono::steady_clock::time_point selected_mesh_touch_time_ =
        std::chrono::steady_clock::now();
    std::unique_ptr<render::DirectionalShadowMapResources> shadow_maps_;
    std::optional<WorkspaceViewportDirectionalShadowOptions> directional_shadows_;
    std::optional<LodCatalog> lod_catalog_;

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
