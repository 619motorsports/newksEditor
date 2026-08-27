#pragma once

#include "apex/app/fbx_preview_document.hpp"
#include "apex/app/workspace_ai_spline.hpp"
#include "apex/app/workspace_session.hpp"
#include "apex/render/device.hpp"
#include "apex/render/decoded_dds_texture.hpp"
#include "apex/render/directional_shadow.hpp"
#include "apex/render/external_texture_authority.hpp"
#include "apex/render/selection_axis.hpp"
#include "apex/render/skeleton_overlay.hpp"
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

// External files remain under an application-owned AssetSource grant. Viewport
// preparation resolves and copies them into an opaque effective KN5 texture
// table before any GPU resource is allocated. Neither backend retains these
// grants, requests, or paths.
struct WorkspaceViewportExternalTextureRequest {
    std::span<const render::ExternalTextureGrant> grants{};
    std::span<const render::ExternalTextureRequest> requests{};
    render::ExternalTextureAuthorityLimits limits{};
};

[[nodiscard]] WorkspaceViewportLightingResult evaluateWorkspaceViewportLighting(
    const WorkspaceViewportLightingRequest& request);

struct WorkspaceViewportAiSplineGenerationOwner final {};

struct WorkspaceViewportAiSplineGeneration {
    std::shared_ptr<const WorkspaceViewportAiSplineGenerationOwner> owner;
    std::uint64_t revision = 0U;
    // This counter identifies each visible overlay publication for one owner.
    std::uint64_t publication = 0U;

    [[nodiscard]] bool valid() const noexcept { return owner != nullptr; }
    [[nodiscard]] bool sameOwner(
        const WorkspaceViewportAiSplineGeneration& other) const noexcept {
        return valid() && other.valid() && !owner.owner_before(other.owner) &&
               !other.owner.owner_before(owner);
    }

    friend bool operator==(const WorkspaceViewportAiSplineGeneration& left,
                           const WorkspaceViewportAiSplineGeneration& right)
        noexcept {
        return left.revision == right.revision &&
               left.publication == right.publication &&
               left.sameOwner(right);
    }
};

// Opt-in portable reflection capture. This applies the recovered stock face
// parameters through the portable camera builder and generates the bounded
// RGBA16F mip chain. The previous completed cube is sampled during capture.
// The stock editor instead clears texture slot 10.
struct WorkspaceViewportPortableReflectionCaptureOptions {
    std::uint32_t size = 512U;
};

// CPU-decoded cloud textures are borrowed only during synchronous viewport
// preparation. The viewport retains backend resources, never filesystem
// paths or AssetSource authority. Null slots remain intentionally unavailable.
struct WorkspaceViewportPortableCloudOptions {
    render::PortableCloudSettings settings{};
    render::PortableCloudBuildOptions build{};
    // Per-frame weather scalars are kept beside the prepared geometry. The
    // light direction, colours, and fog distance come from frame constants.
    float cloud_cover = 0.0F;
    float cloud_cutoff = 0.0F;
    float cloud_color = 0.0F;
    std::array<const render::DecodedTexturePlan*,
               render::portable_cloud_texture_count>
        textures{};
};

// The caller evaluates CSP mesh and material selection before preparation.
// The viewport copies the generated expanded vertices and decoded atlas into
// backend-owned resources. It does not retain the input spans or file paths.
struct WorkspaceViewportPortableGrassFrameOptions {
    bool visible = true;
    float wetness = 0.0F;
    std::array<float, 2U> wind_direction = {1.0F, 0.0F};
    float wind_strength = 0.0F;
    // Presence supplies deterministic playback time. Absence uses elapsed
    // time since viewport preparation.
    std::optional<float> elapsed_seconds;
};

struct WorkspaceViewportPortableGrassOptions {
    std::span<const render::PortableGrassSourceTriangle> triangles{};
    render::PortableGrassSettings settings{};
    render::PortableGrassBuildOptions build{};
    const render::DecodedTexturePlan* atlas = nullptr;
    WorkspaceViewportPortableGrassFrameOptions frame{};
};

