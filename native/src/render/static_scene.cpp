#include "apex/render/static_scene.hpp"

#include "apex/render/decoded_dds_texture.hpp"
#include "apex/render/selected_mesh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

constexpr std::size_t invalid_resource_index = std::numeric_limits<std::size_t>::max();

StaticSceneResourceResult fail(StaticSceneResourceStatus status, std::string code,
                               std::string message) {
    return {status, {std::move(code), std::move(message)}, nullptr};
}

bool checked_add(std::uint64_t value, std::uint64_t& total) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
}

bool checked_bytes(std::size_t count, std::size_t element_size,
                   std::uint64_t& bytes) noexcept {
    const auto count64 = static_cast<std::uint64_t>(count);
    const auto element64 = static_cast<std::uint64_t>(element_size);
    if (element64 != 0U && count64 > std::numeric_limits<std::uint64_t>::max() / element64)
        return false;
    bytes = count64 * element64;
    return true;
}

bool checked_charge(std::size_t count, std::size_t element_size,
                    std::uint64_t& total, std::uint64_t limit) noexcept {
    std::uint64_t bytes = 0U;
    return checked_bytes(count, element_size, bytes) && checked_add(bytes, total) &&
           total <= limit;
}

bool canonical_resource_equals(std::string_view value,
                               std::string_view expected) noexcept {
    std::size_t first = 0U;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0)
        --last;
    if (last - first != expected.size()) return false;
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(
                value[first + index]))) != expected[index])
            return false;
    }
    return true;
}

[[nodiscard]] std::string canonical_texture_name(std::string_view value) {
    std::size_t first = 0U;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0)
        --last;
    std::string result;
    result.reserve(last - first);
    for (std::size_t index = first; index < last; ++index)
        result.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(value[index]))));
    return result;
}

bool finite_material_constants(const KsPerPixelMaterialConstants& constants) noexcept {
    for (const float value : constants.lighting)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.fresnel)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.emissive)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.detail)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.damage_zones)
        if (!std::isfinite(value)) return false;
    return true;
}

bool finite_multimap_reflection_constants(
    const KsPerPixelMultiMapReflectionConstants& constants) noexcept {
    return std::all_of(constants.fresnel_and_additive.begin(),
                       constants.fresnel_and_additive.end(),
                       [](const float value) { return std::isfinite(value); });
}

bool finite_stock_shadow_constants(
    const StockShadowCasterMaterialConstants& constants) noexcept {
    for (const float value : constants.lighting)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.emissive_and_alpha_ref)
        if (!std::isfinite(value)) return false;
    return constants.emissive_and_alpha_ref[3] >= 0.0F &&
           constants.emissive_and_alpha_ref[3] <= 1.0F;
}

bool finite_frame_constants(const KsPerPixelFrameConstants& constants) noexcept {
    for (const float value : constants.sun_direction)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.sun_color)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.ambient_color)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.camera_position)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.horizon_color)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.sky_color)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.fog_color)
        if (!std::isfinite(value)) return false;
    for (const float value : constants.fog)
        if (!std::isfinite(value)) return false;
    return true;
}

bool valid_frame_sun_direction(const KsPerPixelFrameConstants& constants) noexcept {
    const double x = constants.sun_direction[0];
    const double y = constants.sun_direction[1];
    const double z = constants.sun_direction[2];
    const double length_squared = x * x + y * y + z * z;
    return std::isfinite(length_squared) && length_squared > 1.0e-12;
}

StaticSceneResourceStatus map_upload_status(StaticMeshUploadStatus status) noexcept {
    switch (status) {
    case StaticMeshUploadStatus::ready: return StaticSceneResourceStatus::ready;
    case StaticMeshUploadStatus::invalid_request: return StaticSceneResourceStatus::invalid_request;
    case StaticMeshUploadStatus::unsupported: return StaticSceneResourceStatus::unsupported;
    case StaticMeshUploadStatus::allocation_failed: return StaticSceneResourceStatus::allocation_failed;
    case StaticMeshUploadStatus::upload_failed: return StaticSceneResourceStatus::upload_failed;
    }
    return StaticSceneResourceStatus::upload_failed;
}

StaticSceneResourceStatus map_skinned_upload_status(SkinnedMeshUploadStatus status) noexcept {
    switch (status) {
    case SkinnedMeshUploadStatus::ready: return StaticSceneResourceStatus::ready;
    case SkinnedMeshUploadStatus::invalid_request: return StaticSceneResourceStatus::invalid_request;
    case SkinnedMeshUploadStatus::unsupported: return StaticSceneResourceStatus::unsupported;
    case SkinnedMeshUploadStatus::allocation_failed: return StaticSceneResourceStatus::allocation_failed;
    case SkinnedMeshUploadStatus::upload_failed: return StaticSceneResourceStatus::upload_failed;
    }
    return StaticSceneResourceStatus::upload_failed;
}

bool same_draw_flags(const DrawPacketFlags& left, const DrawPacketFlags& right) noexcept {
    return left.transparent == right.transparent && left.blend_enabled == right.blend_enabled &&
           left.alpha_to_coverage == right.alpha_to_coverage && left.depth_test == right.depth_test &&
           left.depth_write == right.depth_write && left.wireframe == right.wireframe &&
           left.selected == right.selected && left.cast_shadows == right.cast_shadows &&
           left.double_face_shadow == right.double_face_shadow;
}

bool same_draw_resources(const DrawPacket& left, const DrawPacket& right) noexcept {
    if (left.resources.size() != right.resources.size()) return false;
    for (std::size_t index = 0U; index < left.resources.size(); ++index) {
        const DrawResourceSlot& a = left.resources[index];
        const DrawResourceSlot& b = right.resources[index];
        if (a.slot != b.slot || a.bind_point != b.bind_point ||
            a.texture_index != b.texture_index || a.texture != b.texture)
            return false;
    }
    return true;
}

bool same_material_profile(const MaterialRenderProfile& left,
                           const MaterialRenderProfile& right) noexcept {
    return left.shader == right.shader && left.stock == right.stock &&
           left.serialized_blend_mode == right.serialized_blend_mode &&
           left.native_blend_mode == right.native_blend_mode &&
           left.effective_blend_mode == right.effective_blend_mode &&
           left.blend_source == right.blend_source &&
           left.alpha_to_coverage == right.alpha_to_coverage &&
           left.shadow_alpha_tested == right.shadow_alpha_tested &&
           left.transparent == right.transparent &&
           left.blend_enabled == right.blend_enabled && left.blend == right.blend &&
           left.blend_mode == right.blend_mode && left.depth_mode == right.depth_mode &&
           left.depth_test == right.depth_test && left.depth_write == right.depth_write &&
           left.cull == right.cull && left.cull_source == right.cull_source &&
           left.windscreen == right.windscreen && left.broken_glass == right.broken_glass &&
           left.reflection_alpha == right.reflection_alpha &&
           left.refractive == right.refractive && left.glass_mode == right.glass_mode;
}

bool same_prepared_draw_contract(const DrawPacket& prepared,
                                 const DrawPacket& refreshed) noexcept {
    return prepared.node == refreshed.node && prepared.material == refreshed.material &&
           prepared.primitive == refreshed.primitive &&
           prepared.vertex_offset == refreshed.vertex_offset &&
           prepared.vertex_count == refreshed.vertex_count &&
           prepared.index_offset == refreshed.index_offset &&
           prepared.index_count == refreshed.index_count &&
           prepared.vertex_stride_floats == refreshed.vertex_stride_floats &&
           prepared.order == refreshed.order && prepared.layer == refreshed.layer &&
           prepared.transparency_overridden == refreshed.transparency_overridden &&
           prepared.shadow_only == refreshed.shadow_only &&
           same_material_profile(prepared.material_profile, refreshed.material_profile) &&
           same_draw_flags(prepared.flags, refreshed.flags) &&
           same_draw_resources(prepared, refreshed) &&
           prepared.shader_execution_supported == refreshed.shader_execution_supported;
}

bool finite_world_matrix(const DrawPacket& packet) noexcept {
    for (const float value : packet.world_matrix)
        if (!std::isfinite(value)) return false;
    return true;
}

StaticSceneResourceStatus map_texture_status(TextureStatus status) noexcept {
    switch (status) {
    case TextureStatus::ready: return StaticSceneResourceStatus::ready;
    case TextureStatus::invalid_description: return StaticSceneResourceStatus::invalid_request;
    case TextureStatus::unsupported: return StaticSceneResourceStatus::unsupported;
    case TextureStatus::allocation_failed: return StaticSceneResourceStatus::allocation_failed;
    case TextureStatus::upload_failed: return StaticSceneResourceStatus::upload_failed;
    }
    return StaticSceneResourceStatus::upload_failed;
}

StaticSceneResourceStatus map_sampler_status(SamplerStatus status) noexcept {
    switch (status) {
    case SamplerStatus::ready: return StaticSceneResourceStatus::ready;
    case SamplerStatus::invalid_description: return StaticSceneResourceStatus::invalid_request;
    case SamplerStatus::unsupported: return StaticSceneResourceStatus::unsupported;
    case SamplerStatus::allocation_failed: return StaticSceneResourceStatus::allocation_failed;
    }
    return StaticSceneResourceStatus::allocation_failed;
}

StaticSceneResourceStatus map_buffer_status(BufferStatus status) noexcept {
    switch (status) {
    case BufferStatus::ready: return StaticSceneResourceStatus::ready;
    case BufferStatus::invalid_description: return StaticSceneResourceStatus::invalid_request;
    case BufferStatus::unsupported: return StaticSceneResourceStatus::unsupported;
    case BufferStatus::allocation_failed: return StaticSceneResourceStatus::allocation_failed;
    case BufferStatus::upload_failed: return StaticSceneResourceStatus::upload_failed;
    }
    return StaticSceneResourceStatus::upload_failed;
}

struct ExecutorPipelineValidation {
    StaticSceneResourceStatus status = StaticSceneResourceStatus::ready;
    std::string error;
    std::uint64_t shader_bytes = 0U;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

ExecutorPipelineValidation validate_executor_pipeline(
    Backend backend, const PipelineProgram& pipeline, const DrawPacket& packet,
    const PipelineLimits& limits, bool stock_native) {
    const PipelineValidationResult validation = validate_pipeline(pipeline, limits);
    if (!validation.valid) {
        if (!validation.diagnostics.empty())
            return {StaticSceneResourceStatus::invalid_request,
                    "Pipeline validation failed: " + validation.diagnostics.front().message, 0U};
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline validation failed without a diagnostic", 0U};
    }
    const auto shader_bytes = static_cast<std::uint64_t>(validation.shader_bytes);
    const PipelineTransformContract required_transform =
        stock_native ? PipelineTransformContract::none
                     : PipelineTransformContract::draw_matrices;
    if (pipeline.transform_contract != required_transform)
        return {StaticSceneResourceStatus::unsupported,
                stock_native
                    ? "Native stock static scenes require the recovered constant-buffer transform contract"
                    : "Static-scene execution requires the draw-matrices transform contract",
                0U};
    if (!stock_native &&
        classify_indexed_portable_resource_layout(pipeline) ==
            IndexedPortableResourceLayout::unsupported)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene material execution requires the portable diffuse layout with optional constants", 0U};
    if (!stock_native &&
        pipeline.resources.empty() != packet.resources.empty())
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline and draw-packet resource declarations do not match", 0U};
    const std::uint32_t color_samples = pipeline.targets.colors.empty()
                                             ? 0U
                                             : pipeline.targets.colors.front().samples;
    if (pipeline.targets.colors.size() != 1U ||
        (pipeline.blend.alpha_to_coverage
             ? color_samples != 4U
             : (color_samples != 1U && color_samples != 4U)))
        return {StaticSceneResourceStatus::unsupported,
                pipeline.blend.alpha_to_coverage
                    ? "Static-scene alpha-to-coverage requires one four-sample color target"
                    : "Static-scene execution requires one single- or four-sample color target",
                0U};
    const PipelineRenderTargetFormat color_format = pipeline.targets.colors.front().format;
    if (color_format != PipelineRenderTargetFormat::rgba16_float &&
        color_format != PipelineRenderTargetFormat::rgba8_unorm &&
        color_format != PipelineRenderTargetFormat::rgba8_srgb &&
        color_format != PipelineRenderTargetFormat::bgra8_unorm &&
        color_format != PipelineRenderTargetFormat::bgra8_srgb)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution supports RGBA16F, RGBA8, and BGRA8 color targets", 0U};
    if (packet.flags.depth_write && !packet.flags.depth_test)
        return {StaticSceneResourceStatus::invalid_request,
                "Static-scene depth writes require depth testing", 0U};
    if (pipeline.depth.test_enabled != packet.flags.depth_test ||
        pipeline.depth.write_enabled != packet.flags.depth_write)
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline depth state does not match its draw packet", 0U};
    if (pipeline.blend.enabled != packet.flags.blend_enabled ||
        pipeline.blend.alpha_to_coverage != packet.flags.alpha_to_coverage)
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline blend state does not match its draw packet", 0U};
    if ((pipeline.raster.fill == PipelineFillMode::wireframe) !=
        packet.flags.wireframe)
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline fill state does not match its draw packet", 0U};
    if ((pipeline.depth.test_enabled || pipeline.depth.write_enabled) &&
        !pipeline.targets.has_depth)
        return {StaticSceneResourceStatus::invalid_request,
                "Static-scene depth state requires a depth target", 0U};
    if (pipeline.depth.test_enabled && pipeline.depth.compare != PipelineCompareOperation::less)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution requires the source-evidenced LESS depth comparison", 0U};
    if (pipeline.targets.has_depth &&
        (pipeline.targets.depth.format != PipelineRenderTargetFormat::depth32_float ||
         pipeline.targets.depth.samples != color_samples))
        return {StaticSceneResourceStatus::unsupported,
                pipeline.blend.alpha_to_coverage
                    ? "Static-scene alpha-to-coverage requires a matching four-sample D32 depth target"
                    : "Static-scene execution requires a matching D32 depth target",
                0U};
    if (packet.vertex_stride_floats == 0U ||
        static_cast<std::uint64_t>(packet.vertex_stride_floats) * sizeof(float) !=
            pipeline.vertex_layout.stride)
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline vertex stride does not match its draw packet", 0U};
    if (pipeline.shaders.size() != 2U)
        return {StaticSceneResourceStatus::invalid_request,
                "Static-scene execution requires two shader stages", 0U};

    bool vertex = false;
    bool fragment = false;
    for (const PipelineShaderModule& shader : pipeline.shaders) {
        vertex = vertex || shader.stage == PipelineShaderStage::vertex;
        fragment = fragment || shader.stage == PipelineShaderStage::fragment;
        const bool format_matches =
            backend == Backend::Vulkan
                ? shader.format == PipelineShaderFormat::spirv
                : shader.format == PipelineShaderFormat::dxbc ||
                      shader.format == PipelineShaderFormat::dxil;
        if (!format_matches)
            return {StaticSceneResourceStatus::unsupported,
                    "Pipeline shader bytecode format does not match the device backend", 0U};
    }
    if (!vertex || !fragment)
        return {StaticSceneResourceStatus::invalid_request,
                "Static-scene execution requires vertex and fragment shaders", 0U};
    return {StaticSceneResourceStatus::ready, {}, shader_bytes};
}

}  // namespace

const char* static_scene_resource_status_name(StaticSceneResourceStatus status) noexcept {
    switch (status) {
    case StaticSceneResourceStatus::ready: return "ready";
    case StaticSceneResourceStatus::invalid_request: return "invalid_request";
    case StaticSceneResourceStatus::unsupported: return "unsupported";
    case StaticSceneResourceStatus::allocation_failed: return "allocation_failed";
    case StaticSceneResourceStatus::upload_failed: return "upload_failed";
    }
    return "unknown";
}

std::size_t StaticSceneResources::owned_texture_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& texture : owned_textures_)
        count += texture != nullptr ? 1U : 0U;
    return count;
}

