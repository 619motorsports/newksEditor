#include "apex/render/stock_ks_per_pixel.hpp"
#include "dxbc_reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

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

[[nodiscard]] bool valid_register_class(
    StockShaderRegisterClass value) noexcept {
    return value == StockShaderRegisterClass::constant_buffer ||
           value == StockShaderRegisterClass::sampled_texture ||
           value == StockShaderRegisterClass::sampler;
}

[[nodiscard]] bool valid_stage_mask(StockShaderStageMask value) noexcept {
    return value == StockShaderStageMask::vertex ||
           value == StockShaderStageMask::fragment ||
           value == StockShaderStageMask::vertex_fragment;
}

[[nodiscard]] bool valid_descriptor_kind(
    StockShaderDescriptorKind value) noexcept {
    return value == StockShaderDescriptorKind::uniform_buffer ||
           value == StockShaderDescriptorKind::sampled_image ||
           value == StockShaderDescriptorKind::sampler;
}

[[nodiscard]] StockShaderDescriptorKind descriptor_kind_for(
    StockShaderRegisterClass value) noexcept {
    switch (value) {
    case StockShaderRegisterClass::constant_buffer:
        return StockShaderDescriptorKind::uniform_buffer;
    case StockShaderRegisterClass::sampled_texture:
        return StockShaderDescriptorKind::sampled_image;
    case StockShaderRegisterClass::sampler:
        return StockShaderDescriptorKind::sampler;
    }
    return static_cast<StockShaderDescriptorKind>(255U);
}

[[nodiscard]] std::uint32_t vulkan_set_for(
    StockShaderRegisterClass value) noexcept {
    switch (value) {
    case StockShaderRegisterClass::constant_buffer: return 0U;
    case StockShaderRegisterClass::sampled_texture: return 1U;
    case StockShaderRegisterClass::sampler: return 2U;
    }
    return 3U;
}

struct ExpectedRdefResource {
    std::string_view name;
    std::uint32_t type = 0U;
    std::uint32_t return_type = 0U;
    std::uint32_t dimension = 0U;
    std::uint32_t sample_count = 0U;
    std::uint32_t bind_point = 0U;
    std::uint32_t flags = 0U;
};

struct ExpectedRdefConstantBuffer {
    std::string_view name;
    std::uint32_t bytes = 0U;
};

inline constexpr std::array<ExpectedRdefResource, 4U>
    vertex_rdef_resources = {{{"cbCamera", 0U, 0U, 0U, 0U, 0U, 1U},
                              {"cbPerObject", 0U, 0U, 0U, 0U, 1U, 1U},
                              {"cbLighting", 0U, 0U, 0U, 0U, 2U, 1U},
                              {"cbShadowMaps", 0U, 0U, 0U, 0U, 3U, 1U}}};

inline constexpr std::array<ExpectedRdefResource, 9U>
    pixel_rdef_resources = {{{"samLinear", 3U, 0U, 0U, 0U, 0U, 1U},
                             {"samShadow", 3U, 0U, 0U, 0U, 1U, 3U},
                             {"txDiffuse", 2U, 5U, 4U, 0xffffffffU, 0U, 13U},
                             {"txShadow0", 2U, 5U, 4U, 0xffffffffU, 6U, 1U},
                             {"txShadow1", 2U, 5U, 4U, 0xffffffffU, 7U, 1U},
                             {"txShadow2", 2U, 5U, 4U, 0xffffffffU, 8U, 1U},
                             {"cbLighting", 0U, 0U, 0U, 0U, 2U, 1U},
                             {"cbShadowMaps", 0U, 0U, 0U, 0U, 3U, 1U},
                             {"cbMaterial", 0U, 0U, 0U, 0U, 4U, 1U}}};

inline constexpr std::array<ExpectedRdefConstantBuffer, 4U>
    vertex_rdef_constant_buffers = {{{"cbCamera", 224U},
                                     {"cbPerObject", 64U},
                                     {"cbLighting", 160U},
                                     {"cbShadowMaps", 208U}}};

inline constexpr std::array<ExpectedRdefConstantBuffer, 3U>
    pixel_rdef_constant_buffers = {{{"cbLighting", 160U},
                                    {"cbMaterial", 32U},
                                    {"cbShadowMaps", 208U}}};

