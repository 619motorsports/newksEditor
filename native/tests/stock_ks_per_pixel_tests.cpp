#include "apex/render/stock_ks_per_pixel.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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

void projects_native_registers_without_namespace_collisions() {
    require(validate_stock_ks_per_pixel_backend_contract(
                stock_ks_per_pixel_backend_contract) ==
                StockKsPerPixelBindingStatus::ready,
            "canonical native backend projection accepted");
    auto reordered = stock_ks_per_pixel_backend_contract;
    std::swap(reordered[0U], reordered[10U]);
    require(validate_stock_ks_per_pixel_backend_contract(reordered) ==
                StockKsPerPixelBindingStatus::ready,
            "reflected binding table order does not affect validation");
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(
                stock_ks_per_pixel_d3d12_root_ranges) ==
                StockKsPerPixelBindingStatus::ready,
            "canonical D3D12 root ranges accepted");
    for (std::size_t index = 0U;
         index < stock_ks_per_pixel_register_contract.size(); ++index) {
        const auto& native = stock_ks_per_pixel_register_contract[index];
        const auto& backend = stock_ks_per_pixel_backend_contract[index];
        require(native.name == backend.name &&
                    native.register_class == backend.register_class &&
                    native.register_index == backend.register_index &&
                    native.stages == backend.stages &&
                    native.constant_bytes == backend.constant_bytes &&
                    native.comparison_sampler == backend.comparison_sampler,
                "backend projection matches canonical native register entry");
    }

    const auto& cb0 = stock_ks_per_pixel_backend_contract[0U];
    const auto& t0 = stock_ks_per_pixel_backend_contract[5U];
    const auto& s0 = stock_ks_per_pixel_backend_contract[9U];
    require(cb0.register_index == 0U && t0.register_index == 0U &&
                s0.register_index == 0U &&
                cb0.register_class != t0.register_class &&
                t0.register_class != s0.register_class,
            "D3D b0, t0, and s0 remain independent native namespaces");
    require(cb0.vulkan_set == 0U && t0.vulkan_set == 1U &&
                s0.vulkan_set == 2U && cb0.vulkan_binding == 0U &&
                t0.vulkan_binding == 0U && s0.vulkan_binding == 0U,
            "Vulkan separates b0, t0, and s0 into three descriptor sets");

    for (std::size_t left = 0U;
         left < stock_ks_per_pixel_backend_contract.size(); ++left) {
        for (std::size_t right = left + 1U;
             right < stock_ks_per_pixel_backend_contract.size(); ++right) {
            require(stock_ks_per_pixel_backend_contract[left].vulkan_set !=
                            stock_ks_per_pixel_backend_contract[right].vulkan_set ||
                        stock_ks_per_pixel_backend_contract[left].vulkan_binding !=
                            stock_ks_per_pixel_backend_contract[right].vulkan_binding,
                    "Vulkan stock descriptor locations are unique");
        }
    }

    const auto& vertex_cbvs = stock_ks_per_pixel_d3d12_root_ranges[0U];
    const auto& fragment_cbvs = stock_ks_per_pixel_d3d12_root_ranges[1U];
    require(vertex_cbvs.register_class ==
                    StockShaderRegisterClass::constant_buffer &&
                vertex_cbvs.base_register == 0U &&
                vertex_cbvs.descriptor_count == 4U &&
                vertex_cbvs.stages == StockShaderStageMask::vertex &&
                fragment_cbvs.base_register == 2U &&
                fragment_cbvs.descriptor_count == 3U &&
                fragment_cbvs.stages == StockShaderStageMask::fragment,
            "D3D12 root ranges preserve exact stage visibility");
}

void exposes_recovered_vertex_and_sampler_contracts() {
    require(stock_ks_per_pixel_vertex_stride_bytes == 44U &&
                stock_ks_per_pixel_index_bits == 16U &&
                stock_ks_per_pixel_vertex_contract.size() == 4U,
            "native mesh stream width and index format");
    require(stock_ks_per_pixel_vertex_contract[0U].semantic ==
                    StockKsPerPixelVertexSemantic::position &&
                stock_ks_per_pixel_vertex_contract[0U].offset_bytes == 0U &&
                stock_ks_per_pixel_vertex_contract[0U].component_count == 3U &&
                stock_ks_per_pixel_vertex_contract[1U].semantic ==
                    StockKsPerPixelVertexSemantic::normal &&
                stock_ks_per_pixel_vertex_contract[1U].offset_bytes == 12U &&
                stock_ks_per_pixel_vertex_contract[2U].semantic ==
                    StockKsPerPixelVertexSemantic::texcoord0 &&
                stock_ks_per_pixel_vertex_contract[2U].offset_bytes == 24U &&
                stock_ks_per_pixel_vertex_contract[2U].component_count == 2U &&
                stock_ks_per_pixel_vertex_contract[3U].semantic ==
                    StockKsPerPixelVertexSemantic::tangent &&
                stock_ks_per_pixel_vertex_contract[3U].offset_bytes == 32U,
            "native mesh input elements and offsets");

    const auto& linear = stock_ks_per_pixel_sampler_contract[0U];
    const auto& shadow = stock_ks_per_pixel_sampler_contract[1U];
    require(linear.name == "samLinear" && linear.register_index == 0U &&
                linear.filter == StockKsPerPixelSamplerFilter::anisotropic &&
                linear.address == StockKsPerPixelSamplerAddress::wrap &&
                linear.compare == StockKsPerPixelSamplerCompare::disabled &&
                linear.runtime_max_anisotropy &&
                linear.runtime_mip_lod_bias &&
                linear.maximum_lod == std::numeric_limits<float>::max(),
            "native anisotropic diffuse sampler contract");
    require(shadow.name == "samShadow" && shadow.register_index == 1U &&
                shadow.filter ==
                    StockKsPerPixelSamplerFilter::
                        comparison_min_mag_linear_mip_point &&
                shadow.address == StockKsPerPixelSamplerAddress::clamp &&
                shadow.compare == StockKsPerPixelSamplerCompare::less_equal &&
                !shadow.runtime_max_anisotropy &&
                !shadow.runtime_mip_lod_bias &&
                shadow.maximum_lod == std::numeric_limits<float>::max(),
            "native comparison shadow sampler contract");
}

