#include "apex/render/static_scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class FakeBuffer final : public Buffer {
public:
    explicit FakeBuffer(BufferDescription description) : info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    BufferInfo info_;
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(Backend backend = Backend::Vulkan) : backend_(backend) {
        info_.description = {16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
                             TextureUsage::color_attachment | TextureUsage::transfer_source,
                             TextureMemory::device_local, TextureMutability::mutable_data};
    }
    FakeTexture(TextureDescription description, Backend backend = Backend::Vulkan)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    TextureInfo info_{};
};

class FakeSampler final : public Sampler {
public:
    explicit FakeSampler(Backend backend = Backend::Vulkan) : backend_(backend) {}
    FakeSampler(SamplerDescription description, Backend backend = Backend::Vulkan)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const SamplerInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    SamplerInfo info_{};
};

class RecordingDevice final : public Device {
public:
    const DeviceInfo& info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        ++buffer_calls;
        uploaded_bytes.emplace_back(initial_data.begin(), initial_data.end());
        if (fail_buffer_call != 0U && buffer_calls == fail_buffer_call)
            return {BufferStatus::allocation_failed,
                    {"recording_buffer_failure", "injected buffer allocation failure"}, nullptr};
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }
    BufferUpdateResult update_buffer(Buffer&, std::uint64_t, std::span<const std::byte>) override {
        return {BufferStatus::unsupported, {"unused", "unused"}};
    }
    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan& uploads) override {
        ++texture_calls;
        texture_descriptions.push_back(description);
        texture_upload_bytes.emplace_back();
        for (const TextureUpload& upload : uploads.subresources)
            texture_upload_bytes.back().insert(texture_upload_bytes.back().end(),
                                               upload.data.begin(), upload.data.end());
        if (fail_texture_call != 0U && texture_calls == fail_texture_call)
            return {TextureStatus::upload_failed,
                    {"recording_texture_failure", "injected texture upload failure"},
                    nullptr};
        return {TextureStatus::ready, {},
                std::make_unique<FakeTexture>(description)};
    }
    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"unused", "unused"}};
    }
    TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"unused", "unused"}, {}};
    }
    TriangleDrawResult draw_triangle_and_readback(Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"unused", "unused"}, {}};
    }
    SamplerResult create_sampler(const SamplerDescription& description) override {
        ++sampler_calls;
        sampler_descriptions.push_back(description);
        if (fail_sampler)
            return {SamplerStatus::allocation_failed,
                    {"recording_sampler_failure", "injected sampler allocation failure"},
                    nullptr};
        return {SamplerStatus::ready, {},
                std::make_unique<FakeSampler>(description)};
    }
    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }
    IndexedStaticMeshBatchResult draw_indexed_static_mesh_batch_and_readback(
        Texture& texture, const IndexedStaticMeshBatchDescription& batch) override {
        ++batch_calls;
        captured_load_color = batch.load_color;
        captured_clear_color = batch.clear_color;
        nodes.clear();
        pipeline_names.clear();
        vertex_buffers.clear();
        index_buffers.clear();
        authorities.clear();
        resource_authorities.clear();
        sampled_textures.clear();
        sampled_samplers.clear();
        for (const auto& draw : batch.draws) {
            nodes.push_back(draw.packet->node);
            pipeline_names.push_back(draw.pipeline->name);
            vertex_buffers.push_back(draw.vertex_buffer);
            index_buffers.push_back(draw.index_buffer);
            authorities.push_back(draw.shader_authority);
            resource_authorities.push_back(draw.resource_authority);
            sampled_textures.push_back(draw.sampled_binding.texture);
            sampled_samplers.push_back(draw.sampled_binding.sampler);
        }
        Diagnostic diagnostic;
        const auto validation = validate_indexed_static_mesh_batch_description(texture, batch, diagnostic);
        if (validation != IndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (fail_batch)
            return {IndexedStaticMeshBatchStatus::execution_failed,
                    {"recording_batch_failure", "injected batch failure"}, {}};
        return {IndexedStaticMeshBatchStatus::ready, {}, {std::byte{0x11}}};
    }
    void wait_idle() noexcept override {}

    std::size_t buffer_calls = 0U;
    std::size_t fail_buffer_call = 0U;
    std::size_t batch_calls = 0U;
    std::size_t texture_calls = 0U;
    std::size_t fail_texture_call = 0U;
    std::size_t sampler_calls = 0U;
    bool fail_sampler = false;
    bool fail_batch = false;
    bool captured_load_color = false;
    std::array<float, 4> captured_clear_color{};
    std::vector<std::vector<std::byte>> uploaded_bytes;
    std::vector<TextureDescription> texture_descriptions;
    std::vector<std::vector<std::byte>> texture_upload_bytes;
    std::vector<SamplerDescription> sampler_descriptions;
    std::vector<apex::scene::NodeId> nodes;
    std::vector<std::string> pipeline_names;
    std::vector<const Buffer*> vertex_buffers;
    std::vector<const Buffer*> index_buffers;
    std::vector<IndexedShaderAuthority> authorities;
    std::vector<IndexedResourceAuthority> resource_authorities;
    std::vector<const Texture*> sampled_textures;
    std::vector<const Sampler*> sampled_samplers;

private:
    DeviceInfo info_{Backend::Vulkan, "recording", "test", 1U, 0U, 0U, 0U, 0U, true};
};