[[nodiscard]] bool rdef_name(std::span<const std::byte> payload,
                             std::uint32_t offset_word,
                             std::string_view& name) noexcept {
    constexpr std::size_t max_name_bytes = 64U;
    const std::size_t offset = static_cast<std::size_t>(offset_word);
    if (offset >= payload.size()) return false;
    const std::size_t available = payload.size() - offset;
    const std::size_t limit = std::min(available, max_name_bytes + 1U);
    std::size_t length = 0U;
    while (length < limit && payload[offset + length] != std::byte{0})
        ++length;
    if (length == 0U || length == limit || length > max_name_bytes)
        return false;
    name = {reinterpret_cast<const char*>(payload.data() + offset), length};
    return true;
}

template <std::size_t ResourceCount, std::size_t ConstantBufferCount>
[[nodiscard]] StockKsPerPixelReflectionStatus validate_rdef_payload(
    std::span<const std::byte> payload, std::uint32_t expected_target,
    const std::array<ExpectedRdefResource, ResourceCount>& expected_resources,
    const std::array<ExpectedRdefConstantBuffer, ConstantBufferCount>&
        expected_constant_buffers) noexcept {
    constexpr std::size_t header_bytes = 28U;
    constexpr std::size_t resource_stride = 32U;
    constexpr std::size_t constant_buffer_stride = 24U;
    if (payload.size() < header_bytes)
        return StockKsPerPixelReflectionStatus::truncated_rdef_header;

    std::uint32_t constant_buffer_count = 0U;
    std::uint32_t constant_buffer_offset_word = 0U;
    std::uint32_t resource_count = 0U;
    std::uint32_t resource_offset_word = 0U;
    std::uint32_t target = 0U;
    std::uint32_t creator_offset = 0U;
    (void)detail::read_u32_le(payload, 0U, constant_buffer_count);
    (void)detail::read_u32_le(payload, 4U, constant_buffer_offset_word);
    (void)detail::read_u32_le(payload, 8U, resource_count);
    (void)detail::read_u32_le(payload, 12U, resource_offset_word);
    (void)detail::read_u32_le(payload, 16U, target);
    (void)detail::read_u32_le(payload, 24U, creator_offset);
    if (target != expected_target)
        return StockKsPerPixelReflectionStatus::target_mismatch;
    if (constant_buffer_count != ConstantBufferCount ||
        resource_count != ResourceCount)
        return StockKsPerPixelReflectionStatus::count_mismatch;

    std::string_view creator;
    if (!rdef_name(payload, creator_offset, creator))
        return StockKsPerPixelReflectionStatus::invalid_name;
    const std::size_t resource_offset =
        static_cast<std::size_t>(resource_offset_word);
    const std::size_t constant_buffer_offset =
        static_cast<std::size_t>(constant_buffer_offset_word);
    if (resource_offset > payload.size() ||
        ResourceCount > (payload.size() - resource_offset) / resource_stride ||
        constant_buffer_offset > payload.size() ||
        ConstantBufferCount >
            (payload.size() - constant_buffer_offset) /
                constant_buffer_stride)
        return StockKsPerPixelReflectionStatus::table_out_of_bounds;

    std::array<bool, ResourceCount> found_resources{};
    for (std::size_t index = 0U; index < ResourceCount; ++index) {
        const std::size_t offset = resource_offset + index * resource_stride;
        std::array<std::uint32_t, 8U> words{};
        for (std::size_t word = 0U; word < words.size(); ++word)
            (void)detail::read_u32_le(payload, offset + word * 4U,
                                      words[word]);
        std::string_view name;
        if (!rdef_name(payload, words[0], name))
            return StockKsPerPixelReflectionStatus::invalid_name;
        std::size_t expected_index = ResourceCount;
        for (std::size_t candidate = 0U; candidate < ResourceCount;
             ++candidate) {
            if (expected_resources[candidate].name == name) {
                expected_index = candidate;
                break;
            }
        }
        if (expected_index == ResourceCount || found_resources[expected_index])
            return StockKsPerPixelReflectionStatus::resource_mismatch;
        found_resources[expected_index] = true;
        const ExpectedRdefResource& expected =
            expected_resources[expected_index];
        if (words[1] != expected.type || words[2] != expected.return_type ||
            words[3] != expected.dimension ||
            words[4] != expected.sample_count ||
            words[5] != expected.bind_point || words[6] != 1U ||
            words[7] != expected.flags)
            return StockKsPerPixelReflectionStatus::resource_mismatch;
    }

    std::array<bool, ConstantBufferCount> found_constant_buffers{};
    for (std::size_t index = 0U; index < ConstantBufferCount; ++index) {
        const std::size_t offset =
            constant_buffer_offset + index * constant_buffer_stride;
        std::array<std::uint32_t, 6U> words{};
        for (std::size_t word = 0U; word < words.size(); ++word)
            (void)detail::read_u32_le(payload, offset + word * 4U,
                                      words[word]);
        std::string_view name;
        if (!rdef_name(payload, words[0], name))
            return StockKsPerPixelReflectionStatus::invalid_name;
        std::size_t expected_index = ConstantBufferCount;
        for (std::size_t candidate = 0U; candidate < ConstantBufferCount;
             ++candidate) {
            if (expected_constant_buffers[candidate].name == name) {
                expected_index = candidate;
                break;
            }
        }
        if (expected_index == ConstantBufferCount ||
            found_constant_buffers[expected_index])
            return StockKsPerPixelReflectionStatus::constant_buffer_mismatch;
        found_constant_buffers[expected_index] = true;
        if (words[3] != expected_constant_buffers[expected_index].bytes ||
            words[4] != 0U || words[5] != 0U)
            return StockKsPerPixelReflectionStatus::constant_buffer_mismatch;
    }
    return StockKsPerPixelReflectionStatus::ready;
}

