#include "apex/render/static_scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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
        BufferDescription stored_description = description;
        if (truncate_frame_buffer && description.usage == BufferUsage::uniform &&
            description.mutability == BufferMutability::mutable_data)
            stored_description.size_bytes = portable_frame_buffer_view_bytes / 2U;
        buffer_descriptions.push_back(stored_description);
        uploaded_bytes.emplace_back(initial_data.begin(), initial_data.end());
        if (fail_buffer_call != 0U && buffer_calls == fail_buffer_call)
            return {BufferStatus::allocation_failed,
                    {"recording_buffer_failure", "injected buffer allocation failure"}, nullptr};
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(stored_description)};
    }
    BufferUpdateResult update_buffer(Buffer&, std::uint64_t offset,
                                     std::span<const std::byte> data) override {
        ++update_calls;
        update_offsets.push_back(offset);
        updated_bytes.emplace_back(data.begin(), data.end());
        if (fail_update_call != 0U && update_calls == fail_update_call)
            return {BufferStatus::upload_failed,
                    {"recording_update_failure", "injected buffer update failure"}};
        return {BufferStatus::ready, {}};
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
        blend_states.clear();
        fill_modes.clear();
        transparent_flags.clear();
        wireframe_flags.clear();
        vertex_buffers.clear();
        index_buffers.clear();
        authorities.clear();
        resource_authorities.clear();
        sampled_textures.clear();
        sampled_samplers.clear();
        normal_textures.clear();
        normal_samplers.clear();
        material_buffers.clear();
        material_offsets.clear();
        material_ranges.clear();
        frame_buffers.clear();
        frame_offsets.clear();
        frame_ranges.clear();
        for (const auto& draw : batch.draws) {
            nodes.push_back(draw.packet->node);
            pipeline_names.push_back(draw.pipeline->name);
            blend_states.push_back(draw.pipeline->blend);
            fill_modes.push_back(draw.pipeline->raster.fill);
            transparent_flags.push_back(draw.packet->flags.transparent);
            wireframe_flags.push_back(draw.packet->flags.wireframe);
            vertex_buffers.push_back(draw.vertex_buffer);
            index_buffers.push_back(draw.index_buffer);
            authorities.push_back(draw.shader_authority);
            resource_authorities.push_back(draw.resource_authority);
            sampled_textures.push_back(draw.sampled_binding.texture);
            sampled_samplers.push_back(draw.sampled_binding.sampler);
            normal_textures.push_back(draw.normal_binding.texture);
            normal_samplers.push_back(draw.normal_binding.sampler);
            material_buffers.push_back(draw.material_binding.buffer);
            material_offsets.push_back(draw.material_binding.offset_bytes);
            material_ranges.push_back(draw.material_binding.range_bytes);
            frame_buffers.push_back(draw.frame_binding.buffer);
            frame_offsets.push_back(draw.frame_binding.offset_bytes);
            frame_ranges.push_back(draw.frame_binding.range_bytes);
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
    std::size_t update_calls = 0U;
    std::size_t fail_update_call = 0U;
    bool truncate_frame_buffer = false;
    std::size_t batch_calls = 0U;
    std::size_t texture_calls = 0U;
    std::size_t fail_texture_call = 0U;
    std::size_t sampler_calls = 0U;
    bool fail_sampler = false;
    bool fail_batch = false;
    bool captured_load_color = false;
    std::array<float, 4> captured_clear_color{};
    std::vector<std::vector<std::byte>> uploaded_bytes;
    std::vector<std::uint64_t> update_offsets;
    std::vector<std::vector<std::byte>> updated_bytes;
    std::vector<BufferDescription> buffer_descriptions;
    std::vector<TextureDescription> texture_descriptions;
    std::vector<std::vector<std::byte>> texture_upload_bytes;
    std::vector<SamplerDescription> sampler_descriptions;
    std::vector<apex::scene::NodeId> nodes;
    std::vector<std::string> pipeline_names;
    std::vector<PipelineBlendState> blend_states;
    std::vector<PipelineFillMode> fill_modes;
    std::vector<bool> transparent_flags;
    std::vector<bool> wireframe_flags;
    std::vector<const Buffer*> vertex_buffers;
    std::vector<const Buffer*> index_buffers;
    std::vector<IndexedShaderAuthority> authorities;
    std::vector<IndexedResourceAuthority> resource_authorities;
    std::vector<const Texture*> sampled_textures;
    std::vector<const Sampler*> sampled_samplers;
    std::vector<const Texture*> normal_textures;
    std::vector<const Sampler*> normal_samplers;
    std::vector<const Buffer*> material_buffers;
    std::vector<std::uint64_t> material_offsets;
    std::vector<std::uint32_t> material_ranges;
    std::vector<const Buffer*> frame_buffers;
    std::vector<std::uint64_t> frame_offsets;
    std::vector<std::uint32_t> frame_ranges;

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

void make_second_mesh_skinned(Fixture& value) {
    auto& mesh = value.model.root.children[1];
    mesh.type = 3U;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19U;
    mesh.vertices.assign(57U, 0.0F);
    mesh.vertices[0] = -0.5F;
    mesh.vertices[19U] = 0.5F;
    mesh.vertices[38U + 1U] = 0.5F;
    for (std::size_t offset = 0U; offset < mesh.vertices.size(); offset += 19U) {
        mesh.vertices[offset + 4U] = 1.0F;
        mesh.vertices[offset + 8U] = 1.0F;
        mesh.vertices[offset + 11U] = 1.0F;
        mesh.vertices[offset + 15U] = 0.0F;
    }
    mesh.indices = {0U, 1U, 2U};
    mesh.bones = {{"BONE", {}}};
    apex::formats::Kn5Node source_bone;
    source_bone.type = 1U;
    source_bone.kind = "node";
    source_bone.name = "BONE";
    source_bone.active = true;
    value.model.root.children.push_back(std::move(source_bone));
    value.scene.nodes[2].kind = apex::scene::NodeKind::skinned_mesh;
    value.second_pipeline.vertex_layout.stride = 19U * sizeof(float);
    value.second_pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0, PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent, PipelineVertexAttributeFormat::float32x3, 3U, 32U},
        {PipelineVertexSemantic::bone_weights, PipelineVertexAttributeFormat::float32x4, 4U, 44U},
        {PipelineVertexSemantic::bone_indices, PipelineVertexAttributeFormat::float32x4, 5U, 60U},
    };
    value.packets[1].primitive = DrawPrimitiveKind::skinned_mesh;
    value.packets[1].vertex_stride_floats = 19U;
    value.packets[1].bone_palette = {apex::scene::identity_matrix};
    apex::scene::SceneNode bone;
    bone.name = "BONE";
    bone.kind = apex::scene::NodeKind::node;
    bone.renderable = false;
    bone.active = true;
    bone.visible = true;
    (void)value.scene.add_node(std::move(bone), 0U);
}

