#include "apex/render/stock_ks_per_pixel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace apex::render;

StockKsPerPixelLightingConstants lighting_fixture();

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte)
        bytes[offset + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8U));
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

struct RdefResourceFixture {
    const char* name;
    std::uint32_t type;
    std::uint32_t return_type;
    std::uint32_t dimension;
    std::uint32_t samples;
    std::uint32_t bind;
    std::uint32_t flags;
};

struct RdefConstantBufferFixture {
    const char* name;
    std::uint32_t bytes;
};

std::vector<std::uint8_t> rdef_stage_fixture(bool vertex) {
    static constexpr std::array vertex_resources = {
        RdefResourceFixture{"cbCamera", 0U, 0U, 0U, 0U, 0U, 1U},
        RdefResourceFixture{"cbPerObject", 0U, 0U, 0U, 0U, 1U, 1U},
        RdefResourceFixture{"cbLighting", 0U, 0U, 0U, 0U, 2U, 1U},
        RdefResourceFixture{"cbShadowMaps", 0U, 0U, 0U, 0U, 3U, 1U},
    };
    static constexpr std::array pixel_resources = {
        RdefResourceFixture{"samLinear", 3U, 0U, 0U, 0U, 0U, 1U},
        RdefResourceFixture{"samShadow", 3U, 0U, 0U, 0U, 1U, 3U},
        RdefResourceFixture{"txDiffuse", 2U, 5U, 4U, 0xffffffffU, 0U, 13U},
        RdefResourceFixture{"txShadow0", 2U, 5U, 4U, 0xffffffffU, 6U, 1U},
        RdefResourceFixture{"txShadow1", 2U, 5U, 4U, 0xffffffffU, 7U, 1U},
        RdefResourceFixture{"txShadow2", 2U, 5U, 4U, 0xffffffffU, 8U, 1U},
        RdefResourceFixture{"cbLighting", 0U, 0U, 0U, 0U, 2U, 1U},
        RdefResourceFixture{"cbShadowMaps", 0U, 0U, 0U, 0U, 3U, 1U},
        RdefResourceFixture{"cbMaterial", 0U, 0U, 0U, 0U, 4U, 1U},
    };
    static constexpr std::array vertex_buffers = {
        RdefConstantBufferFixture{"cbCamera", 224U},
        RdefConstantBufferFixture{"cbPerObject", 64U},
        RdefConstantBufferFixture{"cbLighting", 160U},
        RdefConstantBufferFixture{"cbShadowMaps", 208U},
    };
    static constexpr std::array pixel_buffers = {
        RdefConstantBufferFixture{"cbLighting", 160U},
        RdefConstantBufferFixture{"cbMaterial", 32U},
        RdefConstantBufferFixture{"cbShadowMaps", 208U},
    };
    const std::span<const RdefResourceFixture> resources =
        vertex ? std::span<const RdefResourceFixture>(vertex_resources)
               : std::span<const RdefResourceFixture>(pixel_resources);
    const std::span<const RdefConstantBufferFixture> buffers =
        vertex ? std::span<const RdefConstantBufferFixture>(vertex_buffers)
               : std::span<const RdefConstantBufferFixture>(pixel_buffers);

    constexpr std::size_t rdef_header_bytes = 28U;
    constexpr std::size_t resource_stride = 32U;
    constexpr std::size_t buffer_stride = 24U;
    const std::size_t resource_offset = rdef_header_bytes;
    const std::size_t buffer_offset =
        resource_offset + resources.size() * resource_stride;
    const std::size_t string_offset =
        buffer_offset + buffers.size() * buffer_stride;
    std::vector<std::uint8_t> rdef(string_offset, 0U);
    put_u32(rdef, 0U, static_cast<std::uint32_t>(buffers.size()));
    put_u32(rdef, 4U, static_cast<std::uint32_t>(buffer_offset));
    put_u32(rdef, 8U, static_cast<std::uint32_t>(resources.size()));
    put_u32(rdef, 12U, static_cast<std::uint32_t>(resource_offset));
    put_u32(rdef, 16U, vertex ? 0xfffe0400U : 0xffff0400U);
    put_u32(rdef, 20U, 0x100U);
    const auto add_name = [&](const char* text) {
        const auto offset = static_cast<std::uint32_t>(rdef.size());
        while (*text != '\0') {
            rdef.push_back(static_cast<std::uint8_t>(*text));
            ++text;
        }
        rdef.push_back(0U);
        return offset;
    };
    for (std::size_t index = 0U; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        const std::size_t offset = resource_offset + index * resource_stride;
        put_u32(rdef, offset, add_name(resource.name));
        put_u32(rdef, offset + 4U, resource.type);
        put_u32(rdef, offset + 8U, resource.return_type);
        put_u32(rdef, offset + 12U, resource.dimension);
        put_u32(rdef, offset + 16U, resource.samples);
        put_u32(rdef, offset + 20U, resource.bind);
        put_u32(rdef, offset + 24U, 1U);
        put_u32(rdef, offset + 28U, resource.flags);
    }
    for (std::size_t index = 0U; index < buffers.size(); ++index) {
        const std::size_t offset = buffer_offset + index * buffer_stride;
        put_u32(rdef, offset, add_name(buffers[index].name));
        put_u32(rdef, offset + 12U, buffers[index].bytes);
    }
    put_u32(rdef, 24U, add_name("synthetic-ksPerPixel"));

    constexpr std::size_t dxbc_table_end = 40U;
    const std::size_t program_offset = dxbc_table_end + 8U + rdef.size();
    const std::size_t total_bytes = program_offset + 12U;
    std::vector<std::uint8_t> stage(total_bytes, 0U);
    stage[0U] = 'D';
    stage[1U] = 'X';
    stage[2U] = 'B';
    stage[3U] = 'C';
    put_u32(stage, 20U, 1U);
    put_u32(stage, 24U, static_cast<std::uint32_t>(stage.size()));
    put_u32(stage, 28U, 2U);
    put_u32(stage, 32U, static_cast<std::uint32_t>(dxbc_table_end));
    put_u32(stage, 36U, static_cast<std::uint32_t>(program_offset));
    put_u32(stage, dxbc_table_end, 0x46454452U);
    put_u32(stage, dxbc_table_end + 4U,
            static_cast<std::uint32_t>(rdef.size()));
    std::copy(rdef.begin(), rdef.end(), stage.begin() + 48U);
    put_u32(stage, program_offset, 0x52444853U);
    put_u32(stage, program_offset + 4U, 4U);
    put_u32(stage, program_offset + 8U,
            vertex ? 0x00010040U : 0x00000040U);
    return stage;
}