template <std::size_t ResourceCount, std::size_t ConstantBufferCount>
[[nodiscard]] StockKsPerPixelReflectionStatus validate_stage_reflection(
    std::span<const std::uint8_t> stage, std::uint32_t expected_target,
    const std::array<ExpectedRdefResource, ResourceCount>& expected_resources,
    const std::array<ExpectedRdefConstantBuffer, ConstantBufferCount>&
        expected_constant_buffers) noexcept {
    detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    const auto bytes = detail::as_bytes(stage);
    if (detail::inspect_dxbc_container(bytes, 4096U, inspection,
                                       error_offset) !=
        detail::DxbcReaderStatus::ready)
        return StockKsPerPixelReflectionStatus::invalid_stage_container;
    std::uint32_t rdef_count = 0U;
    detail::DxbcChunkView rdef;
    for (std::uint32_t index = 0U; index < inspection.chunk_count; ++index) {
        detail::DxbcChunkView chunk;
        if (!detail::read_dxbc_chunk(bytes, index, chunk))
            return StockKsPerPixelReflectionStatus::invalid_stage_container;
        if (chunk.tag != detail::dxbc_tag_rdef) continue;
        ++rdef_count;
        rdef = chunk;
    }
    if (rdef_count == 0U)
        return StockKsPerPixelReflectionStatus::missing_rdef;
    if (rdef_count != 1U)
        return StockKsPerPixelReflectionStatus::duplicate_rdef;
    return validate_rdef_payload(rdef.payload, expected_target,
                                 expected_resources,
                                 expected_constant_buffers);
}

} // namespace

const char* stock_ks_per_pixel_binding_status_name(
    StockKsPerPixelBindingStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelBindingStatus::ready: return "ready";
    case StockKsPerPixelBindingStatus::count_mismatch:
        return "count_mismatch";
    case StockKsPerPixelBindingStatus::invalid_register_class:
        return "invalid_register_class";
    case StockKsPerPixelBindingStatus::invalid_descriptor_kind:
        return "invalid_descriptor_kind";
    case StockKsPerPixelBindingStatus::invalid_stage_mask:
        return "invalid_stage_mask";
    case StockKsPerPixelBindingStatus::invalid_name: return "invalid_name";
    case StockKsPerPixelBindingStatus::duplicate_native_binding:
        return "duplicate_native_binding";
    case StockKsPerPixelBindingStatus::duplicate_vulkan_binding:
        return "duplicate_vulkan_binding";
    case StockKsPerPixelBindingStatus::d3d12_space_mismatch:
        return "d3d12_space_mismatch";
    case StockKsPerPixelBindingStatus::vulkan_namespace_mismatch:
        return "vulkan_namespace_mismatch";
    case StockKsPerPixelBindingStatus::contract_mismatch:
        return "contract_mismatch";
    case StockKsPerPixelBindingStatus::invalid_descriptor_range:
        return "invalid_descriptor_range";
    case StockKsPerPixelBindingStatus::duplicate_descriptor_range:
        return "duplicate_descriptor_range";
    }
    return "unknown";
}