std::vector<std::uint8_t> shader_fixture() {
    constexpr std::array<std::uint32_t, 29> words = {
        0x07230203U, 0x00010000U, 0x00070000U, 0x00000005U, 0x00000000U,
        0x00020011U, 0x00000001U, 0x0003000eU, 0x00000000U, 0x00000001U,
        0x0005000fU, 0x00000000U, 0x00000001U, 0x6e69616dU, 0x00000000U,
        0x00020013U, 0x00000002U, 0x00030021U, 0x00000003U, 0x00000002U,
        0x00050036U, 0x00000002U, 0x00000001U, 0x00000000U, 0x00000003U,
        0x000200f8U, 0x00000004U, 0x000100fdU, 0x00010038U,
    };
    std::vector<std::uint8_t> bytes(words.size() * sizeof(std::uint32_t));
    for (std::size_t index = 0U; index < words.size(); ++index) {
        const std::uint32_t word = words[index];
        bytes[index * 4U] = static_cast<std::uint8_t>(word & 0xffU);
        bytes[index * 4U + 1U] = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        bytes[index * 4U + 2U] = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        bytes[index * 4U + 3U] = static_cast<std::uint8_t>((word >> 24U) & 0xffU);
    }
    return bytes;
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "DDS fixture write is in bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> rgba8_dds(bool srgb,
                                    std::span<const std::uint8_t, 4> pixel) {
    std::vector<std::uint8_t> bytes(152U, 0U);
    put32(bytes, 0U, 0x20534444U);
    put32(bytes, 4U, 124U);
    put32(bytes, 12U, 1U);
    put32(bytes, 16U, 1U);
    put32(bytes, 28U, 1U);
    put32(bytes, 76U, 32U);
    put32(bytes, 80U, 4U);
    bytes[84U] = 'D';
    bytes[85U] = 'X';
    bytes[86U] = '1';
    bytes[87U] = '0';
    put32(bytes, 128U, srgb ? 29U : 28U);
    put32(bytes, 132U, 3U);
    put32(bytes, 140U, 1U);
    std::copy(pixel.begin(), pixel.end(), bytes.begin() + 148);
    return bytes;
}

PipelineProgram pipeline_fixture(std::string name) {
    PipelineProgram pipeline;
    pipeline.name = std::move(name);
    pipeline.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    pipeline.vertex_layout.stride = 11U * sizeof(float);
    pipeline.vertex_layout.attributes.push_back(
        {PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0U, 0U});
    pipeline.shaders.push_back(
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv, shader_fixture()});
    pipeline.shaders.push_back(
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv, shader_fixture()});
    return pipeline;
}

struct Fixture {
    apex::formats::Kn5File model;
    apex::scene::SceneSnapshot scene;
    std::vector<DrawPacket> packets;
    PipelineProgram first_pipeline = pipeline_fixture("material-zero");
    PipelineProgram second_pipeline = pipeline_fixture("material-one");
    std::array<const PipelineProgram*, 2> pipelines = {&first_pipeline, &second_pipeline};
};

