#include "apex/render/stock_ks_per_pixel.hpp"

#include <algorithm>
#include <cmath>

namespace apex::render {
namespace {

using Vec3 = std::array<float, 3U>;

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

template <std::size_t Size>
[[nodiscard]] bool finite(const std::array<float, Size>& values) noexcept {
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return finite(value); });
}

[[nodiscard]] float dot(const Vec3& left, const Vec3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] +
           left[2] * right[2];
}

[[nodiscard]] float length(const Vec3& value) noexcept {
    return std::sqrt(dot(value, value));
}

[[nodiscard]] Vec3 normalize(const Vec3& value, float value_length) noexcept {
    return {value[0] / value_length, value[1] / value_length,
            value[2] / value_length};
}

[[nodiscard]] float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] bool finite(const StockKsPerPixelLightingConstants& value) noexcept {
    return finite(value.light_direction) && finite(value.ambient_color) &&
           finite(value.light_color) && finite(value.horizon_color) &&
           finite(value.zenith_color) && finite(value.exposure) &&
           finite(value.screen_width) && finite(value.screen_height) &&
           finite(value.fog_linear) && finite(value.fog_blend) &&
           finite(value.fog_color) && finite(value.cloud_cover) &&
           finite(value.cloud_cutoff) && finite(value.cloud_color) &&
           finite(value.cloud_offset) && finite(value.minimum_exposure) &&
           finite(value.maximum_exposure) && finite(value.dof_focus) &&
           finite(value.dof_range) && finite(value.saturation) &&
           finite(value.game_time) && finite(value.unknown_152);
}

[[nodiscard]] bool finite(const StockKsPerPixelMaterialConstants& value) noexcept {
    return finite(value.ambient) && finite(value.diffuse) &&
           finite(value.specular) && finite(value.specular_exponent) &&
           finite(value.emissive) && finite(value.alpha_reference);
}

} // namespace

apex::scene::Matrix4 stock_ks_per_pixel_transpose_matrix(
    const apex::scene::Matrix4& matrix) noexcept {
    apex::scene::Matrix4 result{};
    for (std::size_t row = 0U; row < 4U; ++row)
        for (std::size_t column = 0U; column < 4U; ++column)
            result[row * 4U + column] = matrix[column * 4U + row];
    return result;
}

StockKsPerPixelCameraConstants make_stock_ks_per_pixel_camera_constants(
    const apex::scene::Matrix4& view,
    const apex::scene::Matrix4& projection,
    const apex::scene::Matrix4& inverse_view_projection,
    std::array<float, 3U> camera_position, float near_plane, float far_plane,
    float field_of_view, float dof_factor) noexcept {
    StockKsPerPixelCameraConstants constants;
    constants.view = stock_ks_per_pixel_transpose_matrix(view);
    constants.projection = stock_ks_per_pixel_transpose_matrix(projection);
    constants.mvp_inverse =
        stock_ks_per_pixel_transpose_matrix(inverse_view_projection);
    constants.camera_position = camera_position;
    constants.near_plane = near_plane;
    constants.far_plane = far_plane;
    constants.field_of_view = field_of_view;
    constants.dof_factor = dof_factor;
    return constants;
}

StockKsPerPixelObjectConstants make_stock_ks_per_pixel_object_constants(
    const apex::scene::Matrix4& world) noexcept {
    return {stock_ks_per_pixel_transpose_matrix(world)};
}

bool valid_stock_ks_per_pixel_camera_constants(
    const StockKsPerPixelCameraConstants& constants) noexcept {
    return finite(constants.view) && finite(constants.projection) &&
           finite(constants.mvp_inverse) &&
           finite(constants.camera_position) &&
           finite(constants.near_plane) &&
           finite(constants.far_plane) && finite(constants.field_of_view) &&
           finite(constants.dof_factor);
}

bool valid_stock_ks_per_pixel_object_constants(
    const StockKsPerPixelObjectConstants& constants) noexcept {
    return finite(constants.world);
}

bool valid_stock_ks_per_pixel_lighting_constants(
    const StockKsPerPixelLightingConstants& constants) noexcept {
    return finite(constants);
}

bool valid_stock_ks_per_pixel_material_constants(
    const StockKsPerPixelMaterialConstants& constants) noexcept {
    return finite(constants);
}

bool valid_stock_directional_shadow_receiver_constants(
    const StockDirectionalShadowReceiverConstants& constants) noexcept {
    for (const auto& matrix : constants.shadow_matrices)
        if (!finite(matrix)) return false;
    return finite(constants.biases) && finite(constants.texture_size) &&
           constants.texture_size > 0.0F;
}

const char* stock_ks_per_pixel_evaluation_status_name(
    StockKsPerPixelEvaluationStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelEvaluationStatus::ready: return "ready";
    case StockKsPerPixelEvaluationStatus::invalid_variant:
        return "invalid_variant";
    case StockKsPerPixelEvaluationStatus::non_finite_input:
        return "non_finite_input";
    case StockKsPerPixelEvaluationStatus::factor_out_of_range:
        return "factor_out_of_range";
    case StockKsPerPixelEvaluationStatus::degenerate_normal:
        return "degenerate_normal";
    case StockKsPerPixelEvaluationStatus::degenerate_view_direction:
        return "degenerate_view_direction";
    case StockKsPerPixelEvaluationStatus::degenerate_half_vector:
        return "degenerate_half_vector";
    }
    return "unknown";
}