StockKsPerPixelBindingStatus validate_stock_ks_per_pixel_backend_contract(
    std::span<const StockKsPerPixelBackendBinding> bindings) noexcept {
    if (bindings.size() != stock_ks_per_pixel_backend_contract.size())
        return StockKsPerPixelBindingStatus::count_mismatch;

    std::array<bool, stock_ks_per_pixel_backend_contract.size()> found{};
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        const StockKsPerPixelBackendBinding& binding = bindings[index];
        if (!valid_register_class(binding.register_class))
            return StockKsPerPixelBindingStatus::invalid_register_class;
        if (!valid_descriptor_kind(binding.descriptor_kind))
            return StockKsPerPixelBindingStatus::invalid_descriptor_kind;
        if (!valid_stage_mask(binding.stages))
            return StockKsPerPixelBindingStatus::invalid_stage_mask;
        if (binding.name.empty() || binding.name.size() > 64U)
            return StockKsPerPixelBindingStatus::invalid_name;
        if (binding.d3d12_register_space != 0U)
            return StockKsPerPixelBindingStatus::d3d12_space_mismatch;

        for (std::size_t previous = 0U; previous < index; ++previous) {
            const StockKsPerPixelBackendBinding& other = bindings[previous];
            if (binding.register_class == other.register_class &&
                binding.register_index == other.register_index &&
                binding.d3d12_register_space == other.d3d12_register_space)
                return StockKsPerPixelBindingStatus::duplicate_native_binding;
            if (binding.vulkan_set == other.vulkan_set &&
                binding.vulkan_binding == other.vulkan_binding)
                return StockKsPerPixelBindingStatus::duplicate_vulkan_binding;
        }
        if (binding.vulkan_set != vulkan_set_for(binding.register_class) ||
            binding.vulkan_binding != binding.register_index)
            return StockKsPerPixelBindingStatus::vulkan_namespace_mismatch;
        if (binding.descriptor_kind !=
            descriptor_kind_for(binding.register_class))
            return StockKsPerPixelBindingStatus::contract_mismatch;

        std::size_t expected_index =
            stock_ks_per_pixel_backend_contract.size();
        for (std::size_t candidate = 0U;
             candidate < stock_ks_per_pixel_backend_contract.size();
             ++candidate) {
            if (stock_ks_per_pixel_backend_contract[candidate].name ==
                binding.name) {
                expected_index = candidate;
                break;
            }
        }
        if (expected_index == stock_ks_per_pixel_backend_contract.size())
            return StockKsPerPixelBindingStatus::contract_mismatch;
        if (found[expected_index])
            return StockKsPerPixelBindingStatus::duplicate_native_binding;
        found[expected_index] = true;

        const StockKsPerPixelBackendBinding& expected =
            stock_ks_per_pixel_backend_contract[expected_index];
        std::size_t native_index =
            stock_ks_per_pixel_register_contract.size();
        for (std::size_t candidate = 0U;
             candidate < stock_ks_per_pixel_register_contract.size();
             ++candidate) {
            if (stock_ks_per_pixel_register_contract[candidate].name ==
                binding.name) {
                native_index = candidate;
                break;
            }
        }
        if (native_index == stock_ks_per_pixel_register_contract.size())
            return StockKsPerPixelBindingStatus::contract_mismatch;
        const StockShaderRegisterBinding& native =
            stock_ks_per_pixel_register_contract[native_index];
        if (binding.register_class != expected.register_class ||
            binding.descriptor_kind != expected.descriptor_kind ||
            binding.register_index != expected.register_index ||
            binding.d3d12_register_space != expected.d3d12_register_space ||
            binding.stages != expected.stages ||
            binding.constant_bytes != expected.constant_bytes ||
            binding.comparison_sampler != expected.comparison_sampler ||
            binding.vulkan_set != expected.vulkan_set ||
            binding.vulkan_binding != expected.vulkan_binding)
            return StockKsPerPixelBindingStatus::contract_mismatch;
        if (binding.register_class != native.register_class ||
            binding.register_index != native.register_index ||
            binding.stages != native.stages ||
            binding.constant_bytes != native.constant_bytes ||
            binding.comparison_sampler != native.comparison_sampler)
            return StockKsPerPixelBindingStatus::contract_mismatch;
    }
    return std::all_of(found.begin(), found.end(), [](bool value) {
               return value;
           })
               ? StockKsPerPixelBindingStatus::ready
               : StockKsPerPixelBindingStatus::contract_mismatch;
}

