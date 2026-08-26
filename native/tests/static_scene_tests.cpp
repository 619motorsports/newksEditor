#include "apex/render/static_scene.hpp"
#include "apex/render/selected_mesh.hpp"
#include "png_fixture.hpp"

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
    explicit FakeBuffer(BufferDescription description,
                        std::size_t* live_count = nullptr)
        : info_({description}), live_count_(live_count) {
        if (live_count_ != nullptr) ++*live_count_;
    }
    ~FakeBuffer() override {
        if (live_count_ != nullptr) --*live_count_;
    }
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    BufferInfo info_;
    std::size_t* live_count_ = nullptr;
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(Backend backend = Backend::Vulkan) : backend_(backend) {
        info_.description = {16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
                             TextureUsage::color_attachment | TextureUsage::transfer_source,
                             TextureMemory::device_local, TextureMutability::mutable_data};
    }
    FakeTexture(TextureDescription description, Backend backend = Backend::Vulkan,
                std::size_t* live_count = nullptr)
        : backend_(backend), info_({description}), live_count_(live_count) {
        if (live_count_ != nullptr) ++*live_count_;
    }
    ~FakeTexture() override {
        if (live_count_ != nullptr) --*live_count_;
    }
    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    TextureInfo info_{};
    std::size_t* live_count_ = nullptr;
};

class FakeSampler final : public Sampler {
public:
    explicit FakeSampler(Backend backend = Backend::Vulkan) : backend_(backend) {}
    FakeSampler(SamplerDescription description, Backend backend = Backend::Vulkan,
                std::size_t* live_count = nullptr)
        : backend_(backend), info_({description}), live_count_(live_count) {
        if (live_count_ != nullptr) ++*live_count_;
    }
    ~FakeSampler() override {
        if (live_count_ != nullptr) --*live_count_;
    }
    Backend backend() const noexcept override { return backend_; }
    const SamplerInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    SamplerInfo info_{};
    std::size_t* live_count_ = nullptr;
};

class FakeDepthAttachment final : public DepthAttachment {
public:
    explicit FakeDepthAttachment(DepthAttachmentDescription description,
                                 std::size_t* live_count = nullptr)
        : info_({description}), live_count_(live_count) {
        if (live_count_ != nullptr) ++*live_count_;
    }
    ~FakeDepthAttachment() override {
        if (live_count_ != nullptr) --*live_count_;
    }
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }
private:
    DepthAttachmentInfo info_{};
    std::size_t* live_count_ = nullptr;
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
        return {BufferStatus::ready, {},
                std::make_unique<FakeBuffer>(stored_description, &live_buffers)};
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
                std::make_unique<FakeTexture>(description, Backend::Vulkan,
                                              &live_textures)};
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
    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription& description) override {
        ++depth_attachment_calls;
        depth_descriptions.push_back(description);
        if (fail_depth_attachment_call != 0U &&
            depth_attachment_calls == fail_depth_attachment_call)
            return {DepthAttachmentStatus::allocation_failed,
                    {"recording_depth_failure", "injected depth allocation failure"},
                    nullptr};
        return {DepthAttachmentStatus::ready, {},
                std::make_unique<FakeDepthAttachment>(description,
                                                       &live_depth_attachments)};
    }
    SamplerResult create_sampler(const SamplerDescription& description) override {
        ++sampler_calls;
        sampler_descriptions.push_back(description);
        if (fail_sampler)
            return {SamplerStatus::allocation_failed,
                    {"recording_sampler_failure", "injected sampler allocation failure"},
                    nullptr};
        return {SamplerStatus::ready, {},
                std::make_unique<FakeSampler>(description, Backend::Vulkan,
                                              &live_samplers)};
    }
    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }
    IndexedStaticMeshBatchResult draw_indexed_static_mesh_batch_and_readback(
        Texture& texture, const IndexedStaticMeshBatchDescription& batch) override {
        ++batch_calls;
        captured_load_color = batch.load_color;
        captured_clear_color = batch.clear_color;
        captured_resolve_target = batch.resolve_target;
        captured_capture_rgba8 = batch.capture_rgba8;
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
        maps_textures.clear();
        maps_samplers.clear();
        detail_textures.clear();
        detail_samplers.clear();
        normal_detail_textures.clear();
        normal_detail_samplers.clear();
        material_buffers.clear();
        material_offsets.clear();
        material_ranges.clear();
        frame_buffers.clear();
        frame_offsets.clear();
        frame_ranges.clear();
        directional_shadow_maps.clear();
        directional_shadow_samplers.clear();
        directional_shadow_buffers.clear();
        directional_shadow_ranges.clear();
        overlay_positions.clear();
        overlay_buffers.clear();
        selected_positions.clear();
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
            maps_textures.push_back(draw.maps_binding.texture);
            maps_samplers.push_back(draw.maps_binding.sampler);
            detail_textures.push_back(draw.detail_binding.texture);
            detail_samplers.push_back(draw.detail_binding.sampler);
            normal_detail_textures.push_back(draw.normal_detail_binding.texture);
            normal_detail_samplers.push_back(draw.normal_detail_binding.sampler);
            material_buffers.push_back(draw.material_binding.buffer);
            material_offsets.push_back(draw.material_binding.offset_bytes);
            material_ranges.push_back(draw.material_binding.range_bytes);
            frame_buffers.push_back(draw.frame_binding.buffer);
            frame_offsets.push_back(draw.frame_binding.offset_bytes);
            frame_ranges.push_back(draw.frame_binding.range_bytes);
            directional_shadow_maps.push_back(
                draw.directional_shadow_binding.maps);
            directional_shadow_samplers.push_back(
                draw.directional_shadow_binding.sampler);
            directional_shadow_buffers.push_back(
                draw.directional_shadow_binding.constants);
            directional_shadow_ranges.push_back(
                draw.directional_shadow_binding.constants_range_bytes);
        }
        for (const auto& draw : batch.selected_mesh_draws)
            selected_positions.push_back(draw.scene_position);
        for (const auto& draw : batch.overlay_draws) {
            overlay_positions.push_back(draw.scene_position);
            overlay_buffers.push_back(draw.vertex_buffer);
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
    DepthOnlyIndexedStaticMeshBatchResult draw_depth_only_indexed_static_mesh_batch(
        const DepthOnlyIndexedStaticMeshBatchDescription& batch) override {
        ++depth_batch_calls;
        Diagnostic diagnostic;
        const auto validation =
            validate_depth_only_indexed_static_mesh_batch_description(batch, diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic)};
        std::vector<apex::scene::NodeId> captured;
        for (const auto& draw : batch.draws) {
            captured.push_back(draw.packet->node);
            depth_camera_matrices.push_back(draw.camera_frame->view_projection);
            depth_culls.push_back(draw.pipeline->raster.cull);
            depth_material_modes.push_back(draw.material_mode);
            depth_transparent_flags.push_back(draw.packet->flags.transparent);
            depth_blend_flags.push_back(draw.packet->flags.blend_enabled);
            depth_alpha_to_coverage_flags.push_back(
                draw.packet->flags.alpha_to_coverage);
            depth_test_flags.push_back(draw.packet->flags.depth_test);
            depth_write_flags.push_back(draw.packet->flags.depth_write);
            depth_alpha_textures.push_back(draw.alpha_tested_diffuse_binding.texture);
            depth_alpha_samplers.push_back(draw.alpha_tested_diffuse_binding.sampler);
            depth_alpha_material_buffers.push_back(
                draw.alpha_tested_material_binding.buffer);
            depth_alpha_material_ranges.push_back(
                draw.alpha_tested_material_binding.range_bytes);
        }
        depth_nodes.push_back(std::move(captured));
        if (fail_depth_batch_call != 0U && depth_batch_calls == fail_depth_batch_call)
            return {DepthOnlyIndexedStaticMeshBatchStatus::execution_failed,
                    {"recording_depth_batch_failure",
                     "injected depth batch failure"}};
        return {DepthOnlyIndexedStaticMeshBatchStatus::ready, {}};
    }
    void wait_idle() noexcept override {}

    std::size_t buffer_calls = 0U;
    std::size_t live_buffers = 0U;
    std::size_t fail_buffer_call = 0U;
    std::size_t update_calls = 0U;
    std::size_t fail_update_call = 0U;
    bool truncate_frame_buffer = false;
    std::size_t batch_calls = 0U;
    std::size_t texture_calls = 0U;
    std::size_t live_textures = 0U;
    std::size_t fail_texture_call = 0U;
    std::size_t sampler_calls = 0U;
    std::size_t live_samplers = 0U;
    bool fail_sampler = false;
    bool fail_batch = false;
    std::size_t depth_attachment_calls = 0U;
    std::size_t live_depth_attachments = 0U;
    std::size_t fail_depth_attachment_call = 0U;
    std::size_t depth_batch_calls = 0U;
    std::size_t fail_depth_batch_call = 0U;
    bool captured_load_color = false;
    std::array<float, 4> captured_clear_color{};
    Texture* captured_resolve_target = nullptr;
    bool captured_capture_rgba8 = true;
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
    std::vector<const Texture*> maps_textures;
    std::vector<const Sampler*> maps_samplers;
    std::vector<const Texture*> detail_textures;
    std::vector<const Sampler*> detail_samplers;
    std::vector<const Texture*> normal_detail_textures;
    std::vector<const Sampler*> normal_detail_samplers;
    std::vector<const Buffer*> material_buffers;
    std::vector<std::uint64_t> material_offsets;
    std::vector<std::uint32_t> material_ranges;
    std::vector<const Buffer*> frame_buffers;
    std::vector<std::uint64_t> frame_offsets;
    std::vector<std::uint32_t> frame_ranges;
    std::vector<std::array<const DepthAttachment*,
                           indexed_directional_shadow_cascade_count>>
        directional_shadow_maps;
    std::vector<const Sampler*> directional_shadow_samplers;
    std::vector<const Buffer*> directional_shadow_buffers;
    std::vector<std::uint32_t> directional_shadow_ranges;
    std::vector<std::uint32_t> overlay_positions;
    std::vector<const Buffer*> overlay_buffers;
    std::vector<std::uint32_t> selected_positions;
    std::vector<DepthAttachmentDescription> depth_descriptions;
    std::vector<std::vector<apex::scene::NodeId>> depth_nodes;
    std::vector<apex::scene::Matrix4> depth_camera_matrices;
    std::vector<PipelineCullMode> depth_culls;
    std::vector<DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode>
        depth_material_modes;
    std::vector<bool> depth_transparent_flags;
    std::vector<bool> depth_blend_flags;
    std::vector<bool> depth_alpha_to_coverage_flags;
    std::vector<bool> depth_test_flags;
    std::vector<bool> depth_write_flags;
    std::vector<const Texture*> depth_alpha_textures;
    std::vector<const Sampler*> depth_alpha_samplers;
    std::vector<const Buffer*> depth_alpha_material_buffers;
    std::vector<std::uint32_t> depth_alpha_material_ranges;

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

PipelineProgram overlay_pipeline_fixture() {
    PipelineProgram pipeline = pipeline_fixture("authoring-overlay");
    pipeline.vertex_layout.stride = sizeof(OverlayLineVertex);
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::color,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
    };
    pipeline.raster.fill = PipelineFillMode::wireframe;
    return pipeline;
}

