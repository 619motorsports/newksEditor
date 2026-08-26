#include "apex/render/stock_ks_per_pixel.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using namespace apex::render;

StockKsPerPixelLightingConstants lighting_fixture();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 1.0e-5F)
        throw std::runtime_error(std::string(message) + ": " +
                                 std::to_string(actual));
}

void exposes_exact_native_register_contract() {
    require(stock_ks_per_pixel_register_contract.size() == 11U,
            "ksPerPixel register count");
    const auto& camera = stock_ks_per_pixel_register_contract[0U];
    const auto& material = stock_ks_per_pixel_register_contract[4U];
    const auto& diffuse = stock_ks_per_pixel_register_contract[5U];
    const auto& shadow_sampler = stock_ks_per_pixel_register_contract[10U];
    require(camera.name == "cbCamera" && camera.register_index == 0U &&
                camera.constant_bytes == 224U &&
                camera.stages == StockShaderStageMask::vertex,
            "native camera register contract");
    require(material.name == "cbMaterial" && material.register_index == 4U &&
                material.constant_bytes == 32U &&
                material.stages == StockShaderStageMask::fragment,
            "native material register contract");
    require(diffuse.name == "txDiffuse" && diffuse.register_index == 0U &&
                diffuse.register_class ==
                    StockShaderRegisterClass::sampled_texture,
            "native diffuse register contract");
    require(shadow_sampler.name == "samShadow" &&
                shadow_sampler.register_index == 1U &&
                shadow_sampler.comparison_sampler,
            "native comparison sampler contract");
}

StockShaderContainer container_fixture(StockKsPerPixelVariant variant) {
    StockShaderContainer container;
    container.header = {
        2U, variant == StockKsPerPixelVariant::alpha_to_coverage,
        "mesh", 48U, 48U, 0U};
    container.vertex_metadata = {4U, 0U, 1U};
    container.pixel_metadata = {4U, 0U, 1U};
    container.vertex_shader.resize(48U);
    container.pixel_shader.resize(48U);
    return container;
}

void validates_owned_container_shape_before_backend_use() {
    auto base = container_fixture(StockKsPerPixelVariant::base);
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::ready,
            "base container shape accepted");
    auto at = container_fixture(StockKsPerPixelVariant::alpha_to_coverage);
    require(validate_stock_ks_per_pixel_container_shape(
                at, StockKsPerPixelVariant::alpha_to_coverage) ==
                StockKsPerPixelContainerStatus::ready,
            "AT container shape accepted");
    require(validate_stock_ks_per_pixel_container_shape(
                at, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::alpha_flag_mismatch,
            "AT package cannot enter the base pipeline state");
    require(validate_stock_ks_per_pixel_container_shape(
                base, static_cast<StockKsPerPixelVariant>(255U)) ==
                StockKsPerPixelContainerStatus::invalid_variant,
            "unknown stock variant rejected");

    base.header.version = 1U;
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_version,
            "unexpected stock package version rejected");
    base = container_fixture(StockKsPerPixelVariant::base);
    base.header.vertex_layout = "skinned";
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_layout,
            "unexpected stock vertex layout rejected");

    base = container_fixture(StockKsPerPixelVariant::base);
    base.vertex_shader.pop_back();
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::stage_size_mismatch,
            "truncated owned vertex stage rejected");
    base = container_fixture(StockKsPerPixelVariant::base);
    base.vertex_metadata.shader_model_major = 5U;
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_shader_model,
            "unexpected shader model rejected");
    base = container_fixture(StockKsPerPixelVariant::base);
    base.header.geometry_bytes = 48U;
    base.geometry_shader.resize(48U);
    base.geometry_metadata = StockShaderStageMetadata{4U, 0U, 1U};
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_geometry,
            "unexpected geometry stage rejected");
}