void prepares_mixed_static_and_skinned_scene_and_updates_only_after_preflight() {
    Fixture value = fixture();
    make_second_mesh_skinned(value);
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok() && prepared.resources->unique_geometry_count() == 2U &&
                device.buffer_calls == 4U,
            "mixed scene retains static deduplication and creates one skinned upload");

    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    const auto bind_pose = prepared.resources->draw_and_readback(device, target, frame);
    require(bind_pose.ok() && device.update_calls == 1U &&
                device.updated_bytes.front().size() == 57U * sizeof(float) &&
                device.batch_calls == 1U,
            "inactive animation restores the complete skinned bind pose before drawing");
    const auto bind_bytes = device.updated_bytes.front();

    std::vector<DrawPacket> refreshed = value.packets;
    refreshed[1].bone_palette.front()[12] = 1.0F;
    frame.refreshed_packets = refreshed;
    frame.apply_skinning = true;
    const auto animated = prepared.resources->draw_and_readback(device, target, frame);
    require(animated.ok() && device.update_calls == 2U && device.updated_bytes.size() == 2U &&
                device.updated_bytes.back() != bind_bytes,
            "active animation precomputes and commits the refreshed skinned pose");

    const std::size_t updates_before_invalid = device.update_calls;
    const std::array<DrawPacket, 1> short_refresh = {refreshed.front()};
    frame.refreshed_packets = short_refresh;
    const auto invalid_count = prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_count.status == IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_count.diagnostic.code == "static_scene_frame_packet_count_invalid" &&
                device.update_calls == updates_before_invalid,
            "a short refreshed packet table is rejected before any update");

    frame.refreshed_packets = refreshed;
    refreshed[1].flags.wireframe = !refreshed[1].flags.wireframe;
    frame.refreshed_packets = refreshed;
    const auto invalid_contract = prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_contract.status == IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_contract.diagnostic.code == "static_scene_frame_packet_contract_invalid" &&
                device.update_calls == updates_before_invalid,
            "changed pipeline flags are rejected before any skinned update");

    refreshed[1].flags.wireframe = false;
    refreshed[1].material_profile.shader = "changed-profile";
    frame.refreshed_packets = refreshed;
    const auto invalid_profile = prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_profile.status == IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_profile.diagnostic.code == "static_scene_frame_packet_contract_invalid" &&
                device.update_calls == updates_before_invalid,
            "changed material profiles are rejected before any skinned update");

    frame.refreshed_packets = std::span<const DrawPacket>{};
    frame.apply_skinning = true;
    // The prepared resource owns its original packet; a separately refreshed
    // state with no palette must still fail before touching the mutable buffer.
    std::vector<DrawPacket> missing_palette = refreshed;
    missing_palette[1].material_profile = value.packets[1].material_profile;
    missing_palette[1].bone_palette.clear();
    frame.refreshed_packets = missing_palette;
    const auto invalid_palette = prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_palette.status == IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_palette.diagnostic.code == "static_scene_frame_skin_palette_invalid" &&
                device.update_calls == updates_before_invalid,
            "invalid refreshed palettes are rejected before any update");
}

