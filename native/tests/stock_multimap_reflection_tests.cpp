#include "apex/render/stock_multimap_reflection.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace apex::render;

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(const float left, const float right,
          const float tolerance = 0.0001F) {
    return std::abs(left - right) <= tolerance;
}

void resolves_base_additive_direction_mip_and_color() {
    StockMultiMapReflectionInput input;
    input.controls.fresnel_and_additive = {0.05F, 2.0F, 0.8F, 1.0F};
    input.surface_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {1.0F, -1.0F, 0.0F};
    input.maps_specular_exponent = 0.5F;
    input.ks_specular_exponent = 255.0F;
    input.maps_reflection = 0.5F;
    input.lit_rgb = {0.2F, 0.3F, 0.4F};
    input.cube_rgb = {0.8F, 0.6F, 0.4F};

    const auto result = evaluate_stock_multimap_reflection(input);
    const float grazing = 1.0F - std::sqrt(0.5F);
    const float fresnel = 0.05F + grazing * grazing;
    require(result.ready &&
                result.branch == StockMultiMapReflectionBranch::additive &&
                result.sample_operation ==
                    StockMultiMapCubeSampleOperation::explicit_level &&
                near(result.mip_level, 3.0F) &&
                near(result.cube_direction[0U], -std::sqrt(0.5F)) &&
                near(result.cube_direction[1U], std::sqrt(0.5F)) &&
                near(result.cube_direction[2U], 0.0F) &&
                near(result.fresnel, fresnel) &&
                near(result.rgb[0U], 0.2F + fresnel * 0.4F) &&
                near(result.rgb[1U], 0.3F + fresnel * 0.3F) &&
                near(result.rgb[2U], 0.4F + fresnel * 0.2F) &&
                result.alpha == 2.0F,
            "base additive branch matches recovered direction, /255 mip, Fresnel, color, and alpha");
}

void resolves_special_and_default_lerp_branches() {
    StockMultiMapReflectionInput input;
    input.controls.fresnel_and_additive = {0.1F, 2.0F, 0.9F, 2.0F};
    input.surface_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, -1.0F, 0.0F};
    input.maps_specular_exponent = 0.5F;
    input.ks_specular_exponent = 8.0F;
    input.maps_reflection = 0.75F;
    input.lit_rgb = {0.2F, 0.4F, 0.6F};
    input.cube_rgb = {0.8F, 0.4F, 0.2F};

    const auto special = evaluate_stock_multimap_reflection(input);
    require(special.ready &&
                special.branch ==
                    StockMultiMapReflectionBranch::special_lerp &&
                near(special.mip_level, 3.0F) &&
                near(special.fresnel, 0.1F) &&
                near(special.rgb[0U], 0.24F) &&
                near(special.rgb[1U], 0.39F) &&
                near(special.rgb[2U], 0.555F) &&
                special.alpha == 1.0F,
            "isAdditive two uses the recovered /8 mip and lerp branch");

    input.controls.fresnel_and_additive = {0.1F, -2.0F, 0.9F, 3.0F};
    input.camera_to_surface = {std::sqrt(0.4375F), -0.75F, 0.0F};
    input.maps_specular_exponent = 0.5F;
    input.ks_specular_exponent = 255.0F;
    const auto fallback = evaluate_stock_multimap_reflection(input);
    require(fallback.ready &&
                fallback.branch ==
                    StockMultiMapReflectionBranch::default_lerp &&
                near(fallback.mip_level, 3.0F) &&
                near(fallback.fresnel, 0.35F),
            "finite non-special values use default lerp and clamp Fresnel exponent to one");
}

void preserves_variant_specific_sampling_and_alpha() {
    StockMultiMapReflectionInput input;
    input.variant = StockMultiMapReflectionVariant::normal_detail;
    input.controls.fresnel_and_additive = {0.0F, 5.0F, 0.5F, 1.0F};
    input.surface_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {1.0F, -1.0F, 0.0F};
    input.maps_specular_exponent = 1.0F;
    input.ks_specular_exponent = 255.0F;
    const auto bias = evaluate_stock_multimap_reflection(input);
    require(bias.ready && near(bias.mip_level, 0.0F) &&
                bias.sample_operation ==
                    StockMultiMapCubeSampleOperation::bias &&
                bias.cube_direction[0U] < 0.0F,
            "non-AT NMDetail keeps the initial X flip for its low-mip bias sample");

    input.ks_specular_exponent = 0.0F;
    const auto explicit_level = evaluate_stock_multimap_reflection(input);
    require(explicit_level.ready && near(explicit_level.mip_level, 6.0F) &&
                explicit_level.sample_operation ==
                    StockMultiMapCubeSampleOperation::explicit_level &&
                explicit_level.cube_direction[0U] > 0.0F,
            "non-AT NMDetail flips X back for explicit samples above mip 0.5");

    input.variant =
        StockMultiMapReflectionVariant::alpha_tested_normal_detail;
    input.material_alpha = 0.37F;
    const auto alpha_tested = evaluate_stock_multimap_reflection(input);
    require(alpha_tested.ready && alpha_tested.cube_direction[0U] < 0.0F &&
                near(alpha_tested.alpha, 0.37F),
            "AT NMDetail uses the direct explicit-level orientation and preserves material alpha");
}

void rejects_invalid_semantic_inputs() {
    StockMultiMapReflectionInput input;
    input.camera_to_surface = {};
    auto result = evaluate_stock_multimap_reflection(input);
    require(!result.ready &&
                result.diagnostic.code ==
                    "stock_multimap_reflection_direction_invalid",
            "zero direction is rejected before reflection normalization");

    input.camera_to_surface = {0.0F, -1.0F, 0.0F};
    input.controls.fresnel_and_additive[0U] =
        std::numeric_limits<float>::quiet_NaN();
    result = evaluate_stock_multimap_reflection(input);
    require(!result.ready &&
                result.diagnostic.code ==
                    "stock_multimap_reflection_non_finite",
            "non-finite controls are rejected before branch evaluation");

    input.controls.fresnel_and_additive[0U] = 0.0F;
    input.maps_specular_exponent = std::numeric_limits<float>::max();
    input.ks_specular_exponent = std::numeric_limits<float>::max();
    result = evaluate_stock_multimap_reflection(input);
    require(!result.ready &&
                result.diagnostic.code ==
                    "stock_multimap_reflection_roughness_invalid",
            "roughness overflow is rejected deterministically");
}

}  // namespace

int main() {
    try {
        resolves_base_additive_direction_mip_and_color();
        resolves_special_and_default_lerp_branches();
        preserves_variant_specific_sampling_and_alpha();
        rejects_invalid_semantic_inputs();
        std::cout << "stock multimap reflection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