StockShaderContainer reflected_container_fixture() {
    StockShaderContainer container;
    container.header = {2U, false, "mesh", 0U, 0U, 0U};
    container.vertex_metadata = {4U, 0U, 2U};
    container.pixel_metadata = {4U, 0U, 2U};
    container.vertex_shader = rdef_stage_fixture(true);
    container.pixel_shader = rdef_stage_fixture(false);
    container.header.vertex_bytes =
        static_cast<std::uint32_t>(container.vertex_shader.size());
    container.header.pixel_bytes =
        static_cast<std::uint32_t>(container.pixel_shader.size());
    return container;
}

std::vector<std::uint8_t> signature_payload_fixture(
    std::span<const StockShaderSignatureParameter> parameters) {
    constexpr std::size_t header_bytes = 8U;
    constexpr std::size_t parameter_stride = 24U;
    const std::size_t table_end =
        header_bytes + parameters.size() * parameter_stride;
    std::vector<std::uint8_t> payload(table_end, 0U);
    put_u32(payload, 0U, static_cast<std::uint32_t>(parameters.size()));
    put_u32(payload, 4U, static_cast<std::uint32_t>(header_bytes));
    for (std::size_t index = 0U; index < parameters.size(); ++index) {
        const StockShaderSignatureParameter& parameter = parameters[index];
        const std::size_t offset = header_bytes + index * parameter_stride;
        const std::uint32_t name_offset =
            static_cast<std::uint32_t>(payload.size());
        payload.insert(payload.end(), parameter.semantic.begin(),
                       parameter.semantic.end());
        payload.push_back(0U);
        put_u32(payload, offset, name_offset);
        put_u32(payload, offset + 4U, parameter.semantic_index);
        put_u32(payload, offset + 8U,
                static_cast<std::uint32_t>(parameter.system_value));
        put_u32(payload, offset + 12U,
                static_cast<std::uint32_t>(parameter.component_type));
        put_u32(payload, offset + 16U, parameter.shader_register);
        payload[offset + 20U] = parameter.component_mask;
        payload[offset + 21U] = parameter.read_write_mask;
    }
    return payload;
}