void prepares_deduplicated_resources_and_executes_one_ordered_batch() {
    Fixture value = fixture();
    value.packets[1].flags.transparent = true;
    value.packets[1].flags.blend_enabled = true;
    value.second_pipeline.blend.enabled = true;
    value.second_pipeline.blend.source_color = PipelineBlendFactor::source_alpha;
    value.second_pipeline.blend.destination_color =
        PipelineBlendFactor::one_minus_source_alpha;
    value.second_pipeline.blend.source_alpha = PipelineBlendFactor::source_alpha;
    value.second_pipeline.blend.destination_alpha =
        PipelineBlendFactor::one_minus_source_alpha;
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
    require(device.transparent_flags == std::vector<bool>({false, true, false}) &&
                !device.blend_states[0].enabled && device.blend_states[1].enabled &&
                !device.blend_states[2].enabled &&
                device.blend_states[1].source_color ==
                    PipelineBlendFactor::source_alpha &&
                device.blend_states[1].destination_color ==
                    PipelineBlendFactor::one_minus_source_alpha,
            "source-evidenced blend state and transparent order reach the batch");
    require(device.vertex_buffers[0] == device.vertex_buffers[2] &&
                device.index_buffers[0] == device.index_buffers[2] &&
                device.vertex_buffers[0] != device.vertex_buffers[1] &&
                device.captured_load_color && device.captured_clear_color == frame.clear_color,
            "duplicate nodes share immutable buffers and batch state is forwarded once");
    require(device.authorities == std::vector<IndexedShaderAuthority>(
                                      3U, IndexedShaderAuthority::explicit_pipeline),
            "each local request carries explicit executable-pipeline authority");
}

void prepares_source_evidenced_wireframe_batch_state() {
    Fixture value = fixture();
    value.first_pipeline.raster.fill = PipelineFillMode::wireframe;
    value.packets[0].flags.wireframe = true;
    value.packets[2].flags.wireframe = true;
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok() && device.buffer_calls == 4U,
            "matching wireframe scene state prepares all geometry");

    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() &&
                device.fill_modes == std::vector<PipelineFillMode>(
                                         {PipelineFillMode::wireframe,
                                          PipelineFillMode::solid,
                                          PipelineFillMode::wireframe}) &&
                device.wireframe_flags == std::vector<bool>({true, false, true}),
            "wireframe fill and packet state reach the ordered batch");

    value = fixture();
    value.first_pipeline.raster.fill = PipelineFillMode::wireframe;
    const auto mismatch =
        prepare_static_scene_resources(device, request_for(value));
    require(!mismatch.ok() &&
                mismatch.status == StaticSceneResourceStatus::invalid_request &&
                mismatch.diagnostic.code ==
                    "static_scene_material_pipeline_invalid" &&
                device.buffer_calls == 4U,
            "wireframe mismatch fails before another backend allocation");
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
    value.second_pipeline.blend.enabled = true;
    const auto blend_mismatch =
        prepare_static_scene_resources(device, request_for(value));
    require(!blend_mismatch.ok() &&
                blend_mismatch.status == StaticSceneResourceStatus::invalid_request &&
                blend_mismatch.diagnostic.code ==
                    "static_scene_material_pipeline_invalid" &&
                device.buffer_calls == 0U,
            "packet and pipeline blend mismatch is rejected before allocation");

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