StaticSceneResourceResult prepare_static_scene_resources(
    Device& device, const StaticScenePrepareRequest& request) try {
    if (request.model == nullptr || request.scene == nullptr)
        return fail(StaticSceneResourceStatus::invalid_request, "static_scene_request_missing",
                    "Static-scene preparation requires a model and scene");
    if (request.texture_authority != StaticSceneTextureAuthority::caller_tables &&
        request.texture_authority !=
            StaticSceneTextureAuthority::owned_model_payloads)
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_texture_authority_invalid",
                    "The static-scene texture authority is invalid");
    const auto& model = *request.model;
    const auto& scene = *request.scene;
    const auto& limits = request.limits;
    const bool owned_model_textures =
        request.texture_authority ==
            StaticSceneTextureAuthority::owned_model_payloads;
    if (limits.max_draws == 0U || limits.max_draws > max_indexed_static_mesh_batch_draws ||
        limits.max_materials == 0U || limits.max_textures == 0U ||
        limits.max_resource_string_bytes == 0U ||
        (owned_model_textures &&
         (limits.texture_decode.maxInputBytes == 0U ||
          limits.texture_decode.maxOutputBytes == 0U ||
          limits.max_total_texture_source_bytes == 0U ||
          limits.max_total_decoded_texture_bytes == 0U)) ||
        limits.max_total_vertex_bytes == 0U ||
        limits.max_total_index_bytes == 0U || limits.max_total_shader_bytes == 0U ||
        limits.max_validation_bytes == 0U || limits.max_preparation_bytes == 0U ||
        limits.max_total_update_bytes == 0U)
        return fail(StaticSceneResourceStatus::invalid_request, "static_scene_limits_invalid",
                    "Static-scene resource limits are invalid");
    if (request.packets.empty() || request.packets.size() > limits.max_draws)
        return fail(StaticSceneResourceStatus::invalid_request, "static_scene_packet_limit",
                    "Static-scene packet count is outside the bounded batch limit");
    const bool material_pipeline_table =
        request.pipelines_by_material.size() == model.materials.size() &&
        request.pipelines_by_packet.empty();
    const bool packet_pipeline_table = request.pipelines_by_material.empty() &&
                                       request.pipelines_by_packet.size() == request.packets.size();
    if (model.materials.size() != scene.materials.size() ||
        model.materials.size() > limits.max_materials ||
        (!material_pipeline_table && !packet_pipeline_table))
        return fail(StaticSceneResourceStatus::invalid_request, "static_scene_material_table_invalid",
                    "Model and scene material tables must match, with one complete pipeline authority");
    const bool has_stock_vulkan_source_table =
        !request.stock_vulkan_source_program_by_packet.empty();
    if (has_stock_vulkan_source_table &&
        request.stock_vulkan_source_program_by_packet.size() !=
            request.packets.size())
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_vulkan_source_packet_table_invalid",
            "The Vulkan source-program packet table must match the prepared packet count");
    if (!has_stock_vulkan_source_table &&
        !request.stock_vulkan_source_programs.empty())
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_vulkan_source_owner_table_unreferenced",
            "A Vulkan source-program owner table requires a packet-index table");
    bool has_stock_vulkan_source = false;
    if (has_stock_vulkan_source_table) {
        for (const std::uint32_t raw_index :
             request.stock_vulkan_source_program_by_packet) {
            if (raw_index == invalid_static_scene_source_program_index)
                continue;
            has_stock_vulkan_source = true;
            if (device.info().backend != Backend::Vulkan)
                return fail(
                    StaticSceneResourceStatus::unsupported,
                    "static_scene_stock_vulkan_source_backend_unsupported",
                    "Retained Vulkan source-equivalent programs require a Vulkan device");
            if (static_cast<std::size_t>(raw_index) >=
                    request.stock_vulkan_source_programs.size() ||
                request.stock_vulkan_source_programs[raw_index] == nullptr)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_vulkan_source_owner_index_invalid",
                    "A retained Vulkan source-program index has no valid owner");
        }
    }
    if (has_stock_vulkan_source &&
        request.stock_vulkan_source_material_constants_by_material.size() !=
            model.materials.size())
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_vulkan_source_material_table_invalid",
            "Vulkan source-equivalent material constants must match the final material table");
    if (has_stock_vulkan_source &&
        limits.max_total_stock_vulkan_source_constant_bytes == 0U)
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_vulkan_source_constant_limits_invalid",
            "The Vulkan source-equivalent constant-buffer limit must be nonzero");
    const StockKsPerPixelNativeSamplerSettings& source_sampler_settings =
        request.stock_vulkan_source_sampler_settings;
    const auto valid_native_sampler_settings = [](
        const StockKsPerPixelNativeSamplerSettings& settings) noexcept {
        return std::isfinite(settings.max_anisotropy) &&
               settings.max_anisotropy >= 2.0F &&
               settings.max_anisotropy <= 16.0F &&
               std::trunc(settings.max_anisotropy) ==
                   settings.max_anisotropy &&
               std::isfinite(settings.mip_lod_bias) &&
               settings.mip_lod_bias >= -16.0F &&
               settings.mip_lod_bias <= 16.0F;
    };
    if (has_stock_vulkan_source &&
        !valid_native_sampler_settings(source_sampler_settings))
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_vulkan_source_sampler_settings_invalid",
            "Vulkan source sampler settings require integer anisotropy from 2 through 16 and LOD bias from -16 through 16");
    const bool has_stock_d3d12_native_table =
        !request.stock_d3d12_native_program_by_packet.empty();
    if (has_stock_d3d12_native_table &&
        request.stock_d3d12_native_program_by_packet.size() !=
            request.packets.size())
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_d3d12_native_packet_table_invalid",
            "The D3D12 native-program packet table must match the prepared packet count");
    if (!has_stock_d3d12_native_table &&
        !request.stock_d3d12_native_programs.empty())
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_d3d12_native_owner_table_unreferenced",
            "A D3D12 native-program owner table requires a packet-index table");
    bool has_stock_d3d12_native = false;
    if (has_stock_d3d12_native_table) {
        for (const std::uint32_t raw_index :
             request.stock_d3d12_native_program_by_packet) {
            if (raw_index == invalid_static_scene_source_program_index)
                continue;
            has_stock_d3d12_native = true;
            if (device.info().backend != Backend::D3D12)
                return fail(
                    StaticSceneResourceStatus::unsupported,
                    "static_scene_stock_d3d12_native_backend_unsupported",
                    "Retained installed native programs require a D3D12 device");
            if (static_cast<std::size_t>(raw_index) >=
                    request.stock_d3d12_native_programs.size() ||
                request.stock_d3d12_native_programs[raw_index] == nullptr ||
                request.stock_d3d12_native_programs[raw_index]
                        ->validation_status() !=
                    StockKsPerPixelNativeProgramStatus::ready ||
                (request.stock_d3d12_native_programs[raw_index]->variant() !=
                     StockKsPerPixelVariant::base &&
                 request.stock_d3d12_native_programs[raw_index]->variant() !=
                     StockKsPerPixelVariant::alpha_to_coverage))
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_d3d12_native_owner_index_invalid",
                    "A retained D3D12 native-program index has no valid base or alpha-to-coverage owner");
        }
    }
    const std::size_t stock_d3d12_native_packet_count =
        has_stock_d3d12_native_table
            ? static_cast<std::size_t>(std::count_if(
                  request.stock_d3d12_native_program_by_packet.begin(),
                  request.stock_d3d12_native_program_by_packet.end(),
                  [](std::uint32_t value) {
                      return value != invalid_static_scene_source_program_index;
                  }))
            : 0U;
    if (stock_d3d12_native_packet_count >
        max_stock_ks_per_pixel_native_batch_draws)
        return fail(
            StaticSceneResourceStatus::unsupported,
            "static_scene_stock_d3d12_native_draw_limit",
            "The installed D3D12 native scene exceeds the sampler-heap batch limit");
    if (has_stock_vulkan_source && has_stock_d3d12_native)
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_native_authority_conflict",
            "One static scene cannot retain Vulkan source and installed D3D12 native authority");
    if (has_stock_d3d12_native &&
        request.stock_d3d12_native_material_constants_by_material.size() !=
            model.materials.size())
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_d3d12_native_material_table_invalid",
            "D3D12 native material constants must match the final material table");
    if (has_stock_d3d12_native &&
        limits.max_total_stock_d3d12_native_constant_bytes == 0U)
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_d3d12_native_constant_limits_invalid",
            "The D3D12 native constant-buffer limit must be nonzero");
    const StockKsPerPixelNativeSamplerSettings& d3d12_sampler_settings =
        request.stock_d3d12_native_sampler_settings;
    if (has_stock_d3d12_native &&
        !valid_native_sampler_settings(d3d12_sampler_settings))
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_stock_d3d12_native_sampler_settings_invalid",
            "D3D12 native sampler settings require integer anisotropy from 2 through 16 and LOD bias from -16 through 16");
    if (!request.stock_shadow_constants_by_material.empty() &&
        request.stock_shadow_constants_by_material.size() != model.materials.size())
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_shadow_material_table_invalid",
                    "The stock shadow material table must match the final material table");
    if (model.textures.size() > limits.max_textures ||
        model.textures.size() > static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max()))
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_texture_table_limit",
                    "The final KN5 texture table exceeds the static-scene limit");

    using TextureScopeKey = std::pair<std::optional<std::size_t>, std::string>;
    std::uint64_t preparation_bytes = 0U;
    const auto charge = [&](std::size_t count, std::size_t element_size) noexcept {
        return checked_charge(count, element_size, preparation_bytes,
                              limits.max_preparation_bytes);
    };
    const auto preparation_limit = [] {
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_preparation_aggregate_limit",
                    "Static-scene host storage exceeds the aggregate preparation limit");
    };
    constexpr std::size_t map_node_links = 3U * sizeof(void*);
    if (!charge(1U, sizeof(StaticSceneResources)) ||
        !charge(model.textures.size(),
                sizeof(TextureScopeKey) + sizeof(std::optional<std::uint32_t>) +
                    map_node_links) ||
        !charge(request.packets.size(),
                sizeof(DrawPacket) + 4U * sizeof(std::size_t) +
                    sizeof(StaticSceneResources::PacketTextureIndices)) ||
        (has_stock_vulkan_source &&
         (!charge(request.packets.size(), sizeof(std::uint32_t) +
                  sizeof(std::unique_ptr<
                      StockKsPerPixelNativeConstantBuffers>)) ||
          !charge(request.stock_vulkan_source_programs.size(),
                  sizeof(std::shared_ptr<const
                      ValidatedStockKsPerPixelVulkanSourceProgram>)))) ||
        (has_stock_d3d12_native &&
         (!charge(request.packets.size(), sizeof(std::uint32_t) +
                  sizeof(std::unique_ptr<
                      StockKsPerPixelNativeDrawResources>)) ||
          !charge(request.stock_d3d12_native_programs.size(),
                  sizeof(std::shared_ptr<const
                      ValidatedStockKsPerPixelNativeProgram>)))) ||
        !charge(model.materials.size(), 3U * sizeof(std::size_t)) ||
        !charge(request.packets.size(),
                4U * sizeof(std::size_t) + 2U * sizeof(const formats::Kn5Node*)) ||
        !charge(request.packets.size(), 4U * sizeof(void*)) ||
        (owned_model_textures &&
         (!charge(model.textures.size(),
                  sizeof(std::optional<DecodedTexturePlan>) + 2U * sizeof(bool)) ||
          !charge(model.textures.size(), sizeof(std::unique_ptr<Texture>)))))
        return preparation_limit();
    for (const formats::Kn5Texture& texture : model.textures) {
        if (!charge(texture.name.size(), sizeof(char))) return preparation_limit();
    }
    for (const DrawPacket& packet : request.packets) {
        if (!charge(packet.bone_palette.size(), sizeof(apex::scene::Matrix4)) ||
            !charge(packet.resources.size(), sizeof(DrawResourceSlot)) ||
            !charge(packet.material_profile.shader.size(), sizeof(char)) ||
            !charge(packet.material_profile.blend_source.size(), sizeof(char)) ||
            !charge(packet.material_profile.blend.size(), sizeof(char)) ||
            !charge(packet.material_profile.blend_mode.size(), sizeof(char)) ||
            !charge(packet.material_profile.depth_mode.size(), sizeof(char)) ||
            !charge(packet.material_profile.cull_source.size(), sizeof(char)))
            return preparation_limit();
        for (const DrawResourceSlot& resource : packet.resources) {
            if (!charge(resource.slot.size(), sizeof(char)) ||
                !charge(resource.texture.size(), sizeof(char)))
                return preparation_limit();
        }
    }
    const std::size_t node_map_work_capacity =
        std::min<std::size_t>(limits.node_map.max_work_items, 1024U);
    if (!charge(scene.nodes.size(), sizeof(const formats::Kn5Node*)) ||
        !charge(node_map_work_capacity, 2U * sizeof(std::size_t)) ||
        !charge(request.packets.size(), sizeof(PipelineProgram)) ||
        !charge(std::min(request.packets.size(),
                         limits.max_material_constant_buffers),
                sizeof(KsPerPixelMaterialConstants)) ||
        !charge(std::min({request.packets.size(), model.materials.size(),
                          limits.max_material_constant_buffers}),
                sizeof(StockShadowCasterMaterialConstants)) ||
        !charge(std::min({request.packets.size(), model.materials.size(),
                          limits.max_material_constant_buffers}),
                sizeof(std::unique_ptr<Buffer>)))
        return preparation_limit();

    std::map<TextureScopeKey, std::optional<std::uint32_t>> unique_texture_indices;
    for (std::size_t texture_index = 0U; texture_index < model.textures.size();
         ++texture_index) {
        const auto& texture = model.textures[texture_index];
        if (texture.name.size() > limits.max_resource_string_bytes)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_resource_string_limit",
                        "A KN5 texture name exceeds the static-scene resource string limit");
        const std::string key = canonical_texture_name(texture.name);
        if (key.empty()) continue;
        const auto [found, inserted] = unique_texture_indices.emplace(
            TextureScopeKey{texture.workspaceFileIndex, key},
            static_cast<std::uint32_t>(texture_index));
        if (!inserted) found->second.reset();
    }

    const Kn5SceneNodeMapResult node_map =
        map_kn5_scene_nodes(model.root, scene, limits.node_map);
    if (!node_map.ok())
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_" + node_map.diagnostic.code,
                    node_map.diagnostic.message);

    if (!charge(node_map.source_nodes.size(), sizeof(std::size_t)) ||
        !charge(request.packets.size(),
                2U * sizeof(std::unique_ptr<StaticMeshUpload>)) ||
        !charge(model.materials.size(), sizeof(std::unique_ptr<Buffer>)))
        return preparation_limit();

    std::vector<std::size_t> upload_by_node(node_map.source_nodes.size(), invalid_resource_index);
    std::vector<std::size_t> pipeline_by_material(model.materials.size(), invalid_resource_index);
    std::vector<std::size_t> upload_for_packet(request.packets.size(), invalid_resource_index);
    std::vector<std::size_t> skinned_upload_for_packet(request.packets.size(), invalid_resource_index);
    std::vector<std::size_t> pipeline_for_packet(request.packets.size(), invalid_resource_index);
    std::vector<std::uint32_t> stock_vulkan_source_program_for_packet(
        request.packets.size(), invalid_static_scene_source_program_index);
    if (has_stock_vulkan_source_table)
        stock_vulkan_source_program_for_packet.assign(
            request.stock_vulkan_source_program_by_packet.begin(),
            request.stock_vulkan_source_program_by_packet.end());
    std::vector<bool> stock_vulkan_source_program_used(
        request.stock_vulkan_source_programs.size(), false);
    std::vector<std::uint32_t> stock_d3d12_native_program_for_packet(
        request.packets.size(), invalid_static_scene_source_program_index);
    if (has_stock_d3d12_native_table)
        stock_d3d12_native_program_for_packet.assign(
            request.stock_d3d12_native_program_by_packet.begin(),
            request.stock_d3d12_native_program_by_packet.end());
    std::vector<bool> stock_d3d12_native_program_used(
        request.stock_d3d12_native_programs.size(), false);
    std::map<const PipelineProgram*, std::size_t> packet_pipeline_indices;
    std::vector<StaticSceneResources::PacketTextureIndices> textures_for_packet(
        request.packets.size());
    std::vector<std::size_t> material_constant_by_material(model.materials.size(),
                                                           invalid_resource_index);
    std::vector<std::size_t> material_constant_for_packet(request.packets.size(),
                                                          invalid_resource_index);
    std::vector<std::size_t> multimap_reflection_constant_by_material(
        model.materials.size(), invalid_resource_index);
    std::vector<std::size_t> multimap_reflection_constant_for_packet(
        request.packets.size(), invalid_resource_index);
    std::vector<std::size_t> stock_shadow_constant_for_material(
        model.materials.size(), invalid_resource_index);
    std::vector<const formats::Kn5Node*> unique_meshes;
    std::vector<std::size_t> representative_packets;
    std::vector<const formats::Kn5Node*> skinned_meshes;
    std::vector<std::size_t> skinned_packet_indices;
    std::vector<PipelineProgram> pipelines;
    std::vector<KsPerPixelMaterialConstants> material_constants;
    std::vector<KsPerPixelMultiMapReflectionConstants>
        multimap_reflection_constants;
    std::vector<StockShadowCasterMaterialConstants> stock_shadow_constants;
    unique_meshes.reserve(request.packets.size());
    representative_packets.reserve(request.packets.size());
    skinned_meshes.reserve(request.packets.size());
    skinned_packet_indices.reserve(request.packets.size());
    pipelines.reserve(request.packets.size());
    material_constants.reserve(std::min(request.packets.size(),
                                        limits.max_material_constant_buffers));
    multimap_reflection_constants.reserve(std::min(
        request.packets.size(), limits.max_material_constant_buffers));
    stock_shadow_constants.reserve(std::min(request.packets.size(),
                                            limits.max_material_constant_buffers));

    std::uint64_t total_vertex_bytes = 0U;
    std::uint64_t total_index_bytes = 0U;
    std::uint64_t total_shader_bytes = 0U;
    std::uint64_t total_material_constant_bytes = 0U;
    std::uint64_t total_multimap_reflection_constant_bytes = 0U;
    std::uint64_t total_shadow_material_constant_bytes = 0U;
    std::uint64_t total_stock_native_constant_bytes = 0U;
    std::uint64_t total_update_bytes = 0U;
    std::uint64_t validation_bytes = 0U;
    std::optional<bool> batch_has_depth;
    std::optional<PipelineRenderTargetFormat> batch_color_format;
    std::optional<std::uint32_t> batch_color_samples;
    bool requires_frame_constants = false;
    bool requires_directional_shadow_receiver = false;
    for (std::size_t packet_index = 0U; packet_index < request.packets.size(); ++packet_index) {
        const DrawPacket& packet = request.packets[packet_index];
        const auto node_index = static_cast<std::size_t>(packet.node);
        const auto material_index = static_cast<std::size_t>(packet.material);
        const bool static_mesh = packet.primitive == DrawPrimitiveKind::static_mesh;
        const bool skinned_mesh = packet.primitive == DrawPrimitiveKind::skinned_mesh;
        if (!static_mesh && !skinned_mesh)
            return fail(StaticSceneResourceStatus::unsupported,
                        "static_scene_packet_unsupported",
                        "Static-scene execution supports static or skinned meshes");
        if ((static_mesh && !packet.bone_palette.empty()) ||
            (skinned_mesh && packet.bone_palette.empty()))
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_packet_palette_invalid",
                        "Static and skinned packets must have the corresponding bone-palette state");
        if (packet.shadow_only && !packet.flags.cast_shadows)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_shadow_only_authority_invalid",
                        "A shadow-only packet must retain cast-shadow authority");
        if (node_index >= node_map.source_nodes.size() || material_index >= model.materials.size())
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_packet_identity_invalid",
                        "A static-scene packet has an invalid node or material ID");
        const formats::Kn5Node& mesh = *node_map.source_nodes[node_index];
        if (scene.nodes[node_index].material != packet.material || mesh.materialId != packet.material)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_packet_material_mismatch",
                        "A static-scene packet material does not match its source node");
        std::uint64_t packet_vertex_bytes = 0U;
        std::uint64_t packet_index_bytes = 0U;
        if (!checked_bytes(mesh.vertices.size(), sizeof(float), packet_vertex_bytes) ||
            !checked_bytes(mesh.indices.size(), sizeof(std::uint16_t), packet_index_bytes) ||
            !checked_add(packet_vertex_bytes, validation_bytes) ||
            !checked_add(packet_index_bytes, validation_bytes) ||
            validation_bytes > limits.max_validation_bytes)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_validation_work_limit",
                        "Static-scene source validation exceeds its byte-work limit");
        Diagnostic mesh_diagnostic;
        if (static_mesh) {
            const StaticMeshUploadStatus mesh_status =
                validate_static_mesh_upload(mesh, packet, limits.mesh, mesh_diagnostic);
            if (mesh_status != StaticMeshUploadStatus::ready)
                return {map_upload_status(mesh_status), std::move(mesh_diagnostic), nullptr};
        } else {
            const SkinnedMeshUploadStatus mesh_status =
                validate_skinned_mesh_upload(mesh, packet, limits.skinned, mesh_diagnostic);
            if (mesh_status != SkinnedMeshUploadStatus::ready)
                return {map_skinned_upload_status(mesh_status), std::move(mesh_diagnostic), nullptr};
        }

        if (static_mesh && upload_by_node[node_index] == invalid_resource_index) {
            std::uint64_t vertex_bytes = 0U;
            std::uint64_t index_bytes = 0U;
            if (!checked_bytes(mesh.vertices.size(), sizeof(float), vertex_bytes) ||
                !checked_bytes(mesh.indices.size(), sizeof(std::uint16_t), index_bytes) ||
                !checked_add(vertex_bytes, total_vertex_bytes) ||
                !checked_add(index_bytes, total_index_bytes) ||
                total_vertex_bytes > limits.max_total_vertex_bytes ||
                total_index_bytes > limits.max_total_index_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_geometry_aggregate_limit",
                            "Unique static-scene geometry exceeds aggregate byte limits");
            upload_by_node[node_index] = unique_meshes.size();
            unique_meshes.push_back(&mesh);
            representative_packets.push_back(packet_index);
        }
        if (static_mesh) {
            upload_for_packet[packet_index] = upload_by_node[node_index];
        } else {
            if (!checked_add(packet_vertex_bytes, total_update_bytes) ||
                total_update_bytes > limits.max_total_update_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_skinning_update_aggregate_limit",
                            "Skinned frame updates exceed the aggregate byte limit");
            skinned_upload_for_packet[packet_index] = skinned_meshes.size();
            skinned_meshes.push_back(&mesh);
            skinned_packet_indices.push_back(packet_index);
            if (!checked_add(packet_vertex_bytes, total_vertex_bytes) ||
                !checked_add(packet_index_bytes, total_index_bytes) ||
                total_vertex_bytes > limits.max_total_vertex_bytes ||
                total_index_bytes > limits.max_total_index_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_geometry_aggregate_limit",
                            "Unique static-scene geometry exceeds aggregate byte limits");
        }

        const PipelineProgram* pipeline = packet_pipeline_table
                                              ? request.pipelines_by_packet[packet_index]
                                              : request.pipelines_by_material[material_index];
        if (pipeline == nullptr)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_material_pipeline_missing",
                        "A used static-scene material has no executable pipeline");
        const std::uint32_t raw_source_program =
            stock_vulkan_source_program_for_packet[packet_index];
        const bool stock_vulkan_source =
            raw_source_program != invalid_static_scene_source_program_index;
        const std::uint32_t raw_d3d12_native_program =
            stock_d3d12_native_program_for_packet[packet_index];
        const bool stock_d3d12_native =
            raw_d3d12_native_program !=
            invalid_static_scene_source_program_index;
        const bool stock_native =
            stock_vulkan_source || stock_d3d12_native;
        const std::shared_ptr<const
            ValidatedStockKsPerPixelVulkanSourceProgram>* source_program =
            stock_vulkan_source
                ? &request.stock_vulkan_source_programs[
                      static_cast<std::size_t>(raw_source_program)]
                : nullptr;
        if (stock_vulkan_source) {
            const bool material_base = canonical_resource_equals(
                model.materials[material_index].shader, "ksperpixel");
            const bool material_at = canonical_resource_equals(
                model.materials[material_index].shader, "ksperpixelat");
            const bool profile_base = canonical_resource_equals(
                packet.material_profile.shader, "ksperpixel");
            const bool profile_at = canonical_resource_equals(
                packet.material_profile.shader, "ksperpixelat");
            if (!static_mesh ||
                !((material_base && profile_base) ||
                  (material_at && profile_at)))
                return fail(
                    StaticSceneResourceStatus::unsupported,
                    "static_scene_stock_vulkan_source_material_unsupported",
                    "Vulkan source-equivalent execution supports only resolved static ksPerPixel and ksPerPixelAT packets");
            if (source_program == nullptr || *source_program == nullptr ||
                &(*source_program)->pipeline() != pipeline ||
                (*source_program)->validation_status() !=
                    StockKsPerPixelVulkanSourceStatus::ready)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_vulkan_source_owner_mismatch",
                    "A Vulkan source-equivalent packet must use its retained owner's exact validated pipeline");
            const bool expected_a2c =
                (*source_program)->variant() ==
                StockKsPerPixelVariant::alpha_to_coverage;
            if (packet.flags.alpha_to_coverage != expected_a2c ||
                pipeline->blend.alpha_to_coverage != expected_a2c)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_vulkan_source_variant_mismatch",
                    "The retained Vulkan source variant must match packet and pipeline A2C state");
            if (!valid_stock_ks_per_pixel_material_constants(
                    request.stock_vulkan_source_material_constants_by_material[
                        material_index]))
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_vulkan_source_material_non_finite",
                    "A retained Vulkan source material record contains non-finite values");
        }
        if (stock_d3d12_native) {
            const auto& native_program =
                request.stock_d3d12_native_programs[
                    static_cast<std::size_t>(raw_d3d12_native_program)];
            const bool native_alpha_to_coverage =
                native_program != nullptr &&
                native_program->variant() ==
                    StockKsPerPixelVariant::alpha_to_coverage;
            const std::string_view native_shader_key =
                native_alpha_to_coverage ? "ksperpixelat" : "ksperpixel";
            if (!static_mesh ||
                !canonical_resource_equals(
                    model.materials[material_index].shader,
                    native_shader_key) ||
                !canonical_resource_equals(
                    packet.material_profile.shader, native_shader_key) ||
                packet.flags.transparent || packet.flags.blend_enabled ||
                packet.flags.wireframe ||
                native_program == nullptr ||
                packet.flags.alpha_to_coverage != native_alpha_to_coverage ||
                pipeline->blend.alpha_to_coverage !=
                    native_alpha_to_coverage)
                return fail(
                    StaticSceneResourceStatus::unsupported,
                    "static_scene_stock_d3d12_native_material_unsupported",
                    "The installed D3D12 native batch supports only matching opaque solid static ksPerPixel variants");
            if (pipeline->resources.size() != 0U ||
                pipeline->shaders.size() != 2U)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_d3d12_native_pipeline_invalid",
                    "An installed D3D12 native pipeline must contain two exact DXBC stages and no portable resources");
            bool vertex_matches = false;
            bool fragment_matches = false;
            for (const PipelineShaderModule& module : pipeline->shaders) {
                if (module.format != PipelineShaderFormat::dxbc) continue;
                const auto matches = [](const std::vector<std::uint8_t>& left,
                                        std::span<const std::uint8_t> right) {
                    return left.size() == right.size() &&
                           std::equal(left.begin(), left.end(), right.begin());
                };
                vertex_matches =
                    vertex_matches ||
                    (module.stage == PipelineShaderStage::vertex &&
                     matches(module.bytes,
                             native_program->vertex_shader()));
                fragment_matches =
                    fragment_matches ||
                    (module.stage == PipelineShaderStage::fragment &&
                     matches(module.bytes,
                             native_program->pixel_shader()));
            }
            if (!vertex_matches || !fragment_matches)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_d3d12_native_owner_mismatch",
                    "The D3D12 native packet pipeline must preserve its validated owner's exact DXBC stages");
            if (!valid_stock_ks_per_pixel_material_constants(
                    request.stock_d3d12_native_material_constants_by_material[
                        material_index]))
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_d3d12_native_material_non_finite",
                    "A retained D3D12 native material record contains non-finite values");
        }
        const ExecutorPipelineValidation pipeline_validation = validate_executor_pipeline(
            device.info().backend, *pipeline, packet, limits.pipeline,
            stock_native);
        if (!pipeline_validation.ok())
            return fail(pipeline_validation.status,
                        pipeline_validation.status == StaticSceneResourceStatus::invalid_request
                            ? "static_scene_material_pipeline_invalid"
                            : "static_scene_material_pipeline_unsupported",
                        pipeline_validation.error);
        const IndexedPortableResourceLayout material_layout =
            stock_native
                ? IndexedPortableResourceLayout::resource_free
                : classify_indexed_portable_resource_layout(*pipeline);
        requires_directional_shadow_receiver =
            requires_directional_shadow_receiver ||
            (!stock_native &&
             pipeline_declares_directional_shadow_receiver(*pipeline));
        const bool pipeline_has_multimap_reflection =
            !stock_native && pipeline_declares_multimap_reflection(*pipeline);
        const bool normal_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame;
        const bool maps_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame;
        const bool detail_stack_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame;
        const bool damage_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame;
        const bool damage_dust_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        if (stock_native || !pipeline->resources.empty()) {
            const std::size_t expected_resources =
                (detail_stack_layout || damage_layout) ? 5U
                                                      : damage_dust_layout ? 6U
                                                      : maps_layout ? 3U : normal_layout ? 2U : 1U;
            if (packet.resources.size() != expected_resources)
                return fail(StaticSceneResourceStatus::unsupported,
                            "static_scene_material_packet_unsupported",
                            damage_dust_layout
                                ? "The portable damage-dust path requires txDiffuse, txNormal, txMaps, txDust, txDamage, and txDamageMask"
                                : damage_layout
                                ? "The portable damage path requires txDiffuse, txNormal, txMaps, txDamage, and txDamageMask"
                                : detail_stack_layout
                                ? "The portable detail-stack path requires txDiffuse, txNormal, txMaps, txDetail, and txNormalDetail packet resources"
                                : maps_layout
                                ? "The portable txMaps path requires txDiffuse, txNormal, and txMaps packet resources"
                                : normal_layout
                                    ? "The portable ksPerPixelNM path requires txDiffuse and txNormal packet resources"
                                    : stock_native
                                        ? "Native ksPerPixel requires one txDiffuse packet resource"
                                        : "The portable diffuse path requires one txDiffuse packet resource");
            for (const DrawResourceSlot& resource : packet.resources) {
                if (resource.slot.size() > limits.max_resource_string_bytes ||
                    resource.texture.size() > limits.max_resource_string_bytes)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_resource_string_limit",
                                "A static-scene resource string exceeds its byte limit");
                const bool diffuse = canonical_resource_equals(resource.slot, "txdiffuse");
                const bool normal = (normal_layout || maps_layout || detail_stack_layout ||
                                     damage_layout || damage_dust_layout) &&
                                    canonical_resource_equals(resource.slot, "txnormal");
                const bool maps = (maps_layout || detail_stack_layout || damage_layout ||
                                   damage_dust_layout) &&
                                  canonical_resource_equals(resource.slot, "txmaps");
                const bool detail = detail_stack_layout &&
                                    canonical_resource_equals(resource.slot, "txdetail");
                const bool normal_detail =
                    detail_stack_layout &&
                    (canonical_resource_equals(resource.slot, "txnormaldetail") ||
                     canonical_resource_equals(resource.slot, "txdetailnm"));
                const bool damage = (damage_layout || damage_dust_layout) &&
                                    canonical_resource_equals(resource.slot, "txdamage");
                const bool damage_mask = (damage_layout || damage_dust_layout) &&
                                         canonical_resource_equals(resource.slot, "txdamagemask");
                const bool dust = damage_dust_layout &&
                                  canonical_resource_equals(resource.slot, "txdust");
                if (!diffuse && !normal && !maps && !detail && !normal_detail &&
                    !damage && !damage_mask && !dust)
                    return fail(StaticSceneResourceStatus::unsupported,
                                "static_scene_material_packet_unsupported",
                                damage_dust_layout
                                    ? "The portable damage-dust path accepts txDiffuse, txNormal, txMaps, txDust, txDamage, and txDamageMask"
                                    : damage_layout
                                    ? "The portable damage path accepts txDiffuse, txNormal, txMaps, txDamage, and txDamageMask"
                                    : detail_stack_layout
                                    ? "The portable detail-stack path accepts only txDiffuse, txNormal, txMaps, txDetail, and txNormalDetail"
                                    : maps_layout
                                    ? "The portable txMaps path accepts only txDiffuse, txNormal, and txMaps"
                                    : normal_layout
                                        ? "The portable ksPerPixelNM path accepts only txDiffuse and txNormal"
                                        : stock_native
                                            ? "Native ksPerPixel accepts only txDiffuse"
                                            : "The portable diffuse path accepts only txDiffuse");
                std::uint32_t& stored_index = diffuse
                                                  ? textures_for_packet[packet_index].diffuse
                                                  : normal
                                                      ? textures_for_packet[packet_index].normal
                                                      : maps
                                                          ? textures_for_packet[packet_index].maps
                                                          : detail
                                                              ? textures_for_packet[packet_index].detail
                                                              : normal_detail
                                                                  ? textures_for_packet[packet_index].normal_detail
                                                                  : damage
                                                                      ? textures_for_packet[packet_index].damage
                                                                      : damage_mask
                                                                          ? textures_for_packet[packet_index].damage_mask
                                                                          : textures_for_packet[packet_index].dust;
                if (stored_index != invalid_draw_texture_index)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_resource_slot_duplicate",
                                "A portable static-scene texture slot is duplicated");
                if (resource.texture_index == invalid_draw_texture_index ||
                    static_cast<std::size_t>(resource.texture_index) >= model.textures.size())
                    return fail(StaticSceneResourceStatus::invalid_request,
                                diffuse ? "static_scene_diffuse_texture_index_invalid"
                                        : normal ? "static_scene_normal_texture_index_invalid"
                                                 : maps ? "static_scene_maps_texture_index_invalid"
                                                        : detail ? "static_scene_detail_texture_index_invalid"
                                                                 : normal_detail ? "static_scene_normal_detail_texture_index_invalid"
                                                                 : damage ? "static_scene_damage_texture_index_invalid"
                                                                 : damage_mask ? "static_scene_damage_mask_texture_index_invalid"
                                                                               : "static_scene_dust_texture_index_invalid",
                                diffuse ? "A txDiffuse resource has an invalid global texture index"
                                        : normal ? "A txNormal resource has an invalid global texture index"
                                                 : maps ? "A txMaps resource has an invalid global texture index"
                                                        : detail ? "A txDetail resource has an invalid global texture index"
                                                                 : normal_detail ? "A txNormalDetail resource has an invalid global texture index"
                                                                 : damage ? "A txDamage resource has an invalid global texture index"
                                                                 : damage_mask ? "A txDamageMask resource has an invalid global texture index"
                                                                               : "A txDust resource has an invalid global texture index");
                const formats::Kn5Texture& source_texture =
                    model.textures[static_cast<std::size_t>(resource.texture_index)];
                if (source_texture.name.size() > limits.max_resource_string_bytes ||
                    !checked_add(static_cast<std::uint64_t>(resource.slot.size()), validation_bytes) ||
                    !checked_add(static_cast<std::uint64_t>(resource.texture.size()), validation_bytes) ||
                    !checked_add(static_cast<std::uint64_t>(source_texture.name.size()), validation_bytes) ||
                    validation_bytes > limits.max_validation_bytes)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_validation_work_limit",
                                "Static-scene resource validation exceeds its byte-work limit");
                const auto unique_texture = unique_texture_indices.find(
                    TextureScopeKey{source_texture.workspaceFileIndex,
                                    canonical_texture_name(resource.texture)});
                if (!source_texture.active || resource.texture.empty() ||
                    resource.texture != source_texture.name ||
                    source_texture.workspaceFileIndex !=
                        model.materials[material_index].workspaceFileIndex ||
                    unique_texture == unique_texture_indices.end() ||
                    !unique_texture->second.has_value() ||
                    *unique_texture->second != resource.texture_index)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                diffuse ? "static_scene_diffuse_texture_identity_invalid"
                                        : normal ? "static_scene_normal_texture_identity_invalid"
                                                 : maps ? "static_scene_maps_texture_identity_invalid"
                                                        : detail ? "static_scene_detail_texture_identity_invalid"
                                                                 : normal_detail ? "static_scene_normal_detail_texture_identity_invalid"
                                                                 : damage ? "static_scene_damage_texture_identity_invalid"
                                                                 : damage_mask ? "static_scene_damage_mask_texture_identity_invalid"
                                                                               : "static_scene_dust_texture_identity_invalid",
                                diffuse
                                    ? "A txDiffuse resource does not match its unique active scoped KN5 texture"
                                    : normal
                                        ? "A txNormal resource does not match its unique active scoped KN5 texture"
                                        : maps
                                            ? "A txMaps resource does not match its unique active scoped KN5 texture"
                                            : detail
                                                ? "A txDetail resource does not match its unique active scoped KN5 texture"
                                                : normal_detail
                                                    ? "A txNormalDetail resource does not match its unique active scoped KN5 texture"
                                                    : damage
                                                        ? "A txDamage resource does not match its unique active scoped KN5 texture"
                                                        : damage_mask
                                                            ? "A txDamageMask resource does not match its unique active scoped KN5 texture"
                                                            : "A txDust resource does not match its unique active scoped KN5 texture");
                stored_index = resource.texture_index;
            }
            if (textures_for_packet[packet_index].diffuse == invalid_draw_texture_index ||
                ((normal_layout || maps_layout || detail_stack_layout || damage_layout || damage_dust_layout) &&
                 textures_for_packet[packet_index].normal == invalid_draw_texture_index) ||
                ((maps_layout || detail_stack_layout || damage_layout || damage_dust_layout) && textures_for_packet[packet_index].maps ==
                                    invalid_draw_texture_index) ||
                (detail_stack_layout &&
                 (textures_for_packet[packet_index].detail == invalid_draw_texture_index ||
                  textures_for_packet[packet_index].normal_detail == invalid_draw_texture_index)) ||
                (damage_layout &&
                 (textures_for_packet[packet_index].damage == invalid_draw_texture_index ||
                  textures_for_packet[packet_index].damage_mask == invalid_draw_texture_index)) ||
                (damage_dust_layout &&
                 (textures_for_packet[packet_index].dust == invalid_draw_texture_index ||
                  textures_for_packet[packet_index].damage == invalid_draw_texture_index ||
                  textures_for_packet[packet_index].damage_mask == invalid_draw_texture_index)))
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_resource_slot_missing",
                            damage_dust_layout
                                ? "The portable damage-dust resources are incomplete"
                                : damage_layout
                                ? "The portable damage resources are incomplete"
                                : detail_stack_layout
                                ? "The portable detail-stack resources are incomplete"
                                : maps_layout
                                ? "The portable txMaps resources are incomplete"
                                : normal_layout
                                    ? "The portable ksPerPixelNM resources are incomplete"
                                    : "The portable diffuse resource is missing");
        }
        if (material_layout == IndexedPortableResourceLayout::diffuse_with_frame ||
            material_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            normal_layout || maps_layout || detail_stack_layout || damage_layout || damage_dust_layout)
            requires_frame_constants = true;
        if (material_layout == IndexedPortableResourceLayout::diffuse_with_constants ||
            material_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            normal_layout || maps_layout || detail_stack_layout || damage_layout || damage_dust_layout) {
            if (limits.max_material_constant_buffers == 0U ||
                limits.max_total_material_constant_bytes == 0U)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_material_constant_limits_invalid",
                            "Material-constant limits must be nonzero when a used pipeline declares constants");
            if (request.material_constants_by_material.size() != model.materials.size())
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_material_constant_table_invalid",
                            "The material-constant table must match the final material table");
            if (material_constant_by_material[material_index] == invalid_resource_index) {
                if (material_constants.size() >= limits.max_material_constant_buffers)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_material_constant_count_limit",
                                "Used materials exceed the constant-buffer count limit");
                if (!checked_add(portable_material_buffer_view_bytes,
                                 total_material_constant_bytes) ||
                    total_material_constant_bytes >
                        limits.max_total_material_constant_bytes)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_material_constant_aggregate_limit",
                                "Used material constants exceed the aggregate buffer limit");
                const KsPerPixelMaterialConstants& constants =
                    request.material_constants_by_material[material_index];
                if (!finite_material_constants(constants))
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_material_constant_non_finite",
                                "A used material constant contains a non-finite value");
                if ((normal_layout || maps_layout || detail_stack_layout || damage_layout ||
                     damage_dust_layout) && !pipeline_has_multimap_reflection &&
                    constants.fresnel[2] > 0.0F)
                    return fail(StaticSceneResourceStatus::unsupported,
                                "static_scene_normal_fresnel_unsupported",
                                "The bounded ksPerPixelNM path requires disabled Fresnel reflection");
                material_constant_by_material[material_index] = material_constants.size();
                material_constants.push_back(constants);
            }
            material_constant_for_packet[packet_index] =
                material_constant_by_material[material_index];
        }
        if (pipeline_has_multimap_reflection) {
            if (!maps_layout && !detail_stack_layout)
                return fail(
                    StaticSceneResourceStatus::unsupported,
                    "static_scene_multimap_reflection_layout_unsupported",
                    "Portable MultiMap reflection requires a bounded txMaps material layout");
            if (limits.max_material_constant_buffers == 0U ||
                limits.max_total_multimap_reflection_constant_bytes == 0U)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_multimap_reflection_constant_limits_invalid",
                    "MultiMap reflection-constant limits must be nonzero when a used pipeline declares reflection");
            if (request.multimap_reflection_constants_by_material.size() !=
                model.materials.size())
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_multimap_reflection_constant_table_invalid",
                    "The MultiMap reflection-constant table must match the final material table");
            if (multimap_reflection_constant_by_material[material_index] ==
                invalid_resource_index) {
                if (multimap_reflection_constants.size() >=
                    limits.max_material_constant_buffers)
                    return fail(
                        StaticSceneResourceStatus::invalid_request,
                        "static_scene_multimap_reflection_constant_count_limit",
                        "Used MultiMap reflection materials exceed the constant-buffer count limit");
                if (!checked_add(
                        portable_multimap_reflection_buffer_view_bytes,
                        total_multimap_reflection_constant_bytes) ||
                    total_multimap_reflection_constant_bytes >
                        limits.max_total_multimap_reflection_constant_bytes)
                    return fail(
                        StaticSceneResourceStatus::invalid_request,
                        "static_scene_multimap_reflection_constant_aggregate_limit",
                        "Used MultiMap reflection constants exceed the aggregate buffer limit");
                const KsPerPixelMultiMapReflectionConstants& constants =
                    request.multimap_reflection_constants_by_material[
                        material_index];
                if (!finite_multimap_reflection_constants(constants))
                    return fail(
                        StaticSceneResourceStatus::invalid_request,
                        "static_scene_multimap_reflection_constant_non_finite",
                        "A used MultiMap reflection constant contains a non-finite value");
                multimap_reflection_constant_by_material[material_index] =
                    multimap_reflection_constants.size();
                multimap_reflection_constants.push_back(constants);
            }
            multimap_reflection_constant_for_packet[packet_index] =
                multimap_reflection_constant_by_material[material_index];
        }
        const PipelineRenderTargetFormat color_format = pipeline->targets.colors.front().format;
        if (batch_color_format.has_value() && *batch_color_format != color_format)
            return fail(StaticSceneResourceStatus::unsupported,
                        "static_scene_mixed_color_targets_unsupported",
                        "One ordered static-scene batch requires one color-target format");
        batch_color_format = color_format;
        const std::uint32_t color_samples = pipeline->targets.colors.front().samples;
        if (batch_color_samples.has_value() && *batch_color_samples != color_samples)
            return fail(StaticSceneResourceStatus::unsupported,
                        "static_scene_mixed_color_samples_unsupported",
                        "One ordered static-scene batch requires one color-target sample count");
        batch_color_samples = color_samples;

        if (packet.material_profile.shadow_alpha_tested &&
            !request.stock_shadow_constants_by_material.empty() &&
            stock_shadow_constant_for_material[material_index] == invalid_resource_index) {
            if (limits.max_material_constant_buffers == 0U ||
                limits.max_total_material_constant_bytes == 0U)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_shadow_material_constant_limits_invalid",
                            "Stock shadow material-constant limits must be nonzero for alpha casters");
            if (stock_shadow_constants.size() >= limits.max_material_constant_buffers)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_shadow_material_constant_count_limit",
                            "Used alpha materials exceed the constant-buffer count limit");
            if (!checked_add(stock_shadow_caster_buffer_alignment,
                             total_shadow_material_constant_bytes) ||
                total_shadow_material_constant_bytes >
                    limits.max_total_material_constant_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_shadow_material_constant_aggregate_limit",
                            "Used alpha shadow constants exceed the aggregate buffer limit");
            const auto& constants =
                request.stock_shadow_constants_by_material[material_index];
            if (!finite_stock_shadow_constants(constants))
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_shadow_material_constant_invalid",
                            "A stock shadow alpha constant is non-finite or outside [0, 1]");
            stock_shadow_constant_for_material[material_index] =
                stock_shadow_constants.size();
            stock_shadow_constants.push_back(constants);
        }
        if (stock_native) {
            constexpr std::uint64_t per_draw_constant_bytes =
                static_cast<std::uint64_t>(
                    stock_ks_per_pixel_native_constant_buffer_view_bytes) *
                static_cast<std::uint64_t>(
                    StockKsPerPixelNativeConstantSlot::count);
            if (!checked_add(per_draw_constant_bytes,
                             total_stock_native_constant_bytes) ||
                total_stock_native_constant_bytes >
                    (stock_vulkan_source
                         ? limits
                               .max_total_stock_vulkan_source_constant_bytes
                         : limits
                               .max_total_stock_d3d12_native_constant_bytes))
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    stock_vulkan_source
                        ? "static_scene_stock_vulkan_source_constant_aggregate_limit"
                        : "static_scene_stock_d3d12_native_constant_aggregate_limit",
                    "Retained stock native constants exceed the aggregate limit");
            constexpr std::uint64_t per_draw_update_bytes =
                static_cast<std::uint64_t>(
                    stock_ks_per_pixel_native_constant_buffer_view_bytes) *
                4U;
            if (!checked_add(per_draw_update_bytes, total_update_bytes) ||
                total_update_bytes > limits.max_total_update_bytes)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    stock_vulkan_source
                        ? "static_scene_stock_vulkan_source_update_aggregate_limit"
                        : "static_scene_stock_d3d12_native_update_aggregate_limit",
                    "Retained stock native frame updates exceed the aggregate limit");
            if (batch_has_depth.has_value() &&
                *batch_has_depth != pipeline->targets.has_depth)
                return fail(
                    StaticSceneResourceStatus::unsupported,
                    "static_scene_mixed_depth_targets_unsupported",
                    "One ordered static-scene batch requires one depth-target contract");
            batch_has_depth = pipeline->targets.has_depth;
            if (stock_vulkan_source) {
                const std::size_t source_index =
                    static_cast<std::size_t>(raw_source_program);
                if (!stock_vulkan_source_program_used[source_index]) {
                    if (!checked_add(pipeline_validation.shader_bytes,
                                     total_shader_bytes) ||
                        total_shader_bytes > limits.max_total_shader_bytes)
                        return fail(
                            StaticSceneResourceStatus::invalid_request,
                            "static_scene_shader_aggregate_limit",
                            "Used static-scene pipelines exceed the aggregate shader byte limit");
                    stock_vulkan_source_program_used[source_index] = true;
                }
                continue;
            }
            const std::size_t native_index =
                static_cast<std::size_t>(raw_d3d12_native_program);
            stock_d3d12_native_program_used[native_index] = true;
            // Each selected D3D12 packet receives its own shader/constant/
            // sampler bundle, so charge the cloned shader payload per draw.
            {
                if (!checked_add(pipeline_validation.shader_bytes,
                                 total_shader_bytes) ||
                    total_shader_bytes > limits.max_total_shader_bytes)
                    return fail(
                        StaticSceneResourceStatus::invalid_request,
                        "static_scene_shader_aggregate_limit",
                        "Used static-scene pipelines exceed the aggregate shader byte limit");
            }
        }
        if (packet_pipeline_table) {
            const auto existing_pipeline = packet_pipeline_indices.find(pipeline);
            if (existing_pipeline != packet_pipeline_indices.end()) {
                pipeline_for_packet[packet_index] = existing_pipeline->second;
                continue;
            }
            if (!checked_add(pipeline_validation.shader_bytes, total_shader_bytes) ||
                total_shader_bytes > limits.max_total_shader_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_shader_aggregate_limit",
                            "Used static-scene pipelines exceed the aggregate shader byte limit");
            if (batch_has_depth.has_value() && *batch_has_depth != pipeline->targets.has_depth)
                return fail(StaticSceneResourceStatus::unsupported,
                            "static_scene_mixed_depth_targets_unsupported",
                            "One ordered static-scene batch requires one depth-target contract");
            batch_has_depth = pipeline->targets.has_depth;
            if (!charge(pipeline->name.size(), sizeof(char)) ||
                !charge(pipeline->shaders.size(), sizeof(PipelineShaderModule)) ||
                !charge(pipeline->vertex_layout.attributes.size(),
                        sizeof(PipelineVertexAttribute)) ||
                !charge(pipeline->targets.colors.size(), sizeof(PipelineRenderTarget)) ||
                !charge(pipeline->resources.size(), sizeof(PipelineResourceBinding)))
                return preparation_limit();
            for (const PipelineShaderModule& shader : pipeline->shaders) {
                if (!charge(shader.bytes.size(), sizeof(std::uint8_t)))
                    return preparation_limit();
            }
            for (const PipelineResourceBinding& resource : pipeline->resources) {
                if (!charge(resource.name.size(), sizeof(char)))
                    return preparation_limit();
            }
            pipeline_for_packet[packet_index] = pipelines.size();
            pipelines.push_back(*pipeline);
            packet_pipeline_indices.emplace(pipeline, pipeline_for_packet[packet_index]);
        } else if (pipeline_by_material[material_index] == invalid_resource_index) {
            if (!checked_add(pipeline_validation.shader_bytes, total_shader_bytes) ||
                total_shader_bytes > limits.max_total_shader_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_shader_aggregate_limit",
                            "Used static-scene pipelines exceed the aggregate shader byte limit");
            if (batch_has_depth.has_value() && *batch_has_depth != pipeline->targets.has_depth)
                return fail(StaticSceneResourceStatus::unsupported,
                            "static_scene_mixed_depth_targets_unsupported",
                            "One ordered static-scene batch requires one depth-target contract");
            batch_has_depth = pipeline->targets.has_depth;
            if (!charge(pipeline->name.size(), sizeof(char)) ||
                !charge(pipeline->shaders.size(), sizeof(PipelineShaderModule)) ||
                !charge(pipeline->vertex_layout.attributes.size(),
                        sizeof(PipelineVertexAttribute)) ||
                !charge(pipeline->targets.colors.size(),
                        sizeof(PipelineRenderTarget)) ||
                !charge(pipeline->resources.size(), sizeof(PipelineResourceBinding)))
                return preparation_limit();
            for (const PipelineShaderModule& shader : pipeline->shaders) {
                if (!charge(shader.bytes.size(), sizeof(std::uint8_t)))
                    return preparation_limit();
            }
            for (const PipelineResourceBinding& resource : pipeline->resources) {
                if (!charge(resource.name.size(), sizeof(char)))
                    return preparation_limit();
            }
            pipeline_by_material[material_index] = pipelines.size();
            pipelines.push_back(*pipeline);
        }
        if (!packet_pipeline_table)
            pipeline_for_packet[packet_index] = pipeline_by_material[material_index];
    }

    if (requires_frame_constants &&
        limits.max_total_frame_constant_bytes < portable_frame_buffer_view_bytes)
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_frame_constant_aggregate_limit",
                    "The frame-constant buffer exceeds the static-scene aggregate limit");
    if (requires_directional_shadow_receiver &&
        limits.max_total_directional_shadow_constant_bytes <
            portable_directional_shadow_buffer_view_bytes)
        return fail(
            StaticSceneResourceStatus::invalid_request,
            "static_scene_directional_shadow_constant_aggregate_limit",
            "The directional-shadow receiver buffer exceeds the static-scene aggregate limit");

    std::vector<std::optional<DecodedTexturePlan>> decoded_textures;
    if (owned_model_textures) {
        decoded_textures.resize(model.textures.size());
        std::vector<bool> maps_texture_indices(model.textures.size(), false);
        std::vector<bool> normal_detail_texture_indices(model.textures.size(), false);
        std::vector<bool> damage_mask_texture_indices(model.textures.size(), false);
        for (const StaticSceneResources::PacketTextureIndices& packet_textures :
             textures_for_packet) {
            if (packet_textures.maps != invalid_draw_texture_index)
                maps_texture_indices[static_cast<std::size_t>(packet_textures.maps)] = true;
            if (packet_textures.normal_detail != invalid_draw_texture_index)
                normal_detail_texture_indices[
                    static_cast<std::size_t>(packet_textures.normal_detail)] = true;
            if (packet_textures.damage_mask != invalid_draw_texture_index)
                damage_mask_texture_indices[
                    static_cast<std::size_t>(packet_textures.damage_mask)] = true;
        }
        std::uint64_t total_source_bytes = 0U;
        std::uint64_t total_decoded_bytes = 0U;
        for (const StaticSceneResources::PacketTextureIndices& packet_textures :
             textures_for_packet) {
            for (const std::uint32_t raw_index :
                 {packet_textures.diffuse, packet_textures.normal,
                  packet_textures.maps, packet_textures.detail,
                  packet_textures.normal_detail, packet_textures.dust, packet_textures.damage,
                  packet_textures.damage_mask}) {
                if (raw_index == invalid_draw_texture_index) continue;
                const std::size_t texture_index = static_cast<std::size_t>(raw_index);
                if (decoded_textures[texture_index].has_value()) continue;
                const formats::Kn5Texture& source_texture = model.textures[texture_index];
                if (source_texture.data.empty() ||
                    source_texture.size != source_texture.data.size())
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_embedded_texture_payload_invalid",
                                "A used embedded KN5 texture has missing or inconsistent bytes");
                if (!checked_add(static_cast<std::uint64_t>(source_texture.data.size()),
                                 total_source_bytes) ||
                    total_source_bytes > limits.max_total_texture_source_bytes)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_texture_source_aggregate_limit",
                                "Used embedded KN5 texture bytes exceed the aggregate limit");

                const std::uint64_t remaining_decoded_bytes =
                    limits.max_total_decoded_texture_bytes - total_decoded_bytes;
                apex::core::ParseLimits decode_limits = limits.texture_decode;
                decode_limits.maxOutputBytes = std::min(
                    decode_limits.maxOutputBytes,
                    static_cast<std::size_t>(std::min<std::uint64_t>(
                        remaining_decoded_bytes,
                        std::numeric_limits<std::size_t>::max())));
                DecodedTexturePlanResult decoded = plan_decoded_texture_payload(
                    source_texture.data, source_texture.name, decode_limits);
                if (!decoded.ok()) {
                    if (decoded.diagnostic.code == "output_too_large" &&
                        decode_limits.maxOutputBytes < limits.texture_decode.maxOutputBytes)
                        return fail(StaticSceneResourceStatus::invalid_request,
                                    "static_scene_texture_decode_aggregate_limit",
                                    "Decoded embedded KN5 textures exceed the aggregate limit");
                    const StaticSceneResourceStatus status =
                        decoded.status == TextureUploadStatus::unsupported
                            ? StaticSceneResourceStatus::unsupported
                        : decoded.diagnostic.code == "allocation_failed"
                            ? StaticSceneResourceStatus::allocation_failed
                            : StaticSceneResourceStatus::invalid_request;
                    return fail(status,
                                "static_scene_embedded_texture_" + decoded.diagnostic.code,
                                decoded.diagnostic.source + " at byte " +
                                    std::to_string(decoded.diagnostic.offset) + ": " +
                                    decoded.diagnostic.message);
                }
                if ((maps_texture_indices[texture_index] ||
                     normal_detail_texture_indices[texture_index] ||
                     damage_mask_texture_indices[texture_index]) &&
                    decoded.plan.description.format != TextureFormat::rgba8_unorm &&
                    decoded.plan.description.format != TextureFormat::bgra8_unorm)
                    return fail(StaticSceneResourceStatus::unsupported,
                                maps_texture_indices[texture_index]
                                    ? "static_scene_maps_texture_format_unsupported"
                                    : normal_detail_texture_indices[texture_index]
                                        ? "static_scene_normal_detail_texture_format_unsupported"
                                        : "static_scene_damage_mask_texture_format_unsupported",
                                maps_texture_indices[texture_index]
                                    ? "An embedded txMaps texture must decode to linear RGBA8 or BGRA8"
                                    : normal_detail_texture_indices[texture_index]
                                        ? "An embedded txNormalDetail texture must decode to linear RGBA8 or BGRA8"
                                        : "An embedded txDamageMask texture must decode to linear RGBA8 or BGRA8");
                if (!charge(decoded.plan.levels.size(), sizeof(DecodedTextureLevel)) ||
                    !charge(decoded.plan.levels.size(), sizeof(TextureUpload)))
                    return preparation_limit();
                for (const DecodedTextureLevel& level : decoded.plan.levels) {
                    if (!checked_add(static_cast<std::uint64_t>(level.pixels.size()),
                                     total_decoded_bytes) ||
                        total_decoded_bytes > limits.max_total_decoded_texture_bytes)
                        return fail(StaticSceneResourceStatus::invalid_request,
                                    "static_scene_texture_decode_aggregate_limit",
                                    "Decoded embedded KN5 textures exceed the aggregate limit");
                }
                decoded_textures[texture_index] = std::move(decoded.plan);
            }
        }
    }

    std::vector<std::shared_ptr<const
        ValidatedStockKsPerPixelVulkanSourceProgram>>
        retained_stock_vulkan_source_programs;
    retained_stock_vulkan_source_programs.reserve(
        request.stock_vulkan_source_programs.size());
    std::vector<std::uint32_t> retained_source_program_indices(
        request.stock_vulkan_source_programs.size(),
        invalid_static_scene_source_program_index);
    for (std::size_t index = 0U;
         index < stock_vulkan_source_program_used.size(); ++index) {
        if (!stock_vulkan_source_program_used[index]) continue;
        if (retained_stock_vulkan_source_programs.size() >=
            static_cast<std::size_t>(
                invalid_static_scene_source_program_index))
            return fail(
                StaticSceneResourceStatus::invalid_request,
                "static_scene_stock_vulkan_source_owner_limit",
                "Retained Vulkan source-program owners exceed index capacity");
        retained_source_program_indices[index] =
            static_cast<std::uint32_t>(
                retained_stock_vulkan_source_programs.size());
        retained_stock_vulkan_source_programs.push_back(
            request.stock_vulkan_source_programs[index]);
    }
    for (std::uint32_t& index :
         stock_vulkan_source_program_for_packet) {
        if (index == invalid_static_scene_source_program_index) continue;
        index = retained_source_program_indices[index];
    }

    std::vector<std::shared_ptr<const
        ValidatedStockKsPerPixelNativeProgram>>
        retained_stock_d3d12_native_programs;
    retained_stock_d3d12_native_programs.reserve(
        request.stock_d3d12_native_programs.size());
    std::vector<std::uint32_t> retained_d3d12_native_program_indices(
        request.stock_d3d12_native_programs.size(),
        invalid_static_scene_source_program_index);
    for (std::size_t index = 0U;
         index < stock_d3d12_native_program_used.size(); ++index) {
        if (!stock_d3d12_native_program_used[index]) continue;
        if (retained_stock_d3d12_native_programs.size() >=
            static_cast<std::size_t>(
                invalid_static_scene_source_program_index))
            return fail(
                StaticSceneResourceStatus::invalid_request,
                "static_scene_stock_d3d12_native_owner_limit",
                "Retained D3D12 native-program owners exceed index capacity");
        retained_d3d12_native_program_indices[index] =
            static_cast<std::uint32_t>(
                retained_stock_d3d12_native_programs.size());
        retained_stock_d3d12_native_programs.push_back(
            request.stock_d3d12_native_programs[index]);
    }
    for (std::uint32_t& index :
         stock_d3d12_native_program_for_packet) {
        if (index == invalid_static_scene_source_program_index) continue;
        index = retained_d3d12_native_program_indices[index];
    }

    auto resources = std::make_unique<StaticSceneResources>();
    resources->backend_ = device.info().backend;
    resources->device_ = &device;
    resources->packets_.assign(request.packets.begin(), request.packets.end());
    resources->pipelines_ = std::move(pipelines);
    resources->upload_for_packet_ = std::move(upload_for_packet);
    resources->skinned_upload_for_packet_ = std::move(skinned_upload_for_packet);
    resources->pipeline_for_packet_ = std::move(pipeline_for_packet);
    resources->stock_vulkan_source_programs_ =
        std::move(retained_stock_vulkan_source_programs);
    resources->stock_vulkan_source_program_for_packet_ =
        std::move(stock_vulkan_source_program_for_packet);
    resources->stock_d3d12_native_programs_ =
        std::move(retained_stock_d3d12_native_programs);
    resources->stock_d3d12_native_program_for_packet_ =
        std::move(stock_d3d12_native_program_for_packet);
    resources->textures_for_packet_ = std::move(textures_for_packet);
    resources->material_constant_for_packet_ =
        std::move(material_constant_for_packet);
    resources->multimap_reflection_constant_for_packet_ =
        std::move(multimap_reflection_constant_for_packet);
    resources->stock_shadow_constant_for_material_ =
        std::move(stock_shadow_constant_for_material);
    resources->texture_count_ = model.textures.size();
    resources->texture_authority_ = request.texture_authority;
    for (std::size_t packet_index = 0U;
         packet_index < resources->textures_for_packet_.size(); ++packet_index) {
        const StaticSceneResources::PacketTextureIndices& indices =
            resources->textures_for_packet_[packet_index];
        const bool has_packet_textures =
            indices.diffuse != invalid_draw_texture_index ||
            indices.normal != invalid_draw_texture_index ||
            indices.maps != invalid_draw_texture_index ||
            indices.detail != invalid_draw_texture_index ||
            indices.normal_detail != invalid_draw_texture_index ||
            indices.dust != invalid_draw_texture_index ||
            indices.damage != invalid_draw_texture_index ||
            indices.damage_mask != invalid_draw_texture_index;
        resources->has_texture_resources_ =
            resources->has_texture_resources_ || has_packet_textures;
        resources->has_portable_texture_resources_ =
            resources->has_portable_texture_resources_ ||
            (has_packet_textures &&
             resources->stock_vulkan_source_program_for_packet_[packet_index] ==
                 invalid_static_scene_source_program_index &&
             resources->stock_d3d12_native_program_for_packet_[packet_index] ==
                 invalid_static_scene_source_program_index);
    }
    resources->owned_material_constants_.reserve(material_constants.size());
    for (const KsPerPixelMaterialConstants& constants : material_constants) {
        std::array<std::byte, portable_material_buffer_view_bytes> bytes{};
        std::memcpy(bytes.data(), &constants, sizeof(constants));
        const BufferDescription description{
            bytes.size(), BufferUsage::uniform, BufferMemory::device_local,
            BufferMutability::immutable};
        BufferResult buffer = device.create_buffer(description, bytes);
        if (!buffer.ok())
            return {map_buffer_status(buffer.status), std::move(buffer.diagnostic), nullptr};
        resources->owned_material_constants_.push_back(std::move(buffer.buffer));
    }
    resources->owned_multimap_reflection_constants_.reserve(
        multimap_reflection_constants.size());
    for (const KsPerPixelMultiMapReflectionConstants& constants :
         multimap_reflection_constants) {
        std::array<std::byte,
                   portable_multimap_reflection_buffer_view_bytes>
            bytes{};
        std::memcpy(bytes.data(), &constants, sizeof(constants));
        const BufferDescription description{
            bytes.size(), BufferUsage::uniform, BufferMemory::device_local,
            BufferMutability::immutable};
        BufferResult buffer = device.create_buffer(description, bytes);
        if (!buffer.ok())
            return {map_buffer_status(buffer.status),
                    std::move(buffer.diagnostic), nullptr};
        resources->owned_multimap_reflection_constants_.push_back(
            std::move(buffer.buffer));
    }
    resources->owned_stock_shadow_constants_.reserve(stock_shadow_constants.size());
    for (const StockShadowCasterMaterialConstants& constants : stock_shadow_constants) {
        std::array<std::byte, stock_shadow_caster_buffer_alignment> bytes{};
        std::memcpy(bytes.data(), &constants, sizeof(constants));
        const BufferDescription description{
            bytes.size(), BufferUsage::uniform, BufferMemory::device_local,
            BufferMutability::immutable};
        BufferResult buffer = device.create_buffer(description, bytes);
        if (!buffer.ok())
            return {map_buffer_status(buffer.status), std::move(buffer.diagnostic), nullptr};
        resources->owned_stock_shadow_constants_.push_back(std::move(buffer.buffer));
    }
    resources->stock_vulkan_source_constants_for_packet_.resize(
        resources->packets_.size());
    if (has_stock_vulkan_source) {
        for (std::size_t packet_index = 0U;
             packet_index < resources->packets_.size(); ++packet_index) {
            if (resources->stock_vulkan_source_program_for_packet_[packet_index] ==
                invalid_static_scene_source_program_index)
                continue;
            StockKsPerPixelNativeConstantData constants;
            constants.camera = make_stock_ks_per_pixel_camera_constants(
                apex::scene::identity_matrix, apex::scene::identity_matrix,
                apex::scene::identity_matrix, {0.0F, 0.0F, 0.0F}, 0.1F,
                100.0F, 1.0F);
            constants.object = make_stock_ks_per_pixel_object_constants(
                resources->packets_[packet_index].world_matrix);
            constants.shadow_maps.shadow_matrices.fill(
                apex::scene::identity_matrix);
            constants.shadow_maps.texture_size = 1.0F;
            constants.material =
                request.stock_vulkan_source_material_constants_by_material[
                    static_cast<std::size_t>(
                        resources->packets_[packet_index].material)];
            StockKsPerPixelNativeConstantBufferResult allocated =
                allocate_stock_ks_per_pixel_native_constant_buffers(
                    device, constants);
            if (!allocated.ok())
                return {allocated.status ==
                                StockKsPerPixelNativeConstantBufferStatus::
                                    backend_unsupported
                            ? StaticSceneResourceStatus::unsupported
                        : allocated.status ==
                                  StockKsPerPixelNativeConstantBufferStatus::
                                      invalid_constants ||
                              allocated.status ==
                                  StockKsPerPixelNativeConstantBufferStatus::
                                      invalid_buffer
                            ? StaticSceneResourceStatus::invalid_request
                        : allocated.status ==
                                  StockKsPerPixelNativeConstantBufferStatus::
                                      allocation_failed
                            ? StaticSceneResourceStatus::allocation_failed
                            : StaticSceneResourceStatus::upload_failed,
                        std::move(allocated.diagnostic), nullptr};
            resources->stock_vulkan_source_constants_for_packet_[packet_index] =
                std::move(allocated.buffers);
        }

        SamplerDescription linear_description;
        linear_description.min_filter = SamplerFilter::anisotropic;
        linear_description.mag_filter = SamplerFilter::anisotropic;
        linear_description.mip_filter = SamplerFilter::anisotropic;
        linear_description.mip_lod_bias =
            source_sampler_settings.mip_lod_bias;
        linear_description.max_anisotropy =
            source_sampler_settings.max_anisotropy;
        linear_description.max_lod = std::numeric_limits<float>::max();
        SamplerResult linear = device.create_sampler(linear_description);
        if (!linear.ok())
            return {map_sampler_status(linear.status),
                    std::move(linear.diagnostic), nullptr};
        resources->stock_vulkan_source_linear_sampler_ =
            std::move(linear.sampler);

        SamplerDescription shadow_description;
        shadow_description.min_filter = SamplerFilter::linear;
        shadow_description.mag_filter = SamplerFilter::linear;
        shadow_description.mip_filter = SamplerFilter::nearest;
        shadow_description.address_u = SamplerAddressMode::clamp_to_edge;
        shadow_description.address_v = SamplerAddressMode::clamp_to_edge;
        shadow_description.address_w = SamplerAddressMode::clamp_to_edge;
        shadow_description.compare = SamplerCompare::less;
        shadow_description.max_lod = std::numeric_limits<float>::max();
        SamplerResult shadow = device.create_sampler(shadow_description);
        if (!shadow.ok())
            return {map_sampler_status(shadow.status),
                    std::move(shadow.diagnostic), nullptr};
        resources->stock_vulkan_source_shadow_sampler_ =
            std::move(shadow.sampler);
    }
    resources->stock_d3d12_native_resources_for_packet_.resize(
        resources->packets_.size());
    if (has_stock_d3d12_native) {
        for (std::size_t packet_index = 0U;
             packet_index < resources->packets_.size(); ++packet_index) {
            const std::uint32_t raw_program =
                resources->stock_d3d12_native_program_for_packet_[
                    packet_index];
            if (raw_program == invalid_static_scene_source_program_index)
                continue;
            const std::size_t program_index =
                static_cast<std::size_t>(raw_program);
            if (program_index >=
                    resources->stock_d3d12_native_programs_.size() ||
                resources->stock_d3d12_native_programs_[program_index] ==
                    nullptr)
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_d3d12_native_owner_index_invalid",
                    "A retained D3D12 native packet has no validated program owner");

            StockKsPerPixelNativeProgramResult cloned =
                clone_validated_stock_ks_per_pixel_native_program(
                    *resources->stock_d3d12_native_programs_[program_index]);
            if (!cloned.ok())
                return fail(
                    StaticSceneResourceStatus::invalid_request,
                    "static_scene_stock_d3d12_native_clone_invalid",
                    "A retained D3D12 native program could not be cloned through its validation gate");
            StockKsPerPixelNativeShaderResult shaders =
                allocate_stock_ks_per_pixel_native_shaders(
                    device, std::move(*cloned.program));
            if (!shaders.ok())
                return {shaders.status ==
                                StockKsPerPixelNativeShaderStatus::
                                    backend_unsupported
                            ? StaticSceneResourceStatus::unsupported
                        : shaders.status ==
                                  StockKsPerPixelNativeShaderStatus::
                                      allocation_failed
                            ? StaticSceneResourceStatus::allocation_failed
                        : shaders.status ==
                                      StockKsPerPixelNativeShaderStatus::
                                          invalid_program ||
                                  shaders.status ==
                                      StockKsPerPixelNativeShaderStatus::
                                          invalid_shader_module
                            ? StaticSceneResourceStatus::invalid_request
                            : StaticSceneResourceStatus::upload_failed,
                        std::move(shaders.diagnostic), nullptr};

            StockKsPerPixelNativeConstantData constants;
            constants.camera = make_stock_ks_per_pixel_camera_constants(
                apex::scene::identity_matrix, apex::scene::identity_matrix,
                apex::scene::identity_matrix, {0.0F, 0.0F, 0.0F}, 0.1F,
                100.0F, 1.0F);
            constants.object = make_stock_ks_per_pixel_object_constants(
                resources->packets_[packet_index].world_matrix);
            constants.shadow_maps.shadow_matrices.fill(
                apex::scene::identity_matrix);
            constants.shadow_maps.texture_size = 1.0F;
            constants.material =
                request.stock_d3d12_native_material_constants_by_material[
                    static_cast<std::size_t>(
                        resources->packets_[packet_index].material)];
            StockKsPerPixelNativeConstantBufferResult native_constants =
                allocate_stock_ks_per_pixel_native_constant_buffers(
                    device, constants);
            if (!native_constants.ok())
                return {native_constants.status ==
                                StockKsPerPixelNativeConstantBufferStatus::
                                    backend_unsupported
                            ? StaticSceneResourceStatus::unsupported
                        : native_constants.status ==
                                  StockKsPerPixelNativeConstantBufferStatus::
                                      allocation_failed
                            ? StaticSceneResourceStatus::allocation_failed
                        : native_constants.status ==
                                      StockKsPerPixelNativeConstantBufferStatus::
                                          invalid_constants ||
                                  native_constants.status ==
                                      StockKsPerPixelNativeConstantBufferStatus::
                                          invalid_buffer
                            ? StaticSceneResourceStatus::invalid_request
                            : StaticSceneResourceStatus::upload_failed,
                        std::move(native_constants.diagnostic), nullptr};
            StockKsPerPixelNativeSamplerResult samplers =
                allocate_stock_ks_per_pixel_native_samplers(
                    device, d3d12_sampler_settings);
            if (!samplers.ok())
                return {samplers.status ==
                                StockKsPerPixelNativeSamplerStatus::
                                    backend_unsupported
                            ? StaticSceneResourceStatus::unsupported
                        : samplers.status ==
                                  StockKsPerPixelNativeSamplerStatus::
                                      allocation_failed
                            ? StaticSceneResourceStatus::allocation_failed
                        : samplers.status ==
                                      StockKsPerPixelNativeSamplerStatus::
                                          invalid_settings ||
                                  samplers.status ==
                                      StockKsPerPixelNativeSamplerStatus::
                                          invalid_sampler
                            ? StaticSceneResourceStatus::invalid_request
                            : StaticSceneResourceStatus::upload_failed,
                        std::move(samplers.diagnostic), nullptr};
            StockKsPerPixelNativeDrawResourcesResult native_resources =
                make_stock_ks_per_pixel_native_draw_resources(
                    device, std::move(shaders.program),
                    std::move(native_constants.buffers),
                    std::move(samplers.samplers));
            if (!native_resources.ok())
                return {native_resources.status ==
                                StockKsPerPixelNativeDrawResourcesStatus::
                                    backend_unsupported
                            ? StaticSceneResourceStatus::unsupported
                        : native_resources.status ==
                                  StockKsPerPixelNativeDrawResourcesStatus::
                                      allocation_failed
                            ? StaticSceneResourceStatus::allocation_failed
                            : StaticSceneResourceStatus::invalid_request,
                        std::move(native_resources.diagnostic), nullptr};
            resources->stock_d3d12_native_resources_for_packet_[packet_index] =
                std::move(native_resources.resources);
        }
    }
    if (requires_frame_constants) {
        std::array<std::byte, portable_frame_buffer_view_bytes> bytes{};
        const BufferDescription description{
            bytes.size(), BufferUsage::uniform, BufferMemory::device_local,
            BufferMutability::mutable_data};
        BufferResult buffer = device.create_buffer(description, bytes);
        if (!buffer.ok())
            return {map_buffer_status(buffer.status), std::move(buffer.diagnostic), nullptr};
        resources->owned_frame_constants_ = std::move(buffer.buffer);
    }
    if (requires_directional_shadow_receiver) {
        std::array<std::byte, portable_directional_shadow_buffer_view_bytes> bytes{};
        const BufferDescription description{
            bytes.size(), BufferUsage::uniform, BufferMemory::device_local,
            BufferMutability::mutable_data};
        BufferResult buffer = device.create_buffer(description, bytes);
        if (!buffer.ok())
            return {map_buffer_status(buffer.status), std::move(buffer.diagnostic), nullptr};
        resources->owned_directional_shadow_constants_ = std::move(buffer.buffer);

        SamplerDescription sampler_description;
        sampler_description.min_filter = SamplerFilter::nearest;
        sampler_description.mag_filter = SamplerFilter::nearest;
        sampler_description.mip_filter = SamplerFilter::nearest;
        sampler_description.address_u = SamplerAddressMode::clamp_to_edge;
        sampler_description.address_v = SamplerAddressMode::clamp_to_edge;
        sampler_description.address_w = SamplerAddressMode::clamp_to_edge;
        sampler_description.compare = SamplerCompare::disabled;
        Diagnostic sampler_diagnostic;
        const SamplerStatus sampler_validation =
            validate_sampler_description(sampler_description, sampler_diagnostic);
        if (sampler_validation != SamplerStatus::ready)
            return {map_sampler_status(sampler_validation),
                    std::move(sampler_diagnostic), nullptr};
        SamplerResult sampler = device.create_sampler(sampler_description);
        if (!sampler.ok())
            return {map_sampler_status(sampler.status),
                    std::move(sampler.diagnostic), nullptr};
        resources->owned_directional_shadow_sampler_ = std::move(sampler.sampler);
    }
    if (resources->has_texture_resources_ &&
        request.texture_authority ==
            StaticSceneTextureAuthority::owned_model_payloads) {
        resources->owned_textures_.resize(resources->texture_count_);
        for (std::size_t index = 0U; index < decoded_textures.size(); ++index) {
            if (!decoded_textures[index].has_value()) continue;
            const TextureUploadPlan uploads =
                decoded_textures[index]->make_upload_plan();
            TextureResult texture = device.create_texture(
                decoded_textures[index]->description, uploads);
            if (!texture.ok())
                return {map_texture_status(texture.status),
                        std::move(texture.diagnostic), nullptr};
            resources->owned_textures_[index] = std::move(texture.texture);
        }
    }
    resources->uploads_.reserve(unique_meshes.size());
    for (std::size_t index = 0U; index < unique_meshes.size(); ++index) {
        StaticMeshUploadResult uploaded = upload_static_mesh(
            device, *unique_meshes[index],
            resources->packets_[representative_packets[index]], limits.mesh);
        if (!uploaded.ok())
            return {map_upload_status(uploaded.status), std::move(uploaded.diagnostic), nullptr};
        resources->uploads_.push_back(std::move(uploaded.upload));
    }
    resources->skinned_uploads_.reserve(skinned_meshes.size());
    for (std::size_t index = 0U; index < skinned_meshes.size(); ++index) {
        const std::size_t packet_index = skinned_packet_indices[index];
        SkinnedMeshUploadResult uploaded = upload_skinned_mesh(
            device, *skinned_meshes[index], resources->packets_[packet_index], limits.skinned);
        if (!uploaded.ok())
            return {map_skinned_upload_status(uploaded.status),
                    std::move(uploaded.diagnostic), nullptr};
        resources->skinned_uploads_.push_back(std::move(uploaded.upload));
    }
    if (resources->has_texture_resources_ &&
        request.texture_authority ==
            StaticSceneTextureAuthority::owned_model_payloads) {
        SamplerDescription sampler_description;
        Diagnostic sampler_diagnostic;
        const SamplerStatus sampler_validation =
            validate_sampler_description(sampler_description, sampler_diagnostic);
        if (sampler_validation != SamplerStatus::ready)
            return {map_sampler_status(sampler_validation),
                    std::move(sampler_diagnostic), nullptr};
        SamplerResult sampler = device.create_sampler(sampler_description);
        if (!sampler.ok())
            return {map_sampler_status(sampler.status),
                    std::move(sampler.diagnostic), nullptr};
        resources->owned_sampler_ = std::move(sampler.sampler);
    }
    return {StaticSceneResourceStatus::ready, {}, std::move(resources)};
} catch (const std::bad_alloc&) {
    return fail(StaticSceneResourceStatus::allocation_failed,
                "static_scene_allocation_failed",
                "Static-scene preparation has insufficient memory for bounded storage");
}