apex::formats::Kn5Node mesh_fixture(std::string name, std::uint32_t material) {
    apex::formats::Kn5Node mesh;
    mesh.type = 2U;
    mesh.kind = "mesh";
    mesh.name = std::move(name);
    mesh.materialId = material;
    mesh.vertexStride = 11U;
    mesh.vertices.resize(33U, 0.0F);
    mesh.vertices[0] = -0.5F;
    mesh.vertices[11U] = 0.5F;
    mesh.vertices[23U] = 0.5F;
    mesh.indices = {0U, 1U, 2U};
    return mesh;
}

DrawPacket packet_fixture(apex::scene::NodeId node, apex::scene::MaterialId material,
                          std::size_t order) {
    DrawPacket packet;
    packet.node = node;
    packet.material = material;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.order = order;
    packet.flags = {false, false, false, false, false, false, false, false};
    return packet;
}

Fixture fixture() {
    Fixture value;
    value.model.materials.resize(2U);
    value.model.root.type = 1U;
    value.model.root.kind = "node";
    value.model.root.name = "ROOT";
    value.model.root.children.push_back(mesh_fixture("A", 0U));
    value.model.root.children.push_back(mesh_fixture("B", 1U));

    (void)value.scene.add_material({"zero", "fixture", apex::scene::BlendMode::opaque});
    (void)value.scene.add_material({"one", "fixture", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = value.scene.add_node(std::move(root));
    apex::scene::SceneNode first;
    first.name = "A";
    first.kind = apex::scene::NodeKind::mesh;
    first.material = 0U;
    const auto first_id = value.scene.add_node(std::move(first), root_id);
    apex::scene::SceneNode second;
    second.name = "B";
    second.kind = apex::scene::NodeKind::mesh;
    second.material = 1U;
    const auto second_id = value.scene.add_node(std::move(second), root_id);
    value.packets = {packet_fixture(first_id, 0U, 0U), packet_fixture(second_id, 1U, 1U),
                     packet_fixture(first_id, 0U, 2U)};
    value.packets.back().world_matrix[12] = 2.0F;
    return value;
}

StaticScenePrepareRequest request_for(Fixture& value) {
    value.pipelines = {&value.first_pipeline, &value.second_pipeline};
    StaticScenePrepareRequest request;
    request.model = &value.model;
    request.scene = &value.scene;
    request.packets = value.packets;
    request.pipelines_by_material = value.pipelines;
    return request;
}

void prepares_deduplicated_resources_and_executes_one_ordered_batch() {
    Fixture value = fixture();
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok() && prepared.resources->draw_count() == 3U &&
                prepared.resources->unique_geometry_count() == 2U && device.buffer_calls == 4U,
            "valid scene uploads each unique node once");
    require(!value.packets[0].shader_execution_supported &&
                !value.packets[1].shader_execution_supported &&
                !value.packets[2].shader_execution_supported,
            "preparation does not relabel production packets");

    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.load_color = true;
    frame.clear_color = {0.1F, 0.2F, 0.3F, 1.0F};
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() && drawn.rgba8.size() == 1U && device.batch_calls == 1U,
            "prepared scene executes as one ordered batch");
    require(device.nodes == std::vector<apex::scene::NodeId>({1U, 2U, 1U}) &&
                device.pipeline_names == std::vector<std::string>({"material-zero", "material-one",
                                                                   "material-zero"}),
            "packet order and global material pipeline lookup are preserved");
    require(device.vertex_buffers[0] == device.vertex_buffers[2] &&
                device.index_buffers[0] == device.index_buffers[2] &&
                device.vertex_buffers[0] != device.vertex_buffers[1] &&
                device.captured_load_color && device.captured_clear_color == frame.clear_color,
            "duplicate nodes share immutable buffers and batch state is forwarded once");
    require(device.authorities == std::vector<IndexedShaderAuthority>(
                                      3U, IndexedShaderAuthority::explicit_pipeline),
            "each local request carries explicit executable-pipeline authority");
}