// Presence prepares the recovered editor skeleton pass from the immutable
// scene snapshot. The adapter maps snapshot.root to the installed graph's
// parented traversal entry without adding an inferred engine node.
struct WorkspaceViewportSkeletonOverlayOptions {
    bool visible = true;
    render::SkeletonOverlayLimits limits{};
};

struct WorkspaceViewportPrepareRequest {
    render::PresentationTargetDescription presentation{};
    // Presence enables the portable HDR scene target and the source-evidenced
    // tone-map approximation. HdrToneMapParameters::bloom carries the
    // optional five-level glare configuration. The default LDR path remains
    // unchanged.
    std::optional<render::HdrToneMapParameters> hdr_tone_map;
    // Automatic exposure measures the resolved HDR scene synchronously before
    // tone mapping. It is intentionally opt-in and has no temporal history.
    render::HdrExposureMode hdr_exposure_mode = render::HdrExposureMode::manual;
    // Draw the portable WebGL-aligned sky before retained scene geometry.
    bool sky_enabled = false;
    std::optional<WorkspaceViewportPortableCloudOptions> portable_clouds;
    std::optional<WorkspaceViewportPortableGrassOptions> portable_grass;
    std::optional<WorkspaceViewportSkeletonOverlayOptions> skeleton_overlay;
    // Apply the recovered post-tone-map FXAA pass. This requires HDR tone
    // mapping and keeps the default LDR path unchanged.
    bool fxaa_enabled = false;
    // A presentation target is single-sample. Four-sample scenes resolve to
    // an owned single-sample texture before presentation.
    std::uint32_t color_samples = 1U;
    std::span<const render::StockMaterialShaderModules> shader_modules{};
    // Explicitly select the immutable source-equivalent package only when no
    // caller module set matches a static ksPerPixel packet. This path is
    // Vulkan-only and requires retained directional-shadow resources.
    render::BuiltinVulkanStockSourceSelector builtin_vulkan_source =
        render::BuiltinVulkanStockSourceSelector::disabled;
    render::StockKsPerPixelNativeSamplerSettings
        builtin_vulkan_source_sampler_settings{};
    // Explicit installed-DXBC path for base and alpha-to-coverage ksPerPixel
    // D3D12 draws. The validated owner table is borrowed only during
    // preparation; retained scene resources clone each selected packet.
    render::BuiltinD3D12StockNativeSelector builtin_d3d12_native =
        render::BuiltinD3D12StockNativeSelector::disabled;
    std::span<const render::StockMaterialD3D12NativeProgram>
        builtin_d3d12_native_programs{};
    render::StockKsPerPixelNativeSamplerSettings
        builtin_d3d12_native_sampler_settings{};
    std::span<const render::MaterialBindingOverrides> overrides_by_material{};
    // Presence requires at least one external request. The matching file
    // overrides are consumed into owned embedded payloads before render
    // preparation; unrelated overrides remain active.
    std::optional<WorkspaceViewportExternalTextureRequest> external_textures;
    render::RenderPlanOptions render{};
    // Apply the recovered active CameraMeshFilter geometry path on each frame.
    // Packets without a recovered KN5 local sphere remain conservatively visible.
    bool camera_mesh_filter = false;
    std::uint32_t camera_mesh_max_layer = 5U;
    // Recompute the retained browser viewport's color order from the current
    // camera and transformed vertex-AABB centers on every frame. This is an
    // explicit WebGL compatibility mode; the recovered editor uses traversal
    // order for its Classic transparent pass.
    bool webgl_live_transparent_order = false;
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
    // A controller supplies its owner and model revision here. Geometry-only
    // callers leave this empty and use untracked replacement calls.
    std::optional<WorkspaceViewportAiSplineGeneration> ai_spline_generation;
    // Optional recovered interpolated in/out overlay. This is a second blue,
    // depth-off pass and must be supplied with the primary spline pass.
    const WorkspaceAiSplineGeometry* ai_spline_interval_geometry = nullptr;
    std::optional<render::PipelineProgram> ai_spline_interval_pipeline;
    // Optional recovered raw side splines. A controller can retain each
    // pipeline without geometry for an independently hidden side. These
    // passes follow the interval and use the primary normal-depth contract.
    const WorkspaceAiSplineGeometry* ai_spline_left_geometry = nullptr;
    std::optional<render::PipelineProgram> ai_spline_left_pipeline;
    const WorkspaceAiSplineGeometry* ai_spline_right_geometry = nullptr;
    std::optional<render::PipelineProgram> ai_spline_right_pipeline;
    // Optional recovered selected-index markers. A controller can supply the
    // pipeline without geometry to retain an empty interactive selection pass.
    // This normal-depth pass follows the side splines and requires the primary.
    const WorkspaceAiSplineGeometry* ai_spline_selection_geometry = nullptr;
    std::optional<render::PipelineProgram> ai_spline_selection_pipeline;
    // The temporary spline and portable point-marker passes are latent for a
    // controller. Their pipelines remain prepared while edit state is empty.
    const WorkspaceAiSplineGeometry*
        ai_spline_temporary_interpolation_geometry = nullptr;
    std::optional<render::PipelineProgram>
        ai_spline_temporary_interpolation_pipeline;
    const WorkspaceAiSplineGeometry* ai_spline_temporary_marker_geometry =
        nullptr;
    std::optional<render::PipelineProgram> ai_spline_temporary_marker_pipeline;
    const WorkspaceAiSplineGeometry* ai_spline_camber_geometry = nullptr;
    std::optional<render::PipelineProgram> ai_spline_camber_pipeline;
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
    // Select the portable reflection-capable module variants for the four
    // bounded MultiMap families. Each frame must then supply the renderer-
    // owned cube and sampler through WorkspaceViewportFrameRequest.
    bool multimap_reflection = false;
    // When present, the viewport owns and refreshes the portable reflection
    // binding. Caller-supplied frame bindings are then rejected.
    std::optional<WorkspaceViewportPortableReflectionCaptureOptions>
        portable_reflection_capture;
    std::optional<WorkspaceViewportDirectionalShadowOptions> directional_shadows;
    render::StockSceneExecutionLimits limits{};
    workspace::WorkspaceSceneLimits workspace_scene_limits{};
    WorkspaceViewportWorkspaceOptions workspace{};
};