PipelineProgram selected_mesh_pipeline_fixture() {
    PipelineProgram pipeline = pipeline_fixture("selected-mesh");
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
    };
    pipeline.raster.cull = PipelineCullMode::front;
    pipeline.transform_contract = PipelineTransformContract::selected_mesh;
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

    const std::array<std::uint8_t, 3U> hide_skinned = {1U, 0U, 1U};
    frame.packet_visibility = hide_skinned;
    const std::size_t updates_before_hidden_skin = device.update_calls;
    const auto hidden_skin = prepared.resources->draw_and_readback(device, target, frame);
    require(hidden_skin.ok() && device.update_calls == updates_before_hidden_skin &&
                device.nodes == std::vector<apex::scene::NodeId>({1U, 1U}),
            "hidden skinned packet skips pose upload and color submission");

    const std::array<std::uint8_t, 3U> show_only_skinned = {0U, 1U, 0U};
    frame.packet_visibility = show_only_skinned;
    const auto only_skin = prepared.resources->draw_and_readback(device, target, frame);
    require(only_skin.ok() && device.update_calls == updates_before_hidden_skin + 1U &&
                device.nodes == std::vector<apex::scene::NodeId>({2U}),
            "visible skinned packet updates and submits without hidden static packets");

    const std::array<std::uint8_t, 3U> hide_all = {0U, 0U, 0U};
    frame.packet_visibility = hide_all;
    const std::size_t updates_before_clear_only = device.update_calls;
    const auto clear_only = prepared.resources->draw_and_readback(device, target, frame);
    require(clear_only.ok() && device.update_calls == updates_before_clear_only &&
                device.nodes.empty(),
            "all-hidden frame clears and reads back without draws or skin uploads");

    const std::array<std::uint8_t, 1U> short_visibility = {1U};
    frame.packet_visibility = short_visibility;
    const std::size_t batches_before_invalid_visibility = device.batch_calls;
    const auto invalid_visibility_count =
        prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_visibility_count.status ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_visibility_count.diagnostic.code ==
                    "static_scene_frame_visibility_mask_count_invalid" &&
                device.update_calls == updates_before_clear_only &&
                device.batch_calls == batches_before_invalid_visibility,
            "short packet visibility mask fails before update or color submission");

    const std::array<std::uint8_t, 3U> invalid_visibility = {1U, 2U, 1U};
    frame.packet_visibility = invalid_visibility;
    const auto invalid_visibility_value =
        prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_visibility_value.status ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_visibility_value.diagnostic.code ==
                    "static_scene_frame_visibility_mask_value_invalid" &&
                device.update_calls == updates_before_clear_only &&
                device.batch_calls == batches_before_invalid_visibility,
            "non-binary packet visibility fails before update or color submission");
    frame.packet_visibility = {};

    const std::size_t updates_before_invalid_order = device.update_calls;
    const std::size_t batches_before_invalid_order = device.batch_calls;
    const std::array<std::uint32_t, 2U> short_order = {0U, 1U};
    frame.color_packet_order = short_order;
    const auto invalid_order_count =
        prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_order_count.status ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_order_count.diagnostic.code ==
                    "static_scene_frame_color_order_count_invalid" &&
                device.update_calls == updates_before_invalid_order &&
                device.batch_calls == batches_before_invalid_order,
            "short color order fails before skinned updates or submission");

    const std::array<std::uint32_t, 3U> out_of_range_order = {
        0U, 1U, std::numeric_limits<std::uint32_t>::max()};
    frame.color_packet_order = out_of_range_order;
    const auto invalid_order_index =
        prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_order_index.status ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_order_index.diagnostic.code ==
                    "static_scene_frame_color_order_index_invalid" &&
                device.update_calls == updates_before_invalid_order &&
                device.batch_calls == batches_before_invalid_order,
            "out-of-range color order fails before skinned updates or submission");

    const std::array<std::uint32_t, 3U> duplicate_order = {0U, 0U, 2U};
    frame.color_packet_order = duplicate_order;
    const auto invalid_order_duplicate =
        prepared.resources->draw_and_readback(device, target, frame);
    require(invalid_order_duplicate.status ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_order_duplicate.diagnostic.code ==
                    "static_scene_frame_color_order_duplicate" &&
                device.update_calls == updates_before_invalid_order &&
                device.batch_calls == batches_before_invalid_order,
            "duplicate color order fails before skinned updates or submission");
    frame.color_packet_order = {};

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

    const std::array<std::uint32_t, 3U> color_order = {2U, 0U, 1U};
    frame.color_packet_order = color_order;
    const auto reordered =
        prepared.resources->draw_and_readback(device, target, frame);
    require(reordered.ok() &&
                device.nodes ==
                    std::vector<apex::scene::NodeId>({1U, 1U, 2U}) &&
                device.pipeline_names ==
                    std::vector<std::string>({"material-zero", "material-zero",
                                              "material-one"}) &&
                device.transparent_flags ==
                    std::vector<bool>({false, false, true}),
            "explicit color order changes submission without remapping packet resources");
}

void suppresses_shadow_only_packets_from_color_submission() {
    Fixture malformed = fixture();
    malformed.packets[1U].shadow_only = true;
    RecordingDevice malformed_device;
    const auto rejected = prepare_static_scene_resources(
        malformed_device, request_for(malformed));
    require(!rejected.ok() &&
                rejected.diagnostic.code ==
                    "static_scene_shadow_only_authority_invalid" &&
                malformed_device.buffer_calls == 0U,
            "a shadow-only non-caster fails before backend allocation");

    Fixture value = fixture();
    value.packets[1U].shadow_only = true;
    value.packets[1U].flags.cast_shadows = true;
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok() && prepared.resources->draw_count() == 3U,
            "shadow-only packet resources prepare with the complete packet table");

    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    const std::array<std::uint8_t, 3U> all_visible = {1U, 1U, 1U};
    frame.packet_visibility = all_visible;
    const auto drawn =
        prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() &&
                device.nodes ==
                    std::vector<apex::scene::NodeId>({1U, 1U}),
            "an explicit color mask cannot authorize a shadow-only packet");

    std::vector<DrawPacket> refreshed(value.packets.begin(), value.packets.end());
    refreshed[1U].shadow_only = false;
    frame.refreshed_packets = refreshed;
    const auto batches_before_invalid = device.batch_calls;
    const auto invalid =
        prepared.resources->draw_and_readback(device, target, frame);
    require(invalid.status == IndexedStaticMeshBatchStatus::invalid_request &&
                invalid.diagnostic.code ==
                    "static_scene_frame_packet_contract_invalid" &&
                device.batch_calls == batches_before_invalid,
            "a refreshed packet cannot change shadow-only authority");
}

void schedules_selected_and_view_axis_at_transparent_boundary() {
    Fixture value = fixture();
    value.packets[0].flags.selected = true;
    value.packets[1].flags.transparent = true;
    value.packets[1].flags.blend_enabled = true;
    value.second_pipeline.blend.enabled = true;
    value.second_pipeline.blend.source_color =
        PipelineBlendFactor::source_alpha;
    value.second_pipeline.blend.destination_color =
        PipelineBlendFactor::one_minus_source_alpha;
    value.second_pipeline.blend.source_alpha =
        PipelineBlendFactor::source_alpha;
    value.second_pipeline.blend.destination_alpha =
        PipelineBlendFactor::one_minus_source_alpha;
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok(), "phased overlay scene preparation succeeds");

    PipelineProgram overlay_pipeline = overlay_pipeline_fixture();
    PipelineProgram selected_pipeline = selected_mesh_pipeline_fixture();
    FakeBuffer overlay_buffer(
        {6U * sizeof(OverlayLineVertex), BufferUsage::vertex,
         BufferMemory::host_visible, BufferMutability::immutable});
    FakeBuffer scene_finished_buffer(
        {6U * sizeof(OverlayLineVertex), BufferUsage::vertex,
         BufferMemory::host_visible, BufferMutability::immutable});
    FakeBuffer late_overlay_buffer(
        {6U * sizeof(OverlayLineVertex), BufferUsage::vertex,
         BufferMemory::host_visible, BufferMutability::immutable});
    FakeBuffer selected_color(
        {selected_mesh_color_view_bytes, BufferUsage::uniform,
         BufferMemory::host_visible, BufferMutability::mutable_data});
    OverlayLineDrawRequest view_axis;
    view_axis.pipeline = &overlay_pipeline;
    view_axis.vertex_buffer = &overlay_buffer;
    view_axis.vertex_count = 6U;
    OverlayLineDrawRequest scene_finished = view_axis;
    scene_finished.vertex_buffer = &scene_finished_buffer;
    OverlayLineDrawRequest late_overlay = view_axis;
    late_overlay.vertex_buffer = &late_overlay_buffer;
    const std::array scene_finished_draws = {scene_finished};
    const std::array view_axis_draws = {view_axis};
    const std::array late_overlay_draws = {late_overlay};
    const std::array<std::uint8_t, 3U> visible = {1U, 1U, 0U};

    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.packet_visibility = visible;
    frame.selected_mesh_pipeline = &selected_pipeline;
    frame.selected_mesh_color_buffer = &selected_color;
    frame.scene_finished_overlay_draws = scene_finished_draws;
    frame.view_axis_draws = view_axis_draws;
    frame.overlay_draws = late_overlay_draws;
    const auto drawn = prepared.resources->draw_and_readback(
        device, target, frame);
    require(drawn.ok() && device.nodes ==
                std::vector<apex::scene::NodeId>({1U, 2U}),
            "visible ordinary packets retain opaque then transparent order");
    require(device.selected_positions == std::vector<std::uint32_t>({1U}) &&
                device.overlay_positions ==
                    std::vector<std::uint32_t>({
                        1U, 1U,
                        std::numeric_limits<std::uint32_t>::max()}) &&
                device.overlay_buffers ==
                    std::vector<const Buffer*>({&scene_finished_buffer,
                                                &overlay_buffer,
                                                &late_overlay_buffer}),
            "selected mesh, scene-finished lines, and view axis precede transparent geometry and late overlays");

    const std::size_t batches_before_invalid = device.batch_calls;
    frame.packet_visibility = {};
    const auto invalid_order = prepared.resources->draw_and_readback(
        device, target, frame);
    require(invalid_order.status ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_order.diagnostic.code ==
                    "static_scene_draw_phase_order_invalid" &&
                device.batch_calls == batches_before_invalid,
            "phased overlays reject opaque packets after transparent packets");
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