void owns_and_reuses_material_constant_records() {
    Fixture value = fixture();
    value.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    const std::array<PipelineResourceBinding, 3> material_layout = {{
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
    }};
    value.first_pipeline.resources.assign(material_layout.begin(), material_layout.end());
    value.second_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
    };
    for (DrawPacket& packet : value.packets)
        packet.resources = {{"txDiffuse", 21U, 0U, "body.dds"}};

    std::array<KsPerPixelMaterialConstants, 2> constants{};
    constants[0].lighting = {0.2F, 0.4F, 0.6F, 8.0F};
    constants[0].fresnel = {0.1F, 3.0F, 0.5F, 0.0F};
    constants[0].emissive = {0.7F, 0.8F, 0.9F, 0.0F};
    constants[1].lighting[0] = std::numeric_limits<float>::quiet_NaN();
    auto request = request_for(value);
    request.material_constants_by_material = constants;

    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request);
    require(prepared.ok() && prepared.resources->owned_material_constant_count() == 1U &&
                device.buffer_calls == 5U,
            "one used constants material owns one buffer beside two geometry uploads");
    const auto material_upload = std::find_if(
        device.buffer_descriptions.begin(), device.buffer_descriptions.end(),
        [](const BufferDescription& description) {
            return description.usage == BufferUsage::uniform;
        });
    require(material_upload != device.buffer_descriptions.end(),
            "material preparation creates a uniform buffer");
    const std::size_t material_upload_index = static_cast<std::size_t>(
        material_upload - device.buffer_descriptions.begin());
    require(material_upload->size_bytes == portable_material_buffer_view_bytes &&
                material_upload->memory == BufferMemory::device_local &&
                material_upload->mutability == BufferMutability::immutable &&
                device.uploaded_bytes[material_upload_index].size() ==
                    portable_material_buffer_view_bytes &&
                std::memcmp(device.uploaded_bytes[material_upload_index].data(),
                            &constants[0], sizeof(constants[0])) == 0 &&
                std::all_of(device.uploaded_bytes[material_upload_index].begin() +
                                static_cast<std::ptrdiff_t>(sizeof(constants[0])),
                            device.uploaded_bytes[material_upload_index].end(),
                            [](std::byte byte) { return byte == std::byte{0}; }),
            "the owned record contains 48 source values and zero padding");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeSampler sampler;
    const std::array<const Texture*, 1> textures = {&diffuse};
    const std::array<const Sampler*, 1> samplers = {&sampler};
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    FakeTexture target;
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() && device.material_buffers.size() == 3U &&
                device.material_buffers[0] != nullptr &&
                device.material_buffers[0] == device.material_buffers[2] &&
                device.material_buffers[1] == nullptr &&
                device.material_offsets == std::vector<std::uint64_t>{0U, 0U, 0U} &&
                device.material_ranges ==
                    std::vector<std::uint32_t>{portable_material_buffer_view_bytes, 0U,
                                               portable_material_buffer_view_bytes},
            "duplicate packets reuse one record and the diffuse-only draw has no record");

    Fixture distinct = fixture();
    distinct.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    distinct.first_pipeline.resources.assign(material_layout.begin(), material_layout.end());
    distinct.second_pipeline.resources.assign(material_layout.begin(), material_layout.end());
    for (DrawPacket& packet : distinct.packets)
        packet.resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    constants[1] = KsPerPixelMaterialConstants{};
    constants[1].lighting[1] = 0.25F;
    auto distinct_request = request_for(distinct);
    distinct_request.material_constants_by_material = constants;
    RecordingDevice distinct_device;
    auto distinct_prepared =
        prepare_static_scene_resources(distinct_device, distinct_request);
    require(distinct_prepared.ok() &&
                distinct_prepared.resources->owned_material_constant_count() == 2U &&
                distinct_device.buffer_calls == 6U,
            "two used material IDs own two records even when their layouts match");
    StaticSceneFrameDescription distinct_frame = frame;
    const auto distinct_drawn = distinct_prepared.resources->draw_and_readback(
        distinct_device, target, distinct_frame);
    require(distinct_drawn.ok() && distinct_device.material_buffers[0] != nullptr &&
                distinct_device.material_buffers[1] != nullptr &&
                distinct_device.material_buffers[0] != distinct_device.material_buffers[1] &&
                distinct_device.material_buffers[0] == distinct_device.material_buffers[2],
            "material IDs keep distinct records while duplicate packets reuse one record");
}

