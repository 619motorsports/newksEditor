#include "apex/render/directional_shadow.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace apex::render {

namespace {

DirectionalShadowMapResult fail(DirectionalShadowMapStatus status,
                                std::string code, std::string message) {
    return {status, {std::move(code), std::move(message)}, nullptr};
}

struct DirectionalShadowCameraState {
    DirectionalShadowResult metadata;
    apex::scene::Vector3 receiver_position{};
    std::array<CameraFrame, directional_shadow_cascade_count> cameras{};
};

[[nodiscard]] DirectionalShadowMapStatus build_camera_state(
    Backend backend, const DirectionalShadowInput& lighting,
    DirectionalShadowCameraState& state, Diagnostic& diagnostic) {
    state.metadata = computeDirectionalShadowCascades(lighting);
    if (state.metadata.map_size != lighting.map_size ||
        state.metadata.cascades.size() != directional_shadow_cascade_count ||
        state.metadata.splits.size() != directional_shadow_cascade_count) {
        diagnostic = {"directional_shadow_cascade_contract_invalid",
                      "Directional shadow computation must produce exactly three cascades"};
        return DirectionalShadowMapStatus::invalid_request;
    }
    const CameraClipSpace clip_space = backend == Backend::Vulkan
                                           ? CameraClipSpace::vulkan
                                           : CameraClipSpace::d3d12;
    state.receiver_position = lighting.eye;
    for (float& value : state.receiver_position)
        if (!std::isfinite(value)) value = 0.0F;
    for (std::size_t index = 0U; index < directional_shadow_cascade_count; ++index) {
        if (state.metadata.cascades[index].index != index) {
            diagnostic = {"directional_shadow_cascade_index_invalid",
                          "Directional shadow cascade indices must be ordered zero through two"};
            return DirectionalShadowMapStatus::invalid_request;
        }
        const auto converted = convertDirectionalShadowCascadeMatrix(
            state.metadata.cascades[index].matrix, clip_space);
        if (!converted.ok()) {
            diagnostic = {converted.code, converted.message};
            return converted.status == DirectionalShadowClipSpaceStatus::unsupported
                       ? DirectionalShadowMapStatus::unsupported
                       : DirectionalShadowMapStatus::invalid_request;
        }
        CameraFrame camera;
        camera.view_projection = converted.matrix;
        camera.near_plane = state.metadata.cascades[index].near_plane;
        camera.far_plane = state.metadata.cascades[index].far_plane;
        camera.clip_space = clip_space;
        state.cameras[index] = camera;
    }
    return DirectionalShadowMapStatus::ready;
}

[[nodiscard]] bool finite_vector(const apex::scene::Vector3& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](float component) { return std::isfinite(component); });
}

[[nodiscard]] bool valid_refresh_input(
    const DirectionalShadowInput& lighting) noexcept {
    if (!finite_vector(lighting.eye) || !finite_vector(lighting.target) ||
        !finite_vector(lighting.up) || !finite_vector(lighting.sun_direction) ||
        !std::isfinite(lighting.fov_radians) || !std::isfinite(lighting.aspect) ||
        !std::isfinite(lighting.near_plane) || !std::isfinite(lighting.far_plane) ||
        !std::isfinite(lighting.scene_radius) || !(lighting.fov_radians > 0.0F) ||
        !(lighting.fov_radians < 3.14159265358979323846F) ||
        !(lighting.aspect > 0.0F) || !(lighting.near_plane > 0.0F) ||
        !(lighting.far_plane > lighting.near_plane) ||
        !(lighting.scene_radius > 0.0F) || lighting.map_size == 0U)
        return false;
    double forward_length_squared = 0.0;
    double up_length_squared = 0.0;
    double light_length_squared = 0.0;
    std::array<double, 3U> forward{};
    for (std::size_t index = 0U; index < 3U; ++index) {
        forward[index] = static_cast<double>(lighting.target[index]) -
                         static_cast<double>(lighting.eye[index]);
        forward_length_squared += forward[index] * forward[index];
        up_length_squared += static_cast<double>(lighting.up[index]) *
                             static_cast<double>(lighting.up[index]);
        light_length_squared += static_cast<double>(lighting.sun_direction[index]) *
                                static_cast<double>(lighting.sun_direction[index]);
    }
    const std::array<double, 3U> forward_cross_up = {
        forward[1] * lighting.up[2] - forward[2] * lighting.up[1],
        forward[2] * lighting.up[0] - forward[0] * lighting.up[2],
        forward[0] * lighting.up[1] - forward[1] * lighting.up[0],
    };
    const double cross_length_squared =
        forward_cross_up[0] * forward_cross_up[0] +
        forward_cross_up[1] * forward_cross_up[1] +
        forward_cross_up[2] * forward_cross_up[2];
    if (!(forward_length_squared > 1.0e-12) || !(up_length_squared > 1.0e-12) ||
        !(light_length_squared > 1.0e-12) || !(cross_length_squared > 1.0e-12))
        return false;
    return std::all_of(lighting.splits.begin(), lighting.splits.end(),
                       [](float split) { return std::isfinite(split) && split > 0.0F; }) &&
           lighting.splits[0] < lighting.splits[1] &&
           lighting.splits[1] < lighting.splits[2];
}

} // namespace