std::vector<std::uint8_t> signature_stage_fixture(bool vertex) {
    const std::span<const StockShaderSignatureParameter> input =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_input_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_input_signature);
    const std::span<const StockShaderSignatureParameter> output =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_output_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_output_signature);
    const std::vector<std::uint8_t> input_payload =
        signature_payload_fixture(input);
    const std::vector<std::uint8_t> output_payload =
        signature_payload_fixture(output);
    const std::array<std::uint8_t, 4U> program = {
        0x40U, 0U, static_cast<std::uint8_t>(vertex ? 1U : 0U), 0U};

    constexpr std::size_t table_end = 44U;
    std::vector<std::uint8_t> stage(table_end, 0U);
    stage[0U] = 'D';
    stage[1U] = 'X';
    stage[2U] = 'B';
    stage[3U] = 'C';
    put_u32(stage, 20U, 1U);
    put_u32(stage, 28U, 3U);
    const auto add_chunk = [&](std::size_t table_index, std::uint32_t tag,
                               std::span<const std::uint8_t> payload) {
        const std::size_t offset = stage.size();
        put_u32(stage, 32U + table_index * 4U,
                static_cast<std::uint32_t>(offset));
        stage.resize(offset + 8U + payload.size());
        put_u32(stage, offset, tag);
        put_u32(stage, offset + 4U,
                static_cast<std::uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), stage.data() + offset + 8U);
    };
    add_chunk(0U, 0x4e475349U, input_payload);
    add_chunk(1U, 0x4e47534fU, output_payload);
    add_chunk(2U, 0x52444853U, program);
    put_u32(stage, 24U, static_cast<std::uint32_t>(stage.size()));
    return stage;
}

StockShaderContainer signature_container_fixture() {
    StockShaderContainer container;
    container.header = {2U, false, "mesh", 0U, 0U, 0U};
    container.vertex_metadata = {4U, 0U, 3U};
    container.pixel_metadata = {4U, 0U, 3U};
    container.vertex_shader = signature_stage_fixture(true);
    container.pixel_shader = signature_stage_fixture(false);
    container.header.vertex_bytes =
        static_cast<std::uint32_t>(container.vertex_shader.size());
    container.header.pixel_bytes =
        static_cast<std::uint32_t>(container.pixel_shader.size());
    return container;
}

