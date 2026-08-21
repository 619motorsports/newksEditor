#include "apex/render/directional_shadow.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace apex::render {

namespace {

DirectionalShadowMapResult fail(DirectionalShadowMapStatus status,
                                std::string code, std::string message) {
    return {status, {std::move(code), std::move(message)}, nullptr};
}

} // namespace

const CameraFrame& DirectionalShadowMapResources::camera(std::size_t index) const {
    if (index >= directional_shadow_cascade_count)
        throw std::out_of_range("directional shadow cascade index");
    return cameras_[index];
}

DepthAttachment& DirectionalShadowMapResources::attachment(std::size_t index) {
    if (index >= directional_shadow_cascade_count || attachments_[index] == nullptr)
        throw std::out_of_range("directional shadow attachment index");
    return *attachments_[index];
}

const DepthAttachment& DirectionalShadowMapResources::attachment(std::size_t index) const {
    if (index >= directional_shadow_cascade_count || attachments_[index] == nullptr)
        throw std::out_of_range("directional shadow attachment index");
    return *attachments_[index];
}

DirectionalShadowMapResult prepare_directional_shadow_maps(
    Device& device, const DirectionalShadowMapRequest& request) try {
    if (device.info().backend != Backend::Vulkan &&
        device.info().backend != Backend::D3D12) {
        return fail(DirectionalShadowMapStatus::unsupported,
                    "directional_shadow_backend_unsupported",
                    "Directional shadow maps require Vulkan or D3D12");
    }
    if (request.limits.max_map_size == 0U || request.limits.max_total_bytes == 0U ||
        request.lighting.map_size == 0U ||
        request.lighting.map_size > request.limits.max_map_size) {
        return fail(DirectionalShadowMapStatus::invalid_request,
                    "directional_shadow_map_limit",
                    "Directional shadow map size exceeds the bounded resource limit");
    }
    const std::uint64_t size = request.lighting.map_size;
    if (size > std::numeric_limits<std::uint64_t>::max() / size ||
        size * size > std::numeric_limits<std::uint64_t>::max() /
                          (directional_shadow_cascade_count * sizeof(float)) ||
        size * size * directional_shadow_cascade_count * sizeof(float) >
            request.limits.max_total_bytes) {
        return fail(DirectionalShadowMapStatus::invalid_request,
                    "directional_shadow_map_budget",
                    "Three directional shadow maps exceed the bounded byte budget");
    }
    DirectionalShadowResult metadata =
        computeDirectionalShadowCascades(request.lighting);
    if (metadata.map_size != request.lighting.map_size ||
        metadata.cascades.size() != directional_shadow_cascade_count ||
        metadata.splits.size() != directional_shadow_cascade_count) {
        return fail(DirectionalShadowMapStatus::invalid_request,
                    "directional_shadow_cascade_contract_invalid",
                    "Directional shadow computation must produce exactly three cascades");
    }
    const CameraClipSpace clip_space = device.info().backend == Backend::Vulkan
                                           ? CameraClipSpace::vulkan
                                           : CameraClipSpace::d3d12;
    auto resources = std::make_unique<DirectionalShadowMapResources>();
    resources->backend_ = device.info().backend;
    resources->device_ = &device;
    resources->metadata_ = std::move(metadata);
    for (std::size_t index = 0U; index < directional_shadow_cascade_count; ++index) {
        if (resources->metadata_.cascades[index].index != index)
            return fail(DirectionalShadowMapStatus::invalid_request,
                        "directional_shadow_cascade_index_invalid",
                        "Directional shadow cascade indices must be ordered zero through two");
        const auto converted = convertDirectionalShadowCascadeMatrix(
            resources->metadata_.cascades[index].matrix, clip_space);
        if (!converted.ok())
            return fail(converted.status == DirectionalShadowClipSpaceStatus::unsupported
                            ? DirectionalShadowMapStatus::unsupported
                            : DirectionalShadowMapStatus::invalid_request,
                        converted.code, converted.message);
        CameraFrame camera;
        camera.view_projection = converted.matrix;
        camera.near_plane = resources->metadata_.cascades[index].near_plane;
        camera.far_plane = resources->metadata_.cascades[index].far_plane;
        camera.clip_space = clip_space;
        resources->cameras_[index] = camera;
    }
    const DepthAttachmentDescription description{
        request.lighting.map_size, request.lighting.map_size, 1U,
        DepthAttachmentFormat::d32_float};
    for (auto& attachment : resources->attachments_) {
        DepthAttachmentResult created = device.create_depth_attachment(description);
        if (!created.ok())
            return fail(created.status == DepthAttachmentStatus::unsupported
                            ? DirectionalShadowMapStatus::unsupported
                            : created.status == DepthAttachmentStatus::allocation_failed
                                  ? DirectionalShadowMapStatus::allocation_failed
                                  : DirectionalShadowMapStatus::invalid_request,
                        created.diagnostic.code.empty()
                            ? "directional_shadow_map_allocation_failed"
                            : created.diagnostic.code,
                        created.diagnostic.message);
        if (created.attachment->backend() != resources->backend_) {
            return fail(DirectionalShadowMapStatus::unsupported,
                        "directional_shadow_map_backend_mismatch",
                        "A directional shadow attachment has the wrong backend");
        }
        const auto& actual = created.attachment->info().description;
        if (actual.width != description.width || actual.height != description.height ||
            actual.samples != description.samples || actual.format != description.format) {
            return fail(DirectionalShadowMapStatus::invalid_request,
                        "directional_shadow_map_description_mismatch",
                        "A directional shadow attachment changed the validated description");
        }
        attachment = std::move(created.attachment);
    }
    return {DirectionalShadowMapStatus::ready, {}, std::move(resources)};
} catch (const std::bad_alloc&) {
    return fail(DirectionalShadowMapStatus::allocation_failed,
                "directional_shadow_map_allocation_failed",
                "Directional shadow preparation has insufficient memory");
}

const char* directional_shadow_map_status_name(
    DirectionalShadowMapStatus status) noexcept {
    switch (status) {
    case DirectionalShadowMapStatus::ready: return "ready";
    case DirectionalShadowMapStatus::invalid_request: return "invalid_request";
    case DirectionalShadowMapStatus::unsupported: return "unsupported";
    case DirectionalShadowMapStatus::allocation_failed: return "allocation_failed";
    }
    return "unknown";
}

const char* static_scene_directional_shadow_status_name(
    StaticSceneDirectionalShadowStatus status) noexcept {
    switch (status) {
    case StaticSceneDirectionalShadowStatus::ready: return "ready";
    case StaticSceneDirectionalShadowStatus::partial: return "partial";
    case StaticSceneDirectionalShadowStatus::invalid_request: return "invalid_request";
    case StaticSceneDirectionalShadowStatus::unsupported: return "unsupported";
    case StaticSceneDirectionalShadowStatus::execution_failed: return "execution_failed";
    }
    return "unknown";
}

} // namespace apex::render