void rejects_invalid_late_inputs_before_backend_allocation() {
    Fixture value = fixture();
    value.packets.back().node = 99U;
    RecordingDevice device;
    const auto invalid_node = prepare_static_scene_resources(device, request_for(value));
    require(!invalid_node.ok() && invalid_node.status == StaticSceneResourceStatus::invalid_request &&
                device.buffer_calls == 0U && device.batch_calls == 0U,
            "invalid later packet cannot cause a partial upload or draw");

    value = fixture();
    auto invalid_authority_request = request_for(value);
    invalid_authority_request.texture_authority =
        static_cast<StaticSceneTextureAuthority>(255U);
    const auto invalid_authority =
        prepare_static_scene_resources(device, invalid_authority_request);
    require(!invalid_authority.ok() &&
                invalid_authority.diagnostic.code ==
                    "static_scene_texture_authority_invalid" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "invalid texture authority is rejected before allocation");

    value = fixture();
    auto resource_free_limits = request_for(value);
    resource_free_limits.limits.texture_decode.maxInputBytes = 0U;
    resource_free_limits.limits.texture_decode.maxOutputBytes = 0U;
    resource_free_limits.limits.max_total_texture_source_bytes = 0U;
    resource_free_limits.limits.max_total_decoded_texture_bytes = 0U;
    RecordingDevice resource_free_device;
    const auto resource_free = prepare_static_scene_resources(
        resource_free_device, resource_free_limits);
    require(resource_free.ok() && resource_free_device.buffer_calls == 4U &&
                resource_free_device.texture_calls == 0U &&
                resource_free_device.sampler_calls == 0U,
            "caller-table scenes do not require embedded texture limits");

    value = fixture();
    value.model.root.children[1].indices[2] = 9U;
    const auto malformed = prepare_static_scene_resources(device, request_for(value));
    require(!malformed.ok() && malformed.diagnostic.code == "static_mesh_index_out_of_range" &&
                device.buffer_calls == 0U,
            "malformed later source geometry is rejected before allocation");

    value = fixture();
    auto missing_request = request_for(value);
    value.pipelines[1] = nullptr;
    const auto missing_pipeline = prepare_static_scene_resources(device, missing_request);
    require(!missing_pipeline.ok() &&
                missing_pipeline.diagnostic.code == "static_scene_material_pipeline_missing" &&
                device.buffer_calls == 0U,
            "missing used material pipeline is rejected before allocation");

    value = fixture();
    value.second_pipeline.shaders.front().bytes.resize(3U);
    const auto malformed_pipeline = prepare_static_scene_resources(device, request_for(value));
    require(!malformed_pipeline.ok() &&
                malformed_pipeline.status == StaticSceneResourceStatus::invalid_request &&
                malformed_pipeline.diagnostic.code == "static_scene_material_pipeline_invalid" &&
                device.buffer_calls == 0U,
            "malformed executable shader input is rejected before allocation");

    value = fixture();
    value.second_pipeline.targets.colors.front().format = PipelineRenderTargetFormat::rgba16_float;
    const auto unsupported_target = prepare_static_scene_resources(device, request_for(value));
    require(!unsupported_target.ok() &&
                unsupported_target.status == StaticSceneResourceStatus::unsupported &&
                unsupported_target.diagnostic.code ==
                    "static_scene_material_pipeline_unsupported" &&
                device.buffer_calls == 0U,
            "unsupported color target is rejected before allocation");

    value = fixture();
    value.second_pipeline.targets.colors.front().format =
        PipelineRenderTargetFormat::bgra8_unorm;
    const auto mixed_targets = prepare_static_scene_resources(device, request_for(value));
    require(!mixed_targets.ok() &&
                mixed_targets.diagnostic.code ==
                    "static_scene_mixed_color_targets_unsupported" &&
                device.buffer_calls == 0U,
            "mixed batch color targets are rejected before allocation");

    value = fixture();
    value.second_pipeline.targets.has_depth = true;
    value.second_pipeline.targets.depth =
        {PipelineRenderTargetFormat::depth24_stencil8, 1U};
    const auto unsupported_depth = prepare_static_scene_resources(device, request_for(value));
    require(!unsupported_depth.ok() &&
                unsupported_depth.status == StaticSceneResourceStatus::unsupported &&
                unsupported_depth.diagnostic.code ==
                    "static_scene_material_pipeline_unsupported" &&
                device.buffer_calls == 0U,
            "non-D32 depth targets are rejected before allocation");

    value = fixture();
    value.second_pipeline.depth.test_enabled = true;
    value.packets[1].flags.depth_test = true;
    const auto missing_depth_target =
        prepare_static_scene_resources(device, request_for(value));
    require(!missing_depth_target.ok() &&
                missing_depth_target.status ==
                    StaticSceneResourceStatus::invalid_request &&
                missing_depth_target.diagnostic.code ==
                    "static_scene_material_pipeline_invalid" &&
                device.buffer_calls == 0U,
            "enabled depth state without a depth target is rejected before allocation");

    value = fixture();
    value.second_pipeline.vertex_layout.stride = 12U;
    const auto stride_mismatch = prepare_static_scene_resources(device, request_for(value));
    require(!stride_mismatch.ok() &&
                stride_mismatch.status == StaticSceneResourceStatus::invalid_request &&
                stride_mismatch.diagnostic.code == "static_scene_material_pipeline_invalid" &&
                device.buffer_calls == 0U,
            "packet and pipeline stride mismatch is rejected before allocation");

    value = fixture();
    auto bounded_request = request_for(value);
    bounded_request.limits.max_validation_bytes = 128U;
    const auto bounded = prepare_static_scene_resources(device, bounded_request);
    require(!bounded.ok() && bounded.diagnostic.code == "static_scene_validation_work_limit" &&
                device.buffer_calls == 0U,
            "duplicate-packet source validation has an aggregate work limit");
}