StockKsPerPixelBindingStatus validate_stock_ks_per_pixel_d3d12_root_ranges(
    std::span<const StockKsPerPixelD3D12DescriptorRange> ranges) noexcept {
    if (ranges.size() != stock_ks_per_pixel_d3d12_root_ranges.size())
        return StockKsPerPixelBindingStatus::count_mismatch;

    std::array<bool, stock_ks_per_pixel_d3d12_root_ranges.size()> found{};
    for (const StockKsPerPixelD3D12DescriptorRange& range : ranges) {
        if (!valid_register_class(range.register_class) ||
            !valid_stage_mask(range.stages) || range.descriptor_count == 0U ||
            range.descriptor_count >
                stock_ks_per_pixel_backend_contract.size() ||
            range.base_register > 4095U ||
            range.base_register >
                std::numeric_limits<std::uint32_t>::max() -
                    range.descriptor_count ||
            range.register_space != 0U)
            return StockKsPerPixelBindingStatus::invalid_descriptor_range;

        std::size_t expected_index =
            stock_ks_per_pixel_d3d12_root_ranges.size();
        for (std::size_t candidate = 0U;
             candidate < stock_ks_per_pixel_d3d12_root_ranges.size();
             ++candidate) {
            const auto& expected =
                stock_ks_per_pixel_d3d12_root_ranges[candidate];
            if (range.register_class == expected.register_class &&
                range.base_register == expected.base_register &&
                range.descriptor_count == expected.descriptor_count &&
                range.register_space == expected.register_space &&
                range.stages == expected.stages) {
                expected_index = candidate;
                break;
            }
        }
        if (expected_index == stock_ks_per_pixel_d3d12_root_ranges.size())
            return StockKsPerPixelBindingStatus::contract_mismatch;
        if (found[expected_index])
            return StockKsPerPixelBindingStatus::duplicate_descriptor_range;
        found[expected_index] = true;
    }
    return StockKsPerPixelBindingStatus::ready;
}

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

const char* stock_ks_per_pixel_reflection_status_name(
    StockKsPerPixelReflectionStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelReflectionStatus::ready: return "ready";
    case StockKsPerPixelReflectionStatus::invalid_stage_container:
        return "invalid_stage_container";
    case StockKsPerPixelReflectionStatus::missing_rdef:
        return "missing_rdef";
    case StockKsPerPixelReflectionStatus::duplicate_rdef:
        return "duplicate_rdef";
    case StockKsPerPixelReflectionStatus::truncated_rdef_header:
        return "truncated_rdef_header";
    case StockKsPerPixelReflectionStatus::target_mismatch:
        return "target_mismatch";
    case StockKsPerPixelReflectionStatus::count_mismatch:
        return "count_mismatch";
    case StockKsPerPixelReflectionStatus::table_out_of_bounds:
        return "table_out_of_bounds";
    case StockKsPerPixelReflectionStatus::invalid_name:
        return "invalid_name";
    case StockKsPerPixelReflectionStatus::resource_mismatch:
        return "resource_mismatch";
    case StockKsPerPixelReflectionStatus::constant_buffer_mismatch:
        return "constant_buffer_mismatch";
    }
    return "unknown";
}

StockKsPerPixelReflectionStatus validate_stock_ks_per_pixel_reflection(
    const StockShaderContainer& container) noexcept {
    const StockKsPerPixelReflectionStatus vertex = validate_stage_reflection(
        container.vertex_shader, 0xfffe0400U, vertex_rdef_resources,
        vertex_rdef_constant_buffers);
    if (vertex != StockKsPerPixelReflectionStatus::ready) return vertex;
    return validate_stage_reflection(
        container.pixel_shader, 0xffff0400U, pixel_rdef_resources,
        pixel_rdef_constant_buffers);
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