struct WorkspaceViewportStockNativeFrame {
    // Exact recovered b0 and b2 host records. The viewport derives b3 and all
    // three map bindings from its retained, freshly rendered cascades so a
    // caller cannot substitute unrelated GPU resources.
    render::StockKsPerPixelCameraConstants camera{};
    render::StockKsPerPixelLightingConstants lighting{};
};

using WorkspaceViewportStockVulkanSourceFrame =
    WorkspaceViewportStockNativeFrame;
using WorkspaceViewportStockD3D12NativeFrame =
    WorkspaceViewportStockNativeFrame;

// Maps evaluated stock weather into the recovered b2 layout. The screen
// fields are reciprocal active-viewport dimensions, matching setViewport.
[[nodiscard]] std::optional<render::StockKsPerPixelLightingConstants>
buildWorkspaceViewportStockVulkanSourceLighting(
    const render::EvaluatedLighting& lighting, std::uint32_t viewport_width,
    std::uint32_t viewport_height) noexcept;

// Builds the recovered native b0 record from the current Vulkan camera and
// preserves an already-authored b2 record. The checked inverse prevents a
// malformed camera from reaching the source-equivalent shader ABI.
[[nodiscard]] std::optional<WorkspaceViewportStockVulkanSourceFrame>
buildWorkspaceViewportStockVulkanSourceFrame(
    const render::CameraFrame& camera,
    const render::StockKsPerPixelLightingConstants& lighting) noexcept;

// Build the same recovered b0/b2 records for a D3D12 camera. Backend
// authority remains separate in WorkspaceViewportFrameRequest.
[[nodiscard]] std::optional<WorkspaceViewportStockD3D12NativeFrame>
buildWorkspaceViewportStockD3D12NativeFrame(
    const render::CameraFrame& camera,
    const render::StockKsPerPixelLightingConstants& lighting) noexcept;

