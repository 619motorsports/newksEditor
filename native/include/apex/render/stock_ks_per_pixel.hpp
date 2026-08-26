#pragma once

#include "apex/render/material_profile.hpp"
#include "apex/scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace apex::render {

enum class StockShaderRegisterClass : std::uint8_t {
    constant_buffer,
    sampled_texture,
    sampler,
};

enum class StockShaderStageMask : std::uint8_t {
    vertex = 1U,
    fragment = 2U,
    vertex_fragment = 3U,
};

struct StockShaderRegisterBinding {
    std::string_view name;
    StockShaderRegisterClass register_class =
        StockShaderRegisterClass::constant_buffer;
    std::uint32_t register_index = 0U;
    StockShaderStageMask stages = StockShaderStageMask::vertex;
    std::uint32_t constant_bytes = 0U;
    bool comparison_sampler = false;
};

inline constexpr std::array<StockShaderRegisterBinding, 11U>
    stock_ks_per_pixel_register_contract = {{
        {"cbCamera", StockShaderRegisterClass::constant_buffer, 0U,
         StockShaderStageMask::vertex, 224U, false},
        {"cbPerObject", StockShaderRegisterClass::constant_buffer, 1U,
         StockShaderStageMask::vertex, 64U, false},
        {"cbLighting", StockShaderRegisterClass::constant_buffer, 2U,
         StockShaderStageMask::vertex_fragment, 160U, false},
        {"cbShadowMaps", StockShaderRegisterClass::constant_buffer, 3U,
         StockShaderStageMask::vertex_fragment, 208U, false},
        {"cbMaterial", StockShaderRegisterClass::constant_buffer, 4U,
         StockShaderStageMask::fragment, 32U, false},
        {"txDiffuse", StockShaderRegisterClass::sampled_texture, 0U,
         StockShaderStageMask::fragment, 0U, false},
        {"txShadow0", StockShaderRegisterClass::sampled_texture, 6U,
         StockShaderStageMask::fragment, 0U, false},
        {"txShadow1", StockShaderRegisterClass::sampled_texture, 7U,
         StockShaderStageMask::fragment, 0U, false},
        {"txShadow2", StockShaderRegisterClass::sampled_texture, 8U,
         StockShaderStageMask::fragment, 0U, false},
        {"samLinear", StockShaderRegisterClass::sampler, 0U,
         StockShaderStageMask::fragment, 0U, false},
        {"samShadow", StockShaderRegisterClass::sampler, 1U,
         StockShaderStageMask::fragment, 0U, true},
    }};

// These records preserve the native Shader Model 4 constant-buffer packing.
// They do not use the portable material and frame ABI from device.hpp.
struct StockKsPerPixelCameraConstants {
    apex::scene::Matrix4 view{};
    apex::scene::Matrix4 projection{};
    apex::scene::Matrix4 mvp_inverse{};
    std::array<float, 3U> camera_position{};
    std::uint32_t camera_position_padding = 0U;
    float near_plane = 0.0F;
    float far_plane = 0.0F;
    float field_of_view = 0.0F;
    float dof_factor = 0.0F;
};

struct StockKsPerPixelObjectConstants {
    apex::scene::Matrix4 world{};
};

struct StockKsPerPixelLightingConstants {
    std::array<float, 3U> light_direction{};
    std::uint32_t light_direction_padding = 0U;
    std::array<float, 4U> ambient_color{};
    std::array<float, 3U> light_color{};
    std::uint32_t light_color_padding = 0U;
    std::array<float, 4U> horizon_color{};
    std::array<float, 4U> zenith_color{};
    float exposure = 0.0F;
    float screen_width = 0.0F;
    float screen_height = 0.0F;
    float fog_linear = 0.0F;
    float fog_blend = 0.0F;
    std::array<float, 3U> fog_color{};
    float cloud_cover = 0.0F;
    float cloud_cutoff = 0.0F;
    float cloud_color = 0.0F;
    float cloud_offset = 0.0F;
    float minimum_exposure = 0.0F;
    float maximum_exposure = 0.0F;
    float dof_focus = 0.0F;
    float dof_range = 0.0F;
    float saturation = 0.0F;
    float game_time = 0.0F;
    std::array<float, 2U> unknown_152{};
};