void resolves_portable_diffuse_tables_without_owning_handles() {
    Fixture value = fixture();
    value.model.textures.push_back({true, "body.dds", 4U,
                                    {1U, 2U, 3U, 4U}, {}});
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
    };
    value.packets[0].resources.push_back({" txDiffuse ", 21U, 0U, "body.dds"});
    value.packets[2].resources.push_back({"TXDIFFUSE", 21U, 0U, "body.dds"});

    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok() && device.buffer_calls == 4U,
            "portable diffuse packet mappings prepare before geometry upload");
    require(!value.packets[0].shader_execution_supported &&
                value.packets[0].resources.front().bind_point == 21U,
            "preparation preserves staged packets and shader bind-point metadata");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeSampler sampler;
    const std::array<const Texture*, 1> textures = {&diffuse};
    const std::array<const Sampler*, 1> samplers = {&sampler};
    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() && device.batch_calls == 1U,
            "portable diffuse tables execute in one ordered static-scene batch");
    require(device.resource_authorities ==
                std::vector<IndexedResourceAuthority>{
                    IndexedResourceAuthority::explicit_bindings,
                    IndexedResourceAuthority::packet_contract,
                    IndexedResourceAuthority::explicit_bindings} &&
                device.sampled_textures ==
                    std::vector<const Texture*>{&diffuse, nullptr, &diffuse} &&
                device.sampled_samplers ==
                    std::vector<const Sampler*>{&sampler, nullptr, &sampler},
            "global texture-table indices map to non-owning request-local bindings");

    StaticSceneFrameDescription missing_tables;
    missing_tables.camera.clip_space = CameraClipSpace::vulkan;
    const auto missing =
        prepared.resources->draw_and_readback(device, target, missing_tables);
    require(missing.status == IndexedStaticMeshBatchStatus::invalid_request &&
                missing.diagnostic.code == "static_scene_resource_table_size_invalid" &&
                device.batch_calls == 1U,
            "short resource tables are rejected before the backend batch call");

    const std::array<const Texture*, 1> null_textures = {nullptr};
    missing_tables.textures_by_global_index = null_textures;
    missing_tables.samplers_by_global_index = samplers;
    const auto null_entry =
        prepared.resources->draw_and_readback(device, target, missing_tables);
    require(null_entry.status == IndexedStaticMeshBatchStatus::invalid_request &&
                null_entry.diagnostic.code == "static_scene_resource_table_entry_missing" &&
                device.batch_calls == 1U,
            "null used resource entries are rejected before the backend batch call");
}

void configure_embedded_diffuse(Fixture& value,
                                std::vector<std::uint8_t> bytes) {
    const std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
    value.model.textures.push_back(
        {true, "body.dds", size, std::move(bytes), {}});
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
    };
    value.packets[0].resources.push_back(
        {"txDiffuse", 21U, 0U, "body.dds"});
    value.packets[2].resources.push_back(
        {"txDiffuse", 21U, 0U, "body.dds"});
}