struct WorkspaceViewportFrameRequest {
    render::CameraFrame camera{};
    // Override the prepared tone-map and optional bloom values for this frame.
    // An LDR viewport rejects this value because it has no HDR scene target.
    std::optional<render::HdrToneMapParameters> hdr_tone_map;
    // Override the prepared exposure mode for this frame. Automatic exposure
    // requires a prepared HDR scene and performs one synchronous measurement.
    std::optional<render::HdrExposureMode> hdr_exposure_mode;
    bool load_color = false;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    bool clear_depth = true;
    float depth_clear_value = 1.0F;
    std::span<const render::DrawPacket> refreshed_packets{};
    // Optional stable prepared-packet visibility mask. See the static-scene
    // frame contract for count and value requirements. A nonempty mask is
    // authoritative. An empty mask lets a prepared car-LOD catalog derive
    // color visibility from the frame camera.
    std::span<const std::uint8_t> packet_visibility{};
    // Optional independent Shadowgen mask in prepared packet order. A
    // nonempty value is authoritative for shadows. When this span is empty,
    // a nonempty packet_visibility mask keeps the former shared-mask behavior.
    // When both spans are empty, the prepared catalog derives each pass mask.
    std::span<const std::uint8_t> shadow_packet_visibility{};
    // Optional complete Shadowgen permutation in prepared packet-index space.
    // Empty uses the retained source traversal from viewport preparation.
    std::span<const std::uint32_t> shadow_packet_order{};
    bool apply_skinning = false;
    std::optional<render::KsPerPixelFrameConstants> frame_constants;
    // Frame-owned renderer cube. It is deliberately separate from KN5
    // texture authority and required exactly when reflection was prepared.
    render::IndexedSampledTextureBinding multimap_reflection_cube{};
    // Required exactly when preparation selected at least one immutable
    // Vulkan source program. Portable constants are never reinterpreted as
    // this recovered native ABI.
    std::optional<WorkspaceViewportStockVulkanSourceFrame>
        stock_vulkan_source_frame;
    // Required exactly when preparation selects installed D3D12 native
    // owners. The viewport supplies its retained shadow maps after refresh.
    std::optional<WorkspaceViewportStockD3D12NativeFrame>
        stock_d3d12_native_frame;
    // Override the prepared grid state. A true value requires grid resources
    // to have been requested during viewport preparation.
    std::optional<bool> grid_visible;
    // Override the prepared view-axis state. A true value requires view-axis
    // resources to have been requested during viewport preparation.
    std::optional<bool> view_axis_visible;
    // Override the prepared skeleton state. A true value requires retained
    // skeleton geometry and the authoring overlay pipeline.
    std::optional<bool> skeleton_overlay_visible;
    // Override the prepared selected-node world transform for an animated
    // frame. Supplying this without a prepared selection axis is invalid.
    std::optional<apex::scene::Matrix4> selection_axis_world;
    // Override the prepared portable-grass weather and wind values. This is
    // invalid when the viewport did not prepare portable grass.
    std::optional<WorkspaceViewportPortableGrassFrameOptions> portable_grass;
    // Override the elapsed selection time for deterministic playback/tests.
    // When absent, the viewport uses time since its preparation completed.
    std::optional<std::uint32_t> selected_mesh_elapsed_ms;
};

enum class WorkspaceViewportAiSplineUpdateStatus : std::uint8_t {
    ready,
    invalid,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct WorkspaceViewportAiSplineUpdateResult {
    WorkspaceViewportAiSplineUpdateStatus status =
        WorkspaceViewportAiSplineUpdateStatus::invalid;
    render::Diagnostic diagnostic;
    std::size_t replaced_pass_count = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceViewportAiSplineUpdateStatus::ready;
    }
};

struct WorkspaceViewportAiSplineGenerationTransition {
    WorkspaceViewportAiSplineGeneration expected;
    WorkspaceViewportAiSplineGeneration replacement;
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
    // This scalar is for status output only. Binding checks must use the
    // complete owner-and-revision identity below.
    [[nodiscard]] std::optional<std::uint64_t>
    aiSplineRevision() const noexcept {
        return ai_spline_generation_.has_value()
                   ? std::optional<std::uint64_t>(
                         ai_spline_generation_->revision)
                   : std::nullopt;
    }
    [[nodiscard]] const std::optional<WorkspaceViewportAiSplineGeneration>&
    aiSplineGenerationIdentity() const noexcept {
        return ai_spline_generation_;
    }

