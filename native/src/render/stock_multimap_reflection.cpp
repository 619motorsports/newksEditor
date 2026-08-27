#include "apex/render/stock_multimap_reflection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::render {
namespace {

[[nodiscard]] bool finite(const std::array<float, 3U>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const float component) {
                           return std::isfinite(component);
                       });
}

[[nodiscard]] bool normalize(std::array<float, 3U>& value) noexcept {
    const double length_squared =
        static_cast<double>(value[0U]) * value[0U] +
        static_cast<double>(value[1U]) * value[1U] +
        static_cast<double>(value[2U]) * value[2U];
    if (!std::isfinite(length_squared) ||
        length_squared <= std::numeric_limits<float>::min())
        return false;
    const float inverse_length =
        static_cast<float>(1.0 / std::sqrt(length_squared));
    for (float& component : value) component *= inverse_length;
    return finite(value);
}

[[nodiscard]] float dot(const std::array<float, 3U>& left,
                        const std::array<float, 3U>& right) noexcept {
    return left[0U] * right[0U] + left[1U] * right[1U] +
           left[2U] * right[2U];
}

[[nodiscard]] float saturate(const float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] bool alpha_tested(
    const StockMultiMapReflectionVariant variant) noexcept {
    return variant == StockMultiMapReflectionVariant::alpha_tested ||
           variant ==
               StockMultiMapReflectionVariant::alpha_tested_normal_detail;
}

}  // namespace

StockMultiMapReflectionResult evaluate_stock_multimap_reflection(
    const StockMultiMapReflectionInput& input) noexcept {
    StockMultiMapReflectionResult result;
    const auto& controls = input.controls.fresnel_and_additive;
    if (!finite(input.surface_normal) ||
        !finite(input.camera_to_surface) || !finite(input.lit_rgb) ||
        !finite(input.cube_rgb) ||
        !std::all_of(controls.begin(), controls.end(), [](const float value) {
            return std::isfinite(value);
        }) ||
        !std::isfinite(input.maps_specular_exponent) ||
        !std::isfinite(input.ks_specular_exponent) ||
        !std::isfinite(input.maps_reflection) ||
        !std::isfinite(input.material_alpha)) {
        result.diagnostic = {
            "stock_multimap_reflection_non_finite",
            "Recovered MultiMap reflection inputs must be finite"};
        return result;
    }

    std::array<float, 3U> normal = input.surface_normal;
    std::array<float, 3U> view = input.camera_to_surface;
    if (!normalize(normal) || !normalize(view)) {
        result.diagnostic = {
            "stock_multimap_reflection_direction_invalid",
            "Recovered MultiMap reflection requires nonzero normal and camera-to-surface vectors"};
        return result;
    }

    if (controls[3U] == 1.0F)
        result.branch = StockMultiMapReflectionBranch::additive;
    else if (controls[3U] == 2.0F)
        result.branch = StockMultiMapReflectionBranch::special_lerp;

    const float roughness =
        input.maps_specular_exponent * input.ks_specular_exponent;
    if (!std::isfinite(roughness)) {
        result.diagnostic = {
            "stock_multimap_reflection_roughness_invalid",
            "Recovered MultiMap reflection roughness multiplication overflowed"};
        return result;
    }
    const float roughness_divisor =
        result.branch == StockMultiMapReflectionBranch::special_lerp
            ? 8.0F
            : 255.0F;
    result.mip_level =
        6.0F * saturate(1.0F - roughness / roughness_divisor);

    const float view_dot_normal = dot(view, normal);
    std::array<float, 3U> reflected = {
        view[0U] - 2.0F * view_dot_normal * normal[0U],
        view[1U] - 2.0F * view_dot_normal * normal[1U],
        view[2U] - 2.0F * view_dot_normal * normal[2U],
    };
    if (!normalize(reflected)) {
        result.diagnostic = {
            "stock_multimap_reflection_direction_invalid",
            "Recovered MultiMap reflection produced an invalid cube direction"};
        return result;
    }
    result.cube_direction = {
        -reflected[0U], reflected[1U], reflected[2U]};
    if (input.variant == StockMultiMapReflectionVariant::normal_detail) {
        if (result.mip_level <= 0.5F) {
            result.sample_operation =
                StockMultiMapCubeSampleOperation::bias;
        } else {
            // The installed non-AT NMDetail package flips X back before its
            // explicit-level sample when the computed mip exceeds 0.5.
            result.cube_direction[0U] = reflected[0U];
        }
    }

    const float grazing = saturate(1.0F + dot(normal, view));
    const float exponent =
        result.branch == StockMultiMapReflectionBranch::default_lerp
            ? std::max(controls[1U], 1.0F)
            : controls[1U];
    const float angle_power =
        grazing > 0.0F ? std::pow(grazing, exponent) : 0.0F;
    result.fresnel = std::min(controls[0U] + angle_power, controls[2U]);
    if (!std::isfinite(result.fresnel)) {
        result.diagnostic = {
            "stock_multimap_reflection_fresnel_invalid",
            "Recovered MultiMap reflection produced a non-finite Fresnel factor"};
        return result;
    }

    for (std::size_t channel = 0U; channel < result.rgb.size(); ++channel) {
        const float reflected_channel =
            input.maps_reflection * input.cube_rgb[channel];
        result.rgb[channel] =
            result.branch == StockMultiMapReflectionBranch::additive
                ? input.lit_rgb[channel] +
                      result.fresnel * reflected_channel
                : input.lit_rgb[channel] +
                      result.fresnel *
                          (reflected_channel - input.lit_rgb[channel]);
    }
    result.alpha = alpha_tested(input.variant)
                       ? input.material_alpha
                       : result.branch ==
                                 StockMultiMapReflectionBranch::additive
                             ? 2.0F
                             : 1.0F;
    if (!finite(result.rgb) || !std::isfinite(result.alpha)) {
        result.diagnostic = {
            "stock_multimap_reflection_output_invalid",
            "Recovered MultiMap reflection produced a non-finite output"};
        return result;
    }
    result.ready = true;
    return result;
}

}  // namespace apex::render