void owns_and_updates_frame_constant_record_for_mixed_packets() {
    Fixture value = fixture();
    value.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
    };
    value.second_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
    };
    for (DrawPacket& packet : value.packets)
        packet.resources = { {"txDiffuse", 21U, 0U, "body.dds"} };

    auto request = request_for(value);
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request);
    require(prepared.ok() && prepared.resources->owns_frame_constants() &&
                device.buffer_calls == 5U,
            "a mixed scene owns one mutable frame record beside geometry");
    const auto frame_upload = std::find_if(
        device.buffer_descriptions.begin(), device.buffer_descriptions.end(),
        [](const BufferDescription& description) {
            return description.usage == BufferUsage::uniform &&
                   description.mutability == BufferMutability::mutable_data;
        });
    require(frame_upload != device.buffer_descriptions.end() &&
                frame_upload->size_bytes == portable_frame_buffer_view_bytes,
            "frame record uses one bounded mutable uniform view");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeSampler sampler;
    const std::array<const Texture*, 1> textures = {&diffuse};
    const std::array<const Sampler*, 1> samplers = {&sampler};
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    FakeTexture target;

    auto missing = prepared.resources->draw_and_readback(device, target, frame);
    require(missing.status == IndexedStaticMeshBatchStatus::invalid_request &&
                missing.diagnostic.code == "static_scene_frame_constants_missing" &&
                device.update_calls == 0U && device.batch_calls == 0U,
            "missing frame constants fail before update or recording");

    KsPerPixelFrameConstants constants{};
    constants.sun_color[0] = std::numeric_limits<float>::infinity();
    frame.frame_constants = constants;
    auto non_finite = prepared.resources->draw_and_readback(device, target, frame);
    require(non_finite.status == IndexedStaticMeshBatchStatus::invalid_request &&
                non_finite.diagnostic.code == "static_scene_frame_constants_non_finite" &&
                device.update_calls == 0U && device.batch_calls == 0U,
            "non-finite frame constants fail before update or recording");

    constants = KsPerPixelFrameConstants{};
    constants.sun_direction = {0.0F, 0.0F, 0.0F, 0.0F};
    frame.frame_constants = constants;
    auto zero_direction = prepared.resources->draw_and_readback(device, target, frame);
    require(zero_direction.status == IndexedStaticMeshBatchStatus::invalid_request &&
                zero_direction.diagnostic.code ==
                    "static_scene_frame_sun_direction_invalid" &&
                device.update_calls == 0U && device.batch_calls == 0U,
            "a zero sun direction fails before update or recording");

    constants = KsPerPixelFrameConstants{};
    frame.frame_constants = constants;
    frame.camera.position[0] = std::numeric_limits<float>::quiet_NaN();
    auto non_finite_camera =
        prepared.resources->draw_and_readback(device, target, frame);
    require(non_finite_camera.status == IndexedStaticMeshBatchStatus::invalid_request &&
                non_finite_camera.diagnostic.code ==
                    "static_scene_frame_camera_position_invalid" &&
                device.update_calls == 0U && device.batch_calls == 0U,
            "a non-finite camera position fails before update or recording");

    constants = KsPerPixelFrameConstants{};
    constants.sun_direction = {1.0F, 2.0F, 3.0F, 4.0F};
    constants.camera_position = {91.0F, 92.0F, 93.0F, 94.0F};
    frame.camera.position = {4.0F, 5.0F, 6.0F};
    frame.frame_constants = constants;
    device.fail_update_call = 1U;
    auto update_failure = prepared.resources->draw_and_readback(device, target, frame);
    require(update_failure.status == IndexedStaticMeshBatchStatus::execution_failed &&
                update_failure.diagnostic.code == "recording_update_failure" &&
                device.update_calls == 1U && device.batch_calls == 0U,
            "frame update failure prevents recording");

    RecordingDevice truncated_device;
    truncated_device.truncate_frame_buffer = true;
    auto truncated_prepared = prepare_static_scene_resources(
        truncated_device, request_for(value));
    require(truncated_prepared.ok(), "truncated frame fixture prepares before draw validation");
    auto truncated_frame = frame;
    auto range_failure = truncated_prepared.resources->draw_and_readback(
        truncated_device, target, truncated_frame);
    require(range_failure.status == IndexedStaticMeshBatchStatus::invalid_request &&
                range_failure.diagnostic.code == "static_scene_frame_constant_range_invalid" &&
                truncated_device.update_calls == 0U && truncated_device.batch_calls == 0U,
            "a truncated frame buffer fails its range preflight");

    require(device.frame_buffers.size() == 0U,
            "a failed frame update does not submit a mixed batch");
    device.fail_update_call = 0U;
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    KsPerPixelFrameConstants uploaded_constants{};
    require(!device.updated_bytes.empty() &&
                device.updated_bytes.back().size() >= sizeof(uploaded_constants),
            "successful frame update records the portable constants");
    std::memcpy(&uploaded_constants, device.updated_bytes.back().data(),
                sizeof(uploaded_constants));
    require(drawn.ok() && device.frame_buffers.size() == 3U &&
                device.frame_buffers[0] != nullptr && device.frame_buffers[1] == nullptr &&
                device.frame_buffers[2] == device.frame_buffers[0] &&
                device.frame_offsets == std::vector<std::uint64_t>{0U, 0U, 0U} &&
                device.frame_ranges == std::vector<std::uint32_t>{portable_frame_buffer_view_bytes, 0U,
                                                                    portable_frame_buffer_view_bytes},
            "one frame record binds only to applicable mixed packets");
    require(uploaded_constants.camera_position ==
                std::array<float, 4>{4.0F, 5.0F, 6.0F, 0.0F},
            "static-scene frame upload derives camera position from CameraFrame");
}