void preserves_native_constant_bytes_and_rejects_nonfinite_records() {
    StockKsPerPixelMaterialConstants material;
    material.ambient = 0.35F;
    material.diffuse = 0.80F;
    material.specular = 0.20F;
    material.specular_exponent = 30.0F;
    material.emissive = {0.1F, 0.2F, 0.3F};
    material.alpha_reference = 0.5F;
    const auto words =
        std::bit_cast<std::array<std::uint32_t, 8U>>(material);
    const std::array<std::uint32_t, 8U> expected = {
        0x3eb33333U, 0x3f4ccccdU, 0x3e4ccccdU, 0x41f00000U,
        0x3dcccccdU, 0x3e4ccccdU, 0x3e99999aU, 0x3f000000U};
    require(words == expected, "native material bytes match reflected packing");
    require(valid_stock_ks_per_pixel_material_constants(material),
            "finite native material record accepted");
    material.emissive[1U] = std::numeric_limits<float>::infinity();
    require(!valid_stock_ks_per_pixel_material_constants(material),
            "non-finite native material record rejected");

    StockKsPerPixelCameraConstants camera;
    StockKsPerPixelObjectConstants object;
    auto lighting = lighting_fixture();
    require(valid_stock_ks_per_pixel_camera_constants(camera) &&
                valid_stock_ks_per_pixel_object_constants(object) &&
                valid_stock_ks_per_pixel_lighting_constants(lighting),
            "finite native camera, object, and lighting records accepted");
    camera.projection[15U] = std::numeric_limits<float>::quiet_NaN();
    object.world[8U] = std::numeric_limits<float>::infinity();
    lighting.game_time = -std::numeric_limits<float>::infinity();
    require(!valid_stock_ks_per_pixel_camera_constants(camera) &&
                !valid_stock_ks_per_pixel_object_constants(object) &&
                !valid_stock_ks_per_pixel_lighting_constants(lighting),
            "non-finite native constant records rejected");

    std::array<apex::scene::Matrix4,
               stock_ks_per_pixel_shadow_cascade_count>
        matrices{};
    const std::array<float, stock_ks_per_pixel_shadow_cascade_count> biases = {
        0.000002F, 0.000015F, 0.0003F};
    const auto shadow = make_stock_directional_shadow_receiver_constants(
        matrices, biases, 2048U);
    require(valid_stock_directional_shadow_receiver_constants(shadow) &&
                shadow.texture_size == 1.0F / 2048.0F,
            "finite native shadow record accepted");
    const auto zero_width = make_stock_directional_shadow_receiver_constants(
        matrices, biases, 0U);
    require(!valid_stock_directional_shadow_receiver_constants(zero_width),
            "zero-width native shadow record rejected");
}

void transposes_native_host_matrices_before_upload() {
    const apex::scene::Matrix4 source = {
        1.0F, 2.0F, 3.0F, 4.0F,
        5.0F, 6.0F, 7.0F, 8.0F,
        9.0F, 10.0F, 11.0F, 12.0F,
        13.0F, 14.0F, 15.0F, 16.0F};
    const apex::scene::Matrix4 expected = {
        1.0F, 5.0F, 9.0F, 13.0F,
        2.0F, 6.0F, 10.0F, 14.0F,
        3.0F, 7.0F, 11.0F, 15.0F,
        4.0F, 8.0F, 12.0F, 16.0F};
    require(stock_ks_per_pixel_transpose_matrix(source) == expected,
            "native host matrix transpose");
    const auto camera = make_stock_ks_per_pixel_camera_constants(
        source, source, source, {17.0F, 18.0F, 19.0F}, 0.1F, 1000.0F,
        1.2F, 0.25F);
    require(camera.view == expected && camera.projection == expected &&
                camera.mvp_inverse == expected &&
                camera.camera_position ==
                    std::array<float, 3U>{17.0F, 18.0F, 19.0F} &&
                camera.near_plane == 0.1F && camera.far_plane == 1000.0F &&
                camera.field_of_view == 1.2F && camera.dof_factor == 0.25F,
            "native camera record uses transposed matrices and reflected offsets");
    const auto object = make_stock_ks_per_pixel_object_constants(source);
    require(object.world == expected,
            "native object record uses transposed world matrix");
}

StockKsPerPixelLightingConstants lighting_fixture() {
    StockKsPerPixelLightingConstants lighting;
    lighting.light_direction = {0.0F, -1.0F, 0.0F};
    lighting.ambient_color = {0.2F, 0.3F, 0.4F, 1.0F};
    lighting.light_color = {1.0F, 1.0F, 1.0F};
    lighting.horizon_color[3U] = 1.0F;
    lighting.zenith_color[3U] = 1.0F;
    lighting.fog_color = {0.4F, 0.2F, 0.1F};
    return lighting;
}

StockKsPerPixelMaterialConstants material_fixture() {
    StockKsPerPixelMaterialConstants material;
    material.ambient = 0.5F;
    material.diffuse = 0.8F;
    material.emissive = {0.1F, 0.2F, 0.3F};
    return material;
}

