#include "apex/app/workspace_viewport.hpp"

#include "apex/core/parse_error.hpp"
#include "apex/workspace/workspace_scene.hpp"

#include <cmath>
#include <new>
#include <utility>
#include <vector>

namespace apex::app {
namespace {

using render::PipelineRenderTarget;
using render::PipelineRenderTargetFormat;

[[nodiscard]] render::Diagnostic diagnostic(const char* code, const char* message) {
    return {code, message};
}

[[nodiscard]] std::optional<PipelineRenderTargetFormat> pipelineColorFormat(
    render::TextureFormat format) noexcept {
    switch (format) {
    case render::TextureFormat::rgba8_unorm:
        return PipelineRenderTargetFormat::rgba8_unorm;
    case render::TextureFormat::rgba8_srgb:
        return PipelineRenderTargetFormat::rgba8_srgb;
    case render::TextureFormat::bgra8_unorm:
        return PipelineRenderTargetFormat::bgra8_unorm;
    case render::TextureFormat::bgra8_srgb:
        return PipelineRenderTargetFormat::bgra8_srgb;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] render::CameraClipSpace expectedClipSpace(render::Backend backend) noexcept {
    return backend == render::Backend::Vulkan ? render::CameraClipSpace::vulkan
                                               : render::CameraClipSpace::d3d12;
}

[[nodiscard]] WorkspaceViewportStatus preparationStatus(
    render::StaticSceneResourceStatus status) noexcept {
    switch (status) {
    case render::StaticSceneResourceStatus::ready:
        return WorkspaceViewportStatus::ready;
    case render::StaticSceneResourceStatus::unsupported:
        return WorkspaceViewportStatus::unsupported;
    case render::StaticSceneResourceStatus::allocation_failed:
        return WorkspaceViewportStatus::allocation_failed;
    case render::StaticSceneResourceStatus::invalid_request:
    case render::StaticSceneResourceStatus::upload_failed:
        return WorkspaceViewportStatus::invalid;
    }
    return WorkspaceViewportStatus::invalid;
}

[[nodiscard]] WorkspaceViewportFrameStatus frameStatus(
    render::PresentationFrameStatus status) noexcept {
    switch (status) {
    case render::PresentationFrameStatus::ready:
        return WorkspaceViewportFrameStatus::ready;
    case render::PresentationFrameStatus::invalid_request:
        return WorkspaceViewportFrameStatus::invalid;
    case render::PresentationFrameStatus::unsupported:
        return WorkspaceViewportFrameStatus::unsupported;
    case render::PresentationFrameStatus::execution_failed:
        return WorkspaceViewportFrameStatus::execution_failed;
    }
    return WorkspaceViewportFrameStatus::execution_failed;
}

[[nodiscard]] WorkspaceViewportFrameStatus drawStatus(
    render::IndexedStaticMeshBatchStatus status) noexcept {
    switch (status) {
    case render::IndexedStaticMeshBatchStatus::ready:
        return WorkspaceViewportFrameStatus::ready;
    case render::IndexedStaticMeshBatchStatus::invalid_request:
        return WorkspaceViewportFrameStatus::invalid;
    case render::IndexedStaticMeshBatchStatus::unsupported:
        return WorkspaceViewportFrameStatus::unsupported;
    case render::IndexedStaticMeshBatchStatus::execution_failed:
        return WorkspaceViewportFrameStatus::execution_failed;
    }
    return WorkspaceViewportFrameStatus::execution_failed;
}

}  // namespace

const char* workspace_viewport_status_name(WorkspaceViewportStatus status) noexcept {
    switch (status) {
    case WorkspaceViewportStatus::ready: return "ready";
    case WorkspaceViewportStatus::invalid: return "invalid";
    case WorkspaceViewportStatus::unsupported: return "unsupported";
    case WorkspaceViewportStatus::allocation_failed: return "allocation_failed";
    }
    return "unsupported";
}

const char* workspace_viewport_frame_status_name(
    WorkspaceViewportFrameStatus status) noexcept {
    switch (status) {
    case WorkspaceViewportFrameStatus::ready: return "ready";
    case WorkspaceViewportFrameStatus::invalid: return "invalid";
    case WorkspaceViewportFrameStatus::unsupported: return "unsupported";
    case WorkspaceViewportFrameStatus::execution_failed: return "execution_failed";
    }
    return "execution_failed";
}

WorkspaceViewport::WorkspaceViewport(
    render::Backend backend,
    render::PresentationTargetDescription presentation,
    std::unique_ptr<render::Texture> color,
    std::unique_ptr<render::DepthAttachment> depth,
    std::unique_ptr<render::StockSceneExecutionResult> execution)
    : backend_(backend), presentation_(presentation), color_(std::move(color)),
      depth_(std::move(depth)), execution_(std::move(execution)) {}

WorkspaceViewport::~WorkspaceViewport() = default;

WorkspaceViewportFrameStatus WorkspaceViewport::drawAndPresent(
    render::Device& device, render::PresentationTarget& target,
    const WorkspaceViewportFrameRequest& request,
    render::Diagnostic& output_diagnostic) {
    output_diagnostic = {};
    if (device.info().backend != backend_ || target.backend() != backend_) {
        output_diagnostic = diagnostic(
            "workspace_viewport_backend_mismatch",
            "workspace viewport, device, and presentation target must use one backend");
        return WorkspaceViewportFrameStatus::invalid;
    }
    const auto& description = target.info().description;
    if (description.width != presentation_.width ||
        description.height != presentation_.height ||
        description.format != presentation_.format) {
        output_diagnostic = diagnostic(
            "workspace_viewport_target_mismatch",
            "presentation target dimensions and format must match viewport preparation");
        return WorkspaceViewportFrameStatus::invalid;
    }
    if (request.camera.clip_space != expectedClipSpace(backend_)) {
        output_diagnostic = diagnostic(
            "workspace_viewport_camera_clip_space",
            "camera clip space does not match the prepared backend");
        return WorkspaceViewportFrameStatus::invalid;
    }

    render::StaticSceneFrameDescription frame;
    frame.camera = request.camera;
    frame.depth_attachment = depth_.get();
    frame.load_color = request.load_color;
    frame.clear_color = request.clear_color;
    frame.clear_depth = request.clear_depth;
    frame.depth_clear_value = request.depth_clear_value;
    frame.refreshed_packets = request.refreshed_packets;
    frame.apply_skinning = request.apply_skinning;
    frame.frame_constants = request.frame_constants;

    const auto drawn = execution_->resources->draw_and_readback(
        device, *color_, frame);
    if (!drawn.ok()) {
        output_diagnostic = drawn.diagnostic;
        return drawStatus(drawn.status);
    }
    const auto presented = device.present_texture(target, *color_);
    output_diagnostic = presented.diagnostic;
    return frameStatus(presented.status);
}

WorkspaceViewportPrepareResult prepareWorkspaceViewport(
    render::Device& device, const WorkspaceSessionDocument& document,
    const WorkspaceViewportPrepareRequest& request) {
    WorkspaceViewportPrepareResult result;
    try {
        if (request.color_samples != 1U) {
            result.status = WorkspaceViewportStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_viewport_multisample_unsupported",
                "workspace presentation currently requires a single-sample color target");
            return result;
        }
        render::Diagnostic target_diagnostic;
        if (render::validate_presentation_target_description(
                request.presentation, target_diagnostic) !=
            render::PresentationTargetStatus::ready) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = std::move(target_diagnostic);
            return result;
        }
        const auto color_format = pipelineColorFormat(request.presentation.format);
        if (!color_format.has_value()) {
            result.status = WorkspaceViewportStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_viewport_color_format_unsupported",
                "presentation format has no bounded stock-scene color contract");
            return result;
        }
        if (document.assembly.model.root.kind.empty() ||
            document.scene.snapshot.root == apex::scene::invalid_node_id) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_document_invalid",
                "workspace document has no valid model and scene roots");
            return result;
        }

        render::RenderPlanOptions render_options = request.render;
        if (render_options.workspace_kind.empty())
            render_options.workspace_kind = document.scene.snapshot.workspace_kind;
        if (render_options.bounds_radius <= 0.0F)
            render_options.bounds_radius = document.scene.snapshot.bounds_radius;

        std::vector<apex::scene::NodeId> excluded_roots(
            render_options.excluded_subtree_roots.begin(),
            render_options.excluded_subtree_roots.end());
        std::vector<apex::scene::NodeId> suppressed_roots(
            render_options.suppressed_subtree_roots.begin(),
            render_options.suppressed_subtree_roots.end());
        std::vector<apex::scene::NodeActivityOverride> activity_overrides(
            render_options.activity_overrides.begin(),
            render_options.activity_overrides.end());

        if (request.workspace.lod_bounds_center.has_value()) {
            workspace::WorkspaceLodResolutionRequest lod_request;
            lod_request.workspace = &document.assembly.workspace;
            lod_request.scene = &document.scene.snapshot;
            lod_request.file_root_nodes = document.sceneBinding.file_root_nodes;
            lod_request.bounds_center = *request.workspace.lod_bounds_center;
            lod_request.camera_position = render_options.camera_position;
            lod_request.selected_index = request.workspace.lod_index;
            lod_request.lod_fov_degrees = request.workspace.lod_fov_degrees;
            lod_request.lod_distance_divisor = request.workspace.lod_distance_divisor;
            lod_request.track_camera = request.workspace.lod_track_camera;
            const auto lod = workspace::resolveWorkspaceLod(
                lod_request, request.workspace_scene_limits);
            excluded_roots.insert(excluded_roots.end(),
                                  lod.excluded_root_nodes.begin(),
                                  lod.excluded_root_nodes.end());
        }

        if (request.workspace.cockpit_high_visible.has_value() ||
            request.workspace.blurred_rims_visible.has_value() ||
            request.workspace.driver_cockpit ||
            !request.workspace.driver_hidden_names.empty()) {
            workspace::WorkspacePreviewResolutionRequest preview_request;
            preview_request.scene = &document.scene.snapshot;
            preview_request.cockpit_high_visible = request.workspace.cockpit_high_visible;
            preview_request.blurred_rims_visible = request.workspace.blurred_rims_visible;
            preview_request.driver_cockpit = request.workspace.driver_cockpit;
            preview_request.driver_hidden_names = request.workspace.driver_hidden_names;
            const auto preview = workspace::resolveWorkspacePreview(
                preview_request, request.workspace_scene_limits);
            activity_overrides.insert(activity_overrides.end(),
                                      preview.activity_overrides.begin(),
                                      preview.activity_overrides.end());
            suppressed_roots.insert(suppressed_roots.end(),
                                    preview.suppressed_root_nodes.begin(),
                                    preview.suppressed_root_nodes.end());
        }
        render_options.excluded_subtree_roots = excluded_roots;
        render_options.suppressed_subtree_roots = suppressed_roots;
        render_options.activity_overrides = activity_overrides;

        render::TextureDescription color_description;
        color_description.width = request.presentation.width;
        color_description.height = request.presentation.height;
        color_description.format = request.presentation.format;
        color_description.usage = render::TextureUsage::color_attachment |
                                   render::TextureUsage::transfer_source;
        color_description.mutability = render::TextureMutability::mutable_data;
        color_description.samples = 1U;
        auto color = device.create_texture(color_description);
        if (!color.ok()) {
            result.status = color.status == render::TextureStatus::allocation_failed
                                ? WorkspaceViewportStatus::allocation_failed
                                : WorkspaceViewportStatus::unsupported;
            result.diagnostic = color.diagnostic;
            return result;
        }

        render::DepthAttachmentDescription depth_description;
        depth_description.width = request.presentation.width;
        depth_description.height = request.presentation.height;
        depth_description.samples = 1U;
        auto depth = device.create_depth_attachment(depth_description);
        if (!depth.ok()) {
            result.status = depth.status == render::DepthAttachmentStatus::allocation_failed
                                ? WorkspaceViewportStatus::allocation_failed
                                : WorkspaceViewportStatus::unsupported;
            result.diagnostic = depth.diagnostic;
            return result;
        }

        render::StockSceneExecutionRequest scene_request;
        scene_request.model = &document.assembly.model;
        scene_request.scene = &document.scene.snapshot;
        scene_request.render = render_options;
        scene_request.packets = request.packets;
        scene_request.shader_modules = request.shader_modules;
        scene_request.overrides_by_material = request.overrides_by_material;
        scene_request.evaluate_damage_preview = request.evaluate_damage_preview;
        scene_request.damage_broken_visible = request.damage_broken_visible;
        scene_request.targets.colors = {PipelineRenderTarget{*color_format, 1U}};
        scene_request.targets.has_depth = true;
        scene_request.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
        scene_request.wireframe = request.wireframe;
        scene_request.directional_shadow_receiver = request.directional_shadow_receiver;
        scene_request.texture_authority = render::StaticSceneTextureAuthority::embedded_kn5;
        scene_request.limits = request.limits;
        auto execution = std::make_unique<render::StockSceneExecutionResult>(
            render::prepare_stock_scene_execution(device, scene_request));
        if (!execution->ok()) {
            result.status = preparationStatus(execution->status);
            result.diagnostic = execution->diagnostic;
            return result;
        }

        result.viewport = std::unique_ptr<WorkspaceViewport>(new WorkspaceViewport(
            device.info().backend, request.presentation, std::move(color.texture),
            std::move(depth.attachment), std::move(execution)));
        result.status = WorkspaceViewportStatus::ready;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceViewportStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_viewport_allocation_failed",
            "workspace viewport preparation exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        result.status = WorkspaceViewportStatus::invalid;
        result.diagnostic = {"workspace_viewport_prepare_failed", error.what()};
        return result;
    }
}

}  // namespace apex::app