std::vector<std::uint8_t> complete_native_stage_fixture(bool vertex) {
    const std::vector<std::uint8_t> rdef_stage =
        rdef_stage_fixture(vertex);
    const std::uint32_t rdef_bytes = get_u32(rdef_stage, 44U);
    const std::span<const std::uint8_t> rdef_payload(
        rdef_stage.data() + 48U, rdef_bytes);
    const std::span<const StockShaderSignatureParameter> input =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_input_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_input_signature);
    const std::span<const StockShaderSignatureParameter> output =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_output_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_output_signature);
    const std::vector<std::uint8_t> input_payload =
        signature_payload_fixture(input);
    const std::vector<std::uint8_t> output_payload =
        signature_payload_fixture(output);
    const std::array<std::uint8_t, 4U> program = {
        0x40U, 0U, static_cast<std::uint8_t>(vertex ? 1U : 0U), 0U};

    constexpr std::size_t table_end = 48U;
    std::vector<std::uint8_t> stage(table_end, 0U);
    stage[0U] = 'D';
    stage[1U] = 'X';
    stage[2U] = 'B';
    stage[3U] = 'C';
    put_u32(stage, 20U, 1U);
    put_u32(stage, 28U, 4U);
    const auto add_chunk = [&](std::size_t table_index, std::uint32_t tag,
                               std::span<const std::uint8_t> payload) {
        const std::size_t offset = stage.size();
        put_u32(stage, 32U + table_index * 4U,
                static_cast<std::uint32_t>(offset));
        stage.resize(offset + 8U + payload.size());
        put_u32(stage, offset, tag);
        put_u32(stage, offset + 4U,
                static_cast<std::uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), stage.data() + offset + 8U);
    };
    add_chunk(0U, 0x46454452U, rdef_payload);
    add_chunk(1U, 0x4e475349U, input_payload);
    add_chunk(2U, 0x4e47534fU, output_payload);
    add_chunk(3U, 0x52444853U, program);
    put_u32(stage, 24U, static_cast<std::uint32_t>(stage.size()));
    return stage;
}

StockShaderContainer complete_native_container_fixture(
    StockKsPerPixelVariant variant = StockKsPerPixelVariant::base) {
    StockShaderContainer container;
    container.header = {
        2U, variant == StockKsPerPixelVariant::alpha_to_coverage,
        "mesh", 0U, 0U, 0U};
    container.vertex_metadata = {4U, 0U, 4U};
    container.pixel_metadata = {4U, 0U, 4U};
    container.vertex_shader = complete_native_stage_fixture(true);
    container.pixel_shader = complete_native_stage_fixture(false);
    container.header.vertex_bytes =
        static_cast<std::uint32_t>(container.vertex_shader.size());
    container.header.pixel_bytes =
        static_cast<std::uint32_t>(container.pixel_shader.size());
    return container;
}

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

void validates_bounded_rdef_resource_contract() {
    auto container = reflected_container_fixture();
    require(validate_stock_ks_per_pixel_reflection(container) ==
                StockKsPerPixelReflectionStatus::ready,
            "synthetic reflected stock contract accepted");

    const std::uint32_t vertex_rdef_bytes =
        get_u32(container.vertex_shader, 44U);
    for (std::uint32_t prefix = 0U; prefix < vertex_rdef_bytes; ++prefix) {
        auto truncated = container;
        put_u32(truncated.vertex_shader, 44U, prefix);
        require(validate_stock_ks_per_pixel_reflection(truncated) !=
                    StockKsPerPixelReflectionStatus::ready,
                "every truncated RDEF payload is rejected");
    }

    auto malformed = container;
    put_u32(malformed.vertex_shader, 40U, 0x54415453U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::missing_rdef,
            "missing RDEF chunk rejected");

    malformed = container;
    const std::size_t vertex_program = get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader, vertex_program, 0x46454452U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::duplicate_rdef,
            "duplicate RDEF chunk rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, 44U, 27U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::truncated_rdef_header,
            "truncated RDEF header rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, 48U + 16U, 0xffff0400U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::target_mismatch,
            "RDEF target-stage mismatch rejected");

    malformed = container;
    put_u32(malformed.pixel_shader, 48U + 8U,
            std::numeric_limits<std::uint32_t>::max());
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::count_mismatch,
            "oversized RDEF resource count rejected before multiplication");

    malformed = container;
    put_u32(malformed.pixel_shader, 48U + 12U,
            std::numeric_limits<std::uint32_t>::max());
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::table_out_of_bounds,
            "out-of-range RDEF resource table rejected");

    malformed = container;
    const std::uint32_t pixel_rdef_bytes =
        get_u32(malformed.pixel_shader, 44U);
    put_u32(malformed.pixel_shader, 48U + 24U, pixel_rdef_bytes - 1U);
    malformed.pixel_shader[48U + pixel_rdef_bytes - 1U] = 'X';
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::invalid_name,
            "unterminated RDEF creator name rejected");

    malformed = container;
    const std::size_t pixel_resource_offset =
        48U + get_u32(malformed.pixel_shader, 48U + 12U);
    put_u32(malformed.pixel_shader,
            pixel_resource_offset + 2U * 32U + 20U, 1U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::resource_mismatch,
            "wrong reflected texture register rejected");

    malformed = container;
    put_u32(malformed.pixel_shader, pixel_resource_offset + 32U,
            get_u32(malformed.pixel_shader, pixel_resource_offset));
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::resource_mismatch,
            "duplicate reflected resource name rejected");

    malformed = container;
    const std::size_t pixel_buffer_offset =
        48U + get_u32(malformed.pixel_shader, 48U + 4U);
    put_u32(malformed.pixel_shader, pixel_buffer_offset + 12U, 159U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::constant_buffer_mismatch,
            "wrong reflected constant-buffer size rejected");

    malformed = container;
    malformed.vertex_shader.pop_back();
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::invalid_stage_container,
            "truncated DXBC stage is rejected before RDEF access");
}

