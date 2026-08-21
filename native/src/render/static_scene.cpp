#include "apex/render/static_scene.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
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

bool portable_diffuse_layout(const PipelineProgram& pipeline) noexcept {
    if (pipeline.resources.size() != 2U) return false;
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        texture = texture || (resource.kind == PipelineResourceKind::sampled_texture &&
                              resource.set == 0U && resource.binding == 0U);
        sampler = sampler || (resource.kind == PipelineResourceKind::sampler &&
                              resource.set == 0U && resource.binding == 1U);
    }
    return texture && sampler;
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
    if (pipeline.raster.fill != PipelineFillMode::solid ||
        pipeline.blend.enabled || pipeline.blend.alpha_to_coverage)
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene execution supports opaque solid pipelines", 0U};
    if (!pipeline.resources.empty() && !portable_diffuse_layout(pipeline))
        return {StaticSceneResourceStatus::unsupported,
                "Static-scene material execution supports only the portable diffuse resource layout", 0U};
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

StaticSceneResourceResult prepare_static_scene_resources(
    Device& device, const StaticScenePrepareRequest& request) {
    if (request.model == nullptr || request.scene == nullptr)
        return fail(StaticSceneResourceStatus::invalid_request, "static_scene_request_missing",
                    "Static-scene preparation requires a model and scene");
    const auto& model = *request.model;
    const auto& scene = *request.scene;
    const auto& limits = request.limits;
    if (limits.max_draws == 0U || limits.max_draws > max_indexed_static_mesh_batch_draws ||
        limits.max_materials == 0U || limits.max_textures == 0U ||
        limits.max_resource_string_bytes == 0U ||
        limits.max_total_vertex_bytes == 0U ||
        limits.max_total_index_bytes == 0U || limits.max_total_shader_bytes == 0U ||
        limits.max_validation_bytes == 0U)
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
    if (model.textures.size() > limits.max_textures)
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_texture_table_limit",
                    "The final KN5 texture table exceeds the static-scene limit");

    const Kn5SceneNodeMapResult node_map =
        map_kn5_scene_nodes(model.root, scene, limits.node_map);
    if (!node_map.ok())
        return fail(StaticSceneResourceStatus::invalid_request,
                    "static_scene_" + node_map.diagnostic.code,
                    node_map.diagnostic.message);

    std::vector<std::size_t> upload_by_node(node_map.source_nodes.size(), invalid_resource_index);
    std::vector<std::size_t> pipeline_by_material(model.materials.size(), invalid_resource_index);
    std::vector<std::size_t> upload_for_packet(request.packets.size(), invalid_resource_index);
    std::vector<std::size_t> pipeline_for_packet(request.packets.size(), invalid_resource_index);
    std::vector<std::uint32_t> texture_for_packet(request.packets.size(),
                                                  invalid_draw_texture_index);
    std::vector<const formats::Kn5Node*> unique_meshes;
    std::vector<std::size_t> representative_packets;
    std::vector<PipelineProgram> pipelines;
    unique_meshes.reserve(request.packets.size());
    representative_packets.reserve(request.packets.size());
    pipelines.reserve(request.packets.size());

    std::uint64_t total_vertex_bytes = 0U;
    std::uint64_t total_index_bytes = 0U;
    std::uint64_t total_shader_bytes = 0U;
    std::uint64_t validation_bytes = 0U;
    std::optional<bool> batch_has_depth;
    std::optional<PipelineRenderTargetFormat> batch_color_format;
    for (std::size_t packet_index = 0U; packet_index < request.packets.size(); ++packet_index) {
        const DrawPacket& packet = request.packets[packet_index];
        const auto node_index = static_cast<std::size_t>(packet.node);
        const auto material_index = static_cast<std::size_t>(packet.material);
        if (packet.primitive != DrawPrimitiveKind::static_mesh || !packet.bone_palette.empty() ||
            packet.flags.transparent || packet.flags.blend_enabled ||
            packet.flags.alpha_to_coverage || packet.flags.wireframe)
            return fail(StaticSceneResourceStatus::unsupported,
                        "static_scene_packet_unsupported",
                        "Static-scene execution supports opaque static meshes");
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
        const StaticMeshUploadStatus mesh_status =
            validate_static_mesh_upload(mesh, packet, limits.mesh, mesh_diagnostic);
        if (mesh_status != StaticMeshUploadStatus::ready)
            return {map_upload_status(mesh_status), std::move(mesh_diagnostic), nullptr};

        if (upload_by_node[node_index] == invalid_resource_index) {
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
        upload_for_packet[packet_index] = upload_by_node[node_index];

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
        if (!pipeline->resources.empty()) {
            if (packet.resources.size() != 1U)
                return fail(StaticSceneResourceStatus::unsupported,
                            "static_scene_diffuse_packet_unsupported",
                            "The portable static-scene material path requires one txDiffuse packet resource");
            const DrawResourceSlot& diffuse = packet.resources.front();
            if (diffuse.slot.size() > limits.max_resource_string_bytes ||
                diffuse.texture.size() > limits.max_resource_string_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_resource_string_limit",
                            "A static-scene resource string exceeds its byte limit");
            if (!canonical_resource_equals(diffuse.slot, "txdiffuse"))
                return fail(StaticSceneResourceStatus::unsupported,
                            "static_scene_diffuse_packet_unsupported",
                            "The portable static-scene material path requires one txDiffuse packet resource");
            if (diffuse.texture_index == invalid_draw_texture_index ||
                static_cast<std::size_t>(diffuse.texture_index) >= model.textures.size())
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_diffuse_texture_index_invalid",
                            "A txDiffuse packet resource has an invalid global texture index");
            const formats::Kn5Texture& source_texture =
                model.textures[static_cast<std::size_t>(diffuse.texture_index)];
            if (source_texture.name.size() > limits.max_resource_string_bytes ||
                !checked_add(static_cast<std::uint64_t>(diffuse.slot.size()), validation_bytes) ||
                !checked_add(static_cast<std::uint64_t>(diffuse.texture.size()), validation_bytes) ||
                !checked_add(static_cast<std::uint64_t>(source_texture.name.size()), validation_bytes) ||
                validation_bytes > limits.max_validation_bytes)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_validation_work_limit",
                            "Static-scene resource validation exceeds its byte-work limit");
            if (!source_texture.active || diffuse.texture.empty() ||
                diffuse.texture != source_texture.name ||
                source_texture.workspaceFileIndex !=
                    model.materials[material_index].workspaceFileIndex)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "static_scene_diffuse_texture_identity_invalid",
                            "A txDiffuse packet resource does not match its active scoped global KN5 texture");
            texture_for_packet[packet_index] = diffuse.texture_index;
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
            pipeline_by_material[material_index] = pipelines.size();
            pipelines.push_back(*pipeline);
        }
        pipeline_for_packet[packet_index] = pipeline_by_material[material_index];
    }

    auto resources = std::make_unique<StaticSceneResources>();
    resources->backend_ = device.info().backend;
    resources->device_ = &device;
    resources->packets_.assign(request.packets.begin(), request.packets.end());
    resources->pipelines_ = std::move(pipelines);
    resources->upload_for_packet_ = std::move(upload_for_packet);
    resources->pipeline_for_packet_ = std::move(pipeline_for_packet);
    resources->texture_for_packet_ = std::move(texture_for_packet);
    resources->texture_count_ = model.textures.size();
    for (const std::uint32_t index : resources->texture_for_packet_)
        resources->has_material_resources_ =
            resources->has_material_resources_ || index != invalid_draw_texture_index;
    resources->uploads_.reserve(unique_meshes.size());
    for (std::size_t index = 0U; index < unique_meshes.size(); ++index) {
        StaticMeshUploadResult uploaded = upload_static_mesh(
            device, *unique_meshes[index],
            resources->packets_[representative_packets[index]], limits.mesh);
        if (!uploaded.ok())
            return {map_upload_status(uploaded.status), std::move(uploaded.diagnostic), nullptr};
        resources->uploads_.push_back(std::move(uploaded.upload));
    }
    return {StaticSceneResourceStatus::ready, {}, std::move(resources)};
}