    // Draw is synchronous. If draw fails, present_texture is not called.
    // The target must have the dimensions and format supplied at preparation.
    [[nodiscard]] WorkspaceViewportFrameStatus drawAndPresent(
        render::Device& device, render::PresentationTarget& target,
        const WorkspaceViewportFrameRequest& request,
        render::Diagnostic& diagnostic);

    // Allocate every replacement buffer before changing retained resources.
    // A failed call leaves every previously visible pass unchanged.
    // Call this synchronous operation on the render thread, between draws.
    [[nodiscard]] WorkspaceViewportAiSplineUpdateResult
    replaceAiSplineOverlays(render::Device& device,
                            const WorkspaceAiSplineOverlaySet& overlays,
                            std::optional<
                                WorkspaceViewportAiSplineGenerationTransition>
                                generation = std::nullopt);

private:
    struct AiSplineUpdateRequest {
        const WorkspaceAiSplineGeometry* primary = nullptr;
        const WorkspaceAiSplineGeometry* interval = nullptr;
        const WorkspaceAiSplineGeometry* left = nullptr;
        const WorkspaceAiSplineGeometry* right = nullptr;
        const WorkspaceAiSplineGeometry* selection = nullptr;
        const WorkspaceAiSplineGeometry* temporaryInterpolation = nullptr;
        const WorkspaceAiSplineGeometry* temporaryMarkers = nullptr;
        const WorkspaceAiSplineGeometry* camber = nullptr;
    };

    struct FrameCatalog {
        struct ColorOrderPacket {
            apex::scene::Vector3 local_aabb_center{};
        };

        bool workspace_lod = false;
        apex::scene::Vector3 bounds_center{};
        std::optional<std::uint32_t> selected_index;
        float fov_degrees = 45.0F;
        float distance_divisor = 1.0F;
        bool track_camera = false;
        std::vector<std::optional<workspace::CarLodManifest>> file_lods;
        std::vector<std::size_t> file_for_packet;
        std::vector<std::optional<render::CameraMeshRenderable>> mesh_filters;
        std::uint32_t max_layer = 5U;
        std::vector<std::uint8_t> frame_color_visibility;
        std::vector<std::uint8_t> frame_shadow_visibility;
        bool webgl_live_transparent_order = false;
        std::vector<ColorOrderPacket> color_order_packets;
        std::vector<std::uint32_t> frame_color_order;
        std::vector<double> frame_color_distance_squared;
    };

    struct AiSplinePassResources {
        std::optional<render::PipelineProgram> pipeline;
        std::unique_ptr<render::Buffer> buffer;
        std::vector<WorkspaceAiSplineChunk> chunks;
    };

    struct PortableReflectionCaptureResources {
        std::array<std::unique_ptr<render::Texture>, 2U> cubes;
        std::unique_ptr<render::Texture> black_cube;
        std::unique_ptr<render::Sampler> sampler;
        std::unique_ptr<render::DepthAttachment> depth;
        std::vector<std::uint8_t> packet_visibility;
        std::optional<std::size_t> published_cube;
        std::size_t write_cube = 0U;
    };

    struct PortableCloudResources {
        render::PortableCloudSettings settings{};
        float cloud_cover = 0.0F;
        float cloud_cutoff = 0.0F;
        float cloud_color = 0.0F;
        std::vector<render::PortableCloudTextureRun> texture_runs;
        std::array<std::unique_ptr<render::Texture>,
                   render::portable_cloud_texture_count>
            textures;
        std::unique_ptr<render::Buffer> vertex_buffer;
        std::unique_ptr<render::Sampler> sampler;
        std::chrono::steady_clock::time_point start_time{};
    };

