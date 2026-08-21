#include "apex/render/static_scene.hpp"

#include "apex/render/decoded_dds_texture.hpp"

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
    return true;
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
           left.selected == right.selected && left.cast_shadows == right.cast_shadows;
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
    const PipelineLimits& limits) {
    const PipelineValidationResult validation = validate_pipeline(pipeline, limits);
    if (!validation.valid) {
        if (!validation.diagnostics.empty())
            return {StaticSceneResourceStatus::invalid_request,
                    "Pipeline validation failed: " + validation.diagnostics.front().message, 0U};
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline validation failed without a diagnostic", 0U};
    }
    const auto shader_bytes = static_cast<std::uint64_t>(validation.shader_bytes);
    if (pipeline.transform_contract != PipelineTransformContract::draw_matrices)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution requires the draw-matrices transform contract", 0U};
    if (pipeline.blend.alpha_to_coverage)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution does not support alpha-to-coverage", 0U};
    if (classify_indexed_portable_resource_layout(pipeline) ==
        IndexedPortableResourceLayout::unsupported)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene material execution requires the portable diffuse layout with optional constants", 0U};
    if (pipeline.resources.empty() != packet.resources.empty())
        return {StaticSceneResourceStatus::invalid_request,
                "Pipeline and draw-packet resource declarations do not match", 0U};
    if (pipeline.targets.colors.size() != 1U || pipeline.targets.colors.front().samples != 1U)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution requires one single-sample color target", 0U};
    const PipelineRenderTargetFormat color_format = pipeline.targets.colors.front().format;
    if (color_format != PipelineRenderTargetFormat::rgba8_unorm &&
        color_format != PipelineRenderTargetFormat::rgba8_srgb &&
        color_format != PipelineRenderTargetFormat::bgra8_unorm &&
        color_format != PipelineRenderTargetFormat::bgra8_srgb)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution supports only RGBA8 and BGRA8 color targets", 0U};
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
         pipeline.targets.depth.samples != 1U))
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution requires a single-sample D32 depth target", 0U};
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
    const PipelineShaderFormat required_format = backend == Backend::Vulkan
                                                     ? PipelineShaderFormat::spirv
                                                     : PipelineShaderFormat::dxil;
    for (const PipelineShaderModule& shader : pipeline.shaders) {
        vertex = vertex || shader.stage == PipelineShaderStage::vertex;
        fragment = fragment || shader.stage == PipelineShaderStage::fragment;
        if (shader.format != required_format)
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
        request.texture_authority != StaticSceneTextureAuthority::embedded_kn5)
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_texture_authority_invalid",
                    "The static-scene texture authority is invalid");
    const auto& model = *request.model;
    const auto& scene = *request.scene;
    const auto& limits = request.limits;
    const bool embedded_textures =
        request.texture_authority == StaticSceneTextureAuthority::embedded_kn5;
    if (limits.max_draws == 0U || limits.max_draws > max_indexed_static_mesh_batch_draws ||
        limits.max_materials == 0U || limits.max_textures == 0U ||
        limits.max_resource_string_bytes == 0U ||
        (embedded_textures &&
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
    if (model.materials.size() != scene.materials.size() ||
        model.materials.size() > limits.max_materials ||
        request.pipelines_by_material.size() != model.materials.size())
        return fail(StaticSceneResourceStatus::invalid_request, "static_scene_material_table_invalid",
                    "Model, scene, and pipeline material tables must have the same bounded size");
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
        !charge(model.materials.size(), 2U * sizeof(std::size_t)) ||
        !charge(request.packets.size(),
                3U * sizeof(std::size_t) + 2U * sizeof(const formats::Kn5Node*)) ||
        (embedded_textures &&
         (!charge(model.textures.size(),
                  sizeof(std::optional<DecodedDdsTexturePlan>) + 2U * sizeof(bool)) ||
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
                sizeof(KsPerPixelMaterialConstants)))
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
    std::vector<StaticSceneResources::PacketTextureIndices> textures_for_packet(
        request.packets.size());
    std::vector<std::size_t> material_constant_by_material(model.materials.size(),
                                                           invalid_resource_index);
    std::vector<std::size_t> material_constant_for_packet(request.packets.size(),
                                                          invalid_resource_index);
    std::vector<const formats::Kn5Node*> unique_meshes;
    std::vector<std::size_t> representative_packets;
    std::vector<const formats::Kn5Node*> skinned_meshes;
    std::vector<std::size_t> skinned_packet_indices;
    std::vector<PipelineProgram> pipelines;
    std::vector<KsPerPixelMaterialConstants> material_constants;
    unique_meshes.reserve(request.packets.size());
    representative_packets.reserve(request.packets.size());
    pipelines.reserve(request.packets.size());
    material_constants.reserve(std::min(request.packets.size(),
                                        limits.max_material_constant_buffers));

    std::uint64_t total_vertex_bytes = 0U;
    std::uint64_t total_index_bytes = 0U;
    std::uint64_t total_shader_bytes = 0U;
    std::uint64_t total_material_constant_bytes = 0U;
    std::uint64_t total_update_bytes = 0U;
    std::uint64_t validation_bytes = 0U;
    std::optional<bool> batch_has_depth;
    std::optional<PipelineRenderTargetFormat> batch_color_format;
    bool requires_frame_constants = false;
    for (std::size_t packet_index = 0U; packet_index < request.packets.size(); ++packet_index) {
        const DrawPacket& packet = request.packets[packet_index];
        const auto node_index = static_cast<std::size_t>(packet.node);
        const auto material_index = static_cast<std::size_t>(packet.material);
        const bool static_mesh = packet.primitive == DrawPrimitiveKind::static_mesh;
        const bool skinned_mesh = packet.primitive == DrawPrimitiveKind::skinned_mesh;
        if ((!static_mesh && !skinned_mesh) || packet.flags.alpha_to_coverage)
            return fail(StaticSceneResourceStatus::unsupported,
                        "static_scene_packet_unsupported",
                        "Static-scene execution supports static or skinned meshes without alpha-to-coverage");
        if ((static_mesh && !packet.bone_palette.empty()) ||
            (skinned_mesh && packet.bone_palette.empty()))
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_packet_palette_invalid",
                        "Static and skinned packets must have the corresponding bone-palette state");
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

        const PipelineProgram* pipeline = request.pipelines_by_material[material_index];
        if (pipeline == nullptr)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "static_scene_material_pipeline_missing",
                        "A used static-scene material has no executable pipeline");
        const ExecutorPipelineValidation pipeline_validation = validate_executor_pipeline(
            device.info().backend, *pipeline, packet, limits.pipeline);
        if (!pipeline_validation.ok())
            return fail(pipeline_validation.status,
                        pipeline_validation.status == StaticSceneResourceStatus::invalid_request
                            ? "static_scene_material_pipeline_invalid"
                            : "static_scene_material_pipeline_unsupported",
                        pipeline_validation.error);
        const IndexedPortableResourceLayout material_layout =
            classify_indexed_portable_resource_layout(*pipeline);
        const bool normal_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame;
        const bool maps_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame;
        const bool detail_stack_layout =
            material_layout ==
            IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame;
        if (!pipeline->resources.empty()) {
            const std::size_t expected_resources =
                detail_stack_layout ? 5U : maps_layout ? 3U : normal_layout ? 2U : 1U;
            if (packet.resources.size() != expected_resources)
                return fail(StaticSceneResourceStatus::unsupported,
                            "static_scene_material_packet_unsupported",
                            detail_stack_layout
                                ? "The portable detail-stack path requires txDiffuse, txNormal, txMaps, txDetail, and txNormalDetail packet resources"
                                : maps_layout
                                ? "The portable txMaps path requires txDiffuse, txNormal, and txMaps packet resources"
                                : normal_layout
                                    ? "The portable ksPerPixelNM path requires txDiffuse and txNormal packet resources"
                                    : "The portable diffuse path requires one txDiffuse packet resource");
            for (const DrawResourceSlot& resource : packet.resources) {
                if (resource.slot.size() > limits.max_resource_string_bytes ||
                    resource.texture.size() > limits.max_resource_string_bytes)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "static_scene_resource_string_limit",
                                "A static-scene resource string exceeds its byte limit");
                const bool diffuse = canonical_resource_equals(resource.slot, "txdiffuse");
                const bool normal = (normal_layout || maps_layout || detail_stack_layout) &&
                                    canonical_resource_equals(resource.slot, "txnormal");
                const bool maps = (maps_layout || detail_stack_layout) &&
                                  canonical_resource_equals(resource.slot, "txmaps");
                const bool detail = detail_stack_layout &&
                                    canonical_resource_equals(resource.slot, "txdetail");
                const bool normal_detail =
                    detail_stack_layout &&
                    (canonical_resource_equals(resource.slot, "txnormaldetail") ||
                     canonical_resource_equals(resource.slot, "txdetailnm"));
                if (!diffuse && !normal && !maps && !detail && !normal_detail)
                    return fail(StaticSceneResourceStatus::unsupported,
                                "static_scene_material_packet_unsupported",
                                detail_stack_layout
                                    ? "The portable detail-stack path accepts only txDiffuse, txNormal, txMaps, txDetail, and txNormalDetail"
                                    : maps_layout
                                    ? "The portable txMaps path accepts only txDiffuse, txNormal, and txMaps"
                                    : normal_layout
                                        ? "The portable ksPerPixelNM path accepts only txDiffuse and txNormal"
                                        : "The portable diffuse path accepts only txDiffuse");
                std::uint32_t& stored_index = diffuse
                                                  ? textures_for_packet[packet_index].diffuse
                                                  : normal
                                                      ? textures_for_packet[packet_index].normal
                                                      : maps
                                                          ? textures_for_packet[packet_index].maps
                                                          : detail
                                                              ? textures_for_packet[packet_index].detail
                                                              : textures_for_packet[packet_index].normal_detail;
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
                                                                  : "static_scene_normal_detail_texture_index_invalid",
                                diffuse ? "A txDiffuse resource has an invalid global texture index"
                                        : normal ? "A txNormal resource has an invalid global texture index"
                                                 : maps ? "A txMaps resource has an invalid global texture index"
                                                        : detail ? "A txDetail resource has an invalid global texture index"
                                                                  : "A txNormalDetail resource has an invalid global texture index");
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
                                                                  : "static_scene_normal_detail_texture_identity_invalid",
                                diffuse
                                    ? "A txDiffuse resource does not match its unique active scoped KN5 texture"
                                    : normal
                                        ? "A txNormal resource does not match its unique active scoped KN5 texture"
                                        : maps
                                            ? "A txMaps resource does not match its unique active scoped KN5 texture"
                                            : detail
                                                ? "A txDetail resource does not match its unique active scoped KN5 texture"
                                                : "A txNormalDetail resource does not match its unique active scoped KN5 texture");
                stored_index = resource.texture_index;
            }
            if (textures_for_packet[packet_index].diffuse == invalid_draw_texture_index ||
                ((normal_layout || maps_layout || detail_stack_layout) &&
                 textures_for_packet[packet_index].normal == invalid_draw_texture_index) ||
                ((maps_layout || detail_stack_layout) && textures_for_packet[packet_index].maps ==
                                    invalid_draw_texture_index) ||
                (detail_stack_layout &&
                 (textures_for_packet[packet_index].detail == invalid_draw_texture_index ||
                  textures_for_packet[packet_index].normal_detail == invalid_draw_texture_index)))
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_resource_slot_missing",
                            detail_stack_layout
                                ? "The portable detail-stack resources are incomplete"
                                : maps_layout
                                ? "The portable txMaps resources are incomplete"
                                : normal_layout
                                    ? "The portable ksPerPixelNM resources are incomplete"
                                    : "The portable diffuse resource is missing");
        }
        if (material_layout == IndexedPortableResourceLayout::diffuse_with_frame ||
            material_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            normal_layout || maps_layout || detail_stack_layout)
            requires_frame_constants = true;
        if (material_layout == IndexedPortableResourceLayout::diffuse_with_constants ||
            material_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            normal_layout || maps_layout || detail_stack_layout) {
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
                if ((normal_layout || maps_layout || detail_stack_layout) && constants.fresnel[2] > 0.0F)
                    return fail(StaticSceneResourceStatus::unsupported,
                                "static_scene_normal_fresnel_unsupported",
                                "The bounded ksPerPixelNM path requires disabled Fresnel reflection");
                material_constant_by_material[material_index] = material_constants.size();
                material_constants.push_back(constants);
            }
            material_constant_for_packet[packet_index] =
                material_constant_by_material[material_index];
        }
        const PipelineRenderTargetFormat color_format = pipeline->targets.colors.front().format;
        if (batch_color_format.has_value() && *batch_color_format != color_format)
            return fail(StaticSceneResourceStatus::unsupported,
                        "static_scene_mixed_color_targets_unsupported",
                        "One ordered static-scene batch requires one color-target format");
        batch_color_format = color_format;
        if (pipeline_by_material[material_index] == invalid_resource_index) {
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
        pipeline_for_packet[packet_index] = pipeline_by_material[material_index];
    }

    if (requires_frame_constants &&
        limits.max_total_frame_constant_bytes < portable_frame_buffer_view_bytes)
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_frame_constant_aggregate_limit",
                    "The frame-constant buffer exceeds the static-scene aggregate limit");

    std::vector<std::optional<DecodedDdsTexturePlan>> decoded_textures;
    if (embedded_textures) {
        decoded_textures.resize(model.textures.size());
        std::vector<bool> maps_texture_indices(model.textures.size(), false);
        std::vector<bool> normal_detail_texture_indices(model.textures.size(), false);
        for (const StaticSceneResources::PacketTextureIndices& packet_textures :
             textures_for_packet) {
            if (packet_textures.maps != invalid_draw_texture_index)
                maps_texture_indices[static_cast<std::size_t>(packet_textures.maps)] = true;
            if (packet_textures.normal_detail != invalid_draw_texture_index)
                normal_detail_texture_indices[
                    static_cast<std::size_t>(packet_textures.normal_detail)] = true;
        }
        std::uint64_t total_source_bytes = 0U;
        std::uint64_t total_decoded_bytes = 0U;
        for (const StaticSceneResources::PacketTextureIndices& packet_textures :
             textures_for_packet) {
            for (const std::uint32_t raw_index :
                 {packet_textures.diffuse, packet_textures.normal,
                  packet_textures.maps, packet_textures.detail,
                  packet_textures.normal_detail}) {
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
                DecodedDdsTexturePlanResult decoded = plan_decoded_dds_texture(
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
                     normal_detail_texture_indices[texture_index]) &&
                    decoded.plan.description.format != TextureFormat::rgba8_unorm &&
                    decoded.plan.description.format != TextureFormat::bgra8_unorm)
                    return fail(StaticSceneResourceStatus::unsupported,
                                maps_texture_indices[texture_index]
                                    ? "static_scene_maps_texture_format_unsupported"
                                    : "static_scene_normal_detail_texture_format_unsupported",
                                maps_texture_indices[texture_index]
                                    ? "An embedded txMaps texture must decode to linear RGBA8 or BGRA8"
                                    : "An embedded txNormalDetail texture must decode to linear RGBA8 or BGRA8");
                if (!charge(decoded.plan.levels.size(), sizeof(formats::DdsLevel)) ||
                    !charge(decoded.plan.levels.size(), sizeof(TextureUpload)))
                    return preparation_limit();
                for (const formats::DdsLevel& level : decoded.plan.levels) {
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

    auto resources = std::make_unique<StaticSceneResources>();
    resources->backend_ = device.info().backend;
    resources->device_ = &device;
    resources->packets_.assign(request.packets.begin(), request.packets.end());
    resources->pipelines_ = std::move(pipelines);
    resources->upload_for_packet_ = std::move(upload_for_packet);
    resources->skinned_upload_for_packet_ = std::move(skinned_upload_for_packet);
    resources->pipeline_for_packet_ = std::move(pipeline_for_packet);
    resources->textures_for_packet_ = std::move(textures_for_packet);
    resources->material_constant_for_packet_ =
        std::move(material_constant_for_packet);
    resources->texture_count_ = model.textures.size();
    resources->texture_authority_ = request.texture_authority;
    for (const StaticSceneResources::PacketTextureIndices& indices :
         resources->textures_for_packet_)
        resources->has_texture_resources_ = resources->has_texture_resources_ ||
                                            indices.diffuse != invalid_draw_texture_index ||
                                            indices.normal != invalid_draw_texture_index ||
                                            indices.maps != invalid_draw_texture_index ||
                                            indices.detail != invalid_draw_texture_index ||
                                            indices.normal_detail != invalid_draw_texture_index;
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
    if (resources->has_texture_resources_ &&
        request.texture_authority == StaticSceneTextureAuthority::embedded_kn5) {
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
        request.texture_authority == StaticSceneTextureAuthority::embedded_kn5) {
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

IndexedStaticMeshBatchResult StaticSceneResources::draw_and_readback(
    Device& device, Texture& target, const StaticSceneFrameDescription& frame) {
    if (&device != device_ || device.info().backend != backend_ || target.backend() != backend_)
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_device_mismatch",
                 "Static-scene resources require their preparing device and backend"}, {}};
    if (texture_authority_ != StaticSceneTextureAuthority::caller_tables &&
        texture_authority_ != StaticSceneTextureAuthority::embedded_kn5)
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_texture_authority_invalid",
                 "The prepared static-scene texture authority is invalid"}, {}};
    if (packets_.empty() || packets_.size() != upload_for_packet_.size() ||
        packets_.size() != pipeline_for_packet_.size() ||
        packets_.size() != textures_for_packet_.size() ||
        packets_.size() != material_constant_for_packet_.size() ||
        packets_.size() != skinned_upload_for_packet_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_resources_invalid", "Static-scene resource mappings are incomplete"}, {}};
    if (!frame.refreshed_packets.empty() &&
        frame.refreshed_packets.size() != packets_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_frame_packet_count_invalid",
                 "Refreshed static-scene packet states must match the prepared packet count"}, {}};
    if (has_texture_resources_ &&
        texture_authority_ == StaticSceneTextureAuthority::caller_tables &&
        (frame.textures_by_global_index.size() != texture_count_ ||
         frame.samplers_by_global_index.size() != texture_count_))
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_resource_table_size_invalid",
                 "Static-scene texture and sampler tables must match the final KN5 texture count"}, {}};

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
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        const DrawPacket& packet = packet_for_frame(index);
        if (!same_prepared_draw_contract(packets_[index], packet) || !finite_world_matrix(packet))
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_frame_packet_contract_invalid",
                     "Refreshed packet state changed a prepared draw contract"}, {}};
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
    std::vector<IndexedStaticMeshDrawRequest> draws;
    draws.reserve(packets_.size());
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        const DrawPacket& packet = packet_for_frame(index);
        const bool upload_index_invalid =
            packet.primitive == DrawPrimitiveKind::skinned_mesh
                ? skinned_upload_for_packet_[index] >= skinned_uploads_.size()
                : upload_for_packet_[index] >= uploads_.size();
        if (upload_index_invalid || pipeline_for_packet_[index] >= pipelines_.size())
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_resources_invalid", "Static-scene resource index is invalid"}, {}};
        Diagnostic diagnostic;
        std::optional<IndexedStaticMeshDrawRequest> draw;
        if (packet.primitive == DrawPrimitiveKind::skinned_mesh) {
            if (skinned_upload_for_packet_[index] >= skinned_uploads_.size())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_resources_invalid", "Skinned static-scene resource index is invalid"}, {}};
            draw = skinned_uploads_[skinned_upload_for_packet_[index]]->make_request(
                packet, pipelines_[pipeline_for_packet_[index]], frame.camera,
                0U, 0U, {0.0F, 0.0F, 0.0F, 1.0F}, diagnostic);
        } else {
            draw = uploads_[upload_for_packet_[index]]->make_request(
                packet, pipelines_[pipeline_for_packet_[index]], frame.camera,
                0U, 0U, {0.0F, 0.0F, 0.0F, 1.0F}, diagnostic);
        }
        if (!draw.has_value())
            return {IndexedStaticMeshBatchStatus::invalid_request, std::move(diagnostic), {}};
        draw->shader_authority = IndexedShaderAuthority::explicit_pipeline;
        const PacketTextureIndices texture_indices = textures_for_packet_[index];
        std::optional<IndexedStaticMeshBatchResult> resource_failure;
        const auto resolve_texture = [&](std::uint32_t raw_index,
                                         std::string_view role) {
            IndexedSampledTextureBinding binding;
            if (raw_index == invalid_draw_texture_index) return binding;
            const std::size_t resource_index = static_cast<std::size_t>(raw_index);
            if (texture_authority_ == StaticSceneTextureAuthority::embedded_kn5) {
                if (resource_index >= owned_textures_.size() ||
                    owned_textures_[resource_index] == nullptr || owned_sampler_ == nullptr) {
                    resource_failure = IndexedStaticMeshBatchResult{
                        IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_owned_resource_missing",
                         "A used static-scene " + std::string(role) +
                             " texture has no owned texture or sampler"}, {}};
                    return binding;
                }
                binding = {owned_textures_[resource_index].get(), owned_sampler_.get()};
            } else {
                if (resource_index >= texture_count_ ||
                    frame.textures_by_global_index[resource_index] == nullptr ||
                    frame.samplers_by_global_index[resource_index] == nullptr) {
                    resource_failure = IndexedStaticMeshBatchResult{
                        IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_resource_table_entry_missing",
                         "A used static-scene " + std::string(role) +
                             " table entry has no texture or sampler handle"}, {}};
                    return binding;
                }
                binding = {frame.textures_by_global_index[resource_index],
                           frame.samplers_by_global_index[resource_index]};
            }
            return binding;
        };
        draw->sampled_binding = resolve_texture(texture_indices.diffuse, "diffuse");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->normal_binding = resolve_texture(texture_indices.normal, "normal");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->maps_binding = resolve_texture(texture_indices.maps, "maps");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->detail_binding = resolve_texture(texture_indices.detail, "detail");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        draw->normal_detail_binding = resolve_texture(texture_indices.normal_detail, "normal-detail");
        if (resource_failure.has_value()) return std::move(*resource_failure);
        if (draw->sampled_binding.texture != nullptr || draw->normal_binding.texture != nullptr ||
            draw->maps_binding.texture != nullptr || draw->detail_binding.texture != nullptr ||
            draw->normal_detail_binding.texture != nullptr)
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
        const IndexedPortableResourceLayout resource_layout =
            classify_indexed_portable_resource_layout(*draw->pipeline);
        if (resource_layout == IndexedPortableResourceLayout::diffuse_with_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame) {
            if (!owns_frame_constants())
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_owned_frame_constant_missing",
                         "A used pipeline requires an owned frame-constant buffer"}, {}};
            draw->resource_authority = IndexedResourceAuthority::explicit_bindings;
            draw->frame_binding = {
                owned_frame_constants_.get(), 0U, portable_frame_buffer_view_bytes};
        }
        draws.push_back(std::move(*draw));
    }
    const IndexedStaticMeshBatchDescription batch{
        draws, frame.depth_attachment, frame.load_color, frame.clear_color,
        frame.clear_depth, frame.depth_clear_value};
    Diagnostic batch_diagnostic;
    const IndexedStaticMeshBatchStatus batch_validation =
        validate_indexed_static_mesh_batch_description(target, batch, batch_diagnostic);
    if (batch_validation != IndexedStaticMeshBatchStatus::ready)
        return {batch_validation, std::move(batch_diagnostic), {}};
    // Every frame pose, frame/resource mapping, and draw contract is now
    // validated. Backend updates are sequential, so a runtime upload failure
    // can leave earlier mutable uploads committed even though no batch is
    // submitted. A caller can safely retry the complete frame. Commit the
    // shared frame record last so a failed skin upload cannot advance lighting
    // independently of the requested pose.
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
}

}  // namespace apex::render