struct StockKsPerPixelMaterialConstants {
    float ambient = 0.0F;
    float diffuse = 0.0F;
    float specular = 0.0F;
    float specular_exponent = 0.0F;
    std::array<float, 3U> emissive{};
    float alpha_reference = 0.0F;
};

static_assert(sizeof(StockKsPerPixelCameraConstants) == 224U);
static_assert(offsetof(StockKsPerPixelCameraConstants, view) == 0U);
static_assert(offsetof(StockKsPerPixelCameraConstants, projection) == 64U);
static_assert(offsetof(StockKsPerPixelCameraConstants, mvp_inverse) == 128U);
static_assert(offsetof(StockKsPerPixelCameraConstants, camera_position) == 192U);
static_assert(offsetof(StockKsPerPixelCameraConstants, near_plane) == 208U);
static_assert(offsetof(StockKsPerPixelCameraConstants, dof_factor) == 220U);
static_assert(std::is_trivially_copyable_v<StockKsPerPixelCameraConstants>);

static_assert(sizeof(StockKsPerPixelObjectConstants) == 64U);
static_assert(offsetof(StockKsPerPixelObjectConstants, world) == 0U);
static_assert(std::is_trivially_copyable_v<StockKsPerPixelObjectConstants>);

static_assert(sizeof(StockKsPerPixelLightingConstants) == 160U);
static_assert(offsetof(StockKsPerPixelLightingConstants, light_direction) == 0U);
static_assert(offsetof(StockKsPerPixelLightingConstants, ambient_color) == 16U);
static_assert(offsetof(StockKsPerPixelLightingConstants, light_color) == 32U);
static_assert(offsetof(StockKsPerPixelLightingConstants, horizon_color) == 48U);
static_assert(offsetof(StockKsPerPixelLightingConstants, zenith_color) == 64U);
static_assert(offsetof(StockKsPerPixelLightingConstants, exposure) == 80U);
static_assert(offsetof(StockKsPerPixelLightingConstants, fog_blend) == 96U);
static_assert(offsetof(StockKsPerPixelLightingConstants, fog_color) == 100U);
static_assert(offsetof(StockKsPerPixelLightingConstants, cloud_cover) == 112U);
static_assert(offsetof(StockKsPerPixelLightingConstants, minimum_exposure) == 128U);
static_assert(offsetof(StockKsPerPixelLightingConstants, saturation) == 144U);
static_assert(offsetof(StockKsPerPixelLightingConstants, unknown_152) == 152U);
static_assert(std::is_trivially_copyable_v<StockKsPerPixelLightingConstants>);

static_assert(sizeof(StockKsPerPixelMaterialConstants) == 32U);
static_assert(offsetof(StockKsPerPixelMaterialConstants, ambient) == 0U);
static_assert(offsetof(StockKsPerPixelMaterialConstants, emissive) == 16U);
static_assert(offsetof(StockKsPerPixelMaterialConstants, alpha_reference) == 28U);
static_assert(std::is_trivially_copyable_v<StockKsPerPixelMaterialConstants>);

[[nodiscard]] apex::scene::Matrix4 stock_ks_per_pixel_transpose_matrix(
    const apex::scene::Matrix4& matrix) noexcept;

// The installed host computes inverse(view * projection) before this call.
// This helper transposes all three matrices into the native upload layout.
[[nodiscard]] StockKsPerPixelCameraConstants
make_stock_ks_per_pixel_camera_constants(
    const apex::scene::Matrix4& view,
    const apex::scene::Matrix4& projection,
    const apex::scene::Matrix4& inverse_view_projection,
    std::array<float, 3U> camera_position, float near_plane, float far_plane,
    float field_of_view, float dof_factor = 0.0F) noexcept;

[[nodiscard]] StockKsPerPixelObjectConstants
make_stock_ks_per_pixel_object_constants(
    const apex::scene::Matrix4& world) noexcept;

[[nodiscard]] bool valid_stock_ks_per_pixel_camera_constants(
    const StockKsPerPixelCameraConstants& constants) noexcept;
[[nodiscard]] bool valid_stock_ks_per_pixel_object_constants(
    const StockKsPerPixelObjectConstants& constants) noexcept;
[[nodiscard]] bool valid_stock_ks_per_pixel_lighting_constants(
    const StockKsPerPixelLightingConstants& constants) noexcept;
[[nodiscard]] bool valid_stock_ks_per_pixel_material_constants(
    const StockKsPerPixelMaterialConstants& constants) noexcept;