void evaluates_recovered_base_pixel_equation() {
    StockKsPerPixelPixelInput input;
    input.interpolated_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};
    input.sampled_diffuse = {0.25F, 0.5F, 0.75F, 0.6F};
    input.fog_factor = 0.25F;
    const auto result = evaluate_stock_ks_per_pixel(
        input, lighting_fixture(), material_fixture(),
        StockKsPerPixelVariant::base);
    require(result.ok(), "recovered base pixel equation evaluates");
    require_near(result.rgba[0U], 0.2875F, "recovered red output");
    require_near(result.rgba[1U], 0.48125F, "recovered green output");
    require_near(result.rgba[2U], 0.75625F, "recovered blue output");
    require_near(result.rgba[3U], 0.6F, "recovered diffuse alpha output");
}

void retains_the_recovered_at_normalization_difference() {
    StockKsPerPixelPixelInput input;
    input.interpolated_normal = {0.0F, 0.5F, 0.0F};
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};
    input.sampled_diffuse = {1.0F, 1.0F, 1.0F, 0.25F};
    const auto base = evaluate_stock_ks_per_pixel(
        input, lighting_fixture(), material_fixture(),
        StockKsPerPixelVariant::base);
    const auto at = evaluate_stock_ks_per_pixel(
        input, lighting_fixture(), material_fixture(),
        StockKsPerPixelVariant::alpha_to_coverage);
    require(base.ok() && at.ok(), "base and AT pixel equations evaluate");
    require(at.rgba[0U] > base.rgba[0U] &&
                at.rgba[1U] > base.rgba[1U] &&
                at.rgba[2U] > base.rgba[2U],
            "AT normal normalization changes recovered lighting");
    require_near(base.rgba[3U], 0.25F, "base diffuse alpha");
    require_near(at.rgba[3U], 0.25F, "AT diffuse alpha");
}

void rejects_unsafe_reference_inputs() {
    StockKsPerPixelPixelInput input;
    input.interpolated_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};
    input.sampled_diffuse = {1.0F, 1.0F, 1.0F, 1.0F};
    auto lighting = lighting_fixture();
    const auto material = material_fixture();

    require(evaluate_stock_ks_per_pixel(
                input, lighting, material,
                static_cast<StockKsPerPixelVariant>(255U))
                .status == StockKsPerPixelEvaluationStatus::invalid_variant,
            "unknown pixel variant rejected");

    input.sampled_diffuse[0U] = std::numeric_limits<float>::quiet_NaN();
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status == StockKsPerPixelEvaluationStatus::non_finite_input,
            "non-finite sampled color rejected");
    input.sampled_diffuse[0U] = 1.0F;

    input.shadow_factor = 1.01F;
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status == StockKsPerPixelEvaluationStatus::factor_out_of_range,
            "shadow factor outside the sampled range rejected");
    input.shadow_factor = 1.0F;

    input.camera_to_surface = {};
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status ==
                StockKsPerPixelEvaluationStatus::degenerate_view_direction,
            "degenerate view direction rejected");
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};

    input.interpolated_normal = {};
    require(evaluate_stock_ks_per_pixel(
                input, lighting, material,
                StockKsPerPixelVariant::alpha_to_coverage)
                .status == StockKsPerPixelEvaluationStatus::degenerate_normal,
            "degenerate AT normal rejected");

    input.interpolated_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, 1.0F, 0.0F};
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status == StockKsPerPixelEvaluationStatus::degenerate_half_vector,
            "degenerate native half vector rejected");
}

void names_all_reference_statuses() {
    require(std::string_view(stock_ks_per_pixel_evaluation_status_name(
                StockKsPerPixelEvaluationStatus::ready)) == "ready",
            "ready evaluation status name");
    require(std::string_view(stock_ks_per_pixel_evaluation_status_name(
                static_cast<StockKsPerPixelEvaluationStatus>(255U))) == "unknown",
            "unknown evaluation status name");
    require(std::string_view(stock_ks_per_pixel_container_status_name(
                StockKsPerPixelContainerStatus::stage_size_mismatch)) ==
                "stage_size_mismatch",
            "container shape status name");
    require(std::string_view(stock_ks_per_pixel_container_status_name(
                static_cast<StockKsPerPixelContainerStatus>(255U))) ==
                "unknown",
            "unknown container shape status name");
}

} // namespace

int main() {
    try {
        exposes_exact_native_register_contract();
        validates_owned_container_shape_before_backend_use();
        preserves_native_constant_bytes_and_rejects_nonfinite_records();
        transposes_native_host_matrices_before_upload();
        evaluates_recovered_base_pixel_equation();
        retains_the_recovered_at_normalization_difference();
        rejects_unsafe_reference_inputs();
        names_all_reference_statuses();
        std::cout << "stock ksPerPixel tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stock ksPerPixel tests: " << error.what() << '\n';
        return 1;
    }
}