void resolves_bounded_normal_map_resources_and_rejects_malformed_packets() {
    const auto configure_normal_scene = [](Fixture& value,
                                           bool embed_textures) {
        const std::array<std::uint8_t, 4> diffuse_pixel = {17U, 34U, 51U, 255U};
        const std::array<std::uint8_t, 4> normal_pixel = {128U, 255U, 128U, 255U};
        value.model.textures.push_back(
            {true, "body.dds", embed_textures ? 152U : 0U,
             embed_textures ? rgba8_dds(true, diffuse_pixel)
                            : std::vector<std::uint8_t>{}, {}});
        value.model.textures.push_back(
            {true, "body_nm.dds", embed_textures ? 152U : 0U,
             embed_textures ? rgba8_dds(false, normal_pixel)
                            : std::vector<std::uint8_t>{}, {}});
        value.first_pipeline.vertex_layout.attributes = {
            {PipelineVertexSemantic::position,
             PipelineVertexAttributeFormat::float32x3, 0U, 0U},
            {PipelineVertexSemantic::normal,
             PipelineVertexAttributeFormat::float32x3, 1U, 12U},
            {PipelineVertexSemantic::texcoord0,
             PipelineVertexAttributeFormat::float32x2, 2U, 24U},
            {PipelineVertexSemantic::tangent,
             PipelineVertexAttributeFormat::float32x3, 3U, 32U},
        };
        value.first_pipeline.resources = {
            {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
            {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
            {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
            {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
            {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
            {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
        };
        const std::vector<DrawResourceSlot> slots = {
            {"txDiffuse", 21U, 0U, "body.dds"},
            {"txNormal", 22U, 1U, "body_nm.dds"},
        };
        value.packets[0].resources = slots;
        value.packets[2].resources = slots;
    };

    Fixture value = fixture();
    configure_normal_scene(value, false);
    std::array<KsPerPixelMaterialConstants, 2> material_constants{};
    material_constants[0].fresnel[2] = 0.0F;
    auto request = request_for(value);
    request.material_constants_by_material = material_constants;
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request);
    require(prepared.ok() && prepared.resources->owns_frame_constants() &&
                prepared.resources->owned_material_constant_count() == 1U &&
                device.buffer_calls == 6U,
            "bounded ksPerPixelNM scene owns material and frame constants");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeTexture normal(sampled_description);
    FakeSampler diffuse_sampler;
    FakeSampler normal_sampler;
    const std::array<const Texture*, 2> textures = {&diffuse, &normal};
    const std::array<const Sampler*, 2> samplers = {&diffuse_sampler, &normal_sampler};
    KsPerPixelFrameConstants frame_constants{};
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.position = {2.0F, 3.0F, 4.0F};
    frame.frame_constants = frame_constants;
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    FakeTexture target;
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() &&
                device.sampled_textures ==
                    std::vector<const Texture*>{&diffuse, nullptr, &diffuse} &&
                device.normal_textures ==
                    std::vector<const Texture*>{&normal, nullptr, &normal} &&
                device.sampled_samplers ==
                    std::vector<const Sampler*>{&diffuse_sampler, nullptr,
                                                &diffuse_sampler} &&
                device.normal_samplers ==
                    std::vector<const Sampler*>{&normal_sampler, nullptr,
                                                &normal_sampler},
            "caller tables preserve distinct diffuse and normal bindings in packet order");

    Fixture embedded = fixture();
    configure_normal_scene(embedded, true);
    auto embedded_request = request_for(embedded);
    embedded_request.material_constants_by_material = material_constants;
    embedded_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice embedded_device;
    auto embedded_prepared =
        prepare_static_scene_resources(embedded_device, embedded_request);
    require(embedded_prepared.ok() &&
                embedded_prepared.resources->owned_texture_count() == 2U &&
                embedded_device.texture_calls == 2U && embedded_device.sampler_calls == 1U,
            "embedded ksPerPixelNM resources decode and own both texture roles");
    StaticSceneFrameDescription embedded_frame;
    embedded_frame.camera.clip_space = CameraClipSpace::vulkan;
    embedded_frame.camera.position = frame.camera.position;
    embedded_frame.frame_constants = frame_constants;
    const auto embedded_drawn = embedded_prepared.resources->draw_and_readback(
        embedded_device, target, embedded_frame);
    require(embedded_drawn.ok() && embedded_device.sampled_textures[0] != nullptr &&
                embedded_device.normal_textures[0] != nullptr &&
                embedded_device.sampled_textures[0] !=
                    embedded_device.normal_textures[0],
            "owned normal and diffuse bindings survive through batch submission");

    Fixture malformed = fixture();
    configure_normal_scene(malformed, false);
    malformed.packets[2].resources[1] = malformed.packets[2].resources[0];
    auto malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = material_constants;
    RecordingDevice malformed_device;
    const auto duplicate =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!duplicate.ok() &&
                duplicate.diagnostic.code == "static_scene_resource_slot_duplicate" &&
                malformed_device.buffer_calls == 0U,
            "a duplicated later normal-map slot fails before backend allocation");

    malformed = fixture();
    configure_normal_scene(malformed, false);
    malformed.packets[2].resources.pop_back();
    malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = material_constants;
    const auto truncated =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!truncated.ok() &&
                truncated.diagnostic.code == "static_scene_material_packet_unsupported" &&
                malformed_device.buffer_calls == 0U,
            "a truncated later normal-map resource list fails before allocation");

    malformed = fixture();
    configure_normal_scene(malformed, false);
    material_constants[0].fresnel[2] = 0.05F;
    malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = material_constants;
    const auto fresnel =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!fresnel.ok() &&
                fresnel.diagnostic.code == "static_scene_normal_fresnel_unsupported" &&
                malformed_device.buffer_calls == 0U,
            "normal execution rejects production Fresnel until its behavior is implemented");
}

void orders_skin_updates_before_the_shared_frame_record() {
    Fixture value = fixture();
    make_second_mesh_skinned(value);
    value.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
    };
    value.packets[0].resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    value.packets[2].resources = value.packets[0].resources;

    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok() && prepared.resources->owns_frame_constants(),
            "mixed skinned scene owns its shared frame record");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeSampler sampler;
    const std::array<const Texture*, 1> textures = {&diffuse};
    const std::array<const Sampler*, 1> samplers = {&sampler};
    KsPerPixelFrameConstants constants{};
    constants.sun_direction = {0.0F, 1.0F, 0.0F, 0.0F};
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = constants;
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    FakeTexture target;

    device.fail_update_call = 1U;
    const auto failed_skin =
        prepared.resources->draw_and_readback(device, target, frame);
    require(failed_skin.status == IndexedStaticMeshBatchStatus::execution_failed &&
                failed_skin.diagnostic.code == "recording_update_failure" &&
                device.update_calls == 1U && device.batch_calls == 0U &&
                device.updated_bytes.front().size() == 57U * sizeof(float),
            "a failed skin upload does not advance the shared frame record");

    device.fail_update_call = 0U;
    const auto drawn = prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() && device.update_calls == 3U && device.batch_calls == 1U &&
                device.updated_bytes[1].size() == 57U * sizeof(float) &&
                device.updated_bytes[2].size() == portable_frame_buffer_view_bytes,
            "a complete retry commits skin data before frame data and submits once");

    device.fail_update_call = 5U;
    const auto failed_frame =
        prepared.resources->draw_and_readback(device, target, frame);
    require(failed_frame.status == IndexedStaticMeshBatchStatus::execution_failed &&
                failed_frame.diagnostic.code == "recording_update_failure" &&
                device.update_calls == 5U && device.batch_calls == 1U &&
                device.updated_bytes[3].size() == 57U * sizeof(float) &&
                device.updated_bytes[4].size() == portable_frame_buffer_view_bytes,
            "a late frame failure keeps the sequential partial-update contract");
}