bool StaticSceneResources::validate_refreshed_packets(
    std::span<const DrawPacket> refreshed_packets,
    Diagnostic& output_diagnostic) const noexcept {
    output_diagnostic = {};
    if (refreshed_packets.empty()) return true;
    if (refreshed_packets.size() != packets_.size()) {
        output_diagnostic = {
            "static_scene_frame_packet_count_invalid",
            "Refreshed static-scene packet states must match the prepared packet count"};
        return false;
    }
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        const DrawPacket& packet = refreshed_packets[index];
        if (!same_prepared_draw_contract(packets_[index], packet) ||
            !finite_world_matrix(packet)) {
            output_diagnostic = {
                "static_scene_frame_packet_contract_invalid",
                "Refreshed packet state changed a prepared draw contract or transform"};
            return false;
        }
        if (packet.primitive != DrawPrimitiveKind::skinned_mesh) continue;
        if (packet.bone_palette.empty()) {
            output_diagnostic = {
                "static_scene_frame_skin_palette_invalid",
                "A skinned frame packet must contain a bone palette"};
            return false;
        }
        for (const auto& matrix : packet.bone_palette) {
            if (std::any_of(matrix.begin(), matrix.end(),
                            [](float value) { return !std::isfinite(value); })) {
                output_diagnostic = {
                    "static_scene_frame_skin_palette_invalid",
                    "A skinned frame packet contains a non-finite bone palette"};
                return false;
            }
        }
    }
    return true;
}