void owns_embedded_diffuse_resources_after_source_lifetime() {
    RecordingDevice device;
    std::unique_ptr<StaticSceneResources> resources;
    {
        Fixture value = fixture();
        const std::array<std::uint8_t, 4> pixel = {17U, 34U, 51U, 255U};
        configure_embedded_diffuse(value, rgba8_dds(true, pixel));
        auto request = request_for(value);
        request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
        auto prepared = prepare_static_scene_resources(device, request);
        require(prepared.ok() && prepared.resources->owned_texture_count() == 1U &&
                    device.sampler_calls == 1U && device.texture_calls == 1U &&
                    device.buffer_calls == 4U,
                "embedded authority creates one deduplicated texture and sampler");
        resources = std::move(prepared.resources);
    }

    require(device.texture_descriptions.size() == 1U &&
                device.texture_descriptions.front().format ==
                    TextureFormat::rgba8_srgb &&
                device.texture_descriptions.front().usage == TextureUsage::sampled &&
                device.texture_upload_bytes.front() ==
                    std::vector<std::byte>{std::byte{17U}, std::byte{34U},
                                           std::byte{51U}, std::byte{255U}},
            "embedded DDS upload preserves sRGB format and decoded bytes");
    require(device.sampler_descriptions.size() == 1U &&
                device.sampler_descriptions.front().min_filter ==
                    SamplerFilter::linear &&
                device.sampler_descriptions.front().mag_filter ==
                    SamplerFilter::linear &&
                device.sampler_descriptions.front().mip_filter ==
                    SamplerFilter::linear &&
                device.sampler_descriptions.front().address_u ==
                    SamplerAddressMode::repeat &&
                device.sampler_descriptions.front().address_v ==
                    SamplerAddressMode::repeat,
            "embedded diffuse sampler matches the linear-repeat source path");

    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    const auto drawn = resources->draw_and_readback(device, target, frame);
    require(drawn.ok() && device.batch_calls == 1U &&
                device.sampled_textures[0] != nullptr &&
                device.sampled_textures[0] == device.sampled_textures[2] &&
                device.sampled_samplers[0] != nullptr &&
                device.sampled_samplers[0] == device.sampled_samplers[2],
            "owned bindings survive the source model and reuse handles in order");
}

void rejects_malformed_embedded_textures_before_allocation() {
    RecordingDevice device;
    Fixture value = fixture();
    configure_embedded_diffuse(value, {1U, 2U, 3U, 4U});
    auto embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    const auto bad_header =
        prepare_static_scene_resources(device, embedded_request);
    require(!bad_header.ok() &&
                bad_header.diagnostic.code ==
                    "static_scene_embedded_texture_invalid_header" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "malformed embedded DDS header fails before backend allocation");

    value = fixture();
    const std::array<std::uint8_t, 4> pixel = {1U, 2U, 3U, 4U};
    auto truncated_bytes = rgba8_dds(false, pixel);
    truncated_bytes.pop_back();
    configure_embedded_diffuse(value, std::move(truncated_bytes));
    embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    const auto truncated =
        prepare_static_scene_resources(device, embedded_request);
    require(!truncated.ok() &&
                truncated.diagnostic.code ==
                    "static_scene_embedded_texture_truncated" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "truncated embedded DDS mip fails before backend allocation");

    value = fixture();
    configure_embedded_diffuse(value, rgba8_dds(false, pixel));
    value.model.textures.push_back(
        {true, "later.dds", 4U, {1U, 2U, 3U, 4U}, {}});
    value.second_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
    };
    value.packets[1].resources = {{"txDiffuse", 21U, 1U, "later.dds"}};
    embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    const auto later_malformed =
        prepare_static_scene_resources(device, embedded_request);
    require(!later_malformed.ok() &&
                later_malformed.diagnostic.code ==
                    "static_scene_embedded_texture_invalid_header" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "a later malformed embedded DDS prevents all backend allocation");

    value = fixture();
    configure_embedded_diffuse(value, rgba8_dds(false, pixel));
    value.model.textures.front().size -= 1U;
    embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    const auto inconsistent =
        prepare_static_scene_resources(device, embedded_request);
    require(!inconsistent.ok() &&
                inconsistent.diagnostic.code ==
                    "static_scene_embedded_texture_payload_invalid" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "inconsistent KN5 texture size fails before backend allocation");

    value.model.textures.front().size =
        static_cast<std::uint32_t>(value.model.textures.front().data.size());
    embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    embedded_request.limits.max_total_texture_source_bytes = 1U;
    const auto source_limited =
        prepare_static_scene_resources(device, embedded_request);
    require(!source_limited.ok() &&
                source_limited.diagnostic.code ==
                    "static_scene_texture_source_aggregate_limit" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "embedded DDS source bytes have an aggregate preallocation limit");

    embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    embedded_request.limits.max_total_decoded_texture_bytes = 3U;
    const auto decoded_limited =
        prepare_static_scene_resources(device, embedded_request);
    require(!decoded_limited.ok() &&
                decoded_limited.diagnostic.code ==
                    "static_scene_texture_decode_aggregate_limit" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "decoded DDS texels have an aggregate preallocation limit");
}