    struct PortableGrassResources {
        WorkspaceViewportPortableGrassFrameOptions frame{};
        std::unique_ptr<render::Buffer> vertex_buffer;
        std::uint32_t vertex_count = 0U;
        std::unique_ptr<render::Texture> atlas;
        std::unique_ptr<render::Sampler> sampler;
        std::chrono::steady_clock::time_point start_time{};
    };

    struct SkeletonOverlayResources {
        bool visible = true;
        std::unique_ptr<render::Buffer> vertex_buffer;
        std::uint32_t vertex_count = 0U;
    };

    [[nodiscard]] WorkspaceViewportAiSplineUpdateResult
    replaceAiSplineOverlaysBorrowed(render::Device& device,
                                    const AiSplineUpdateRequest& request,
                                    std::optional<
                                        WorkspaceViewportAiSplineGenerationTransition>
                                        generation);

    WorkspaceViewport(
        render::Device* device, render::Backend backend,
        render::PresentationTargetDescription presentation,
        std::unique_ptr<render::Texture> color,
        std::unique_ptr<render::Texture> resolved_color,
        std::unique_ptr<render::Texture> tone_mapped_color,
        std::unique_ptr<render::Texture> fxaa_color,
        std::optional<render::HdrToneMapParameters> hdr_tone_map,
        render::HdrExposureMode hdr_exposure_mode,
        bool sky_enabled,
        bool fxaa_enabled,
        std::unique_ptr<render::DepthAttachment> depth,
        std::unique_ptr<render::StockSceneExecutionResult> execution,
        std::optional<render::PipelineProgram> authoring_overlay_pipeline,
        std::array<AiSplinePassResources, workspace_ai_spline_pass_count>
            ai_spline_passes,
        std::optional<WorkspaceViewportAiSplineGeneration>
            ai_spline_generation,
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
        std::optional<PortableCloudResources> portable_clouds,
        std::optional<PortableGrassResources> portable_grass,
        std::optional<SkeletonOverlayResources> skeleton_overlay,
        std::optional<PortableReflectionCaptureResources> reflection_capture,
        std::optional<FrameCatalog> frame_catalog);

    render::Device* device_ = nullptr;
    render::Backend backend_ = render::Backend::Vulkan;
    render::PresentationTargetDescription presentation_{};
    std::unique_ptr<render::Texture> color_;
    std::unique_ptr<render::Texture> resolved_color_;
    std::unique_ptr<render::Texture> tone_mapped_color_;
    std::unique_ptr<render::Texture> fxaa_color_;
    std::optional<render::HdrToneMapParameters> hdr_tone_map_;
    render::HdrExposureMode hdr_exposure_mode_ = render::HdrExposureMode::manual;
    bool sky_enabled_ = false;
    bool fxaa_enabled_ = false;
    std::unique_ptr<render::DepthAttachment> depth_;
    std::unique_ptr<render::StockSceneExecutionResult> execution_;
    std::optional<render::PipelineProgram> authoring_overlay_pipeline_;
    std::array<AiSplinePassResources, workspace_ai_spline_pass_count>
        ai_spline_passes_;
    std::optional<WorkspaceViewportAiSplineGeneration> ai_spline_generation_;
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
    std::optional<PortableCloudResources> portable_clouds_;
    std::optional<PortableGrassResources> portable_grass_;
    std::optional<SkeletonOverlayResources> skeleton_overlay_;
    std::optional<PortableReflectionCaptureResources> reflection_capture_;
    std::optional<FrameCatalog> frame_catalog_;

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

// This overload enforces the FBX staging gate before any backend resource is
// created. Callers cannot accidentally preview an incomplete FBX document.
[[nodiscard]] WorkspaceViewportPrepareResult prepareWorkspaceViewport(
    render::Device& device, const FbxPreviewDocumentResult& document,
    const WorkspaceViewportPrepareRequest& request);

[[nodiscard]] const char* workspace_viewport_status_name(
    WorkspaceViewportStatus status) noexcept;

[[nodiscard]] const char* workspace_viewport_frame_status_name(
    WorkspaceViewportFrameStatus status) noexcept;

[[nodiscard]] const char* workspace_viewport_ai_spline_update_status_name(
    WorkspaceViewportAiSplineUpdateStatus status) noexcept;

}  // namespace apex::app