void rejects_frame_constant_budget_before_allocation() {
    Fixture value = fixture();
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
    };
    value.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    value.packets[0].resources = { {"txDiffuse", 21U, 0U, "body.dds"} };
    value.packets[2].resources = { {"txDiffuse", 21U, 0U, "body.dds"} };
    auto request = request_for(value);
    request.limits.max_total_frame_constant_bytes = portable_frame_buffer_view_bytes - 1U;
    RecordingDevice device;
    const auto result = prepare_static_scene_resources(device, request);
    require(!result.ok() &&
                result.diagnostic.code == "static_scene_frame_constant_aggregate_limit" &&
                device.buffer_calls == 0U,
            "a truncated-equivalent frame budget fails before allocation");
}

void rejects_invalid_material_constant_inputs_before_allocation() {
    Fixture diffuse_only = fixture();
    auto diffuse_only_request = request_for(diffuse_only);
    diffuse_only_request.limits.max_material_constant_buffers = 0U;
    diffuse_only_request.limits.max_total_material_constant_bytes = 0U;
    RecordingDevice diffuse_only_device;
    const auto diffuse_only_prepared =
        prepare_static_scene_resources(diffuse_only_device, diffuse_only_request);
    require(diffuse_only_prepared.ok() && diffuse_only_device.buffer_calls == 4U &&
                diffuse_only_prepared.resources->owned_material_constant_count() == 0U,
            "resource-free scenes do not require or allocate material-constant limits");

    Fixture value = fixture();
    value.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    value.first_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
    };
    value.packets[0].resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    value.packets[2].resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    std::array<KsPerPixelMaterialConstants, 2> constants{};

    RecordingDevice device;
    auto request = request_for(value);
    const auto missing = prepare_static_scene_resources(device, request);
    require(!missing.ok() &&
                missing.diagnostic.code == "static_scene_material_constant_table_invalid" &&
                device.buffer_calls == 0U,
            "a missing material-constant table fails before allocation");

    request.material_constants_by_material =
        std::span<const KsPerPixelMaterialConstants>(constants.data(), 1U);
    const auto short_table = prepare_static_scene_resources(device, request);
    require(!short_table.ok() &&
                short_table.diagnostic.code == "static_scene_material_constant_table_invalid" &&
                device.buffer_calls == 0U,
            "a short material-constant table fails before allocation");

    constants[0].fresnel[1] = std::numeric_limits<float>::infinity();
    request.material_constants_by_material = constants;
    const auto non_finite = prepare_static_scene_resources(device, request);
    require(!non_finite.ok() &&
                non_finite.diagnostic.code == "static_scene_material_constant_non_finite" &&
                device.buffer_calls == 0U,
            "a non-finite used material record fails before allocation");

    constants[0] = KsPerPixelMaterialConstants{};
    request.material_constants_by_material = constants;
    request.limits.max_total_material_constant_bytes =
        portable_material_buffer_view_bytes - 1U;
    const auto limited = prepare_static_scene_resources(device, request);
    require(!limited.ok() &&
                limited.diagnostic.code == "static_scene_material_constant_aggregate_limit" &&
                device.buffer_calls == 0U,
            "the material-buffer byte limit fails before allocation");

    Fixture two_materials = fixture();
    two_materials.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    const std::vector<PipelineResourceBinding> constants_layout = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
    };
    two_materials.first_pipeline.resources = constants_layout;
    two_materials.second_pipeline.resources = constants_layout;
    for (DrawPacket& packet : two_materials.packets)
        packet.resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    auto count_limited_request = request_for(two_materials);
    count_limited_request.material_constants_by_material = constants;
    count_limited_request.limits.max_material_constant_buffers = 1U;
    RecordingDevice count_limited_device;
    const auto count_limited =
        prepare_static_scene_resources(count_limited_device, count_limited_request);
    require(!count_limited.ok() &&
                count_limited.diagnostic.code == "static_scene_material_constant_count_limit" &&
                count_limited_device.buffer_calls == 0U,
            "the material-buffer count limit fails before allocation");

    Fixture malformed_layout = fixture();
    malformed_layout.model.textures.push_back({true, "body.dds", 0U, {}, {}});
    malformed_layout.first_pipeline.resources = constants_layout;
    malformed_layout.first_pipeline.resources.back().binding = 4U;
    malformed_layout.packets[0].resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    malformed_layout.packets[2].resources = {{"txDiffuse", 21U, 0U, "body.dds"}};
    auto malformed_layout_request = request_for(malformed_layout);
    malformed_layout_request.material_constants_by_material = constants;
    RecordingDevice malformed_layout_device;
    const auto bad_layout =
        prepare_static_scene_resources(malformed_layout_device, malformed_layout_request);
    require(!bad_layout.ok() &&
                bad_layout.diagnostic.code ==
                    "static_scene_material_pipeline_unsupported" &&
                malformed_layout_device.buffer_calls == 0U,
            "an incorrect constants binding fails before allocation");

    request = request_for(value);
    request.material_constants_by_material = constants;
    device.fail_buffer_call = 1U;
    const auto allocation_failure = prepare_static_scene_resources(device, request);
    require(!allocation_failure.ok() &&
                allocation_failure.status == StaticSceneResourceStatus::allocation_failed &&
                allocation_failure.diagnostic.code == "recording_buffer_failure" &&
                device.buffer_calls == 1U,
            "a material-buffer allocation error propagates before geometry upload");
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
        prepares_mixed_static_and_skinned_scene_and_updates_only_after_preflight();
        prepares_source_evidenced_wireframe_batch_state();
        rejects_invalid_late_inputs_before_backend_allocation();
        resolves_portable_diffuse_tables_without_owning_handles();
        owns_embedded_diffuse_resources_after_source_lifetime();
        rejects_malformed_embedded_textures_before_allocation();
        rejects_malformed_diffuse_packets_before_allocation();
        owns_and_reuses_material_constant_records();
        owns_and_updates_frame_constant_record_for_mixed_packets();
        resolves_bounded_normal_map_resources_and_rejects_malformed_packets();
        orders_skin_updates_before_the_shared_frame_record();
        rejects_frame_constant_budget_before_allocation();
        rejects_invalid_material_constant_inputs_before_allocation();
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