void propagates_embedded_resource_failures() {
    Fixture value = fixture();
    const std::array<std::uint8_t, 4> pixel = {1U, 2U, 3U, 4U};
    configure_embedded_diffuse(value, rgba8_dds(false, pixel));
    auto request = request_for(value);
    request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;

    RecordingDevice device;
    device.fail_sampler = true;
    const auto sampler_failure =
        prepare_static_scene_resources(device, request);
    require(!sampler_failure.ok() &&
                sampler_failure.status ==
                    StaticSceneResourceStatus::allocation_failed &&
                sampler_failure.diagnostic.code == "recording_sampler_failure" &&
                device.sampler_calls == 1U && device.texture_calls == 1U &&
                device.buffer_calls == 4U,
            "terminal embedded sampler allocation failure propagates and cleans up resources");

    device = RecordingDevice{};
    device.fail_texture_call = 1U;
    const auto texture_failure =
        prepare_static_scene_resources(device, request);
    require(!texture_failure.ok() &&
                texture_failure.status == StaticSceneResourceStatus::upload_failed &&
                texture_failure.diagnostic.code == "recording_texture_failure" &&
                device.sampler_calls == 0U && device.texture_calls == 1U &&
                device.buffer_calls == 0U,
            "embedded texture upload failure cannot consume a sampler slot or allocate geometry");

    device = RecordingDevice{};
    device.fail_buffer_call = 1U;
    const auto geometry_failure =
        prepare_static_scene_resources(device, request);
    require(!geometry_failure.ok() &&
                geometry_failure.status ==
                    StaticSceneResourceStatus::allocation_failed &&
                geometry_failure.diagnostic.code == "recording_buffer_failure" &&
                device.texture_calls == 1U && device.buffer_calls == 1U &&
                device.sampler_calls == 0U,
            "embedded geometry failure cannot consume a sampler slot");
}

void rejects_malformed_diffuse_packets_before_allocation() {
    Fixture value = fixture();
    value.model.textures.push_back({true, "body.dds", 4U,
                                    {1U, 2U, 3U, 4U}, {}});
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
    };
    value.packets[0].resources.push_back({"txDiffuse", 999U, 0U, "body.dds"});
    value.packets[2].resources.push_back({"txDiffuse", 999U, 0U, "body.dds"});
    RecordingDevice device;

    value.packets[2].resources.front().texture_index = invalid_draw_texture_index;
    const auto sentinel = prepare_static_scene_resources(device, request_for(value));
    require(!sentinel.ok() &&
                sentinel.diagnostic.code == "static_scene_diffuse_texture_index_invalid" &&
                device.buffer_calls == 0U,
            "invalid diffuse sentinel in a later packet is rejected before allocation");

    value.packets[2].resources.front().texture_index = 1U;
    const auto out_of_range = prepare_static_scene_resources(device, request_for(value));
    require(!out_of_range.ok() &&
                out_of_range.diagnostic.code == "static_scene_diffuse_texture_index_invalid" &&
                device.buffer_calls == 0U,
            "out-of-range diffuse index in a later packet is rejected before allocation");

    value.packets[2].resources.front().texture_index = 0U;
    value.packets[2].resources.front().texture = "other.dds";
    const auto mismatch = prepare_static_scene_resources(device, request_for(value));
    require(!mismatch.ok() &&
                mismatch.diagnostic.code == "static_scene_diffuse_texture_identity_invalid" &&
                device.buffer_calls == 0U,
            "diffuse name and global-index mismatch is rejected before allocation");

    value.packets[2].resources.front().texture = "body.dds";
    value.model.textures.front().active = false;
    const auto inactive = prepare_static_scene_resources(device, request_for(value));
    require(!inactive.ok() &&
                inactive.diagnostic.code == "static_scene_diffuse_texture_identity_invalid" &&
                device.buffer_calls == 0U,
            "inactive diffuse source is rejected before allocation");

    value.model.textures.front().active = true;
    auto string_limited_request = request_for(value);
    string_limited_request.limits.max_resource_string_bytes = 4U;
    const auto string_limited =
        prepare_static_scene_resources(device, string_limited_request);
    require(!string_limited.ok() &&
                string_limited.diagnostic.code == "static_scene_resource_string_limit" &&
                device.buffer_calls == 0U,
            "oversized resource strings are rejected before allocation");

    value.model.textures.push_back({true, "unused.dds", 0U, {}, {}});
    auto table_limited_request = request_for(value);
    table_limited_request.limits.max_textures = 1U;
    const auto table_limited =
        prepare_static_scene_resources(device, table_limited_request);
    require(!table_limited.ok() &&
                table_limited.diagnostic.code == "static_scene_texture_table_limit" &&
                device.buffer_calls == 0U,
            "oversized final texture tables are rejected before allocation");

    value.model.textures.resize(1U);
    value.model.textures.front().workspaceFileIndex = 0U;
    value.model.materials.front().workspaceFileIndex = 0U;
    apex::formats::Kn5Texture foreign_scope = value.model.textures.front();
    foreign_scope.workspaceFileIndex = 1U;
    value.model.textures.push_back(std::move(foreign_scope));
    value.packets[0].resources.front().texture_index = 1U;
    value.packets[2].resources.front().texture_index = 1U;
    const auto scope_mismatch =
        prepare_static_scene_resources(device, request_for(value));
    require(!scope_mismatch.ok() &&
                scope_mismatch.diagnostic.code ==
                    "static_scene_diffuse_texture_identity_invalid" &&
                device.buffer_calls == 0U,
            "same-name texture from another workspace scope is rejected before allocation");
}