void validates_exact_bounded_stage_signatures() {
    require(stock_ks_per_pixel_stage_interface_is_consistent,
            "vertex output and fragment input linkage is consistent");
    require(stock_ks_per_pixel_vertex_input_signature.size() == 4U &&
                stock_ks_per_pixel_vertex_output_signature.size() == 8U &&
                stock_ks_per_pixel_fragment_input_signature.size() == 8U &&
                stock_ks_per_pixel_fragment_output_signature.size() == 1U,
            "exact stock signature counts are public");
    require(stock_ks_per_pixel_vertex_input_signature[3U].semantic ==
                    "TANGENT" &&
                stock_ks_per_pixel_vertex_input_signature[3U]
                        .read_write_mask == 0U &&
                stock_ks_per_pixel_fragment_output_signature[0U].semantic ==
                    "SV_TARGET" &&
                stock_ks_per_pixel_fragment_output_signature[0U]
                        .system_value ==
                    StockShaderSignatureSystemValue::none,
            "native unused tangent and raw SV_TARGET metadata are retained");

    const auto container = signature_container_fixture();
    require(validate_stock_ks_per_pixel_signatures(container) ==
                StockKsPerPixelSignatureStatus::ready,
            "synthetic exact stock signatures accepted");

    const std::uint32_t input_bytes =
        get_u32(container.vertex_shader, 48U);
    for (std::uint32_t prefix = 0U; prefix < input_bytes; ++prefix) {
        auto truncated = container;
        put_u32(truncated.vertex_shader, 48U, prefix);
        require(validate_stock_ks_per_pixel_signatures(truncated) !=
                    StockKsPerPixelSignatureStatus::ready,
                "every truncated signature payload is rejected");
    }

    auto malformed = container;
    put_u32(malformed.vertex_shader, 44U, 0x54415453U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::missing_signature,
            "missing ISGN chunk rejected");

    malformed = container;
    const std::size_t output_chunk =
        get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader, output_chunk, 0x4e475349U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::duplicate_signature,
            "duplicate ISGN chunk rejected");

    constexpr std::size_t input_payload = 52U;
    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 4U, 4U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::table_out_of_bounds,
            "signature table before its header rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 4U, 16U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "non-native signature record offset rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 4U, input_bytes + 1U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::table_out_of_bounds,
            "signature record offset past the payload rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload,
            std::numeric_limits<std::uint32_t>::max());
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::count_mismatch,
            "overflow-sized signature count rejected before multiplication");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U, 0U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_name,
            "signature name inside the record table rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U, input_bytes);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_name,
            "signature name at the payload end rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U, input_bytes - 1U);
    malformed.vertex_shader[input_payload + input_bytes - 1U] = 'X';
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_name,
            "unterminated signature name rejected");

    malformed = container;
    malformed.vertex_shader[input_payload + 8U + 22U] = 1U;
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "nonzero legacy signature reserved field rejected");

    malformed = container;
    malformed.vertex_shader[input_payload + 8U + 20U] = 0x10U;
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "invalid signature component mask rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U + 4U, 1U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "semantic-index drift rejected");

    malformed = container;
    malformed.pixel_shader.pop_back();
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_stage_container,
            "truncated signature stage rejected before record access");
}