void validates_alpha_to_coverage_sample_contract_before_allocation() {
    const auto configure_a2c = [](Fixture& value, std::uint32_t color_samples) {
        value.first_pipeline.targets.colors.front().samples = color_samples;
        value.second_pipeline.targets.colors.front().samples = color_samples;
        value.first_pipeline.blend.alpha_to_coverage = true;
        value.second_pipeline.blend.alpha_to_coverage = true;
        for (DrawPacket& packet : value.packets)
            packet.flags.alpha_to_coverage = true;
    };

    Fixture valid = fixture();
    configure_a2c(valid, 4U);
    RecordingDevice valid_device;
    const auto prepared = prepare_static_scene_resources(valid_device, request_for(valid));
    require(prepared.ok() && valid_device.buffer_calls == 4U,
            "four-sample alpha-to-coverage scene prepares");
    FakeTexture target({16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
                        TextureUsage::color_attachment | TextureUsage::transfer_source,
                        TextureMemory::device_local, TextureMutability::mutable_data, 4U});
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    FakeTexture resolve_target;
    frame.resolve_target = &resolve_target;
    frame.capture_rgba8 = false;
    const auto drawn = prepared.resources->draw_and_readback(valid_device, target, frame);
    require(drawn.ok() && valid_device.batch_calls == 1U &&
                valid_device.captured_resolve_target == &resolve_target &&
                !valid_device.captured_capture_rgba8,
            "four-sample scene forwards its retained resolve and CPU capture policy");

    Fixture shared_msaa = fixture();
    shared_msaa.first_pipeline.targets.colors.front().samples = 4U;
    shared_msaa.first_pipeline.blend.alpha_to_coverage = true;
    shared_msaa.second_pipeline.targets.colors.front().samples = 4U;
    shared_msaa.packets[0].flags.alpha_to_coverage = true;
    shared_msaa.packets[2].flags.alpha_to_coverage = true;
    RecordingDevice shared_msaa_device;
    const auto shared_msaa_prepared =
        prepare_static_scene_resources(shared_msaa_device, request_for(shared_msaa));
    require(shared_msaa_prepared.ok(),
            "ordinary and alpha-to-coverage pipelines share a four-sample batch");
    const auto shared_msaa_drawn =
        shared_msaa_prepared.resources->draw_and_readback(shared_msaa_device, target, frame);
    require(shared_msaa_drawn.ok() && shared_msaa_device.batch_calls == 1U,
            "shared four-sample ordinary and alpha-to-coverage batch executes");

    Fixture one_sample = fixture();
    configure_a2c(one_sample, 1U);
    RecordingDevice one_sample_device;
    const auto one_sample_result =
        prepare_static_scene_resources(one_sample_device, request_for(one_sample));
    require(!one_sample_result.ok() &&
                one_sample_result.status == StaticSceneResourceStatus::invalid_request &&
                one_sample_result.diagnostic.code == "static_scene_material_pipeline_invalid" &&
                one_sample_device.buffer_calls == 0U,
            "one-sample alpha-to-coverage is rejected before allocation");

    Fixture mismatched = fixture();
    mismatched.first_pipeline.targets.colors.front().samples = 4U;
    mismatched.first_pipeline.blend.alpha_to_coverage = true;
    mismatched.packets[0].flags.alpha_to_coverage = true;
    mismatched.packets[2].flags.alpha_to_coverage = true;
    RecordingDevice mismatched_device;
    const auto mismatched_result =
        prepare_static_scene_resources(mismatched_device, request_for(mismatched));
    require(!mismatched_result.ok() &&
                mismatched_result.status == StaticSceneResourceStatus::unsupported &&
                mismatched_result.diagnostic.code == "static_scene_mixed_color_samples_unsupported" &&
                mismatched_device.buffer_calls == 0U,
            "mixed one- and four-sample packets are rejected before allocation");

    Fixture bad_depth = fixture();
    configure_a2c(bad_depth, 4U);
    bad_depth.first_pipeline.targets.has_depth = true;
    bad_depth.first_pipeline.targets.depth =
        {PipelineRenderTargetFormat::depth32_float, 1U};
    bad_depth.first_pipeline.depth.test_enabled = true;
    bad_depth.packets[0].flags.depth_test = true;
    bad_depth.packets[2].flags.depth_test = true;
    RecordingDevice bad_depth_device;
    const auto bad_depth_result =
        prepare_static_scene_resources(bad_depth_device, request_for(bad_depth));
    require(!bad_depth_result.ok() &&
                bad_depth_result.status == StaticSceneResourceStatus::invalid_request &&
                bad_depth_result.diagnostic.code == "static_scene_material_pipeline_invalid" &&
                bad_depth_device.buffer_calls == 0U,
            "alpha-to-coverage depth sample mismatch is rejected before allocation");

    Fixture valid_depth = fixture();
    configure_a2c(valid_depth, 4U);
    for (PipelineProgram* pipeline : {&valid_depth.first_pipeline,
                                      &valid_depth.second_pipeline}) {
        pipeline->targets.has_depth = true;
        pipeline->targets.depth =
            {PipelineRenderTargetFormat::depth32_float, 4U};
        pipeline->depth.test_enabled = true;
        pipeline->depth.compare = PipelineCompareOperation::less;
    }
    for (DrawPacket& packet : valid_depth.packets)
        packet.flags.depth_test = true;
    RecordingDevice valid_depth_device;
    const auto valid_depth_result =
        prepare_static_scene_resources(valid_depth_device, request_for(valid_depth));
    require(valid_depth_result.ok(),
            "alpha-to-coverage accepts matching four-sample D32 depth targets");

    Fixture opaque = fixture();
    RecordingDevice opaque_device;
    const auto opaque_result = prepare_static_scene_resources(opaque_device, request_for(opaque));
    require(opaque_result.ok(), "non-alpha-to-coverage scenes remain single-sample valid");

    Fixture ordinary_msaa = fixture();
    ordinary_msaa.first_pipeline.targets.colors.front().samples = 4U;
    ordinary_msaa.second_pipeline.targets.colors.front().samples = 4U;
    RecordingDevice ordinary_msaa_device;
    const auto ordinary_msaa_result =
        prepare_static_scene_resources(ordinary_msaa_device, request_for(ordinary_msaa));
    require(ordinary_msaa_result.ok(),
            "non-alpha-to-coverage scenes remain four-sample valid");
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

void owns_signature_identified_png_resources() {
    Fixture value = fixture();
    const std::array<std::uint8_t, 4> pixel = {9U, 41U, 137U, 223U};
    configure_embedded_diffuse(value, apex::tests::rgba8PngFixture(pixel));
    auto request = request_for(value);
    request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;

    RecordingDevice device;
    const auto prepared = prepare_static_scene_resources(device, request);
    require(prepared.ok() && prepared.resources->owned_texture_count() == 1U &&
                device.texture_calls == 1U && device.sampler_calls == 1U &&
                device.texture_descriptions.front().format ==
                    TextureFormat::rgba8_unorm &&
                device.texture_upload_bytes.front() ==
                    std::vector<std::byte>{std::byte{9U}, std::byte{41U},
                                           std::byte{137U}, std::byte{223U}},
            "embedded PNG is decoded to one owned backend-neutral RGBA8 texture");
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
    auto truncated_png =
        apex::tests::rgba8PngFixture({5U, 7U, 11U, 255U});
    truncated_png.pop_back();
    configure_embedded_diffuse(value, std::move(truncated_png));
    embedded_request = request_for(value);
    embedded_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    const auto truncated_png_result =
        prepare_static_scene_resources(device, embedded_request);
    require(!truncated_png_result.ok() &&
                truncated_png_result.diagnostic.code ==
                    "static_scene_embedded_texture_truncated" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.sampler_calls == 0U,
            "truncated embedded PNG fails before backend allocation");

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

void bounds_host_preparation_metadata_before_allocation() {
    Fixture baseline = fixture();
    auto baseline_request = request_for(baseline);
    baseline_request.limits.max_preparation_bytes = 32U * 1024U;
    RecordingDevice baseline_device;
    const auto baseline_result =
        prepare_static_scene_resources(baseline_device, baseline_request);
    require(baseline_result.ok(),
            "the focused host-preparation budget accepts the small fixture");

    Fixture many_nodes = fixture();
    for (std::size_t index = 0U; index < 1024U; ++index) {
        apex::formats::Kn5Node source;
        source.type = 1U;
        source.kind = "node";
        source.name = "N" + std::to_string(index);
        many_nodes.model.root.children.push_back(std::move(source));
        apex::scene::SceneNode node;
        node.name = "N" + std::to_string(index);
        node.kind = apex::scene::NodeKind::node;
        node.renderable = false;
        (void)many_nodes.scene.add_node(std::move(node), many_nodes.scene.root);
    }
    auto many_nodes_request = request_for(many_nodes);
    many_nodes_request.limits.max_preparation_bytes = 32U * 1024U;
    RecordingDevice many_nodes_device;
    const auto many_nodes_result =
        prepare_static_scene_resources(many_nodes_device, many_nodes_request);
    require(!many_nodes_result.ok() &&
                many_nodes_result.diagnostic.code ==
                    "static_scene_preparation_aggregate_limit" &&
                many_nodes_device.buffer_calls == 0U &&
                many_nodes_device.texture_calls == 0U &&
                many_nodes_device.sampler_calls == 0U,
            "node-map capacity consumes the host-preparation budget before mapping");

    Fixture many_packets = fixture();
    const DrawPacket repeated = many_packets.packets.front();
    many_packets.packets.clear();
    for (std::size_t index = 0U; index < 128U; ++index) {
        many_packets.packets.push_back(repeated);
        many_packets.packets.back().order = index;
    }
    auto many_packets_request = request_for(many_packets);
    many_packets_request.limits.max_preparation_bytes = 64U * 1024U;
    RecordingDevice many_packets_device;
    const auto many_packets_result =
        prepare_static_scene_resources(many_packets_device, many_packets_request);
    require(!many_packets_result.ok() &&
                many_packets_result.diagnostic.code ==
                    "static_scene_preparation_aggregate_limit" &&
                many_packets_device.buffer_calls == 0U &&
                many_packets_device.texture_calls == 0U &&
                many_packets_device.sampler_calls == 0U,
            "reserved pipeline and material capacity consumes the host budget");

    Fixture embedded = fixture();
    const std::array<std::uint8_t, 4> pixel = {1U, 2U, 3U, 4U};
    configure_embedded_diffuse(embedded, rgba8_dds(false, pixel));
    auto embedded_request = request_for(embedded);
    embedded_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    embedded_request.limits.max_preparation_bytes = 8U * 1024U;
    RecordingDevice embedded_device;
    const auto embedded_result =
        prepare_static_scene_resources(embedded_device, embedded_request);
    require(!embedded_result.ok() &&
                embedded_result.diagnostic.code ==
                    "static_scene_preparation_aggregate_limit" &&
                embedded_device.buffer_calls == 0U &&
                embedded_device.texture_calls == 0U &&
                embedded_device.sampler_calls == 0U,
            "decoded upload-plan metadata consumes the host budget before allocation");

    Fixture many_skinned = fixture();
    make_second_mesh_skinned(many_skinned);
    const DrawPacket skinned_packet = many_skinned.packets[1];
    many_skinned.packets.assign(64U, skinned_packet);
    for (std::size_t index = 0U; index < many_skinned.packets.size(); ++index)
        many_skinned.packets[index].order = index;
    auto many_skinned_request = request_for(many_skinned);
    many_skinned_request.limits.max_preparation_bytes = 32U * 1024U;
    RecordingDevice many_skinned_device;
    const auto many_skinned_result =
        prepare_static_scene_resources(many_skinned_device, many_skinned_request);
    require(!many_skinned_result.ok() &&
                many_skinned_result.diagnostic.code ==
                    "static_scene_preparation_aggregate_limit" &&
                many_skinned_device.buffer_calls == 0U,
            "many skinned packets consume the host-preparation budget before allocation");
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
    constants.fog[0] = std::numeric_limits<float>::quiet_NaN();
    frame.frame_constants = constants;
    auto non_finite_fog =
        prepared.resources->draw_and_readback(device, target, frame);
    require(non_finite_fog.status ==
                    IndexedStaticMeshBatchStatus::invalid_request &&
                non_finite_fog.diagnostic.code ==
                    "static_scene_frame_constants_non_finite" &&
                device.update_calls == 0U && device.batch_calls == 0U,
            "non-finite fog constants fail before update or recording");

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

void resolves_txmaps_resources_with_bounded_ownership_and_preflight() {
    const auto configure = [](Fixture& value, bool embed_textures) {
        const std::array<std::uint8_t, 4> diffuse_pixel = {17U, 34U, 51U, 255U};
        const std::array<std::uint8_t, 4> normal_pixel = {128U, 255U, 128U, 255U};
        const std::array<std::uint8_t, 4> maps_pixel = {9U, 19U, 29U, 255U};
        value.model.textures.push_back(
            {true, "body.dds", embed_textures ? 152U : 0U,
             embed_textures ? rgba8_dds(true, diffuse_pixel)
                            : std::vector<std::uint8_t>{}, {}});
        value.model.textures.push_back(
            {true, "body_nm.dds", embed_textures ? 152U : 0U,
             embed_textures ? rgba8_dds(false, normal_pixel)
                            : std::vector<std::uint8_t>{}, {}});
        value.model.textures.push_back(
            {true, "body_maps.dds", embed_textures ? 152U : 0U,
             embed_textures ? rgba8_dds(false, maps_pixel)
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
            {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTexture"},
            {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
        };
        const std::vector<DrawResourceSlot> slots = {
            {"txDiffuse", 21U, 0U, "body.dds"},
            {"txNormal", 22U, 1U, "body_nm.dds"},
            {"txMaps", 23U, 2U, "body_maps.dds"},
        };
        value.packets[0].resources = slots;
        value.packets[2].resources = slots;
    };

    std::array<KsPerPixelMaterialConstants, 2> constants{};
    constants[0].fresnel[2] = 0.0F;
    Fixture caller = fixture();
    configure(caller, false);
    auto caller_request = request_for(caller);
    caller_request.material_constants_by_material = constants;
    RecordingDevice caller_device;
    const auto prepared = prepare_static_scene_resources(caller_device, caller_request);
    require(prepared.ok(), "txMaps caller-table scene prepares");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeTexture normal(sampled_description);
    FakeTexture maps(sampled_description);
    FakeSampler diffuse_sampler;
    FakeSampler normal_sampler;
    FakeSampler maps_sampler;
    const std::array<const Texture*, 3> textures = {&diffuse, &normal, &maps};
    const std::array<const Sampler*, 3> samplers = {
        &diffuse_sampler, &normal_sampler, &maps_sampler};
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.position = {2.0F, 3.0F, 4.0F};
    frame.frame_constants = KsPerPixelFrameConstants{};
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    FakeTexture target;
    const auto drawn = prepared.resources->draw_and_readback(
        caller_device, target, frame);
    require(drawn.ok() &&
                caller_device.maps_textures ==
                    std::vector<const Texture*>{&maps, nullptr, &maps} &&
                caller_device.maps_samplers ==
                    std::vector<const Sampler*>{&maps_sampler, nullptr, &maps_sampler},
            "caller tables preserve txMaps bindings in packet order");

    Fixture embedded = fixture();
    configure(embedded, true);
    auto embedded_request = request_for(embedded);
    embedded_request.material_constants_by_material = constants;
    embedded_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice embedded_device;
    const auto embedded_prepared =
        prepare_static_scene_resources(embedded_device, embedded_request);
    require(embedded_prepared.ok() &&
                embedded_prepared.resources->owned_texture_count() == 3U &&
                embedded_device.texture_calls == 3U && embedded_device.sampler_calls == 1U,
            "embedded txMaps resources decode with one sampler");

    Fixture malformed = fixture();
    configure(malformed, false);
    malformed.packets[2].resources[2] = malformed.packets[2].resources[0];
    RecordingDevice malformed_device;
    auto malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = constants;
    const auto duplicate =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!duplicate.ok() &&
                duplicate.diagnostic.code == "static_scene_resource_slot_duplicate" &&
                malformed_device.buffer_calls == 0U && malformed_device.texture_calls == 0U,
            "duplicate txMaps slot fails before backend allocation");

    malformed = fixture();
    configure(malformed, false);
    malformed.packets[2].resources.pop_back();
    malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = constants;
    const auto missing =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!missing.ok() &&
                missing.diagnostic.code == "static_scene_material_packet_unsupported" &&
                malformed_device.buffer_calls == 0U,
            "missing txMaps slot fails before backend allocation");

    malformed = fixture();
    configure(malformed, false);
    malformed.packets[2].resources[2].texture = "wrong.dds";
    malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = constants;
    const auto wrong_identity =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!wrong_identity.ok() &&
                wrong_identity.diagnostic.code == "static_scene_maps_texture_identity_invalid" &&
                malformed_device.buffer_calls == 0U,
            "wrong txMaps identity fails before backend allocation");

    malformed = fixture();
    configure(malformed, false);
    malformed.model.textures.push_back(malformed.model.textures.front());
    malformed.model.textures.back().name = "BODY.DDS";
    malformed_request = request_for(malformed);
    malformed_request.material_constants_by_material = constants;
    const auto ambiguous =
        prepare_static_scene_resources(malformed_device, malformed_request);
    require(!ambiguous.ok() &&
                ambiguous.diagnostic.code == "static_scene_diffuse_texture_identity_invalid" &&
                malformed_device.buffer_calls == 0U,
            "same-scope canonical duplicate texture names fail before allocation");

    Fixture truncated = fixture();
    configure(truncated, true);
    truncated.model.textures[2].data.pop_back();
    truncated.model.textures[2].size =
        static_cast<std::uint32_t>(truncated.model.textures[2].data.size());
    auto truncated_request = request_for(truncated);
    truncated_request.material_constants_by_material = constants;
    truncated_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice truncated_device;
    const auto truncated_maps =
        prepare_static_scene_resources(truncated_device, truncated_request);
    require(!truncated_maps.ok() &&
                truncated_maps.diagnostic.code ==
                    "static_scene_embedded_texture_truncated" &&
                truncated_device.buffer_calls == 0U && truncated_device.texture_calls == 0U,
            "a consistent-size truncated txMaps mip fails in DDS preflight");

    Fixture malformed_dx10_maps = fixture();
    configure(malformed_dx10_maps, true);
    put32(malformed_dx10_maps.model.textures[2].data, 132U, 0U);
    auto malformed_dx10_request = request_for(malformed_dx10_maps);
    malformed_dx10_request.material_constants_by_material = constants;
    malformed_dx10_request.texture_authority =
        StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice malformed_dx10_device;
    const auto malformed_dx10 =
        prepare_static_scene_resources(malformed_dx10_device, malformed_dx10_request);
    require(!malformed_dx10.ok() &&
                malformed_dx10.diagnostic.code ==
                    "static_scene_embedded_texture_invalid_header" &&
                malformed_dx10_device.buffer_calls == 0U &&
                malformed_dx10_device.texture_calls == 0U &&
                malformed_dx10_device.sampler_calls == 0U,
            "a malformed txMaps DX10 dimension fails before backend allocation");

    Fixture invalid_size_maps = fixture();
    configure(invalid_size_maps, true);
    put32(invalid_size_maps.model.textures[2].data, 16U, 0U);
    auto invalid_size_request = request_for(invalid_size_maps);
    invalid_size_request.material_constants_by_material = constants;
    invalid_size_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice invalid_size_device;
    const auto invalid_size =
        prepare_static_scene_resources(invalid_size_device, invalid_size_request);
    require(!invalid_size.ok() &&
                invalid_size.diagnostic.code ==
                    "static_scene_embedded_texture_invalid_header" &&
                invalid_size_device.buffer_calls == 0U &&
                invalid_size_device.texture_calls == 0U &&
                invalid_size_device.sampler_calls == 0U,
            "a zero-width txMaps DDS fails before backend allocation");

    Fixture srgb_maps = fixture();
    configure(srgb_maps, true);
    put32(srgb_maps.model.textures[2].data, 128U, 29U);
    auto srgb_maps_request = request_for(srgb_maps);
    srgb_maps_request.material_constants_by_material = constants;
    srgb_maps_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice srgb_maps_device;
    const auto unsupported_maps =
        prepare_static_scene_resources(srgb_maps_device, srgb_maps_request);
    require(!unsupported_maps.ok() &&
                unsupported_maps.status == StaticSceneResourceStatus::unsupported &&
                unsupported_maps.diagnostic.code ==
                    "static_scene_maps_texture_format_unsupported" &&
                srgb_maps_device.buffer_calls == 0U &&
                srgb_maps_device.texture_calls == 0U &&
                srgb_maps_device.sampler_calls == 0U,
            "sRGB txMaps fails before backend allocation");

    Fixture limited = fixture();
    configure(limited, true);
    auto limited_request = request_for(limited);
    limited_request.material_constants_by_material = constants;
    limited_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    limited_request.limits.max_total_texture_source_bytes = 2U * 152U;
    RecordingDevice limited_device;
    const auto source_limited =
        prepare_static_scene_resources(limited_device, limited_request);
    require(!source_limited.ok() &&
                source_limited.diagnostic.code ==
                    "static_scene_texture_source_aggregate_limit" &&
                limited_device.buffer_calls == 0U && limited_device.texture_calls == 0U,
            "txMaps source aggregate limit fails before backend allocation");

    limited_request.limits.max_total_texture_source_bytes =
        std::numeric_limits<std::uint64_t>::max();
    limited_request.limits.max_total_decoded_texture_bytes = 8U;
    const auto decoded_limited =
        prepare_static_scene_resources(limited_device, limited_request);
    require(!decoded_limited.ok() &&
                decoded_limited.diagnostic.code ==
                    "static_scene_texture_decode_aggregate_limit" &&
                limited_device.buffer_calls == 0U && limited_device.texture_calls == 0U,
            "txMaps decoded aggregate limit fails before backend allocation");

    Fixture preparation_limited = fixture();
    configure(preparation_limited, false);
    auto preparation_limited_request = request_for(preparation_limited);
    preparation_limited_request.material_constants_by_material = constants;
    preparation_limited_request.limits.max_preparation_bytes = 1U;
    RecordingDevice preparation_limited_device;
    const auto preparation_limited_result = prepare_static_scene_resources(
        preparation_limited_device, preparation_limited_request);
    require(!preparation_limited_result.ok() &&
                preparation_limited_result.diagnostic.code ==
                    "static_scene_preparation_aggregate_limit" &&
                preparation_limited_device.buffer_calls == 0U &&
                preparation_limited_device.texture_calls == 0U &&
                preparation_limited_device.sampler_calls == 0U,
            "the host preparation budget fails before backend allocation");

    Fixture failed_maps = fixture();
    configure(failed_maps, true);
    auto failed_maps_request = request_for(failed_maps);
    failed_maps_request.material_constants_by_material = constants;
    failed_maps_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice failed_maps_device;
    failed_maps_device.fail_texture_call = 3U;
    const auto third_texture_failure =
        prepare_static_scene_resources(failed_maps_device, failed_maps_request);
    require(!third_texture_failure.ok() &&
                third_texture_failure.status == StaticSceneResourceStatus::upload_failed &&
                third_texture_failure.diagnostic.code == "recording_texture_failure" &&
                failed_maps_device.texture_calls == 3U &&
                failed_maps_device.sampler_calls == 0U &&
                failed_maps_device.batch_calls == 0U &&
                failed_maps_device.live_buffers == 0U &&
                failed_maps_device.live_textures == 0U &&
                failed_maps_device.live_samplers == 0U,
            "a third-texture failure cleans earlier txMaps resources and stops preparation");

    RecordingDevice failed_maps_sampler_device;
    failed_maps_sampler_device.fail_sampler = true;
    const auto maps_sampler_failure =
        prepare_static_scene_resources(failed_maps_sampler_device, failed_maps_request);
    require(!maps_sampler_failure.ok() &&
                maps_sampler_failure.status == StaticSceneResourceStatus::allocation_failed &&
                maps_sampler_failure.diagnostic.code == "recording_sampler_failure" &&
                failed_maps_sampler_device.texture_calls == 3U &&
                failed_maps_sampler_device.sampler_calls == 1U &&
                failed_maps_sampler_device.batch_calls == 0U &&
                failed_maps_sampler_device.live_buffers == 0U &&
                failed_maps_sampler_device.live_textures == 0U &&
                failed_maps_sampler_device.live_samplers == 0U,
            "a txMaps sampler failure cleans all prepared resources and stops submission");
}

void resolves_detail_stack_resources_with_aliases_and_preflight() {
    const auto configure = [](Fixture& value, bool embed_textures) {
        const std::array<std::uint8_t, 4> diffuse_pixel = {17U, 34U, 51U, 255U};
        const std::array<std::uint8_t, 4> normal_pixel = {128U, 255U, 128U, 255U};
        const std::array<std::uint8_t, 4> maps_pixel = {9U, 19U, 29U, 255U};
        const std::array<std::uint8_t, 4> detail_pixel = {200U, 100U, 50U, 128U};
        const std::array<std::uint8_t, 4> normal_detail_pixel = {128U, 128U, 255U, 255U};
        const auto payload = [&](bool srgb, const auto& pixel) {
            return embed_textures ? rgba8_dds(srgb, pixel) : std::vector<std::uint8_t>{};
        };
        value.model.textures.push_back({true, "body.dds", embed_textures ? 152U : 0U,
                                        payload(true, diffuse_pixel), {}});
        value.model.textures.push_back({true, "body_nm.dds", embed_textures ? 152U : 0U,
                                        payload(false, normal_pixel), {}});
        value.model.textures.push_back({true, "body_maps.dds", embed_textures ? 152U : 0U,
                                        payload(false, maps_pixel), {}});
        value.model.textures.push_back({true, "body_detail.dds", embed_textures ? 152U : 0U,
                                        payload(true, detail_pixel), {}});
        value.model.textures.push_back({true, "body_detail_nm.dds", embed_textures ? 152U : 0U,
                                        payload(false, normal_detail_pixel), {}});
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
            {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTexture"},
            {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
            {PipelineResourceKind::sampled_texture, 0U, 8U, "detailTexture"},
            {PipelineResourceKind::sampler, 0U, 9U, "detailSampler"},
            {PipelineResourceKind::sampled_texture, 0U, 10U, "normalDetailTexture"},
            {PipelineResourceKind::sampler, 0U, 11U, "normalDetailSampler"},
        };
        const std::vector<DrawResourceSlot> slots = {
            {"txDiffuse", 21U, 0U, "body.dds"},
            {"txNormal", 22U, 1U, "body_nm.dds"},
            {"txMaps", 23U, 2U, "body_maps.dds"},
            {"txDetail", 24U, 3U, "body_detail.dds"},
            {"txNormalDetail", 25U, 4U, "body_detail_nm.dds"},
        };
        value.packets[0].resources = slots;
        value.packets[2].resources = slots;
    };

    std::array<KsPerPixelMaterialConstants, 2> constants{};
    constants[0].fresnel[2] = 0.0F;
    constants[0].detail = {1.0F, 2.0F, 0.5F, 0.0F};
    Fixture caller = fixture();
    configure(caller, false);
    auto caller_request = request_for(caller);
    caller_request.material_constants_by_material = constants;
    RecordingDevice caller_device;
    const auto prepared = prepare_static_scene_resources(caller_device, caller_request);
    require(prepared.ok(), "detail-stack caller-table scene prepares");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeTexture normal(sampled_description);
    FakeTexture maps(sampled_description);
    FakeTexture detail({1U, 1U, 1U, 1U, TextureFormat::rgba8_srgb,
                        TextureUsage::sampled, TextureMemory::device_local,
                        TextureMutability::immutable});
    FakeTexture normal_detail(sampled_description);
    FakeSampler diffuse_sampler;
    FakeSampler normal_sampler;
    FakeSampler maps_sampler;
    FakeSampler detail_sampler;
    FakeSampler normal_detail_sampler;
    const std::array<const Texture*, 5> textures = {
        &diffuse, &normal, &maps, &detail, &normal_detail};
    const std::array<const Sampler*, 5> samplers = {
        &diffuse_sampler, &normal_sampler, &maps_sampler,
        &detail_sampler, &normal_detail_sampler};
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.position = {2.0F, 3.0F, 4.0F};
    frame.frame_constants = KsPerPixelFrameConstants{};
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    FakeTexture target;
    const auto drawn = prepared.resources->draw_and_readback(
        caller_device, target, frame);
    require(drawn.ok() &&
                caller_device.detail_textures ==
                    std::vector<const Texture*>{&detail, nullptr, &detail} &&
                caller_device.normal_detail_textures ==
                    std::vector<const Texture*>{&normal_detail, nullptr, &normal_detail} &&
                caller_device.detail_samplers ==
                    std::vector<const Sampler*>{&detail_sampler, nullptr, &detail_sampler} &&
                caller_device.normal_detail_samplers ==
                    std::vector<const Sampler*>{&normal_detail_sampler, nullptr,
                                                &normal_detail_sampler},
            "caller tables preserve detail-stack bindings in packet order");

    Fixture embedded = fixture();
    configure(embedded, true);
    auto embedded_request = request_for(embedded);
    embedded_request.material_constants_by_material = constants;
    embedded_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice embedded_device;
    const auto embedded_prepared =
        prepare_static_scene_resources(embedded_device, embedded_request);
    require(embedded_prepared.ok() &&
                embedded_prepared.resources->owned_texture_count() == 5U &&
                embedded_device.texture_calls == 5U && embedded_device.sampler_calls == 1U,
            "embedded detail-stack resources decode with one sampler");

    Fixture texture_failure = fixture();
    configure(texture_failure, true);
    auto texture_failure_request = request_for(texture_failure);
    texture_failure_request.material_constants_by_material = constants;
    texture_failure_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice texture_failure_device;
    texture_failure_device.fail_texture_call = 4U;
    const auto texture_failure_result =
        prepare_static_scene_resources(texture_failure_device, texture_failure_request);
    require(!texture_failure_result.ok() &&
                texture_failure_result.diagnostic.code == "recording_texture_failure" &&
                texture_failure_device.texture_calls == 4U &&
                texture_failure_device.buffer_calls == 2U,
            "detail-stack texture upload failures release prepared resources");

    Fixture alias = fixture();
    configure(alias, false);
    alias.packets[2].resources.back().slot = "txDetailNM";
    auto alias_request = request_for(alias);
    alias_request.material_constants_by_material = constants;
    RecordingDevice alias_device;
    const auto alias_prepared = prepare_static_scene_resources(alias_device, alias_request);
    require(alias_prepared.ok(), "txDetailNM is accepted as the normal-detail alias");

    Fixture duplicate = fixture();
    configure(duplicate, false);
    duplicate.packets[2].resources[3].slot = "txDetailNM";
    auto duplicate_request = request_for(duplicate);
    duplicate_request.material_constants_by_material = constants;
    RecordingDevice duplicate_device;
    const auto duplicate_result =
        prepare_static_scene_resources(duplicate_device, duplicate_request);
    require(!duplicate_result.ok() &&
                duplicate_result.diagnostic.code == "static_scene_resource_slot_duplicate" &&
                duplicate_device.buffer_calls == 0U,
            "duplicate detail-stack role fails before allocation");

    Fixture missing = fixture();
    configure(missing, false);
    missing.packets[2].resources.pop_back();
    auto missing_request = request_for(missing);
    missing_request.material_constants_by_material = constants;
    RecordingDevice missing_device;
    const auto missing_result =
        prepare_static_scene_resources(missing_device, missing_request);
    require(!missing_result.ok() &&
                missing_result.diagnostic.code == "static_scene_material_packet_unsupported" &&
                missing_device.buffer_calls == 0U,
            "missing normal-detail role fails before allocation");

    Fixture wrong_identity = fixture();
    configure(wrong_identity, false);
    wrong_identity.packets[2].resources[4].texture = "wrong.dds";
    auto wrong_request = request_for(wrong_identity);
    wrong_request.material_constants_by_material = constants;
    RecordingDevice wrong_device;
    const auto wrong_result =
        prepare_static_scene_resources(wrong_device, wrong_request);
    require(!wrong_result.ok() &&
                wrong_result.diagnostic.code ==
                    "static_scene_normal_detail_texture_identity_invalid" &&
                wrong_device.buffer_calls == 0U,
            "wrong normal-detail identity fails before allocation");

    Fixture truncated = fixture();
    configure(truncated, true);
    truncated.model.textures[4].data.pop_back();
    truncated.model.textures[4].size =
        static_cast<std::uint32_t>(truncated.model.textures[4].data.size());
    auto truncated_request = request_for(truncated);
    truncated_request.material_constants_by_material = constants;
    truncated_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice truncated_device;
    const auto truncated_result =
        prepare_static_scene_resources(truncated_device, truncated_request);
    require(!truncated_result.ok() &&
                truncated_result.diagnostic.code == "static_scene_embedded_texture_truncated" &&
                truncated_device.buffer_calls == 0U && truncated_device.texture_calls == 0U,
            "truncated normal-detail payload fails before allocation");

    Fixture srgb_normal_detail = fixture();
    configure(srgb_normal_detail, true);
    put32(srgb_normal_detail.model.textures[4].data, 128U, 29U);
    auto srgb_request = request_for(srgb_normal_detail);
    srgb_request.material_constants_by_material = constants;
    srgb_request.texture_authority = StaticSceneTextureAuthority::embedded_kn5;
    RecordingDevice srgb_device;
    const auto srgb_result = prepare_static_scene_resources(srgb_device, srgb_request);
    require(!srgb_result.ok() &&
                srgb_result.diagnostic.code ==
                    "static_scene_normal_detail_texture_format_unsupported" &&
                srgb_device.buffer_calls == 0U && srgb_device.texture_calls == 0U,
            "sRGB normal-detail payload fails before allocation");

    Fixture non_finite = fixture();
    configure(non_finite, false);
    constants[0].detail[1] = std::numeric_limits<float>::infinity();
    auto non_finite_request = request_for(non_finite);
    non_finite_request.material_constants_by_material = constants;
    RecordingDevice non_finite_device;
    const auto non_finite_result =
        prepare_static_scene_resources(non_finite_device, non_finite_request);
    require(!non_finite_result.ok() &&
                non_finite_result.diagnostic.code ==
                    "static_scene_material_constant_non_finite" &&
                non_finite_device.buffer_calls == 0U,
            "non-finite detail constants fail before allocation");
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

void retains_three_directional_maps_and_executes_only_opaque_static_casters() {
    Fixture value = fixture();
    for (DrawPacket& packet : value.packets) {
        packet.flags.cast_shadows = true;
        packet.flags.depth_test = true;
        packet.flags.depth_write = true;
    }
    value.packets[1].material_profile.shadow_alpha_tested = true;
    for (PipelineProgram* pipeline :
         std::array<PipelineProgram*, 2U>{&value.first_pipeline,
                                          &value.second_pipeline}) {
        pipeline->targets.has_depth = true;
        pipeline->targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
        pipeline->depth.test_enabled = true;
        pipeline->depth.write_enabled = true;
        pipeline->depth.compare = PipelineCompareOperation::less;
    }
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    require(prepared.ok(),
            "directional shadow fixture retains its scene geometry: " +
                prepared.diagnostic.code + " " + prepared.diagnostic.message);

    DirectionalShadowMapRequest map_request;
    map_request.lighting.map_size = 32U;
    const auto maps = prepare_directional_shadow_maps(device, map_request);
    require(maps.ok() && maps.resources->map_size() == 32U &&
                maps.resources->metadata().cascades.size() ==
                    directional_shadow_cascade_count &&
                device.depth_attachment_calls == directional_shadow_cascade_count &&
                device.live_depth_attachments == directional_shadow_cascade_count &&
                maps.resources->attachment(0U).info().description.shader_readable,
            "directional shadow preparation owns exactly three bounded D32 maps");

    PipelineProgram depth_pipeline = value.first_pipeline;
    depth_pipeline.name = "portable-opaque-directional-caster";
    depth_pipeline.targets.colors.clear();
    depth_pipeline.shaders.resize(1U);
    StaticSceneDirectionalShadowFrameDescription frame;
    frame.maps = maps.resources.get();
    frame.opaque_pipeline = &depth_pipeline;
    const auto drawn =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(drawn.ok() &&
                drawn.status == StaticSceneDirectionalShadowStatus::partial &&
                drawn.selected_casters == 3U && drawn.opaque_casters == 2U &&
                drawn.staged_alpha_tested == 1U &&
                drawn.cascades_completed == directional_shadow_cascade_count &&
                device.depth_batch_calls == directional_shadow_cascade_count &&
                device.depth_nodes ==
                    std::vector<std::vector<apex::scene::NodeId>>(
                        directional_shadow_cascade_count, {1U, 1U}),
            "three cascades preserve retained opaque order and stage alpha casters");
    require(device.depth_camera_matrices.size() == 6U &&
                device.depth_camera_matrices[0] == device.depth_camera_matrices[1] &&
                device.depth_camera_matrices[2] == device.depth_camera_matrices[3] &&
                device.depth_camera_matrices[4] == device.depth_camera_matrices[5] &&
                device.depth_camera_matrices[0] != device.depth_camera_matrices[2],
            "each pair of retained casters receives one converted cascade matrix");

    require(device.depth_culls.size() == 6U &&
                std::all_of(device.depth_culls.begin(), device.depth_culls.end(),
                            [](PipelineCullMode cull) {
                                return cull == PipelineCullMode::back;
                            }),
            "stock-default opaque casters use back-face shadow culling");

    const std::array<std::uint8_t, 3U> hide_all_shadow_packets = {0U, 0U, 0U};
    frame.packet_visibility = hide_all_shadow_packets;
    const std::size_t shadow_calls_before_clear_only = device.depth_batch_calls;
    const auto clear_only_shadows =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(clear_only_shadows.ok() && clear_only_shadows.selected_casters == 0U &&
                clear_only_shadows.cascades_completed ==
                    directional_shadow_cascade_count &&
                device.depth_batch_calls ==
                    shadow_calls_before_clear_only + directional_shadow_cascade_count &&
                device.depth_nodes.back().empty(),
            "all-hidden shadow frame clears each retained cascade without draws");
    frame.packet_visibility = {};

    const std::array<std::uint8_t, 3U> show_only_alpha_shadow_packet = {
        0U, 1U, 0U};
    frame.packet_visibility = show_only_alpha_shadow_packet;
    const std::size_t shadow_calls_before_staged_only = device.depth_batch_calls;
    const auto staged_only_shadows =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(staged_only_shadows.ok() &&
                staged_only_shadows.status ==
                    StaticSceneDirectionalShadowStatus::partial &&
                staged_only_shadows.diagnostic.code ==
                    "directional_shadow_all_casters_staged" &&
                staged_only_shadows.selected_casters == 1U &&
                staged_only_shadows.staged_alpha_tested == 1U &&
                staged_only_shadows.cascades_completed ==
                    directional_shadow_cascade_count &&
                device.depth_batch_calls ==
                    shadow_calls_before_staged_only +
                        directional_shadow_cascade_count &&
                device.depth_nodes.back().empty(),
            "an all-staged alpha frame clears every cascade instead of retaining stale depth");
    frame.packet_visibility = {};

    value.packets[0].flags.double_face_shadow = true;
    RecordingDevice double_face_device;
    auto double_face_prepared =
        prepare_static_scene_resources(double_face_device, request_for(value));
    require(double_face_prepared.ok(),
            "double-face shadow fixture retains its scene geometry");
    auto double_face_maps =
        prepare_directional_shadow_maps(double_face_device, map_request);
    require(double_face_maps.ok(),
            "double-face shadow fixture retains its three maps");
    StaticSceneDirectionalShadowFrameDescription double_face_frame{
        double_face_maps.resources.get(), &depth_pipeline, {}};
    const auto double_face_drawn =
        double_face_prepared.resources->draw_opaque_directional_shadows(
            double_face_device, double_face_frame);
    require(double_face_drawn.ok() && double_face_device.depth_culls.size() == 6U,
            "double-face opaque shadow fixture executes all cascades");
    for (std::size_t index = 0U; index < double_face_device.depth_culls.size();
         ++index) {
        const PipelineCullMode expected = index % 2U == 0U
                                              ? PipelineCullMode::none
                                              : PipelineCullMode::back;
        require(double_face_device.depth_culls[index] == expected,
                "doubleFaceShadow selects no cull only for the opted-in caster");
    }
    value.packets[0].flags.double_face_shadow = false;

    Fixture mixed = fixture();
    make_second_mesh_skinned(mixed);
    for (DrawPacket& packet : mixed.packets) {
        packet.flags.cast_shadows = true;
        packet.flags.depth_test = true;
        packet.flags.depth_write = true;
    }
    mixed.packets[1].material_profile.shadow_alpha_tested = true;
    for (PipelineProgram* pipeline :
         std::array<PipelineProgram*, 2U>{&mixed.first_pipeline,
                                          &mixed.second_pipeline}) {
        pipeline->targets.has_depth = true;
        pipeline->targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
        pipeline->depth.test_enabled = true;
        pipeline->depth.write_enabled = true;
        pipeline->depth.compare = PipelineCompareOperation::less;
    }
    RecordingDevice mixed_device;
    auto mixed_prepared =
        prepare_static_scene_resources(mixed_device, request_for(mixed));
    auto mixed_maps = prepare_directional_shadow_maps(mixed_device, map_request);
    require(mixed_prepared.ok() && mixed_maps.ok(),
            "mixed shadow fixture retains scene geometry and maps");
    PipelineProgram mixed_depth_pipeline = mixed.first_pipeline;
    mixed_depth_pipeline.targets.colors.clear();
    mixed_depth_pipeline.shaders.resize(1U);
    StaticSceneDirectionalShadowFrameDescription mixed_frame{
        mixed_maps.resources.get(), &mixed_depth_pipeline, {}};
    const auto mixed_drawn = mixed_prepared.resources->draw_opaque_directional_shadows(
        mixed_device, mixed_frame);
    require(mixed_drawn.ok() &&
                mixed_drawn.opaque_casters == 2U &&
                mixed_drawn.staged_skinned == 1U &&
                mixed_drawn.cascades_completed == directional_shadow_cascade_count,
            "non-opaque skinned casters use the skinned staging branch while static casters execute");

    PipelineProgram mixed_skinned_depth_pipeline = mixed.second_pipeline;
    mixed_skinned_depth_pipeline.name = "portable-skinned-directional-caster";
    mixed_skinned_depth_pipeline.targets.colors.clear();
    mixed_skinned_depth_pipeline.shaders.resize(1U);
    mixed_frame.skinned_pipeline = &mixed_skinned_depth_pipeline;
    const std::size_t skin_updates_before_shadow = mixed_device.update_calls;
    const auto skinned_drawn =
        mixed_prepared.resources->draw_opaque_directional_shadows(
            mixed_device, mixed_frame);
    require(skinned_drawn.ok() &&
                skinned_drawn.status == StaticSceneDirectionalShadowStatus::ready &&
                skinned_drawn.opaque_casters == 2U &&
                skinned_drawn.skinned_casters == 1U &&
                skinned_drawn.staged_skinned == 0U &&
                skinned_drawn.cascades_completed == directional_shadow_cascade_count &&
                mixed_device.update_calls == skin_updates_before_shadow + 1U &&
                mixed_device.depth_nodes.back() ==
                    std::vector<apex::scene::NodeId>({1U, 2U, 1U}),
            "explicit skinned shadow pipeline takes precedence over non-opaque material state");

    const std::array<std::uint8_t, 3U> hide_shadow_skin = {1U, 0U, 1U};
    mixed_frame.packet_visibility = hide_shadow_skin;
    const std::size_t updates_before_hidden_shadow_skin = mixed_device.update_calls;
    const auto hidden_shadow_skin =
        mixed_prepared.resources->draw_opaque_directional_shadows(
            mixed_device, mixed_frame);
    require(hidden_shadow_skin.ok() && hidden_shadow_skin.opaque_casters == 2U &&
                hidden_shadow_skin.skinned_casters == 0U &&
                mixed_device.update_calls == updates_before_hidden_shadow_skin &&
                mixed_device.depth_nodes.back() ==
                    std::vector<apex::scene::NodeId>({1U, 1U}),
            "hidden skinned shadow packet skips pose upload and every cascade");

    const std::array<std::uint8_t, 3U> show_only_shadow_skin = {0U, 1U, 0U};
    mixed_frame.packet_visibility = show_only_shadow_skin;
    const auto only_shadow_skin =
        mixed_prepared.resources->draw_opaque_directional_shadows(
            mixed_device, mixed_frame);
    require(only_shadow_skin.ok() && only_shadow_skin.opaque_casters == 0U &&
                only_shadow_skin.skinned_casters == 1U &&
                mixed_device.update_calls == updates_before_hidden_shadow_skin + 1U &&
                mixed_device.depth_nodes.back() ==
                    std::vector<apex::scene::NodeId>({2U}),
            "visible skinned shadow packet executes without hidden static casters");
    mixed_frame.packet_visibility = {};

    const std::array<std::uint8_t, 1U> short_shadow_visibility = {1U};
    frame.packet_visibility = short_shadow_visibility;
    const std::size_t shadow_calls_before_invalid_mask = device.depth_batch_calls;
    const auto invalid_shadow_visibility =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(invalid_shadow_visibility.status ==
                StaticSceneDirectionalShadowStatus::invalid_request &&
                invalid_shadow_visibility.diagnostic.code ==
                    "directional_shadow_visibility_mask_count_invalid" &&
                device.depth_batch_calls == shadow_calls_before_invalid_mask,
            "short shadow visibility mask fails before any cascade write");

    const std::array<std::uint8_t, 3U> invalid_shadow_visibility_values = {
        1U, 3U, 1U};
    frame.packet_visibility = invalid_shadow_visibility_values;
    const auto invalid_shadow_visibility_value =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(invalid_shadow_visibility_value.status ==
                StaticSceneDirectionalShadowStatus::invalid_request &&
                invalid_shadow_visibility_value.diagnostic.code ==
                    "directional_shadow_visibility_mask_value_invalid" &&
                device.depth_batch_calls == shadow_calls_before_invalid_mask,
            "non-binary shadow visibility fails before any cascade write");
    frame.packet_visibility = {};

    std::vector<DrawPacket> malformed = value.packets;
    malformed.back().world_matrix[0] = std::numeric_limits<float>::quiet_NaN();
    frame.refreshed_packets = malformed;
    const std::size_t calls_before_invalid = device.depth_batch_calls;
    const auto invalid =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(invalid.status == StaticSceneDirectionalShadowStatus::invalid_request &&
                invalid.diagnostic.code ==
                    "directional_shadow_packet_contract_invalid" &&
                device.depth_batch_calls == calls_before_invalid,
            "malformed refreshed shadow input fails before any cascade write");

    malformed = value.packets;
    malformed[0].flags.double_face_shadow = true;
    frame.refreshed_packets = malformed;
    const auto changed_shadow_cull =
        prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(changed_shadow_cull.status ==
                    StaticSceneDirectionalShadowStatus::invalid_request &&
                changed_shadow_cull.diagnostic.code ==
                    "directional_shadow_packet_contract_invalid" &&
                device.depth_batch_calls == calls_before_invalid,
            "refreshed shadow cull state cannot change after preparation");

    DirectionalShadowInput refreshed_lighting = map_request.lighting;
    refreshed_lighting.eye = {1.0F, 0.0F, 5.0F};
    refreshed_lighting.target = {0.0F, 0.0F, 0.0F};
    refreshed_lighting.far_plane = 100.0F;
    const std::array<const DepthAttachment*, directional_shadow_cascade_count>
        retained_attachments = {
            &maps.resources->attachment(0U), &maps.resources->attachment(1U),
            &maps.resources->attachment(2U)};
    const auto initial_matrix = maps.resources->metadata().cascades[0U].matrix;
    const auto refreshed_maps = refresh_directional_shadow_maps(
        *maps.resources, refreshed_lighting);
    require(refreshed_maps.ok() &&
                maps.resources->metadata().cascades[0U].matrix != initial_matrix &&
                &maps.resources->attachment(0U) == retained_attachments[0U] &&
                &maps.resources->attachment(1U) == retained_attachments[1U] &&
                &maps.resources->attachment(2U) == retained_attachments[2U] &&
                device.depth_attachment_calls == directional_shadow_cascade_count,
            "camera refresh reuses all three retained map allocations");

    const auto stable_matrix = maps.resources->metadata().cascades[0U].matrix;
    const auto stable_forward = maps.resources->metadata().forward;
    refreshed_lighting.eye[0U] = std::numeric_limits<float>::quiet_NaN();
    const auto invalid_refresh = refresh_directional_shadow_maps(
        *maps.resources, refreshed_lighting);
    require(!invalid_refresh.ok() &&
                invalid_refresh.status ==
                    DirectionalShadowMapStatus::invalid_request &&
                invalid_refresh.diagnostic.code ==
                    "directional_shadow_refresh_camera_invalid" &&
                maps.resources->metadata().cascades[0U].matrix == stable_matrix &&
                maps.resources->metadata().forward == stable_forward &&
                device.depth_attachment_calls == directional_shadow_cascade_count,
            "malformed camera refresh leaves retained cascade state unchanged");

    DirectionalShadowMapRequest oversized = map_request;
    oversized.lighting.map_size = max_directional_shadow_map_size + 1U;
    const auto rejected = prepare_directional_shadow_maps(device, oversized);
    require(!rejected.ok() &&
                rejected.status == DirectionalShadowMapStatus::invalid_request &&
                device.depth_attachment_calls == directional_shadow_cascade_count,
            "oversized three-map input fails before backend allocation");

    DirectionalShadowMapRequest malformed_maps = map_request;
    malformed_maps.lighting.sun_direction[0U] =
        std::numeric_limits<float>::quiet_NaN();
    const auto malformed_map_result = prepare_directional_shadow_maps(
        device, malformed_maps);
    require(!malformed_map_result.ok() &&
                malformed_map_result.status ==
                    DirectionalShadowMapStatus::invalid_request &&
                malformed_map_result.diagnostic.code ==
                    "directional_shadow_input_invalid" &&
                device.depth_attachment_calls == directional_shadow_cascade_count,
            "malformed shadow lighting fails before backend allocation");

    RecordingDevice failing_device;
    failing_device.fail_depth_attachment_call = 2U;
    const auto failed_maps =
        prepare_directional_shadow_maps(failing_device, map_request);
    require(!failed_maps.ok() &&
                failed_maps.status == DirectionalShadowMapStatus::allocation_failed &&
                failing_device.depth_attachment_calls == 2U &&
                failing_device.live_depth_attachments == 0U,
            "a later map allocation failure releases the earlier map");
    require(std::string(directional_shadow_map_status_name(
                       DirectionalShadowMapStatus::ready)) == "ready" &&
                std::string(static_scene_directional_shadow_status_name(
                    StaticSceneDirectionalShadowStatus::partial)) == "partial",
            "directional shadow status names are stable");
}

void executes_explicit_alpha_directional_casters_with_owned_constants() {
    Fixture value = fixture();
    for (DrawPacket& packet : value.packets) {
        packet.flags.cast_shadows = true;
        packet.flags.depth_test = true;
        packet.flags.depth_write = true;
    }
    value.packets[2].flags.cast_shadows = false;
    value.packets[1].material_profile.shadow_alpha_tested = true;
    value.packets[1].flags.transparent = true;
    value.packets[1].flags.blend_enabled = true;
    value.packets[1].flags.depth_write = false;
    value.second_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "txDiffuse"},
        {PipelineResourceKind::sampler, 0U, 1U, "sampDiffuse"}};
    for (PipelineProgram* pipeline :
         std::array<PipelineProgram*, 2U>{&value.first_pipeline,
                                          &value.second_pipeline}) {
        pipeline->targets.has_depth = true;
        pipeline->targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
        pipeline->depth.test_enabled = true;
        pipeline->depth.write_enabled = true;
        pipeline->depth.compare = PipelineCompareOperation::less;
    }
    value.second_pipeline.blend.enabled = true;
    value.second_pipeline.blend.source_color = PipelineBlendFactor::source_alpha;
    value.second_pipeline.blend.destination_color =
        PipelineBlendFactor::one_minus_source_alpha;
    value.second_pipeline.blend.source_alpha = PipelineBlendFactor::source_alpha;
    value.second_pipeline.blend.destination_alpha =
        PipelineBlendFactor::one_minus_source_alpha;
    value.second_pipeline.depth.write_enabled = false;
    value.model.textures.push_back({true, "alpha.dds", 4U, {}, {}});
    value.packets[1].resources = {{"txDiffuse", 0U, 0U, "alpha.dds"}};

    PipelineProgram alpha_pipeline = value.second_pipeline;
    alpha_pipeline.name = "portable-alpha-directional-caster";
    alpha_pipeline.targets.colors.clear();
    alpha_pipeline.targets.has_depth = true;
    alpha_pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    alpha_pipeline.depth.test_enabled = true;
    alpha_pipeline.depth.write_enabled = true;
    alpha_pipeline.depth.compare = PipelineCompareOperation::less;
    alpha_pipeline.blend = {};
    alpha_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "txDiffuse"},
        {PipelineResourceKind::sampler, 0U, 3U, "samLinearShadow"},
        {PipelineResourceKind::uniform_buffer, 0U, 4U, "ksShadowCasterMaterial"},
    };
    std::array<StockShadowCasterMaterialConstants, 2U> shadow_constants{};
    shadow_constants[1].emissive_and_alpha_ref[3] = 0.5F;

    const std::array<StockShadowCasterMaterialConstants, 1U>
        short_shadow_constants{};
    RecordingDevice malformed_device;
    auto malformed_request = request_for(value);
    malformed_request.stock_shadow_constants_by_material =
        short_shadow_constants;
    const auto malformed_table = prepare_static_scene_resources(
        malformed_device, malformed_request);
    require(!malformed_table.ok() &&
                malformed_table.diagnostic.code ==
                    "static_scene_shadow_material_table_invalid" &&
                malformed_device.buffer_calls == 0U,
            "a truncated stock shadow material table fails before allocation");

    auto invalid_shadow_constants = shadow_constants;
    invalid_shadow_constants[1].emissive_and_alpha_ref[3] = 1.01F;
    malformed_request = request_for(value);
    malformed_request.stock_shadow_constants_by_material =
        invalid_shadow_constants;
    const auto invalid_alpha = prepare_static_scene_resources(
        malformed_device, malformed_request);
    require(!invalid_alpha.ok() &&
                invalid_alpha.diagnostic.code ==
                    "static_scene_shadow_material_constant_invalid" &&
                malformed_device.buffer_calls == 0U,
            "an out-of-range stock shadow alpha value fails before allocation");

    malformed_request = request_for(value);
    malformed_request.stock_shadow_constants_by_material = shadow_constants;
    malformed_request.limits.max_total_material_constant_bytes =
        stock_shadow_caster_buffer_alignment - 1U;
    const auto truncated_budget = prepare_static_scene_resources(
        malformed_device, malformed_request);
    require(!truncated_budget.ok() &&
                truncated_budget.diagnostic.code ==
                    "static_scene_shadow_material_constant_aggregate_limit" &&
                malformed_device.buffer_calls == 0U,
            "a truncated stock shadow buffer budget fails before allocation");

    RecordingDevice device;
    auto request = request_for(value);
    request.stock_shadow_constants_by_material = shadow_constants;
    auto prepared = prepare_static_scene_resources(device, request);
    require(prepared.ok() && prepared.resources->owned_material_constant_count() == 0U,
            "alpha shadow preparation succeeds without inventing portable color constants");

    DirectionalShadowMapRequest map_request;
    map_request.lighting.map_size = 32U;
    const auto maps = prepare_directional_shadow_maps(device, map_request);
    require(maps.ok(), "alpha shadow fixture retains its three maps");
    const TextureDescription texture_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable, 1U};
    FakeTexture alpha_texture(texture_description);
    FakeSampler alpha_sampler;
    const std::array<const Texture*, 1U> textures = {&alpha_texture};
    const std::array<const Sampler*, 1U> samplers = {&alpha_sampler};
    const std::array<std::uint8_t, 3U> only_alpha = {0U, 1U, 0U};
    StaticSceneDirectionalShadowFrameDescription frame;
    frame.maps = maps.resources.get();
    frame.alpha_static_pipeline = &alpha_pipeline;
    frame.packet_visibility = only_alpha;
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    const auto drawn = prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(drawn.ok() && drawn.status == StaticSceneDirectionalShadowStatus::ready &&
                drawn.selected_casters == 1U && drawn.opaque_casters == 0U &&
                drawn.alpha_tested_casters == 1U && drawn.staged_alpha_tested == 0U &&
                drawn.cascades_completed == directional_shadow_cascade_count &&
                device.depth_nodes.size() == directional_shadow_cascade_count &&
                std::all_of(device.depth_nodes.begin(), device.depth_nodes.end(),
                            [](const auto& nodes) {
                                return nodes ==
                                       std::vector<apex::scene::NodeId>({2U});
                            }),
            "alpha-only shadow frames execute with no opaque pipeline");
    require(device.depth_material_modes.size() == directional_shadow_cascade_count &&
                std::all_of(device.depth_material_modes.begin(),
                            device.depth_material_modes.end(),
                            [](auto mode) {
                                return mode == DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::stock_alpha_tested;
                            }) &&
                device.depth_alpha_textures.front() == &alpha_texture &&
                device.depth_alpha_samplers.front() == &alpha_sampler &&
                device.depth_alpha_material_buffers.front() != nullptr &&
                device.depth_alpha_material_ranges.front() ==
                    stock_shadow_caster_material_bytes &&
                std::all_of(device.depth_transparent_flags.begin(),
                            device.depth_transparent_flags.end(),
                            [](bool enabled) { return !enabled; }) &&
                std::all_of(device.depth_blend_flags.begin(),
                            device.depth_blend_flags.end(),
                            [](bool enabled) { return !enabled; }) &&
                std::all_of(device.depth_alpha_to_coverage_flags.begin(),
                            device.depth_alpha_to_coverage_flags.end(),
                            [](bool enabled) { return !enabled; }) &&
                std::all_of(device.depth_test_flags.begin(),
                            device.depth_test_flags.end(),
                            [](bool enabled) { return enabled; }) &&
                std::all_of(device.depth_write_flags.begin(),
                            device.depth_write_flags.end(),
                            [](bool enabled) { return enabled; }),
            "alpha-blended shadow draws use recovered opaque depth-pass state with explicit resources");

    frame.alpha_static_pipeline = nullptr;
    const std::size_t calls_before_staged = device.depth_batch_calls;
    const auto staged = prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(staged.ok() && staged.status == StaticSceneDirectionalShadowStatus::partial &&
                staged.staged_alpha_tested == 1U &&
                device.depth_batch_calls == calls_before_staged + directional_shadow_cascade_count &&
                device.depth_nodes.back().empty(),
            "missing alpha pipeline stages the caster and clears every cascade");

    const std::array<std::uint8_t, 3U> only_opaque = {1U, 0U, 0U};
    frame.packet_visibility = only_opaque;
    const auto missing_opaque = prepared.resources->draw_opaque_directional_shadows(device, frame);
    require(missing_opaque.ok() && missing_opaque.status == StaticSceneDirectionalShadowStatus::partial &&
                missing_opaque.selected_casters == 1U && missing_opaque.opaque_casters == 0U &&
                missing_opaque.staged_casters.size() == 1U &&
                missing_opaque.staged_casters.front().code ==
                    "directional_shadow_caster_shader_staged",
            "missing opaque pipeline stages opaque casters without dereferencing it");
}