const char* stock_ks_per_pixel_container_status_name(
    StockKsPerPixelContainerStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelContainerStatus::ready: return "ready";
    case StockKsPerPixelContainerStatus::invalid_variant:
        return "invalid_variant";
    case StockKsPerPixelContainerStatus::unsupported_version:
        return "unsupported_version";
    case StockKsPerPixelContainerStatus::unsupported_layout:
        return "unsupported_layout";
    case StockKsPerPixelContainerStatus::alpha_flag_mismatch:
        return "alpha_flag_mismatch";
    case StockKsPerPixelContainerStatus::stage_size_mismatch:
        return "stage_size_mismatch";
    case StockKsPerPixelContainerStatus::unsupported_geometry:
        return "unsupported_geometry";
    case StockKsPerPixelContainerStatus::unsupported_shader_model:
        return "unsupported_shader_model";
    }
    return "unknown";
}

StockKsPerPixelContainerStatus validate_stock_ks_per_pixel_container_shape(
    const StockShaderContainer& container,
    StockKsPerPixelVariant variant) noexcept {
    if (variant != StockKsPerPixelVariant::base &&
        variant != StockKsPerPixelVariant::alpha_to_coverage)
        return StockKsPerPixelContainerStatus::invalid_variant;
    if (container.header.version != 2U)
        return StockKsPerPixelContainerStatus::unsupported_version;
    if (container.header.vertex_layout != "mesh")
        return StockKsPerPixelContainerStatus::unsupported_layout;
    const bool expected_alpha =
        variant == StockKsPerPixelVariant::alpha_to_coverage;
    if (container.header.alpha_tested != expected_alpha)
        return StockKsPerPixelContainerStatus::alpha_flag_mismatch;
    if (container.vertex_shader.empty() || container.pixel_shader.empty() ||
        container.header.vertex_bytes != container.vertex_shader.size() ||
        container.header.pixel_bytes != container.pixel_shader.size() ||
        container.header.geometry_bytes != container.geometry_shader.size())
        return StockKsPerPixelContainerStatus::stage_size_mismatch;
    if (container.header.geometry_bytes != 0U ||
        !container.geometry_shader.empty() ||
        container.geometry_metadata.has_value())
        return StockKsPerPixelContainerStatus::unsupported_geometry;
    if (container.vertex_metadata.shader_model_major != 4U ||
        container.vertex_metadata.shader_model_minor != 0U ||
        container.pixel_metadata.shader_model_major != 4U ||
        container.pixel_metadata.shader_model_minor != 0U)
        return StockKsPerPixelContainerStatus::unsupported_shader_model;
    return StockKsPerPixelContainerStatus::ready;
}

StockKsPerPixelEvaluationResult evaluate_stock_ks_per_pixel(
    const StockKsPerPixelPixelInput& input,
    const StockKsPerPixelLightingConstants& lighting,
    const StockKsPerPixelMaterialConstants& material,
    StockKsPerPixelVariant variant) noexcept {
    if (variant != StockKsPerPixelVariant::base &&
        variant != StockKsPerPixelVariant::alpha_to_coverage)
        return {StockKsPerPixelEvaluationStatus::invalid_variant, {}};
    if (!finite(input.interpolated_normal) ||
        !finite(input.camera_to_surface) || !finite(input.sampled_diffuse) ||
        !finite(input.fog_factor) || !finite(input.shadow_factor) ||
        !finite(lighting) || !finite(material))
        return {StockKsPerPixelEvaluationStatus::non_finite_input, {}};
    if (input.fog_factor < 0.0F || input.fog_factor > 1.0F ||
        input.shadow_factor < 0.0F || input.shadow_factor > 1.0F)
        return {StockKsPerPixelEvaluationStatus::factor_out_of_range, {}};

    Vec3 normal = input.interpolated_normal;
    if (variant == StockKsPerPixelVariant::alpha_to_coverage) {
        const float normal_length = length(normal);
        if (!(normal_length > 1.0e-8F) || !finite(normal_length))
            return {StockKsPerPixelEvaluationStatus::degenerate_normal, {}};
        normal = normalize(normal, normal_length);
    }

    const Vec3 view_source = {-input.camera_to_surface[0],
                              -input.camera_to_surface[1],
                              -input.camera_to_surface[2]};
    const float view_length = length(view_source);
    if (!(view_length > 1.0e-8F) || !finite(view_length))
        return {StockKsPerPixelEvaluationStatus::degenerate_view_direction,
                {}};
    const Vec3 view_direction = normalize(view_source, view_length);
    const Vec3 light_direction = {-lighting.light_direction[0],
                                  -lighting.light_direction[1],
                                  -lighting.light_direction[2]};
    const Vec3 half_source = {view_direction[0] + light_direction[0],
                              view_direction[1] + light_direction[1],
                              view_direction[2] + light_direction[2]};
    const float half_length = length(half_source);
    if (!(half_length > 1.0e-8F) || !finite(half_length))
        return {StockKsPerPixelEvaluationStatus::degenerate_half_vector, {}};
    const Vec3 half_direction = normalize(half_source, half_length);

    const float direct = saturate(dot(normal, light_direction));
    const float hemisphere = saturate(0.25F * normal[1] + 0.75F);
    const float specular_power = std::pow(
        saturate(dot(normal, half_direction)),
        std::max(1.0F, material.specular_exponent));
    const float specular =
        specular_power * material.specular * input.shadow_factor;

    StockKsPerPixelEvaluationResult result;
    result.status = StockKsPerPixelEvaluationStatus::ready;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const float base_light =
            lighting.ambient_color[channel] * material.ambient * hemisphere +
            material.emissive[channel] +
            lighting.light_color[channel] * material.diffuse * direct *
                input.shadow_factor;
        const float lit = input.sampled_diffuse[channel] * base_light +
                          lighting.light_color[channel] * specular;
        result.rgba[channel] =
            lit + (lighting.fog_color[channel] - lit) * input.fog_factor;
    }
    result.rgba[3] = input.sampled_diffuse[3];
    return result;
}

} // namespace apex::render