IndexedStaticMeshBatchResult StaticSceneResources::draw_and_readback(
    Device& device, Texture& target, const StaticSceneFrameDescription& frame) try {
    if (&device != device_ || device.info().backend != backend_ || target.backend() != backend_)
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_device_mismatch",
                 "Static-scene resources require their preparing device and backend"}, {}};
    if (!device.owns_resource(target))
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_target_device_mismatch",
                 "The color target belongs to another device context"}, {}};
    if (frame.resolve_target != nullptr &&
        !device.owns_resource(*frame.resolve_target))
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_resolve_target_device_mismatch",
                 "The resolve target belongs to another device context"}, {}};
    if (frame.depth_attachment != nullptr &&
        !device.owns_resource(*frame.depth_attachment))
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_depth_attachment_device_mismatch",
                 "The depth attachment belongs to another device context"}, {}};
    if (frame.color_target_format_override.has_value() &&
        (!stock_vulkan_source_programs_.empty() ||
         !stock_d3d12_native_programs_.empty()))
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_frame_target_override_native_unsupported",
                 "A frame color-target override cannot mutate recovered native or source package programs"}, {}};
    if (texture_authority_ != StaticSceneTextureAuthority::caller_tables &&
        texture_authority_ !=
            StaticSceneTextureAuthority::owned_model_payloads)
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_texture_authority_invalid",
                 "The prepared static-scene texture authority is invalid"}, {}};
    if (packets_.empty() || packets_.size() != upload_for_packet_.size() ||
        packets_.size() != pipeline_for_packet_.size() ||
        packets_.size() != textures_for_packet_.size() ||
        packets_.size() != material_constant_for_packet_.size() ||
        packets_.size() !=
            multimap_reflection_constant_for_packet_.size() ||
        packets_.size() != skinned_upload_for_packet_.size() ||
        packets_.size() !=
            stock_vulkan_source_program_for_packet_.size() ||
        packets_.size() !=
            stock_vulkan_source_constants_for_packet_.size() ||
        packets_.size() !=
            stock_d3d12_native_program_for_packet_.size() ||
        packets_.size() !=
            stock_d3d12_native_resources_for_packet_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_resources_invalid", "Static-scene resource mappings are incomplete"}, {}};
    if (!frame.refreshed_packets.empty() &&
        frame.refreshed_packets.size() != packets_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_frame_packet_count_invalid",
                 "Refreshed static-scene packet states must match the prepared packet count"}, {}};
    if (!frame.packet_visibility.empty() &&
        frame.packet_visibility.size() != packets_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_frame_visibility_mask_count_invalid",
                 "Static-scene packet visibility must match the prepared packet count"}, {}};
    if (std::any_of(frame.packet_visibility.begin(), frame.packet_visibility.end(),
                    [](std::uint8_t value) { return value > 1U; }))
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_frame_visibility_mask_value_invalid",
                 "Static-scene packet visibility values must be 0 or 1"}, {}};
    if (!frame.color_packet_order.empty() &&
        frame.color_packet_order.size() != packets_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_frame_color_order_count_invalid",
                 "Static-scene color order must contain every prepared packet"}, {}};
    std::array<bool, max_indexed_static_mesh_batch_draws> ordered_packets{};
    for (const std::uint32_t packet_index : frame.color_packet_order) {
        if (static_cast<std::size_t>(packet_index) >= packets_.size())
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_color_order_index_invalid",
                     "Static-scene color order contains an out-of-range packet index"}, {}};
        if (ordered_packets[packet_index])
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_color_order_duplicate",
                     "Static-scene color order contains a duplicate packet index"}, {}};
        ordered_packets[packet_index] = true;
    }
    if (has_texture_resources_ &&
        texture_authority_ == StaticSceneTextureAuthority::caller_tables &&
        (frame.textures_by_global_index.size() != texture_count_ ||
         (has_portable_texture_resources_ &&
          frame.samplers_by_global_index.size() != texture_count_)))
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_resource_table_size_invalid",
                 has_portable_texture_resources_
                     ? "Static-scene texture and sampler tables must match the final KN5 texture count"
                     : "The static-scene texture table must match the final KN5 texture count"}, {}};
    if (texture_authority_ == StaticSceneTextureAuthority::caller_tables) {
        for (const PacketTextureIndices& indices : textures_for_packet_) {
            const std::array<std::uint32_t, 8U> referenced = {
                indices.diffuse, indices.normal, indices.maps, indices.detail,
                indices.normal_detail, indices.dust, indices.damage,
                indices.damage_mask};
            for (const std::uint32_t raw_index : referenced) {
                if (raw_index == invalid_draw_texture_index) continue;
                const std::size_t index =
                    static_cast<std::size_t>(raw_index);
                if (index >= frame.textures_by_global_index.size())
                    return {IndexedStaticMeshBatchStatus::invalid_request,
                            {"static_scene_texture_index_invalid",
                             "A prepared texture index is outside the caller table"},
                            {}};
                const Texture* texture =
                    frame.textures_by_global_index[index];
                if (texture != nullptr && !device.owns_resource(*texture))
                    return {IndexedStaticMeshBatchStatus::unsupported,
                            {"static_scene_texture_device_mismatch",
                             "A referenced caller texture belongs to another device context"},
                            {}};
                if (has_portable_texture_resources_) {
                    const Sampler* sampler =
                        frame.samplers_by_global_index[index];
                    if (sampler != nullptr &&
                        !device.owns_resource(*sampler))
                        return {IndexedStaticMeshBatchStatus::unsupported,
                                {"static_scene_sampler_device_mismatch",
                                 "A referenced caller sampler belongs to another device context"},
                                {}};
                }
            }
        }
    }

    const bool has_stock_vulkan_source =
        !stock_vulkan_source_programs_.empty();
    if (has_stock_vulkan_source !=
        frame.stock_vulkan_source_frame.has_value())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {has_stock_vulkan_source
                     ? "static_scene_stock_vulkan_source_frame_missing"
                     : "static_scene_stock_vulkan_source_frame_unexpected",
                 has_stock_vulkan_source
                     ? "Retained Vulkan source-equivalent draws require a complete native frame record"
                     : "A native Vulkan source frame was supplied to a portable-only scene"},
                {}};
    if (has_stock_vulkan_source) {
        if (backend_ != Backend::Vulkan ||
            stock_vulkan_source_linear_sampler_ == nullptr ||
            stock_vulkan_source_shadow_sampler_ == nullptr)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_stock_vulkan_source_resources_invalid",
                     "Retained Vulkan source-equivalent sampler ownership is incomplete"},
                    {}};
        const StaticSceneFrameDescription::StockVulkanSourceFrame& source_frame =
            *frame.stock_vulkan_source_frame;
        if (!valid_stock_ks_per_pixel_camera_constants(source_frame.camera) ||
            !valid_stock_ks_per_pixel_lighting_constants(
                source_frame.lighting) ||
            !valid_stock_directional_shadow_receiver_constants(
                source_frame.shadow_constants))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_stock_vulkan_source_frame_invalid",
                     "Native Vulkan source frame records must be finite and complete"},
                    {}};
        for (const DepthAttachment* shadow : source_frame.shadow_maps) {
            if (shadow == nullptr)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_stock_vulkan_source_shadow_map_missing",
                         "Native Vulkan source frames require exactly three shadow maps"},
                        {}};
            const DepthAttachmentDescription& description =
                shadow->info().description;
            if (shadow->backend() != Backend::Vulkan ||
                description.samples != 1U ||
                description.format != DepthAttachmentFormat::d32_float ||
                !description.shader_readable)
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"static_scene_stock_vulkan_source_shadow_map_unsupported",
                         "Native Vulkan source shadow maps must be shader-readable single-sample D32 attachments"},
                        {}};
            if (!device.owns_resource(*shadow))
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"static_scene_stock_vulkan_source_shadow_map_device_mismatch",
                         "A native Vulkan source shadow map belongs to another device context"},
                        {}};
        }
    }

    const bool has_stock_d3d12_native =
        !stock_d3d12_native_programs_.empty();
    if (has_stock_d3d12_native !=
        frame.stock_d3d12_native_frame.has_value())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {has_stock_d3d12_native
                     ? "static_scene_stock_d3d12_native_frame_missing"
                     : "static_scene_stock_d3d12_native_frame_unexpected",
                 has_stock_d3d12_native
                     ? "Retained installed D3D12 draws require a complete native frame record"
                     : "A D3D12 native frame was supplied to a scene without installed-native owners"},
                {}};
    if (has_stock_d3d12_native) {
        if (backend_ != Backend::D3D12)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_stock_d3d12_native_resources_invalid",
                     "Retained installed-native owners require D3D12"}, {}};
        const StaticSceneFrameDescription::StockNativeFrame& native_frame =
            *frame.stock_d3d12_native_frame;
        if (!valid_stock_ks_per_pixel_camera_constants(
                native_frame.camera) ||
            !valid_stock_ks_per_pixel_lighting_constants(
                native_frame.lighting) ||
            !valid_stock_directional_shadow_receiver_constants(
                native_frame.shadow_constants))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_stock_d3d12_native_frame_invalid",
                     "Installed D3D12 native frame records must be finite and complete"},
                    {}};
        for (const DepthAttachment* shadow : native_frame.shadow_maps) {
            if (shadow == nullptr)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_stock_d3d12_native_shadow_map_missing",
                         "Installed D3D12 native frames require exactly three shadow maps"},
                        {}};
            const DepthAttachmentDescription& description =
                shadow->info().description;
            if (shadow->backend() != Backend::D3D12 ||
                description.samples != 1U ||
                description.format != DepthAttachmentFormat::d32_float ||
                !description.shader_readable)
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"static_scene_stock_d3d12_native_shadow_map_unsupported",
                         "Installed D3D12 native shadow maps must be shader-readable single-sample D32 attachments"},
                        {}};
            if (!device.owns_resource(*shadow))
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"static_scene_stock_d3d12_native_shadow_map_device_mismatch",
                         "An installed D3D12 shadow map belongs to another device context"},
                        {}};
        }
    }

    const bool has_multimap_reflection_texture =
        frame.multimap_reflection_cube.texture != nullptr;
    const bool has_multimap_reflection_sampler =
        frame.multimap_reflection_cube.sampler != nullptr;
    const bool requires_multimap_reflection =
        !owned_multimap_reflection_constants_.empty();
    if (has_multimap_reflection_texture !=
        has_multimap_reflection_sampler)
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_multimap_reflection_cube_incomplete",
                 "The frame-owned MultiMap reflection cube requires both texture and sampler handles"},
                {}};
    const bool has_multimap_reflection_cube =
        has_multimap_reflection_texture && has_multimap_reflection_sampler;
    if (requires_multimap_reflection != has_multimap_reflection_cube)
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {requires_multimap_reflection
                     ? "static_scene_multimap_reflection_cube_missing"
                     : "static_scene_multimap_reflection_cube_unexpected",
                 requires_multimap_reflection
                     ? "A prepared MultiMap reflection pipeline requires a frame-owned cube and sampler"
                     : "A frame-owned MultiMap reflection cube was supplied to a scene without a reflection pipeline"},
                {}};
    if (requires_multimap_reflection &&
        (!device.owns_resource(*frame.multimap_reflection_cube.texture) ||
         !device.owns_resource(*frame.multimap_reflection_cube.sampler)))
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_multimap_reflection_cube_device_mismatch",
                 "The frame-owned MultiMap reflection cube or sampler belongs to another device context"},
                {}};

    const bool has_directional_shadow_maps =
        frame.directional_shadow_maps != nullptr;
    const bool requires_directional_shadow_receiver =
        owns_directional_shadow_receiver();
    if (requires_directional_shadow_receiver != has_directional_shadow_maps)
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {requires_directional_shadow_receiver
                     ? "static_scene_directional_shadow_maps_missing"
                     : "static_scene_directional_shadow_maps_unexpected",
                 requires_directional_shadow_receiver
                     ? "A prepared receiver pipeline requires retained directional-shadow maps"
                     : "Directional-shadow maps were supplied to a scene without a receiver pipeline"},
                {}};

    std::array<std::byte, portable_directional_shadow_buffer_view_bytes>
        directional_shadow_bytes{};
    IndexedDirectionalShadowBinding directional_shadow_binding;
    if (requires_directional_shadow_receiver) {
        DirectionalShadowMapResources& maps = *frame.directional_shadow_maps;
        if (frame.directional_shadow_constants_layout !=
                DirectionalShadowReceiverConstantsLayout::portable &&
            frame.directional_shadow_constants_layout !=
                DirectionalShadowReceiverConstantsLayout::stock_ks_shadow_maps)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_directional_shadow_constants_layout_invalid",
                     "Directional-shadow constants layout is not a supported explicit ABI"},
                    {}};
        if (&device != maps.device_ || maps.backend_ != backend_)
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"static_scene_directional_shadow_device_mismatch",
                     "Retained directional-shadow maps require the preparing device and backend"},
                    {}};
        if (maps.metadata_.cascades.size() != directional_shadow_cascade_count ||
            maps.metadata_.splits.size() != directional_shadow_cascade_count ||
            maps.metadata_.map_size == 0U)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_directional_shadow_contract_invalid",
                     "Retained directional-shadow metadata must contain exactly three cascades"},
                    {}};
        const CameraClipSpace expected_clip_space =
            backend_ == Backend::Vulkan ? CameraClipSpace::vulkan
                                        : CameraClipSpace::d3d12;
        if (frame.camera.clip_space != expected_clip_space)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_directional_shadow_camera_invalid",
                     "The receiver camera clip space does not match the prepared backend"},
                    {}};
        const auto finite_vector = [](const apex::scene::Vector3& value) {
            return std::all_of(value.begin(), value.end(),
                               [](float component) {
                                   return std::isfinite(component);
                               });
        };
        if (!finite_vector(frame.camera.position) ||
            !finite_vector(frame.camera.forward) ||
            !finite_vector(maps.receiver_position_) ||
            !finite_vector(maps.metadata_.forward))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_directional_shadow_camera_invalid",
                     "Directional-shadow receiver camera values must be finite"},
                    {}};
        double forward_length_squared = 0.0;
        double map_forward_length_squared = 0.0;
        double forward_dot = 0.0;
        bool same_position = true;
        for (std::size_t component = 0U; component < 3U; ++component) {
            const double frame_forward = frame.camera.forward[component];
            const double map_forward = maps.metadata_.forward[component];
            forward_length_squared += frame_forward * frame_forward;
            map_forward_length_squared += map_forward * map_forward;
            forward_dot += frame_forward * map_forward;
            same_position = same_position &&
                            std::abs(frame.camera.position[component] -
                                     maps.receiver_position_[component]) <= 1.0e-4F;
        }
        if (!(forward_length_squared > 1.0e-12) ||
            !(map_forward_length_squared > 1.0e-12) || !same_position ||
            forward_dot /
                    std::sqrt(forward_length_squared * map_forward_length_squared) <
                0.9999)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_directional_shadow_camera_mismatch",
                     "The main camera must match the camera used to build the retained cascades"},
                    {}};

        DirectionalShadowReceiverConstants constants;
        constants.camera_position = {frame.camera.position[0],
                                     frame.camera.position[1],
                                     frame.camera.position[2], 0.0F};
        constants.camera_forward = {maps.metadata_.forward[0],
                                    maps.metadata_.forward[1],
                                    maps.metadata_.forward[2], 0.0F};
        std::array<apex::scene::Matrix4, directional_shadow_cascade_count>
            shadow_matrices{};
        for (std::size_t cascade = 0U;
             cascade < directional_shadow_cascade_count; ++cascade) {
            const ShadowCascade& metadata = maps.metadata_.cascades[cascade];
            if (metadata.index != cascade ||
                !std::isfinite(maps.metadata_.splits[cascade]) ||
                (cascade != 0U &&
                 !(maps.metadata_.splits[cascade] >
                   maps.metadata_.splits[cascade - 1U])))
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_directional_shadow_contract_invalid",
                         "Directional-shadow cascades and splits must be finite and ordered"},
                        {}};
            if (maps.attachments_[cascade] == nullptr ||
                maps.cameras_[cascade].clip_space != expected_clip_space ||
                !std::all_of(maps.cameras_[cascade].view_projection.begin(),
                             maps.cameras_[cascade].view_projection.end(),
                             [](float value) { return std::isfinite(value); }))
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_directional_shadow_contract_invalid",
                         "A retained directional-shadow cascade is incomplete"},
                        {}};
            const DepthAttachmentDescription& description =
                maps.attachments_[cascade]->info().description;
            if (maps.attachments_[cascade]->backend() != backend_ ||
                description.width != maps.metadata_.map_size ||
                description.height != maps.metadata_.map_size ||
                description.samples != 1U ||
                description.format != DepthAttachmentFormat::d32_float ||
                !description.shader_readable)
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"static_scene_directional_shadow_map_unsupported",
                         "Each retained receiver map must be shader-readable single-sample D32"},
                        {}};
            constants.shadow_matrices[cascade] =
                maps.cameras_[cascade].view_projection;
            shadow_matrices[cascade] = maps.cameras_[cascade].view_projection;
            constants.split_distances[cascade] = maps.metadata_.splits[cascade];
            constants.depth_biases[cascade] = ks_shadow_biases[cascade];
            directional_shadow_binding.maps[cascade] =
                maps.attachments_[cascade].get();
        }
        std::uint32_t constants_range =
            portable_directional_shadow_buffer_view_bytes;
        if (frame.directional_shadow_constants_layout ==
            DirectionalShadowReceiverConstantsLayout::stock_ks_shadow_maps) {
            const StockDirectionalShadowReceiverConstants stock_constants =
                make_stock_directional_shadow_receiver_constants(
                    shadow_matrices, ks_shadow_biases, maps.metadata_.map_size);
            std::memcpy(directional_shadow_bytes.data(), &stock_constants,
                        sizeof(stock_constants));
            constants_range = stock_directional_shadow_buffer_view_bytes;
        } else {
            std::memcpy(directional_shadow_bytes.data(), &constants,
                        sizeof(constants));
        }
        directional_shadow_binding.sampler =
            owned_directional_shadow_sampler_.get();
        directional_shadow_binding.constants =
            owned_directional_shadow_constants_.get();
        directional_shadow_binding.constants_range_bytes = constants_range;
    }

    if (owns_frame_constants()) {
        if (!frame.frame_constants.has_value())
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_constants_missing",
                     "A prepared pipeline requires per-frame constants"}, {}};
        if (!finite_frame_constants(*frame.frame_constants))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_constants_non_finite",
                     "Per-frame constants must contain only finite values"}, {}};
        if (!valid_frame_sun_direction(*frame.frame_constants))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_sun_direction_invalid",
                     "Per-frame lighting requires a nonzero sun direction"}, {}};
        for (const float value : frame.camera.position)
            if (!std::isfinite(value))
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_frame_camera_position_invalid",
                         "Per-frame lighting requires a finite camera position"}, {}};
        if (owned_frame_constants_->info().description.size_bytes <
            portable_frame_buffer_view_bytes)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_constant_range_invalid",
                     "The owned frame-constant buffer is smaller than its portable view"}, {}};
    }

    const auto packet_for_frame = [&](std::size_t index) -> const DrawPacket& {
        return frame.refreshed_packets.empty() ? packets_[index] : frame.refreshed_packets[index];
    };
    const auto packet_visible = [&](std::size_t index) {
        return !packets_[index].shadow_only &&
               (frame.packet_visibility.empty() ||
                frame.packet_visibility[index] != 0U);
    };
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        const DrawPacket& packet = packet_for_frame(index);
        if (!same_prepared_draw_contract(packets_[index], packet) ||
            !finite_world_matrix(packet))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_packet_contract_invalid",
                     "Refreshed packet state changed a prepared draw contract or transform"}, {}};
        if (packet.primitive == DrawPrimitiveKind::skinned_mesh) {
            if (packet.bone_palette.empty())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_frame_skin_palette_invalid",
                         "A skinned frame packet must contain a bone palette"}, {}};
            for (const auto& matrix : packet.bone_palette)
                for (const float value : matrix)
                    if (!std::isfinite(value))
                        return {IndexedStaticMeshBatchStatus::invalid_request,
                                {"static_scene_frame_skin_palette_invalid",
                                 "A skinned frame packet contains a non-finite bone palette"}, {}};
        }
    }

    struct PendingSkinUpdate {
        std::size_t upload_index = invalid_resource_index;
        const DrawPacket* packet = nullptr;
        std::vector<float> vertices;
    };
    std::vector<PendingSkinUpdate> pending_skin_updates;
    pending_skin_updates.reserve(skinned_uploads_.size());
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        if (!packet_visible(index)) continue;
        const std::size_t upload_index = skinned_upload_for_packet_[index];
        if (upload_index == invalid_resource_index) continue;
        if (upload_index >= skinned_uploads_.size() || skinned_uploads_[upload_index] == nullptr)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_resources_invalid", "Skinned static-scene resource index is invalid"}, {}};
        const DrawPacket& packet = packet_for_frame(index);
        PendingSkinUpdate pending;
        pending.upload_index = upload_index;
        pending.packet = &packet;
        if (frame.apply_skinning) {
            SkinnedMeshPoseUpdateResult prepared =
                skinned_uploads_[upload_index]->prepare_pose(packet);
            if (!prepared.ok()) {
                const IndexedStaticMeshBatchStatus status =
                    prepared.status == SkinnedMeshUploadStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : prepared.status == SkinnedMeshUploadStatus::invalid_request
                              ? IndexedStaticMeshBatchStatus::invalid_request
                              : IndexedStaticMeshBatchStatus::execution_failed;
                return {status, std::move(prepared.diagnostic), {}};
            }
            pending.vertices = std::move(prepared.skinned_vertices);
        } else {
            const auto bind_vertices = skinned_uploads_[upload_index]->bind_vertices();
            pending.vertices.assign(bind_vertices.begin(), bind_vertices.end());
        }
        pending_skin_updates.push_back(std::move(pending));
    }
    struct PendingStockNativeUpdate {
        Buffer* buffer = nullptr;
        std::array<std::byte,
                   stock_ks_per_pixel_native_constant_buffer_view_bytes>
            bytes{};
        bool d3d12_native = false;
    };
    std::vector<PendingStockNativeUpdate> pending_stock_native_updates;
    pending_stock_native_updates.reserve(
        packets_.size() *
        ((has_stock_vulkan_source || has_stock_d3d12_native) ? 4U : 0U));
    const auto append_native_update = [&]<typename Record>(
        Buffer* buffer, const Record& record, bool d3d12_native) {
        PendingStockNativeUpdate pending;
        pending.buffer = buffer;
        pending.d3d12_native = d3d12_native;
        std::memcpy(pending.bytes.data(), &record, sizeof(record));
        pending_stock_native_updates.push_back(std::move(pending));
    };
    if (has_stock_vulkan_source) {
        const StaticSceneFrameDescription::StockVulkanSourceFrame& source_frame =
            *frame.stock_vulkan_source_frame;
        for (std::size_t index = 0U; index < packets_.size(); ++index) {
            if (!packet_visible(index) ||
                stock_vulkan_source_program_for_packet_[index] ==
                    invalid_static_scene_source_program_index)
                continue;
            StockKsPerPixelNativeConstantBuffers* constants =
                stock_vulkan_source_constants_for_packet_[index].get();
            if (constants == nullptr || !constants->ready())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_stock_vulkan_source_resources_invalid",
                         "A retained Vulkan source draw has incomplete native constant ownership"},
                        {}};
            const StockKsPerPixelObjectConstants object =
                make_stock_ks_per_pixel_object_constants(
                    packet_for_frame(index).world_matrix);
            append_native_update(constants->buffer(
                                     StockKsPerPixelNativeConstantSlot::camera),
                                 source_frame.camera, false);
            append_native_update(constants->buffer(
                                     StockKsPerPixelNativeConstantSlot::object),
                                 object, false);
            append_native_update(constants->buffer(
                                     StockKsPerPixelNativeConstantSlot::lighting),
                                 source_frame.lighting, false);
            append_native_update(constants->buffer(
                                     StockKsPerPixelNativeConstantSlot::shadow_maps),
                                 source_frame.shadow_constants, false);
        }
    }
    if (has_stock_d3d12_native) {
        const StaticSceneFrameDescription::StockNativeFrame& native_frame =
            *frame.stock_d3d12_native_frame;
        for (std::size_t index = 0U; index < packets_.size(); ++index) {
            if (!packet_visible(index) ||
                stock_d3d12_native_program_for_packet_[index] ==
                    invalid_static_scene_source_program_index)
                continue;
            StockKsPerPixelNativeDrawResources* resources =
                stock_d3d12_native_resources_for_packet_[index].get();
            if (resources == nullptr || !resources->ready())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_stock_d3d12_native_resources_invalid",
                         "A retained installed D3D12 draw has incomplete native ownership"},
                        {}};
            StockKsPerPixelNativeConstantBuffers& constants =
                resources->constant_buffers();
            const StockKsPerPixelObjectConstants object =
                make_stock_ks_per_pixel_object_constants(
                    packet_for_frame(index).world_matrix);
            append_native_update(constants.buffer(
                                     StockKsPerPixelNativeConstantSlot::camera),
                                 native_frame.camera, true);
            append_native_update(constants.buffer(
                                     StockKsPerPixelNativeConstantSlot::object),
                                 object, true);
            append_native_update(constants.buffer(
                                     StockKsPerPixelNativeConstantSlot::lighting),
                                 native_frame.lighting, true);
            append_native_update(constants.buffer(
                                     StockKsPerPixelNativeConstantSlot::shadow_maps),
                                 native_frame.shadow_constants, true);
        }
    }
    std::vector<IndexedStaticMeshDrawRequest> draws;
    draws.reserve(packets_.size());
    std::vector<PipelineProgram> frame_target_pipelines;
    if (frame.color_target_format_override.has_value()) {
        frame_target_pipelines = pipelines_;
        for (PipelineProgram& pipeline : frame_target_pipelines) {
            if (pipeline.targets.colors.size() != 1U)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_frame_target_override_pipeline_invalid",
                         "A frame color-target override requires one retained color target per pipeline"}, {}};
            pipeline.targets.colors.front().format =
                *frame.color_target_format_override;
        }
    }
    std::vector<StockKsPerPixelVulkanSourceDrawBinding>
        stock_vulkan_source_bindings;
    stock_vulkan_source_bindings.reserve(packets_.size());
    std::vector<StockKsPerPixelNativeDrawBinding>
        stock_d3d12_native_bindings;
    stock_d3d12_native_bindings.reserve(packets_.size());
    for (std::size_t order = 0U; order < packets_.size(); ++order) {
        const std::size_t index = frame.color_packet_order.empty()
                                      ? order
                                      : static_cast<std::size_t>(
                                            frame.color_packet_order[order]);
        if (!packet_visible(index)) continue;
        const DrawPacket& packet = packet_for_frame(index);
        const bool upload_index_invalid =
            packet.primitive == DrawPrimitiveKind::skinned_mesh
                ? skinned_upload_for_packet_[index] >= skinned_uploads_.size()
                : upload_for_packet_[index] >= uploads_.size();
        const std::uint32_t raw_source_program =
            stock_vulkan_source_program_for_packet_[index];
        const bool stock_vulkan_source =
            raw_source_program != invalid_static_scene_source_program_index;
        const std::uint32_t raw_d3d12_native_program =
            stock_d3d12_native_program_for_packet_[index];
        const bool stock_d3d12_native =
            raw_d3d12_native_program !=
            invalid_static_scene_source_program_index;
        const bool pipeline_index_invalid =
            stock_vulkan_source
                ? static_cast<std::size_t>(raw_source_program) >=
                      stock_vulkan_source_programs_.size()
                : pipeline_for_packet_[index] >= pipelines_.size();
        if (upload_index_invalid || pipeline_index_invalid)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_resources_invalid", "Static-scene resource index is invalid"}, {}};
        const PipelineProgram& pipeline =
            stock_vulkan_source
                ? stock_vulkan_source_programs_[raw_source_program]->pipeline()
                : frame_target_pipelines.empty()
                      ? pipelines_[pipeline_for_packet_[index]]
                      : frame_target_pipelines[pipeline_for_packet_[index]];
        Diagnostic diagnostic;
        std::optional<IndexedStaticMeshDrawRequest> draw;
        if (packet.primitive == DrawPrimitiveKind::skinned_mesh) {
            if (skinned_upload_for_packet_[index] >= skinned_uploads_.size())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_resources_invalid", "Skinned static-scene resource index is invalid"}, {}};
            draw = skinned_uploads_[skinned_upload_for_packet_[index]]->make_request(
                packet, pipeline, frame.camera,
                0U, 0U, {0.0F, 0.0F, 0.0F, 1.0F}, diagnostic);
        } else {
            draw = uploads_[upload_for_packet_[index]]->make_request(
                packet, pipeline, frame.camera,
                0U, 0U, {0.0F, 0.0F, 0.0F, 1.0F}, diagnostic);
        }
        if (!draw.has_value())
            return {IndexedStaticMeshBatchStatus::invalid_request, std::move(diagnostic), {}};
        const PacketTextureIndices texture_indices = textures_for_packet_[index];
        std::optional<IndexedStaticMeshBatchResult> resource_failure;
        const auto resolve_texture = [&](std::uint32_t raw_index,
                                         std::string_view role,
                                         bool require_sampler = true) {
            IndexedSampledTextureBinding binding;
            if (raw_index == invalid_draw_texture_index) return binding;
            const std::size_t resource_index = static_cast<std::size_t>(raw_index);
            if (texture_authority_ ==
                StaticSceneTextureAuthority::owned_model_payloads) {
                if (resource_index >= owned_textures_.size() ||
                    owned_textures_[resource_index] == nullptr ||
                    (require_sampler && owned_sampler_ == nullptr)) {
                    resource_failure = IndexedStaticMeshBatchResult{
                        IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_owned_resource_missing",
                         "A used static-scene " + std::string(role) +
                             (require_sampler
                                  ? " texture has no owned texture or sampler"
                                  : " texture has no owned texture")}, {}};
                    return binding;
                }
                binding = {owned_textures_[resource_index].get(),
                           require_sampler ? owned_sampler_.get() : nullptr};
            } else {
                if (resource_index >= texture_count_ ||
                    frame.textures_by_global_index[resource_index] == nullptr ||
                    (require_sampler &&
                     frame.samplers_by_global_index[resource_index] == nullptr)) {
                    resource_failure = IndexedStaticMeshBatchResult{
                        IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_resource_table_entry_missing",
                         "A used static-scene " + std::string(role) +
                             (require_sampler
                                  ? " table entry has no texture or sampler handle"
                                  : " table entry has no texture handle")}, {}};
                    return binding;
                }
                binding = {frame.textures_by_global_index[resource_index],
                           require_sampler
                               ? frame.samplers_by_global_index[resource_index]
                               : nullptr};
            }
            return binding;
        };
        const IndexedSampledTextureBinding diffuse_binding =
            resolve_texture(texture_indices.diffuse, "diffuse",
                            !stock_vulkan_source && !stock_d3d12_native);
        if (resource_failure.has_value()) return std::move(*resource_failure);
        if (stock_vulkan_source) {
            if (diffuse_binding.texture == nullptr ||
                index >= stock_vulkan_source_constants_for_packet_.size() ||
                stock_vulkan_source_constants_for_packet_[index] == nullptr ||
                !stock_vulkan_source_constants_for_packet_[index]->ready() ||
                !frame.stock_vulkan_source_frame.has_value())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_stock_vulkan_source_resources_invalid",
                         "A retained Vulkan source draw has incomplete native resources"},
                        {}};
            StockKsPerPixelVulkanSourceDrawBinding& source_binding =
                stock_vulkan_source_bindings.emplace_back();
            source_binding.program =
                stock_vulkan_source_programs_[raw_source_program].get();
            for (std::size_t slot = 0U;
                 slot < source_binding.resources.uniform_buffers.size();
                 ++slot) {
                source_binding.resources.uniform_buffers[slot] = {
                    stock_vulkan_source_constants_for_packet_[index]->buffer(
                        static_cast<StockKsPerPixelNativeConstantSlot>(slot)),
                    0U,
                    stock_ks_per_pixel_native_constant_buffer_view_bytes};
            }
            source_binding.resources.diffuse_texture =
                diffuse_binding.texture;
            source_binding.resources.shadow_maps =
                frame.stock_vulkan_source_frame->shadow_maps;
            source_binding.resources.linear_sampler =
                stock_vulkan_source_linear_sampler_.get();
            source_binding.resources.shadow_sampler =
                stock_vulkan_source_shadow_sampler_.get();
            draw->shader_authority = IndexedShaderAuthority::
                explicit_stock_ks_per_pixel_vulkan_source_equivalent;
            draw->stock_ks_per_pixel_vulkan_source = &source_binding;
            draws.push_back(std::move(*draw));
            continue;
        }
        if (stock_d3d12_native) {
            if (diffuse_binding.texture == nullptr ||
                index >=
                    stock_d3d12_native_resources_for_packet_.size() ||
                stock_d3d12_native_resources_for_packet_[index] == nullptr ||
                !stock_d3d12_native_resources_for_packet_[index]->ready() ||
                !frame.stock_d3d12_native_frame.has_value())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_stock_d3d12_native_resources_invalid",
                         "A retained installed D3D12 draw has incomplete native resources"},
                        {}};
            StockKsPerPixelNativeDrawBinding& native_binding =
                stock_d3d12_native_bindings.emplace_back(
                    stock_d3d12_native_resources_for_packet_[index]->bind(
                        *diffuse_binding.texture,
                        frame.stock_d3d12_native_frame->shadow_maps));
            draw->shader_authority =
                IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
            draw->stock_ks_per_pixel_native = &native_binding;
            draws.push_back(std::move(*draw));
            continue;
        }
        draw->shader_authority = IndexedShaderAuthority::explicit_pipeline;
        draw->sampled_binding = diffuse_binding;
        draw->normal_binding = resolve_texture(texture_indices.normal, "normal");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->maps_binding = resolve_texture(texture_indices.maps, "maps");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        const IndexedPortableResourceLayout resource_layout =
            classify_indexed_portable_resource_layout(*draw->pipeline);
        if (resource_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame)
            draw->detail_binding = resolve_texture(texture_indices.dust, "dust");
        else
            draw->detail_binding = resolve_texture(texture_indices.detail, "detail");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->normal_detail_binding = resolve_texture(texture_indices.normal_detail, "normal-detail");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->damage_binding = resolve_texture(texture_indices.damage, "damage");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->damage_mask_binding = resolve_texture(texture_indices.damage_mask, "damage-mask");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        if (draw->sampled_binding.texture != nullptr || draw->normal_binding.texture != nullptr ||
            draw->maps_binding.texture != nullptr || draw->detail_binding.texture != nullptr ||
            draw->normal_detail_binding.texture != nullptr ||
            draw->damage_binding.texture != nullptr ||
            draw->damage_mask_binding.texture != nullptr)
            draw->resource_authority = IndexedResourceAuthority::explicit_bindings;
        const std::size_t material_index = material_constant_for_packet_[index];
        if (material_index != invalid_resource_index) {
            if (material_index >= owned_material_constants_.size() ||
                owned_material_constants_[material_index] == nullptr)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_owned_material_constant_missing",
                         "A used material has no owned constant buffer"}, {}};
            draw->resource_authority = IndexedResourceAuthority::explicit_bindings;
            draw->material_binding = {
                owned_material_constants_[material_index].get(), 0U,
                portable_material_buffer_view_bytes};
        }
        const std::size_t reflection_index =
            multimap_reflection_constant_for_packet_[index];
        if (reflection_index != invalid_resource_index) {
            if (reflection_index >=
                    owned_multimap_reflection_constants_.size() ||
                owned_multimap_reflection_constants_[reflection_index] ==
                    nullptr)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_owned_multimap_reflection_constant_missing",
                         "A used MultiMap material has no owned reflection constant buffer"},
                        {}};
            draw->resource_authority =
                IndexedResourceAuthority::explicit_bindings;
            draw->multimap_reflection_binding = {
                frame.multimap_reflection_cube,
                {owned_multimap_reflection_constants_[reflection_index].get(),
                 0U, portable_multimap_reflection_buffer_view_bytes}};
        }
        if (resource_layout == IndexedPortableResourceLayout::diffuse_with_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame) {
            if (!owns_frame_constants())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_owned_frame_constant_missing",
                         "A used pipeline requires an owned frame-constant buffer"}, {}};
            draw->resource_authority = IndexedResourceAuthority::explicit_bindings;
            draw->frame_binding = {
                owned_frame_constants_.get(), 0U, portable_frame_buffer_view_bytes};
        }
        if (pipeline_declares_directional_shadow_receiver(*draw->pipeline)) {
            draw->resource_authority =
                IndexedResourceAuthority::explicit_bindings;
            draw->directional_shadow_binding = directional_shadow_binding;
        }
        draws.push_back(std::move(*draw));
    }
    if (frame.scene_finished_overlay_draws.size() > max_overlay_line_draws ||
        frame.view_axis_draws.size() >
            max_overlay_line_draws - frame.scene_finished_overlay_draws.size() ||
        frame.overlay_draws.size() >
            max_overlay_line_draws - frame.scene_finished_overlay_draws.size() -
                frame.view_axis_draws.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_overlay_draw_limit",
                 "Scene-finished, view-axis, and late overlay draws exceed the bounded line-draw limit"},
                {}};
    const bool has_selected_pipeline = frame.selected_mesh_pipeline != nullptr;
    const bool has_selected_color = frame.selected_mesh_color_buffer != nullptr;
    if (has_selected_pipeline != has_selected_color)
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_selected_mesh_resource_incomplete",
                 "Selected-mesh execution requires both its pipeline and color buffer"},
                {}};
    std::size_t transparent_position = draws.size();
    bool transparent_seen = false;
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const bool transparent = draws[index].packet->flags.transparent;
        if (transparent && !transparent_seen) {
            transparent_position = index;
            transparent_seen = true;
        } else if (!transparent && transparent_seen &&
                   (has_selected_pipeline ||
                    !frame.scene_finished_overlay_draws.empty() ||
                    !frame.view_axis_draws.empty())) {
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_draw_phase_order_invalid",
                     "Selected, scene-finished, and view-axis insertion requires opaque packets before transparent packets"},
                    {}};
        }
    }
    std::array<SelectedMeshDrawRequest, max_selected_mesh_draws> selected_draws{};
    std::size_t selected_draw_count = 0U;
    if (has_selected_pipeline) {
        std::optional<std::size_t> selected_packet_index;
        for (std::size_t index = 0U; index < packets_.size(); ++index) {
            if (!packet_for_frame(index).flags.selected) continue;
            if (selected_packet_index.has_value())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_selected_mesh_count_invalid",
                         "Selected-mesh execution requires exactly one selected packet"},
                        {}};
            selected_packet_index = index;
        }
        if (!selected_packet_index.has_value())
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_selected_mesh_missing",
                     "Selected-mesh execution requires one selected packet"},
                    {}};
        const std::size_t packet_index = *selected_packet_index;
        const DrawPacket& packet = packet_for_frame(packet_index);
        if (packet.primitive != DrawPrimitiveKind::static_mesh)
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"static_scene_selected_mesh_skinned_unsupported",
                     "The recovered selected-mesh pass supports only static geometry"},
                    {}};
        if (packet_visible(packet_index)) {
            const std::size_t upload_index = upload_for_packet_[packet_index];
            if (upload_index >= uploads_.size() || uploads_[upload_index] == nullptr)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_selected_mesh_resource_invalid",
                         "The selected packet has no retained static geometry"},
                        {}};
            SelectedMeshDrawRequest& selected =
                selected_draws[selected_draw_count++];
            selected.packet = &packet;
            selected.pipeline = frame.selected_mesh_pipeline;
            selected.vertex_buffer = uploads_[upload_index]->vertex_buffer.get();
            selected.index_buffer = uploads_[upload_index]->index_buffer.get();
            selected.color_buffer = frame.selected_mesh_color_buffer;
            selected.color_range_bytes = selected_mesh_color_view_bytes;
            selected.matrices = {packet.world_matrix,
                                 frame.camera.view_projection};
            selected.scene_position =
                static_cast<std::uint32_t>(transparent_position);
        }
    }
    std::array<OverlayLineDrawRequest, max_overlay_line_draws> overlay_draws{};
    std::size_t overlay_draw_count = 0U;
    for (const OverlayLineDrawRequest& source :
         frame.scene_finished_overlay_draws) {
        OverlayLineDrawRequest& draw = overlay_draws[overlay_draw_count++];
        draw = source;
        draw.scene_position = static_cast<std::uint32_t>(transparent_position);
    }
    for (const OverlayLineDrawRequest& source : frame.view_axis_draws) {
        OverlayLineDrawRequest& draw = overlay_draws[overlay_draw_count++];
        draw = source;
        draw.scene_position = static_cast<std::uint32_t>(transparent_position);
    }
    for (const OverlayLineDrawRequest& source : frame.overlay_draws) {
        OverlayLineDrawRequest& draw = overlay_draws[overlay_draw_count++];
        draw = source;
        draw.scene_position = std::numeric_limits<std::uint32_t>::max();
    }
    IndexedStaticMeshBatchDescription batch;
    batch.draws = draws;
    batch.depth_attachment = frame.depth_attachment;
    batch.load_color = frame.load_color;
    batch.clear_color = frame.clear_color;
    batch.clear_depth = frame.clear_depth;
    batch.depth_clear_value = frame.depth_clear_value;
    batch.resolve_target = frame.resolve_target;
    batch.capture_rgba8 = frame.capture_rgba8;
    batch.target_subresource = frame.target_subresource;
    batch.overlay_draws =
        std::span<const OverlayLineDrawRequest>(overlay_draws)
            .first(overlay_draw_count);
    batch.selected_mesh_draws =
        std::span<const SelectedMeshDrawRequest>(selected_draws)
            .first(selected_draw_count);
    if (frame.draw_sky) {
        if (!frame.frame_constants.has_value())
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_sky_constants_missing",
                     "The portable sky requires the current frame lighting constants"},
                    {}};
        const KsPerPixelFrameConstants& lighting = *frame.frame_constants;
        PortableSkyParameters sky;
        sky.camera = frame.camera;
        std::copy_n(lighting.horizon_color.begin(), 3U,
                    sky.horizon_color.begin());
        std::copy_n(lighting.sky_color.begin(), 3U, sky.sky_color.begin());
        std::copy_n(lighting.sun_color.begin(), 3U, sky.sun_color.begin());
        std::copy_n(lighting.sun_direction.begin(), 3U,
                    sky.sun_direction.begin());
        batch.sky = sky;
    }
    Diagnostic batch_diagnostic;
    const IndexedStaticMeshBatchStatus batch_validation =
        validate_indexed_static_mesh_batch_description(target, batch, batch_diagnostic);
    if (batch_validation != IndexedStaticMeshBatchStatus::ready)
        return {batch_validation, std::move(batch_diagnostic), {}};
    for (const PendingStockNativeUpdate& pending :
         pending_stock_native_updates) {
        if (pending.buffer == nullptr)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {pending.d3d12_native
                         ? "static_scene_stock_d3d12_native_constant_missing"
                         : "static_scene_stock_vulkan_source_constant_missing",
                     "A retained stock native constant slot has no buffer"},
                    {}};
        Diagnostic update_diagnostic;
        const BufferStatus range_status = validate_buffer_update(
            *pending.buffer, 0U, pending.bytes.size(), update_diagnostic);
        if (range_status != BufferStatus::ready)
            return {range_status == BufferStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : IndexedStaticMeshBatchStatus::invalid_request,
                    {pending.d3d12_native
                         ? "static_scene_stock_d3d12_native_constant_range_invalid"
                         : "static_scene_stock_vulkan_source_constant_range_invalid",
                     update_diagnostic.message},
                    {}};
    }
    // Every frame pose, frame/resource mapping, and draw contract is now
    // validated. Backend updates are sequential, so a runtime upload failure
    // can leave earlier mutable uploads committed even though no batch is
    // submitted. A caller can safely retry the complete frame. Commit source
    // ABI records before the portable directional receiver and skinning; the
    // shared portable lighting record remains last so a failed skin upload
    // cannot advance it independently of the requested pose.
    for (PendingStockNativeUpdate& pending : pending_stock_native_updates) {
        const BufferUpdateResult updated =
            device.update_buffer(*pending.buffer, 0U, pending.bytes);
        if (!updated.ok())
            return {updated.status == BufferStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : updated.status == BufferStatus::invalid_description
                              ? IndexedStaticMeshBatchStatus::invalid_request
                              : IndexedStaticMeshBatchStatus::execution_failed,
                    {updated.diagnostic.code.empty()
                         ? pending.d3d12_native
                               ? "static_scene_stock_d3d12_native_constant_update_failed"
                               : "static_scene_stock_vulkan_source_constant_update_failed"
                         : updated.diagnostic.code,
                     updated.diagnostic.message},
                    {}};
    }
    if (requires_directional_shadow_receiver) {
        Diagnostic update_diagnostic;
        const BufferStatus range_status = validate_buffer_update(
            *owned_directional_shadow_constants_, 0U,
            directional_shadow_bytes.size(), update_diagnostic);
        if (range_status != BufferStatus::ready)
            return {range_status == BufferStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : range_status == BufferStatus::invalid_description
                              ? IndexedStaticMeshBatchStatus::invalid_request
                              : IndexedStaticMeshBatchStatus::execution_failed,
                    {"static_scene_directional_shadow_constant_range_invalid",
                     update_diagnostic.message}, {}};
        const BufferUpdateResult updated = device.update_buffer(
            *owned_directional_shadow_constants_, 0U,
            directional_shadow_bytes);
        if (!updated.ok())
            return {updated.status == BufferStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : updated.status == BufferStatus::invalid_description
                              ? IndexedStaticMeshBatchStatus::invalid_request
                              : IndexedStaticMeshBatchStatus::execution_failed,
                    {updated.diagnostic.code.empty()
                         ? "static_scene_directional_shadow_constant_update_failed"
                         : updated.diagnostic.code,
                     updated.diagnostic.message}, {}};
    }
    for (PendingSkinUpdate& pending : pending_skin_updates) {
        SkinnedMeshPoseUpdateResult committed = skinned_uploads_[pending.upload_index]->commit_pose(
            device, *pending.packet, pending.vertices);
        if (!committed.ok())
            return {committed.status == SkinnedMeshUploadStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : IndexedStaticMeshBatchStatus::execution_failed,
                    std::move(committed.diagnostic), {}};
    }
    if (owns_frame_constants()) {
        std::array<std::byte, portable_frame_buffer_view_bytes> bytes{};
        KsPerPixelFrameConstants constants = *frame.frame_constants;
        constants.camera_position = {frame.camera.position[0], frame.camera.position[1],
                                     frame.camera.position[2], 0.0F};
        std::memcpy(bytes.data(), &constants, sizeof(constants));
        Diagnostic update_diagnostic;
        const BufferStatus range_status = validate_buffer_update(
            *owned_frame_constants_, 0U, bytes.size(), update_diagnostic);
        if (range_status != BufferStatus::ready)
            return {range_status == BufferStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : range_status == BufferStatus::invalid_description
                              ? IndexedStaticMeshBatchStatus::invalid_request
                              : IndexedStaticMeshBatchStatus::execution_failed,
                    {"static_scene_frame_constant_range_invalid",
                     update_diagnostic.message}, {}};
        const BufferUpdateResult updated =
            device.update_buffer(*owned_frame_constants_, 0U, bytes);
        if (!updated.ok())
            return {updated.status == BufferStatus::unsupported
                        ? IndexedStaticMeshBatchStatus::unsupported
                        : updated.status == BufferStatus::invalid_description
                              ? IndexedStaticMeshBatchStatus::invalid_request
                              : IndexedStaticMeshBatchStatus::execution_failed,
                    {updated.diagnostic.code.empty()
                         ? "static_scene_frame_constant_update_failed"
                         : updated.diagnostic.code,
                     updated.diagnostic.message}, {}};
    }
    return device.draw_indexed_static_mesh_batch_and_readback(target, batch);
} catch (const std::bad_alloc&) {
    return {IndexedStaticMeshBatchStatus::execution_failed,
            {"static_scene_frame_allocation_failed",
             "Static-scene frame preparation has insufficient memory for bounded storage"},
            {}};
}