void binds_retained_directional_maps_through_static_scene_frames() {
    const auto configure_receiver = [](Fixture& value) {
        value.model.textures.push_back(
            {true, "body.dds", 4U, {1U, 2U, 3U, 4U}, {}});
        value.first_pipeline.resources = {
            {PipelineResourceKind::sampled_texture, 0U, 0U,
             "diffuseTexture"},
            {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
            {PipelineResourceKind::sampled_texture, 0U, 16U, "txShadow0"},
            {PipelineResourceKind::sampled_texture, 0U, 17U, "txShadow1"},
            {PipelineResourceKind::sampled_texture, 0U, 18U, "txShadow2"},
            {PipelineResourceKind::sampler, 0U, 19U, "shadowSampler"},
            {PipelineResourceKind::uniform_buffer, 0U, 20U,
             "shadowReceiver"},
        };
        value.packets[0].resources = {
            {"txDiffuse", 21U, 0U, "body.dds"}};
        value.packets[2].resources = value.packets[0].resources;
    };
    const auto make_maps = [](RecordingDevice& device) {
        DirectionalShadowMapRequest request;
        request.lighting.eye = {0.0F, 0.0F, 0.0F};
        request.lighting.target = {0.0F, 0.0F, -1.0F};
        request.lighting.map_size = 32U;
        return prepare_directional_shadow_maps(device, request);
    };

    Fixture value = fixture();
    configure_receiver(value);
    RecordingDevice device;
    auto prepared = prepare_static_scene_resources(device, request_for(value));
    auto maps = make_maps(device);
    require(prepared.ok() && maps.ok() &&
                prepared.resources->owns_directional_shadow_receiver() &&
                device.buffer_calls == 5U && device.sampler_calls == 1U,
            "receiver preparation owns one bounded record and nearest sampler");
    require(device.sampler_descriptions.front().min_filter ==
                    SamplerFilter::nearest &&
                device.sampler_descriptions.front().mag_filter ==
                    SamplerFilter::nearest &&
                device.sampler_descriptions.front().address_u ==
                    SamplerAddressMode::clamp_to_edge &&
                device.sampler_descriptions.front().address_v ==
                    SamplerAddressMode::clamp_to_edge &&
                device.sampler_descriptions.front().compare ==
                    SamplerCompare::disabled,
            "owned receiver sampler preserves explicit PCF state");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
        TextureMemory::device_local, TextureMutability::immutable};
    FakeTexture diffuse(sampled_description);
    FakeSampler diffuse_sampler;
    const std::array<const Texture*, 1> textures = {&diffuse};
    const std::array<const Sampler*, 1> samplers = {&diffuse_sampler};
    FakeTexture target;
    StaticSceneFrameDescription frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.position = {0.0F, 0.0F, 0.0F};
    frame.camera.forward = {0.0F, 0.0F, -1.0F};
    frame.textures_by_global_index = textures;
    frame.samplers_by_global_index = samplers;
    frame.directional_shadow_maps = maps.resources.get();
    const auto drawn = prepared.resources->draw_and_readback(
        device, target, frame);
    require(drawn.ok() && device.update_calls == 1U &&
                device.batch_calls == 1U &&
                device.directional_shadow_maps.size() == 3U,
            "retained receiver scene executes one preflighted ordered batch");
    for (const std::size_t packet_index : {0U, 2U}) {
        for (std::size_t cascade = 0U;
             cascade < directional_shadow_cascade_count; ++cascade)
            require(device.directional_shadow_maps[packet_index][cascade] ==
                        &maps.resources->attachment(cascade),
                    "receiver draw binds each retained cascade in order");
        require(device.directional_shadow_samplers[packet_index] != nullptr &&
                    device.directional_shadow_buffers[packet_index] != nullptr &&
                    device.directional_shadow_ranges[packet_index] ==
                        portable_directional_shadow_buffer_view_bytes,
                "receiver draw binds the owned sampler and complete constants view");
    }
    require(std::all_of(device.directional_shadow_maps[1U].begin(),
                        device.directional_shadow_maps[1U].end(),
                        [](const DepthAttachment* map) { return map == nullptr; }) &&
                device.directional_shadow_samplers[1U] == nullptr &&
                device.directional_shadow_buffers[1U] == nullptr,
            "ordinary mixed-scene draws do not receive shadow resources");
    require(device.updated_bytes.front().size() ==
                portable_directional_shadow_buffer_view_bytes,
            "receiver update writes one complete bounded record");
    DirectionalShadowReceiverConstants constants;
    std::memcpy(&constants, device.updated_bytes.front().data(),
                sizeof(constants));
    require(constants.shadow_matrices[0] == maps.resources->camera(0U).view_projection &&
                constants.split_distances[0] == 2.0F &&
                constants.split_distances[1] == 12.0F &&
                constants.split_distances[2] == 50.0F &&
                constants.depth_biases[0] == ks_shadow_biases[0] &&
                constants.depth_biases[1] == ks_shadow_biases[1] &&
                constants.depth_biases[2] == ks_shadow_biases[2] &&
                constants.camera_position[0] == 0.0F &&
                constants.camera_forward[2] == -1.0F,
            "receiver record carries matrices, recovered splits and biases, and camera state");

    frame.directional_shadow_constants_layout =
        DirectionalShadowReceiverConstantsLayout::stock_ks_shadow_maps;
    const auto stock_layout_draw = prepared.resources->draw_and_readback(
        device, target, frame);
    StockDirectionalShadowReceiverConstants stock_constants;
    std::memcpy(&stock_constants, device.updated_bytes.back().data(),
                sizeof(stock_constants));
    require(stock_layout_draw.ok() &&
                device.directional_shadow_ranges[0U] ==
                    stock_directional_shadow_buffer_view_bytes &&
                stock_constants.shadow_matrices[0] ==
                    maps.resources->camera(0U).view_projection &&
                stock_constants.biases[0] == ks_shadow_biases[0] &&
                stock_constants.biases[1] == ks_shadow_biases[1] &&
                stock_constants.biases[2] == ks_shadow_biases[2] &&
                stock_constants.texture_size == 1.0F / 32.0F &&
                            std::all_of(device.updated_bytes.back().begin() +
                                stock_directional_shadow_buffer_view_bytes,
                            device.updated_bytes.back().end(),
                            [](std::byte byte_value) {
                                return byte_value == std::byte{0};
                            }),
            "explicit stock receiver layout preserves the recovered 208-byte packing and reciprocal map width");

    const std::size_t updates_before_invalid_layout = device.update_calls;
    const std::size_t batches_before_invalid_layout = device.batch_calls;
    frame.directional_shadow_constants_layout =
        static_cast<DirectionalShadowReceiverConstantsLayout>(255U);
    const auto invalid_layout = prepared.resources->draw_and_readback(
        device, target, frame);
    require(invalid_layout.status == IndexedStaticMeshBatchStatus::invalid_request &&
                invalid_layout.diagnostic.code ==
                    "static_scene_directional_shadow_constants_layout_invalid" &&
                device.update_calls == updates_before_invalid_layout &&
                device.batch_calls == batches_before_invalid_layout,
            "unknown receiver ABI values fail before mutable updates or submission");
    frame.directional_shadow_constants_layout =
        DirectionalShadowReceiverConstantsLayout::portable;

    const std::size_t updates_before_missing = device.update_calls;
    const std::size_t batches_before_missing = device.batch_calls;
    frame.directional_shadow_maps = nullptr;
    const auto missing = prepared.resources->draw_and_readback(
        device, target, frame);
    require(missing.status == IndexedStaticMeshBatchStatus::invalid_request &&
                missing.diagnostic.code ==
                    "static_scene_directional_shadow_maps_missing" &&
                device.update_calls == updates_before_missing &&
                device.batch_calls == batches_before_missing,
            "missing retained maps fail before constants updates or submission");

    frame.directional_shadow_maps = maps.resources.get();
    frame.camera.forward = {1.0F, 0.0F, 0.0F};
    const auto mismatched_camera = prepared.resources->draw_and_readback(
        device, target, frame);
    require(mismatched_camera.status ==
                    IndexedStaticMeshBatchStatus::invalid_request &&
                mismatched_camera.diagnostic.code ==
                    "static_scene_directional_shadow_camera_mismatch" &&
                device.update_calls == updates_before_missing &&
                device.batch_calls == batches_before_missing,
            "stale receiver camera state fails atomically");

    Fixture ordinary = fixture();
    RecordingDevice ordinary_device;
    auto ordinary_prepared = prepare_static_scene_resources(
        ordinary_device, request_for(ordinary));
    auto ordinary_maps = make_maps(ordinary_device);
    StaticSceneFrameDescription unexpected_frame;
    unexpected_frame.camera.clip_space = CameraClipSpace::vulkan;
    unexpected_frame.directional_shadow_maps = ordinary_maps.resources.get();
    const auto unexpected = ordinary_prepared.resources->draw_and_readback(
        ordinary_device, target, unexpected_frame);
    require(unexpected.status == IndexedStaticMeshBatchStatus::invalid_request &&
                unexpected.diagnostic.code ==
                    "static_scene_directional_shadow_maps_unexpected" &&
                ordinary_device.update_calls == 0U &&
                ordinary_device.batch_calls == 0U,
            "maps supplied to a nonreceiver scene are not silently ignored");

    RecordingDevice other_device;
    auto other_maps = make_maps(other_device);
    frame.camera.forward = {0.0F, 0.0F, -1.0F};
    frame.directional_shadow_maps = other_maps.resources.get();
    const auto cross_device = prepared.resources->draw_and_readback(
        device, target, frame);
    require(cross_device.status == IndexedStaticMeshBatchStatus::unsupported &&
                cross_device.diagnostic.code ==
                    "static_scene_directional_shadow_device_mismatch" &&
                device.update_calls == updates_before_missing &&
                device.batch_calls == batches_before_missing,
            "cross-device receiver maps fail before mutable state changes");

    Fixture limited = fixture();
    configure_receiver(limited);
    auto limited_request = request_for(limited);
    limited_request.limits.max_total_directional_shadow_constant_bytes =
        portable_directional_shadow_buffer_view_bytes - 1U;
    RecordingDevice limited_device;
    const auto limited_result = prepare_static_scene_resources(
        limited_device, limited_request);
    require(!limited_result.ok() &&
                limited_result.diagnostic.code ==
                    "static_scene_directional_shadow_constant_aggregate_limit" &&
                limited_device.buffer_calls == 0U &&
                limited_device.sampler_calls == 0U,
            "truncated-equivalent receiver budget fails before allocation");

    Fixture retry = fixture();
    configure_receiver(retry);
    RecordingDevice retry_device;
    auto retry_prepared = prepare_static_scene_resources(
        retry_device, request_for(retry));
    auto retry_maps = make_maps(retry_device);
    StaticSceneFrameDescription retry_frame = frame;
    retry_frame.directional_shadow_maps = retry_maps.resources.get();
    retry_device.fail_update_call = 1U;
    const auto failed_update = retry_prepared.resources->draw_and_readback(
        retry_device, target, retry_frame);
    require(failed_update.status ==
                    IndexedStaticMeshBatchStatus::execution_failed &&
                failed_update.diagnostic.code == "recording_update_failure" &&
                retry_device.update_calls == 1U &&
                retry_device.batch_calls == 0U,
            "receiver constant upload failure prevents batch submission");
    retry_device.fail_update_call = 0U;
    const auto retried = retry_prepared.resources->draw_and_readback(
        retry_device, target, retry_frame);
    require(retried.ok() && retry_device.update_calls == 2U &&
                retry_device.batch_calls == 1U,
            "complete receiver frame retries after an upload failure");
}

}  // namespace