std::optional<std::size_t> select_directional_shadow_cascade(
    float camera_forward_depth,
    const std::array<float, directional_shadow_cascade_count>& splits) noexcept {
    if (!std::isfinite(camera_forward_depth) ||
        !std::all_of(splits.begin(), splits.end(),
                     [](float value) { return std::isfinite(value); }) ||
        splits[0] < 0.0F || splits[0] > splits[1] || splits[1] > splits[2])
        return std::nullopt;
    for (std::size_t cascade = 0U; cascade < splits.size(); ++cascade)
        if (camera_forward_depth <= splits[cascade]) return cascade;
    return std::nullopt;
}

std::optional<float> evaluate_directional_shadow_pcf(
    const std::array<float, 3>& projected_coordinate, float depth_bias,
    std::span<const float> sampled_depths) noexcept {
    if (!std::isfinite(depth_bias) ||
        !std::all_of(projected_coordinate.begin(), projected_coordinate.end(),
                     [](float value) { return std::isfinite(value); }))
        return std::nullopt;
    if (std::any_of(projected_coordinate.begin(), projected_coordinate.end(),
                    [](float value) { return value <= 0.0F || value >= 1.0F; }))
        return 1.0F;
    if (sampled_depths.size() != 9U ||
        !std::all_of(sampled_depths.begin(), sampled_depths.end(),
                     [](float value) { return std::isfinite(value); }))
        return std::nullopt;
    std::size_t lit = 0U;
    const float receiver_depth = projected_coordinate[2] - depth_bias;
    for (const float sampled_depth : sampled_depths)
        if (receiver_depth <= sampled_depth) ++lit;
    return static_cast<float>(lit) / 9.0F;
}

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
    if (!valid_refresh_input(request.lighting)) {
        return fail(DirectionalShadowMapStatus::invalid_request,
                    "directional_shadow_input_invalid",
                    "Directional shadow preparation requires finite camera and lighting values");
    }
    DirectionalShadowCameraState camera_state;
    Diagnostic camera_diagnostic;
    const auto camera_status = build_camera_state(
        device.info().backend, request.lighting, camera_state, camera_diagnostic);
    if (camera_status != DirectionalShadowMapStatus::ready)
        return fail(camera_status, std::move(camera_diagnostic.code),
                    std::move(camera_diagnostic.message));
    auto resources = std::make_unique<DirectionalShadowMapResources>();
    resources->backend_ = device.info().backend;
    resources->device_ = &device;
    resources->metadata_ = std::move(camera_state.metadata);
    resources->receiver_position_ = camera_state.receiver_position;
    resources->cameras_ = camera_state.cameras;
    const DepthAttachmentDescription description{
        request.lighting.map_size, request.lighting.map_size, 1U,
        DepthAttachmentFormat::d32_float, true};
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
            actual.samples != description.samples || actual.format != description.format ||
            actual.shader_readable != description.shader_readable) {
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

DirectionalShadowMapRefreshResult refresh_directional_shadow_maps(
    DirectionalShadowMapResources& resources,
    const DirectionalShadowInput& lighting) try {
    if (resources.backend_ != Backend::Vulkan &&
        resources.backend_ != Backend::D3D12) {
        return {DirectionalShadowMapStatus::unsupported,
                {"directional_shadow_backend_unsupported",
                 "Directional shadow maps require Vulkan or D3D12"}};
    }
    if (resources.device_ == nullptr ||
        lighting.map_size != resources.metadata_.map_size) {
        return {DirectionalShadowMapStatus::invalid_request,
                {"directional_shadow_refresh_resource_mismatch",
                 "Directional shadow refresh must keep the prepared map size and device"}};
    }
    if (!valid_refresh_input(lighting)) {
        return {DirectionalShadowMapStatus::invalid_request,
                {"directional_shadow_refresh_camera_invalid",
                 "Directional shadow refresh requires finite camera and lighting values"}};
    }
    DirectionalShadowCameraState pending;
    Diagnostic pending_diagnostic;
    const auto pending_status = build_camera_state(
        resources.backend_, lighting, pending, pending_diagnostic);
    if (pending_status != DirectionalShadowMapStatus::ready)
        return {pending_status, std::move(pending_diagnostic)};
    resources.metadata_ = std::move(pending.metadata);
    resources.receiver_position_ = pending.receiver_position;
    resources.cameras_ = pending.cameras;
    return {DirectionalShadowMapStatus::ready, {}};
} catch (const std::bad_alloc&) {
    return {DirectionalShadowMapStatus::allocation_failed,
            {"directional_shadow_refresh_allocation_failed",
             "Directional shadow refresh has insufficient memory"}};
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