IndexedStaticMeshBatchResult StaticSceneResources::draw_and_readback(
    Device& device, Texture& target, const StaticSceneFrameDescription& frame) const {
    if (&device != device_ || device.info().backend != backend_ || target.backend() != backend_)
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"static_scene_device_mismatch",
                 "Static-scene resources require their preparing device and backend"}, {}};
    if (packets_.empty() || packets_.size() != upload_for_packet_.size() ||
        packets_.size() != pipeline_for_packet_.size() ||
        packets_.size() != texture_for_packet_.size())
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_resources_invalid", "Static-scene resource mappings are incomplete"}, {}};
    if (has_material_resources_ &&
        (frame.textures_by_global_index.size() != texture_count_ ||
         frame.samplers_by_global_index.size() != texture_count_))
        return {IndexedStaticMeshBatchStatus::invalid_request,
                {"static_scene_resource_table_size_invalid",
                 "Static-scene texture and sampler tables must match the final KN5 texture count"}, {}};

    std::vector<IndexedStaticMeshDrawRequest> draws;
    draws.reserve(packets_.size());
    for (std::size_t index = 0U; index < packets_.size(); ++index) {
        if (upload_for_packet_[index] >= uploads_.size() ||
            pipeline_for_packet_[index] >= pipelines_.size())
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"static_scene_resources_invalid", "Static-scene resource index is invalid"}, {}};
        Diagnostic diagnostic;
        auto draw = uploads_[upload_for_packet_[index]]->make_request(
            packets_[index], pipelines_[pipeline_for_packet_[index]], frame.camera,
            0U, 0U, {0.0F, 0.0F, 0.0F, 1.0F}, diagnostic);
        if (!draw.has_value())
            return {IndexedStaticMeshBatchStatus::invalid_request, std::move(diagnostic), {}};
        draw->shader_authority = IndexedShaderAuthority::explicit_pipeline;
        const std::uint32_t texture_index = texture_for_packet_[index];
        if (texture_index != invalid_draw_texture_index) {
            const std::size_t resource_index = static_cast<std::size_t>(texture_index);
            if (resource_index >= texture_count_ ||
                frame.textures_by_global_index[resource_index] == nullptr ||
                frame.samplers_by_global_index[resource_index] == nullptr)
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"static_scene_resource_table_entry_missing",
                         "A used static-scene texture-table entry has no texture or sampler handle"}, {}};
            draw->resource_authority = IndexedResourceAuthority::explicit_bindings;
            draw->sampled_binding = {frame.textures_by_global_index[resource_index],
                                     frame.samplers_by_global_index[resource_index]};
        }
        draws.push_back(std::move(*draw));
    }
    const IndexedStaticMeshBatchDescription batch{
        draws, frame.depth_attachment, frame.load_color, frame.clear_color,
        frame.clear_depth, frame.depth_clear_value};
    return device.draw_indexed_static_mesh_batch_and_readback(target, batch);
}

}  // namespace apex::render