int main() {
    try {
        prepares_deduplicated_resources_and_executes_one_ordered_batch();
        suppresses_shadow_only_packets_from_color_submission();
        schedules_selected_and_view_axis_at_transparent_boundary();
        prepares_mixed_static_and_skinned_scene_and_updates_only_after_preflight();
        prepares_source_evidenced_wireframe_batch_state();
        rejects_invalid_late_inputs_before_backend_allocation();
        validates_alpha_to_coverage_sample_contract_before_allocation();
        resolves_portable_diffuse_tables_without_owning_handles();
        owns_embedded_diffuse_resources_after_source_lifetime();
        owns_signature_identified_png_resources();
        rejects_malformed_embedded_textures_before_allocation();
        rejects_malformed_diffuse_packets_before_allocation();
        bounds_host_preparation_metadata_before_allocation();
        owns_and_reuses_material_constant_records();
        owns_and_updates_frame_constant_record_for_mixed_packets();
        resolves_bounded_normal_map_resources_and_rejects_malformed_packets();
        resolves_txmaps_resources_with_bounded_ownership_and_preflight();
        resolves_detail_stack_resources_with_aliases_and_preflight();
        orders_skin_updates_before_the_shared_frame_record();
        rejects_frame_constant_budget_before_allocation();
        rejects_invalid_material_constant_inputs_before_allocation();
        propagates_embedded_resource_failures();
        propagates_upload_and_batch_failures();
        retains_three_directional_maps_and_executes_only_opaque_static_casters();
        executes_explicit_alpha_directional_casters_with_owned_constants();
        binds_retained_directional_maps_through_static_scene_frames();
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