void propagates_upload_and_batch_failures() {
    Fixture value = fixture();
    RecordingDevice device;
    device.fail_buffer_call = 2U;
    const auto upload_failure = prepare_static_scene_resources(device, request_for(value));
    require(!upload_failure.ok() &&
                upload_failure.status == StaticSceneResourceStatus::allocation_failed &&
                upload_failure.diagnostic.code == "recording_buffer_failure" &&
                device.batch_calls == 0U,
            "backend allocation failure is preserved and prevents drawing");

    device = RecordingDevice{};
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok(), "scene preparation succeeds before injected batch failure");
    device.fail_batch = true;
    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    const auto batch_failure = prepared.resources->draw_and_readback(device, target, frame);
    require(batch_failure.status == IndexedStaticMeshBatchStatus::execution_failed &&
                batch_failure.diagnostic.code == "recording_batch_failure" &&
                device.batch_calls == 1U,
            "batch execution failure is preserved after one attempt");

    FakeTexture wrong_target(Backend::D3D12);
    const auto mismatch = prepared.resources->draw_and_readback(device, wrong_target, frame);
    require(mismatch.status == IndexedStaticMeshBatchStatus::unsupported &&
                mismatch.diagnostic.code == "static_scene_device_mismatch" &&
                device.batch_calls == 1U,
            "cross-backend execution is rejected before the device call");

    RecordingDevice other_device;
    const auto device_mismatch =
        prepared.resources->draw_and_readback(other_device, target, frame);
    require(device_mismatch.status == IndexedStaticMeshBatchStatus::unsupported &&
                device_mismatch.diagnostic.code == "static_scene_device_mismatch" &&
                other_device.batch_calls == 0U,
            "same-backend cross-device execution is rejected before the device call");
}

}  // namespace

int main() {
    try {
        prepares_deduplicated_resources_and_executes_one_ordered_batch();
        rejects_invalid_late_inputs_before_backend_allocation();
        resolves_portable_diffuse_tables_without_owning_handles();
        owns_embedded_diffuse_resources_after_source_lifetime();
        rejects_malformed_embedded_textures_before_allocation();
        rejects_malformed_diffuse_packets_before_allocation();
        propagates_embedded_resource_failures();
        propagates_upload_and_batch_failures();
        require(std::string(static_scene_resource_status_name(StaticSceneResourceStatus::ready)) ==
                    "ready",
                "static scene status name");
        std::cout << "static scene tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "static scene tests failed: " << error.what() << '\n';
        return 1;
    }
}