StaticSceneDirectionalShadowResult
StaticSceneResources::draw_opaque_directional_shadows(
    Device& device,
    const StaticSceneDirectionalShadowFrameDescription& frame) try {
    const auto fail = [](StaticSceneDirectionalShadowStatus status,
                         std::string code, std::string message) {
        StaticSceneDirectionalShadowResult result;
        result.status = status;
        result.diagnostic = {std::move(code), std::move(message)};
        return result;
    };
    if (frame.maps == nullptr)
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_frame_handle_missing",
                    "Directional shadow execution requires map handles");
    DirectionalShadowMapResources& maps = *frame.maps;
    if (&device != device_ || &device != maps.device_ ||
        device.info().backend != backend_ || maps.backend_ != backend_)
        return fail(StaticSceneDirectionalShadowStatus::unsupported,
                    "directional_shadow_device_mismatch",
                    "Directional shadow resources require their preparing device and backend");
    if (packets_.empty() || packets_.size() != upload_for_packet_.size() ||
        packets_.size() != skinned_upload_for_packet_.size())
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_scene_resources_invalid",
                    "Static-scene geometry mappings are incomplete");
    if (!frame.refreshed_packets.empty() &&
        frame.refreshed_packets.size() != packets_.size())
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_packet_count_invalid",
                    "Refreshed shadow packets must match the prepared packet count");
    if (!frame.packet_visibility.empty() &&
        frame.packet_visibility.size() != packets_.size())
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_visibility_mask_count_invalid",
                    "Shadow packet visibility must match the prepared packet count");
    if (std::any_of(frame.packet_visibility.begin(), frame.packet_visibility.end(),
                    [](std::uint8_t value) { return value > 1U; }))
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_visibility_mask_value_invalid",
                    "Shadow packet visibility values must be 0 or 1");
    if (!frame.shadow_packet_order.empty() &&
        frame.shadow_packet_order.size() != packets_.size())
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_packet_order_count_invalid",
                    "Shadow packet order must contain every prepared packet");
    std::array<bool, max_indexed_static_mesh_batch_draws> ordered_packets{};
    for (const std::uint32_t packet_index : frame.shadow_packet_order) {
        if (static_cast<std::size_t>(packet_index) >= packets_.size())
            return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                        "directional_shadow_packet_order_index_invalid",
                        "Shadow packet order contains an out-of-range index");
        if (ordered_packets[packet_index])
            return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                        "directional_shadow_packet_order_duplicate",
                        "Shadow packet order contains a duplicate index");
        ordered_packets[packet_index] = true;
    }
    if (maps.metadata_.cascades.size() != directional_shadow_cascade_count ||
        maps.metadata_.splits.size() != directional_shadow_cascade_count)
        return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                    "directional_shadow_map_contract_invalid",
                    "Directional shadow resources must retain exactly three cascades");

    const auto packet_for_frame = [&](std::size_t index) -> const DrawPacket& {
        return frame.refreshed_packets.empty() ? packets_[index]
                                               : frame.refreshed_packets[index];
    };
    const auto packet_visible = [&](std::size_t index) {
        return frame.packet_visibility.empty() || frame.packet_visibility[index] != 0U;
    };
    const auto alpha_material_buffer = [&](std::size_t index) -> const Buffer* {
        if (index >= stock_shadow_constant_for_material_.size()) return nullptr;
        const std::size_t material_index =
            stock_shadow_constant_for_material_[index];
        if (material_index == invalid_resource_index ||
            material_index >= owned_stock_shadow_constants_.size())
            return nullptr;
        return owned_stock_shadow_constants_[material_index].get();
    };
    const auto alpha_diffuse_binding =
        [&](std::size_t index) -> IndexedSampledTextureBinding {
        if (index >= textures_for_packet_.size()) return {};
        const std::uint32_t raw = textures_for_packet_[index].diffuse;
        if (raw == invalid_draw_texture_index) return {};
        const std::size_t texture_index = static_cast<std::size_t>(raw);
        if (texture_authority_ ==
            StaticSceneTextureAuthority::owned_model_payloads) {
            if (texture_index >= owned_textures_.size() ||
                owned_textures_[texture_index] == nullptr || owned_sampler_ == nullptr)
                return {};
            return {owned_textures_[texture_index].get(), owned_sampler_.get()};
        }
        if (texture_index >= frame.textures_by_global_index.size() ||
            texture_index >= frame.samplers_by_global_index.size() ||
            frame.textures_by_global_index[texture_index] == nullptr ||
            frame.samplers_by_global_index[texture_index] == nullptr)
            return {};
        return {frame.textures_by_global_index[texture_index],
                frame.samplers_by_global_index[texture_index]};
    };
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        const DrawPacket& packet = packet_for_frame(index);
        if (!same_prepared_draw_contract(packets_[index], packet) ||
            !finite_world_matrix(packet))
            return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                        "directional_shadow_packet_contract_invalid",
                        "A refreshed shadow packet changed its prepared contract or transform");
    }

    // MaterialFilterSM executes after the shadow pass establishes opaque
    // blending and normal depth state. Keep the retained color-pass packets
    // unchanged, but give every executable caster a shadow-local packet with
    // that recovered state. This is required for ordinary alpha-blended
    // materials, whose main pass disables depth writes before the shadow
    // filter selects ksShadowGenAT.
    std::vector<DrawPacket> shadow_packets;
    shadow_packets.reserve(packets_.size());
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        shadow_packets.push_back(packet_for_frame(index));
        DrawPacket& shadow_packet = shadow_packets.back();
        shadow_packet.flags.transparent = false;
        shadow_packet.flags.blend_enabled = false;
        shadow_packet.flags.alpha_to_coverage = false;
        shadow_packet.flags.depth_test = true;
        shadow_packet.flags.depth_write = true;
    }

    StaticSceneDirectionalShadowResult result;
    std::vector<std::size_t> executable_indices;
    std::vector<std::uint8_t> alpha_shadow_packets(packets_.size(), 0U);
    executable_indices.reserve(packets_.size());
    result.staged_casters.reserve(packets_.size());
    for (std::size_t order = 0U; order < packets_.size(); ++order) {
        const std::size_t index = frame.shadow_packet_order.empty()
                                      ? order
                                      : static_cast<std::size_t>(
                                            frame.shadow_packet_order[order]);
        if (!packet_visible(index)) continue;
        const DrawPacket& packet = packet_for_frame(index);
        if (!packet.flags.cast_shadows) continue;
        ++result.selected_casters;
        if (packet.primitive == DrawPrimitiveKind::skinned_mesh) {
            // MaterialFilterSM selects ksShadowGenSKIN before it tests the
            // material blend mode. A non-opaque skinned caster therefore
            // stays on the null-pixel skinned path and does not sample alpha.
            if (frame.skinned_pipeline == nullptr) {
                ++result.staged_skinned;
                result.staged_casters.push_back({
                    "directional_shadow_caster_skinning_staged", packet.node,
                    packet.material});
                continue;
            }
            if (index >= skinned_upload_for_packet_.size() ||
                skinned_upload_for_packet_[index] >= skinned_uploads_.size() ||
                skinned_uploads_[skinned_upload_for_packet_[index]] == nullptr)
                return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                            "directional_shadow_caster_geometry_invalid",
                            "A skinned shadow caster has no retained skinned geometry");
            executable_indices.push_back(index);
            ++result.skinned_casters;
            continue;
        }
        if (packet.material_profile.shadow_alpha_tested) {
            if (frame.alpha_static_pipeline == nullptr ||
                alpha_material_buffer(static_cast<std::size_t>(packet.material)) == nullptr ||
                alpha_diffuse_binding(index).texture == nullptr ||
                alpha_diffuse_binding(index).sampler == nullptr) {
                ++result.staged_alpha_tested;
                result.staged_casters.push_back({
                    "directional_shadow_caster_alpha_test_staged", packet.node,
                    packet.material});
                continue;
            }
            if (index >= upload_for_packet_.size() ||
                upload_for_packet_[index] >= uploads_.size() ||
                uploads_[upload_for_packet_[index]] == nullptr)
                return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                            "directional_shadow_caster_geometry_invalid",
                            "An alpha-tested shadow caster has no retained static geometry");
            alpha_shadow_packets[index] = 1U;
            executable_indices.push_back(index);
            ++result.alpha_tested_casters;
            continue;
        }
        if (frame.opaque_pipeline == nullptr) {
            result.staged_casters.push_back({
                "directional_shadow_caster_shader_staged", packet.node,
                packet.material});
            continue;
        }
        if (index >= upload_for_packet_.size() ||
            upload_for_packet_[index] >= uploads_.size() ||
            uploads_[upload_for_packet_[index]] == nullptr)
            return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                        "directional_shadow_caster_geometry_invalid",
                        "An opaque shadow caster has no retained static geometry");
        executable_indices.push_back(index);
        ++result.opaque_casters;
    }
    const auto clear_all_maps = [&]() -> std::optional<StaticSceneDirectionalShadowResult> {
        for (std::size_t cascade = 0U;
             cascade < directional_shadow_cascade_count; ++cascade) {
            const DepthOnlyIndexedStaticMeshBatchDescription batch{
                {}, maps.attachments_[cascade].get(), true, 1.0F};
            const auto cleared =
                device.draw_depth_only_indexed_static_mesh_batch(batch);
            if (!cleared.ok())
                return fail(
                    cleared.status ==
                            DepthOnlyIndexedStaticMeshBatchStatus::unsupported
                        ? StaticSceneDirectionalShadowStatus::unsupported
                        : cleared.status ==
                                  DepthOnlyIndexedStaticMeshBatchStatus::invalid_request
                              ? StaticSceneDirectionalShadowStatus::invalid_request
                              : StaticSceneDirectionalShadowStatus::execution_failed,
                    cleared.diagnostic.code, cleared.diagnostic.message);
            ++result.cascades_completed;
        }
        return std::nullopt;
    };
    if (result.selected_casters == 0U) {
        if (auto failure = clear_all_maps(); failure.has_value())
            return std::move(*failure);
        result.status = StaticSceneDirectionalShadowStatus::ready;
        result.diagnostic = {"directional_shadow_no_casters",
                             "The scene selected no casters; all three maps contain clear depth"};
        return result;
    }
    if (executable_indices.empty()) {
        // A staged-only frame must not expose depth left by an earlier frame.
        // Clear every retained cascade even though no selected caster has an
        // executable program yet.
        if (auto failure = clear_all_maps(); failure.has_value())
            return std::move(*failure);
        result.status = StaticSceneDirectionalShadowStatus::partial;
        result.diagnostic = {"directional_shadow_all_casters_staged",
                             "All selected directional shadow casters require staged programs; all three maps contain clear depth"};
        return result;
    }

    // The packet flag is an explicit, bounded handoff for the recovered
    // doubleFaceShadow rule. KN5 parsing leaves it at the stock default
    // (false/back cull); callers with source-evidenced overrides may set it.
    PipelineProgram back_cull_pipeline;
    PipelineProgram double_face_pipeline;
    if (result.opaque_casters != 0U) {
        if (frame.opaque_pipeline == nullptr)
            return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                        "directional_shadow_opaque_pipeline_missing",
                        "Executable opaque shadow casters require an opaque pipeline");
        back_cull_pipeline = *frame.opaque_pipeline;
        back_cull_pipeline.raster.cull =
            stock_directional_shadow_cull_mode(false);
        double_face_pipeline = back_cull_pipeline;
        double_face_pipeline.raster.cull = stock_directional_shadow_cull_mode(true);
    }
    PipelineProgram alpha_back_cull_pipeline;
    PipelineProgram alpha_double_face_pipeline;
    if (std::any_of(alpha_shadow_packets.begin(), alpha_shadow_packets.end(),
                    [](std::uint8_t value) { return value != 0U; })) {
        alpha_back_cull_pipeline = *frame.alpha_static_pipeline;
        alpha_back_cull_pipeline.raster.cull =
            stock_directional_shadow_cull_mode(false);
        alpha_double_face_pipeline = alpha_back_cull_pipeline;
        alpha_double_face_pipeline.raster.cull =
            stock_directional_shadow_cull_mode(true);
    }
    PipelineProgram skinned_back_cull_pipeline;
    PipelineProgram skinned_double_face_pipeline;
    if (result.skinned_casters != 0U) {
        if (frame.skinned_pipeline->vertex_layout.stride != 19U * sizeof(float))
            return fail(StaticSceneDirectionalShadowStatus::invalid_request,
                        "directional_shadow_skinned_pipeline_stride_invalid",
                        "Skinned directional-shadow pipelines require the retained 19-float vertex stream");
        skinned_back_cull_pipeline = *frame.skinned_pipeline;
        skinned_back_cull_pipeline.raster.cull =
            stock_directional_shadow_cull_mode(false);
        skinned_double_face_pipeline = skinned_back_cull_pipeline;
        skinned_double_face_pipeline.raster.cull =
            stock_directional_shadow_cull_mode(true);
    }

    struct PendingSkinUpdate {
        std::size_t upload_index = invalid_resource_index;
        const DrawPacket* packet = nullptr;
        std::vector<float> vertices;
    };
    std::vector<PendingSkinUpdate> pending_skin_updates;
    pending_skin_updates.reserve(result.skinned_casters);
    for (const std::size_t index : executable_indices) {
        const DrawPacket& packet = packet_for_frame(index);
        if (packet.primitive != DrawPrimitiveKind::skinned_mesh) continue;
        PendingSkinUpdate pending;
        pending.upload_index = skinned_upload_for_packet_[index];
        pending.packet = &packet;
        SkinnedMeshPoseUpdateResult prepared =
            skinned_uploads_[pending.upload_index]->prepare_pose(packet);
        if (!prepared.ok())
            return fail(prepared.status == SkinnedMeshUploadStatus::unsupported
                            ? StaticSceneDirectionalShadowStatus::unsupported
                            : StaticSceneDirectionalShadowStatus::invalid_request,
                        prepared.diagnostic.code, prepared.diagnostic.message);
        pending.vertices = std::move(prepared.skinned_vertices);
        pending_skin_updates.push_back(std::move(pending));
    }

    std::array<std::vector<DepthOnlyIndexedStaticMeshDrawRequest>,
               directional_shadow_cascade_count> cascade_draws;
    for (std::size_t cascade = 0U; cascade < directional_shadow_cascade_count;
         ++cascade) {
        auto& draws = cascade_draws[cascade];
        draws.reserve(executable_indices.size());
        for (const std::size_t index : executable_indices) {
            const DrawPacket& packet = shadow_packets[index];
            DepthOnlyIndexedStaticMeshDrawRequest draw;
            draw.packet = &packet;
            if (packet.primitive == DrawPrimitiveKind::skinned_mesh) {
                const SkinnedMeshUpload& upload =
                    *skinned_uploads_[skinned_upload_for_packet_[index]];
                draw.pipeline = packet.flags.double_face_shadow
                                    ? &skinned_double_face_pipeline
                                    : &skinned_back_cull_pipeline;
                draw.vertex_buffer = upload.vertex_buffer.get();
                draw.index_buffer = upload.index_buffer.get();
            } else {
                const StaticMeshUpload& upload = *uploads_[upload_for_packet_[index]];
                if (alpha_shadow_packets[index]) {
                    draw.pipeline = packet.flags.double_face_shadow
                                        ? &alpha_double_face_pipeline
                                        : &alpha_back_cull_pipeline;
                    draw.material_mode =
                        DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::
                            stock_alpha_tested;
                    draw.resource_authority =
                        IndexedResourceAuthority::explicit_bindings;
                    draw.alpha_tested_diffuse_binding =
                        alpha_diffuse_binding(index);
                    draw.alpha_tested_material_binding = {
                        alpha_material_buffer(
                            static_cast<std::size_t>(packet.material)),
                        0U, stock_shadow_caster_material_bytes};
                } else {
                    draw.pipeline = packet.flags.double_face_shadow
                                        ? &double_face_pipeline
                                        : &back_cull_pipeline;
                }
                draw.vertex_buffer = upload.vertex_buffer.get();
                draw.index_buffer = upload.index_buffer.get();
            }
            draw.camera_frame = maps.cameras_[cascade];
            draws.push_back(std::move(draw));
        }
        const DepthOnlyIndexedStaticMeshBatchDescription batch{
            draws, maps.attachments_[cascade].get(), true, 1.0F};
        Diagnostic diagnostic;
        const auto validation =
            validate_depth_only_indexed_static_mesh_batch_description(batch,
                                                                       diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshBatchStatus::ready)
            return fail(validation == DepthOnlyIndexedStaticMeshBatchStatus::unsupported
                            ? StaticSceneDirectionalShadowStatus::unsupported
                            : StaticSceneDirectionalShadowStatus::invalid_request,
                        diagnostic.code, diagnostic.message);
    }

    // Skinning output is prepared above and committed only after every
    // cascade has passed validation, preserving the scene's failure-atomic
    // preflight boundary before the first depth write.
    for (PendingSkinUpdate& pending : pending_skin_updates) {
        SkinnedMeshPoseUpdateResult committed =
            skinned_uploads_[pending.upload_index]->commit_pose(
                device, *pending.packet, pending.vertices);
        if (!committed.ok())
            return fail(committed.status == SkinnedMeshUploadStatus::unsupported
                            ? StaticSceneDirectionalShadowStatus::unsupported
                            : StaticSceneDirectionalShadowStatus::execution_failed,
                        committed.diagnostic.code, committed.diagnostic.message);
    }

    // All packets, all three matrices, and all backend resource ranges have
    // passed before the first depth write. Runtime failure after a submission
    // requires the caller to retry the complete set, which clears every map.
    for (std::size_t cascade = 0U; cascade < directional_shadow_cascade_count;
         ++cascade) {
        const DepthOnlyIndexedStaticMeshBatchDescription batch{
            cascade_draws[cascade], maps.attachments_[cascade].get(), true, 1.0F};
        const auto drawn = device.draw_depth_only_indexed_static_mesh_batch(batch);
        if (!drawn.ok()) {
            result.status = drawn.status == DepthOnlyIndexedStaticMeshBatchStatus::unsupported
                                ? StaticSceneDirectionalShadowStatus::unsupported
                                : drawn.status ==
                                          DepthOnlyIndexedStaticMeshBatchStatus::invalid_request
                                      ? StaticSceneDirectionalShadowStatus::invalid_request
                                      : StaticSceneDirectionalShadowStatus::execution_failed;
            result.diagnostic = std::move(drawn.diagnostic);
            return result;
        }
        ++result.cascades_completed;
    }
    result.status = result.staged_casters.empty()
                        ? StaticSceneDirectionalShadowStatus::ready
                        : StaticSceneDirectionalShadowStatus::partial;
    result.diagnostic = result.status == StaticSceneDirectionalShadowStatus::partial
                            ? Diagnostic{
                                  "directional_shadow_partial",
                                  "Executable shadow casters completed. One or more selected casters remain staged"}
                            : Diagnostic{};
    return result;
} catch (const std::bad_alloc&) {
    StaticSceneDirectionalShadowResult result;
    result.status = StaticSceneDirectionalShadowStatus::execution_failed;
    result.diagnostic = {
        "directional_shadow_frame_allocation_failed",
        "Directional shadow frame preparation has insufficient memory"};
    return result;
}

}  // namespace apex::render