inline constexpr std::size_t stock_ks_per_pixel_shadow_cascade_count = 3U;

struct StockDirectionalShadowReceiverConstants {
    std::array<apex::scene::Matrix4,
               stock_ks_per_pixel_shadow_cascade_count>
        shadow_matrices{};
    std::array<float, stock_ks_per_pixel_shadow_cascade_count> biases{};
    float texture_size = 0.0F;
};

static_assert(sizeof(StockDirectionalShadowReceiverConstants) == 208U);
static_assert(offsetof(StockDirectionalShadowReceiverConstants,
                       shadow_matrices) == 0U);
static_assert(offsetof(StockDirectionalShadowReceiverConstants, biases) ==
              192U);
static_assert(offsetof(StockDirectionalShadowReceiverConstants,
                       texture_size) == 204U);
static_assert(std::is_trivially_copyable_v<
              StockDirectionalShadowReceiverConstants>);

inline constexpr std::uint32_t stock_directional_shadow_buffer_view_bytes =
    208U;

[[nodiscard]] inline StockDirectionalShadowReceiverConstants
make_stock_directional_shadow_receiver_constants(
    const std::array<apex::scene::Matrix4,
                     stock_ks_per_pixel_shadow_cascade_count>& shadow_matrices,
    const std::array<float, stock_ks_per_pixel_shadow_cascade_count>& biases,
    std::uint32_t render_target_width) noexcept {
    StockDirectionalShadowReceiverConstants constants;
    constants.shadow_matrices = shadow_matrices;
    constants.biases = biases;
    constants.texture_size = render_target_width == 0U
                                 ? 0.0F
                                 : 1.0F / static_cast<float>(render_target_width);
    return constants;
}

[[nodiscard]] bool valid_stock_directional_shadow_receiver_constants(
    const StockDirectionalShadowReceiverConstants& constants) noexcept;

enum class StockKsPerPixelVariant : std::uint8_t {
    base,
    alpha_to_coverage,
};

enum class StockKsPerPixelContainerStatus : std::uint8_t {
    ready,
    invalid_variant,
    unsupported_version,
    unsupported_layout,
    alpha_flag_mismatch,
    stage_size_mismatch,
    unsupported_geometry,
    unsupported_shader_model,
};

[[nodiscard]] const char* stock_ks_per_pixel_container_status_name(
    StockKsPerPixelContainerStatus status) noexcept;

// The complete container parser remains the untrusted-byte boundary. This
// function checks whether an owned, parsed package has the supported family
// shape before a backend creates shader objects.
[[nodiscard]] StockKsPerPixelContainerStatus
validate_stock_ks_per_pixel_container_shape(
    const StockShaderContainer& container,
    StockKsPerPixelVariant variant) noexcept;

struct StockKsPerPixelPixelInput {
    std::array<float, 3U> interpolated_normal{};
    // The native vertex shader emits world position minus camera position.
    std::array<float, 3U> camera_to_surface{};
    std::array<float, 4U> sampled_diffuse{};
    float fog_factor = 0.0F;
    float shadow_factor = 1.0F;
};

enum class StockKsPerPixelEvaluationStatus : std::uint8_t {
    ready,
    invalid_variant,
    non_finite_input,
    factor_out_of_range,
    degenerate_normal,
    degenerate_view_direction,
    degenerate_half_vector,
};

struct StockKsPerPixelEvaluationResult {
    StockKsPerPixelEvaluationStatus status =
        StockKsPerPixelEvaluationStatus::non_finite_input;
    std::array<float, 4U> rgba{};

    [[nodiscard]] bool ok() const noexcept {
        return status == StockKsPerPixelEvaluationStatus::ready;
    }
};

[[nodiscard]] const char* stock_ks_per_pixel_evaluation_status_name(
    StockKsPerPixelEvaluationStatus status) noexcept;

// This bounded CPU reference follows the recovered base and AT pixel DXBC.
// It does not claim GPU execution of the installed shader bytecode.
[[nodiscard]] StockKsPerPixelEvaluationResult evaluate_stock_ks_per_pixel(
    const StockKsPerPixelPixelInput& input,
    const StockKsPerPixelLightingConstants& lighting,
    const StockKsPerPixelMaterialConstants& material,
    StockKsPerPixelVariant variant) noexcept;

} // namespace apex::render