void gates_the_complete_native_program_before_backend_allocation() {
    auto base = complete_native_container_fixture();
    require(validate_stock_ks_per_pixel_native_program(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::ready,
            "complete base native program contract accepted");
    auto alpha = complete_native_container_fixture(
        StockKsPerPixelVariant::alpha_to_coverage);
    require(validate_stock_ks_per_pixel_native_program(
                alpha, StockKsPerPixelVariant::alpha_to_coverage) ==
                StockKsPerPixelNativeProgramStatus::ready,
            "complete alpha-to-coverage native program contract accepted");
    require(validate_stock_ks_per_pixel_native_program(
                base, static_cast<StockKsPerPixelVariant>(255U)) ==
                StockKsPerPixelNativeProgramStatus::invalid_variant,
            "unknown native program variant rejected");

    auto malformed = base;
    malformed.header.version = 1U;
    require(validate_stock_ks_per_pixel_native_program(
                malformed, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::container_shape_mismatch,
            "native program shape failure stops the allocation gate");

    malformed = base;
    constexpr std::size_t rdef_payload = 56U;
    put_u32(malformed.vertex_shader, rdef_payload + 16U, 0xffff0400U);
    require(validate_stock_ks_per_pixel_native_program(
                malformed, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::reflection_mismatch,
            "native program reflection failure stops the allocation gate");

    malformed = base;
    const std::size_t vertex_input_chunk =
        get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader,
            vertex_input_chunk + 8U + 8U + 4U, 1U);
    require(validate_stock_ks_per_pixel_native_program(
                malformed, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::signature_mismatch,
            "native program signature failure stops the allocation gate");

    malformed = base;
    const std::size_t vertex_output_chunk =
        get_u32(malformed.vertex_shader, 40U);
    put_u32(malformed.vertex_shader, vertex_output_chunk, 0x52444853U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::invalid_stage_container,
            "duplicate executable chunks reject a direct reflection request");
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
    require(std::string_view(stock_ks_per_pixel_reflection_status_name(
                StockKsPerPixelReflectionStatus::table_out_of_bounds)) ==
                "table_out_of_bounds",
            "RDEF reflection status name");
    require(std::string_view(stock_ks_per_pixel_reflection_status_name(
                static_cast<StockKsPerPixelReflectionStatus>(255U))) ==
                "unknown",
            "unknown RDEF reflection status name");
    require(std::string_view(stock_ks_per_pixel_signature_status_name(
                StockKsPerPixelSignatureStatus::invalid_name)) ==
                "invalid_name",
            "signature status name");
    require(std::string_view(stock_ks_per_pixel_signature_status_name(
                static_cast<StockKsPerPixelSignatureStatus>(255U))) ==
                "unknown",
            "unknown signature status name");
    require(std::string_view(stock_ks_per_pixel_native_program_status_name(
                StockKsPerPixelNativeProgramStatus::reflection_mismatch)) ==
                "reflection_mismatch",
            "native program status name");
    require(std::string_view(stock_ks_per_pixel_native_program_status_name(
                static_cast<StockKsPerPixelNativeProgramStatus>(255U))) ==
                "unknown",
            "unknown native program status name");
}

} // namespace

int main() {
    try {
        exposes_exact_native_register_contract();
        projects_native_registers_without_namespace_collisions();
        exposes_recovered_vertex_and_sampler_contracts();
        rejects_malformed_backend_binding_layouts();
        validates_owned_container_shape_before_backend_use();
        validates_bounded_rdef_resource_contract();
        validates_exact_bounded_stage_signatures();
        gates_the_complete_native_program_before_backend_allocation();
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