void rejects_malformed_backend_binding_layouts() {
    require(validate_stock_ks_per_pixel_backend_contract(
                std::span<const StockKsPerPixelBackendBinding>(
                    stock_ks_per_pixel_backend_contract.data(),
                    stock_ks_per_pixel_backend_contract.size() - 1U)) ==
                StockKsPerPixelBindingStatus::count_mismatch,
            "incomplete backend binding table rejected");

    auto bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].register_class =
        static_cast<StockShaderRegisterClass>(255U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_register_class,
            "unknown register class rejected before projection");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].descriptor_kind =
        static_cast<StockShaderDescriptorKind>(255U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_kind,
            "unknown descriptor kind rejected before projection");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].descriptor_kind = StockShaderDescriptorKind::sampled_image;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "constant buffer cannot map to a sampled image");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].descriptor_kind = StockShaderDescriptorKind::sampler;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "sampled texture cannot map to a sampler");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[9U].descriptor_kind = StockShaderDescriptorKind::uniform_buffer;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "sampler cannot map to a uniform buffer");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].stages = static_cast<StockShaderStageMask>(0U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_stage_mask,
            "zero stage mask rejected before projection");
    bindings[0U].stages = static_cast<StockShaderStageMask>(4U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_stage_mask,
            "unknown stage bits rejected before projection");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].stages = StockShaderStageMask::fragment;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "camera buffer cannot become fragment-only");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[4U].stages = StockShaderStageMask::vertex;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "material buffer cannot become vertex-visible");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].name = {};
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_name,
            "empty reflected binding name rejected");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].name =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_name,
            "oversized reflected binding name rejected");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].name = "cbUnknown";
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "unknown reflected binding name rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].d3d12_register_space = 1U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::d3d12_space_mismatch,
            "unrecovered D3D12 register space rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[1U].register_index = bindings[0U].register_index;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::duplicate_native_binding,
            "duplicate D3D constant-buffer register rejected");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[1U].register_index = 5U;
    bindings[1U].vulkan_binding = 5U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "wrong but unique D3D register rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].vulkan_set = bindings[0U].vulkan_set;
    bindings[5U].vulkan_binding = bindings[0U].vulkan_binding;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::duplicate_vulkan_binding,
            "D3D register-class overlap cannot collide in Vulkan");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].vulkan_binding = 5U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::vulkan_namespace_mismatch,
            "compacted Vulkan texture binding rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[4U].constant_bytes = 80U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "portable material size cannot replace native b4 size");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].constant_bytes = 256U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "aligned CBV allocation size cannot replace logical camera size");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].constant_bytes = 4U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "sampled texture cannot declare constant bytes");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[9U].comparison_sampler = true;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "linear sampler cannot become a comparison sampler");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[10U].comparison_sampler = false;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "shadow sampler must retain comparison sampling");

    require(validate_stock_ks_per_pixel_d3d12_root_ranges(
                std::span<const StockKsPerPixelD3D12DescriptorRange>(
                    stock_ks_per_pixel_d3d12_root_ranges.data(), 4U)) ==
                StockKsPerPixelBindingStatus::count_mismatch,
            "incomplete D3D12 root range table rejected");
    auto ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].descriptor_count = 0U;
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "empty D3D12 descriptor range rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].register_class =
        static_cast<StockShaderRegisterClass>(255U);
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "unknown D3D12 descriptor range class rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].stages = static_cast<StockShaderStageMask>(0U);
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "empty D3D12 descriptor range stage mask rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].base_register =
        std::numeric_limits<std::uint32_t>::max();
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "overflowing D3D12 descriptor range rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].descriptor_count =
        std::numeric_limits<std::uint32_t>::max();
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "oversized D3D12 descriptor range rejected before allocation");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[1U] = ranges[0U];
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::duplicate_descriptor_range,
            "duplicate D3D12 root range rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].base_register = 1U;
    ranges[0U].descriptor_count = 3U;
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "valid but incorrect D3D12 root range rejected");
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
    require(std::string_view(stock_ks_per_pixel_binding_status_name(
                StockKsPerPixelBindingStatus::vulkan_namespace_mismatch)) ==
                "vulkan_namespace_mismatch",
            "backend binding status name");
    require(std::string_view(stock_ks_per_pixel_binding_status_name(
                static_cast<StockKsPerPixelBindingStatus>(255U))) == "unknown",
            "unknown backend binding status name");
}

} // namespace

int main() {
    try {
        exposes_exact_native_register_contract();
        projects_native_registers_without_namespace_collisions();
        exposes_recovered_vertex_and_sampler_contracts();
        rejects_malformed_backend_binding_layouts();
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
