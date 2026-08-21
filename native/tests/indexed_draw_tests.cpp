#include "apex/render/draw_packet.hpp"
#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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
    FakeBuffer(Backend backend, BufferDescription description) : backend_(backend), info_({description}) {}

    Backend backend() const noexcept override { return backend_; }
    const BufferInfo& info() const noexcept override { return info_; }

private:
    Backend backend_;
    BufferInfo info_;
};

class FakeTexture final : public Texture {
public:
    FakeTexture(Backend backend, TextureDescription description) : backend_(backend), info_({description}) {}

    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }

private:
    Backend backend_;
    TextureInfo info_;
};

class FakeSampler final : public Sampler {
public:
    FakeSampler(Backend backend, SamplerDescription description = {})
        : backend_(backend), info_({description}) {}

    Backend backend() const noexcept override { return backend_; }
    const SamplerInfo& info() const noexcept override { return info_; }

private:
    Backend backend_;
    SamplerInfo info_;
};

class FakeDepthAttachment final : public DepthAttachment {
public:
    FakeDepthAttachment(Backend backend, DepthAttachmentDescription description)
        : backend_(backend), info_({description}) {}

    Backend backend() const noexcept override { return backend_; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }

private:
    Backend backend_;
    DepthAttachmentInfo info_;
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
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::uint32_t word = words[index];
        bytes[index * 4U] = static_cast<std::uint8_t>(word & 0xffU);
        bytes[index * 4U + 1U] = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        bytes[index * 4U + 2U] = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        bytes[index * 4U + 3U] = static_cast<std::uint8_t>((word >> 24U) & 0xffU);
    }
    return bytes;
}

PipelineProgram pipeline_fixture() {
    PipelineProgram pipeline;
    pipeline.name = "indexed-contract";
    pipeline.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    pipeline.vertex_layout.stride = 11U * sizeof(float);
    pipeline.vertex_layout.attributes.push_back({PipelineVertexSemantic::position,
                                                   PipelineVertexAttributeFormat::float32x3, 0U, 0U});
    pipeline.shaders.push_back({PipelineShaderStage::vertex, PipelineShaderFormat::spirv, shader_fixture()});
    pipeline.shaders.push_back({PipelineShaderStage::fragment, PipelineShaderFormat::spirv, shader_fixture()});
    return pipeline;
}

DrawPacket packet_fixture() {
    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.shader_execution_supported = true;
    packet.flags = {false, false, false, false, false, false, false, false};
    return packet;
}

PipelineProgram skinned_pipeline_fixture() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.name = "indexed-skinned-contract";
    pipeline.vertex_layout.stride = 19U * sizeof(float);
    return pipeline;
}

DrawPacket skinned_packet_fixture() {
    DrawPacket packet = packet_fixture();
    packet.primitive = DrawPrimitiveKind::skinned_mesh;
    packet.vertex_stride_floats = 19U;
    packet.bone_palette.push_back(apex::scene::identity_matrix);
    return packet;
}

TextureDescription target_description() {
    return {16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
            TextureUsage::color_attachment | TextureUsage::transfer_source,
            TextureMemory::device_local, TextureMutability::mutable_data};
}

TextureDescription sampled_description() {
    return {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
            TextureMemory::device_local, TextureMutability::immutable};
}

IndexedStaticMeshDrawRequest request_fixture(const DrawPacket& packet, const PipelineProgram& pipeline,
                                             FakeBuffer& vertices, FakeBuffer& indices) {
    CameraFrame camera;
    camera.clip_space = vertices.backend() == Backend::Vulkan
                            ? CameraClipSpace::vulkan
                            : CameraClipSpace::d3d12;
    return {&packet, &pipeline, &vertices, &indices, StaticMeshIndexType::uint16, 0U, 0U,
            {0.0F, 0.0F, 0.0F, 1.0F}, camera};
}

void accepts_bounded_static_indexed_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    Diagnostic diagnostic;
    packet.world_matrix[12] = 0.25F;
    const auto request = request_fixture(packet, pipeline, vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "valid indexed static-mesh contract accepted");
    require(std::string(indexed_static_mesh_draw_status_name(IndexedStaticMeshDrawStatus::ready)) == "ready",
            "indexed status name");

    FakeTexture d3d_target(Backend::D3D12, target_description());
    FakeBuffer d3d_vertices(Backend::D3D12, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                             BufferMutability::immutable});
    FakeBuffer d3d_indices(Backend::D3D12, {6U, BufferUsage::index, BufferMemory::device_local,
                                            BufferMutability::immutable});
    const auto d3d_request = request_fixture(packet, pipeline, d3d_vertices, d3d_indices);
    require(validate_indexed_static_mesh_draw_request(d3d_target, d3d_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "D3D12 camera clip contract accepted");
}

void accepts_static_and_skinned_buffer_contracts() {
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer static_vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                                 BufferMemory::device_local,
                                                 BufferMutability::immutable});
    FakeBuffer skinned_vertices(Backend::Vulkan, {228U, BufferUsage::vertex,
                                                  BufferMemory::device_local,
                                                  BufferMutability::mutable_data});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    Diagnostic diagnostic;

    PipelineProgram static_pipeline = pipeline_fixture();
    DrawPacket static_packet = packet_fixture();
    auto static_request = request_fixture(static_packet, static_pipeline,
                                          static_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, static_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "static mesh accepts empty palette and immutable vertex buffer");

    PipelineProgram skinned_pipeline = skinned_pipeline_fixture();
    DrawPacket skinned_packet = skinned_packet_fixture();
    auto skinned_request = request_fixture(skinned_packet, skinned_pipeline,
                                           skinned_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, skinned_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "skinned mesh accepts non-empty palette and mutable vertex buffer");

    static_packet.bone_palette.push_back(apex::scene::identity_matrix);
    static_request = request_fixture(static_packet, static_pipeline,
                                     static_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, static_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_skinning_unsupported",
            "static mesh with a palette is rejected");

    static_packet = packet_fixture();
    static_vertices = FakeBuffer(Backend::Vulkan, {132U, BufferUsage::vertex,
                                                   BufferMemory::device_local,
                                                   BufferMutability::mutable_data});
    static_request = request_fixture(static_packet, static_pipeline,
                                     static_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, static_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_buffer_mutable",
            "static mesh with a mutable vertex buffer is rejected");

    skinned_packet.bone_palette.clear();
    skinned_request = request_fixture(skinned_packet, skinned_pipeline,
                                      skinned_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, skinned_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_skinned_mesh_palette_missing",
            "skinned mesh without a palette is rejected");
    skinned_packet = skinned_packet_fixture();
    skinned_vertices = FakeBuffer(Backend::Vulkan, {228U, BufferUsage::vertex,
                                                    BufferMemory::device_local,
                                                    BufferMutability::immutable});
    skinned_request = request_fixture(skinned_packet, skinned_pipeline,
                                      skinned_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, skinned_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_buffer_mutable",
            "skinned mesh with immutable vertex buffer is rejected");

    skinned_vertices = FakeBuffer(Backend::Vulkan, {228U, BufferUsage::vertex,
                                                    BufferMemory::device_local,
                                                    BufferMutability::mutable_data});
    FakeBuffer mutable_indices(Backend::Vulkan, {6U, BufferUsage::index,
                                                 BufferMemory::device_local,
                                                 BufferMutability::mutable_data});
    skinned_request = request_fixture(skinned_packet, skinned_pipeline,
                                      skinned_vertices, mutable_indices);
    require(validate_indexed_static_mesh_draw_request(target, skinned_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_buffer_mutable",
            "skinned mesh with mutable index buffer is rejected");

    static_packet = packet_fixture();
    static_packet.primitive = static_cast<DrawPrimitiveKind>(0xffU);
    static_vertices = FakeBuffer(Backend::Vulkan, {132U, BufferUsage::vertex,
                                                   BufferMemory::device_local,
                                                   BufferMutability::immutable});
    static_request = request_fixture(static_packet, static_pipeline,
                                     static_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, static_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_primitive_unsupported",
            "unknown mesh primitive is rejected");

    static_packet = packet_fixture();
    static_pipeline.vertex_layout.stride = 19U * sizeof(float);
    static_request = request_fixture(static_packet, static_pipeline,
                                     static_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, static_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_stride_mismatch",
            "static mesh requires the 11-float pipeline layout");

    skinned_pipeline = skinned_pipeline_fixture();
    skinned_packet.vertex_stride_floats = 11U;
    skinned_request = request_fixture(skinned_packet, skinned_pipeline,
                                      skinned_vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, skinned_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_stride_mismatch",
            "skinned mesh requires the 19-float pipeline layout");
}

void accepts_explicit_d32_depth_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = PipelineCompareOperation::less;
    DrawPacket packet = packet_fixture();
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    FakeDepthAttachment depth(Backend::Vulkan, {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.depth_attachment = &depth;
    request.clear_depth = true;
    Diagnostic diagnostic;
    require(validate_depth_attachment_description(depth.info().description, diagnostic) ==
                DepthAttachmentStatus::ready,
            "valid D32 depth attachment accepted");
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "indexed draw accepts explicit D32 depth attachment");
    require(std::string(depth_attachment_status_name(DepthAttachmentStatus::ready)) == "ready",
            "depth attachment status name");
}

void accepts_source_evidenced_blend_state() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.blend.enabled = true;
    pipeline.blend.source_color = PipelineBlendFactor::source_alpha;
    pipeline.blend.destination_color = PipelineBlendFactor::one_minus_source_alpha;
    pipeline.blend.source_alpha = PipelineBlendFactor::source_alpha;
    pipeline.blend.destination_alpha = PipelineBlendFactor::one_minus_source_alpha;
    DrawPacket packet = packet_fixture();
    packet.flags.transparent = true;
    packet.flags.blend_enabled = true;
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "source-evidenced alpha blend contract accepted");

    packet.flags.blend_enabled = false;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_blend_state_mismatch",
            "packet and pipeline blend mismatch rejected");
    packet.flags.blend_enabled = true;
    pipeline.blend.alpha_to_coverage = true;
    packet.flags.alpha_to_coverage = true;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_alpha_to_coverage_sample_count",
            "alpha-to-coverage requires a multisampled target");
    TextureDescription multisample_target_description = target_description();
    multisample_target_description.samples = 4U;
    FakeTexture multisample_target(Backend::Vulkan, multisample_target_description);
    pipeline.blend.alpha_to_coverage = false;
    packet.flags.alpha_to_coverage = false;
    pipeline.targets.colors.front().samples = 4U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_pipeline_target_samples_mismatch",
            "pipeline and target sample counts must agree");
    pipeline.blend.alpha_to_coverage = true;
    packet.flags.alpha_to_coverage = true;
    require(validate_indexed_static_mesh_draw_request(multisample_target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "four-sample alpha-to-coverage contract accepted");
}

void validates_multisample_texture_contract() {
    Diagnostic diagnostic;
    TextureDescription description = target_description();
    description.samples = 4U;
    require(validate_texture_description(description, {}, diagnostic) == TextureStatus::ready,
            "four-sample color target description accepted");

    TextureDescription invalid_samples = description;
    invalid_samples.samples = 2U;
    require(validate_texture_description(invalid_samples, {}, diagnostic) == TextureStatus::unsupported &&
                diagnostic.code == "texture_samples_unsupported",
            "unsupported texture sample count rejected");

    TextureDescription sampled = sampled_description();
    sampled.samples = 4U;
    require(validate_texture_description(sampled, {}, diagnostic) == TextureStatus::unsupported &&
                diagnostic.code == "texture_multisample_usage_unsupported",
            "multisampled sampled texture rejected");

    TextureDescription upload = description;
    upload.usage = TextureUsage::color_attachment | TextureUsage::transfer_destination;
    require(validate_texture_description(upload, {}, diagnostic) == TextureStatus::unsupported &&
                diagnostic.code == "texture_multisample_usage_unsupported",
            "multisampled upload texture rejected");

    TextureDescription storage = description;
    storage.usage = TextureUsage::storage;
    require(validate_texture_description(storage, {}, diagnostic) == TextureStatus::unsupported &&
                diagnostic.code == "texture_multisample_usage_unsupported",
            "multisampled storage texture rejected");

    const std::array<std::byte, 4> upload_bytes{};
    TextureUploadPlan uploads;
    uploads.subresources.push_back({0U, 0U, 16U, 16U, 64U,
                                    std::span<const std::byte>(upload_bytes)});
    require(validate_texture_upload_plan(description, uploads, diagnostic) ==
                TextureStatus::unsupported &&
                diagnostic.code == "texture_multisample_upload_unsupported",
            "multisampled color upload rejected");
}

void accepts_multisample_depth_and_indexed_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.targets.colors.front().samples = 4U;
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 4U};
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = PipelineCompareOperation::less;
    DrawPacket packet = packet_fixture();
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    TextureDescription target_desc = target_description();
    target_desc.samples = 4U;
    FakeTexture target(Backend::Vulkan, target_desc);
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    FakeDepthAttachment depth(Backend::Vulkan, {16U, 16U, 4U, DepthAttachmentFormat::d32_float});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.depth_attachment = &depth;
    Diagnostic diagnostic;
    require(validate_depth_attachment_description(depth.info().description, diagnostic) ==
                DepthAttachmentStatus::ready,
            "four-sample D32 depth attachment accepted");
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "indexed color and depth sample counts accepted");

    FakeDepthAttachment single_sample_depth(Backend::Vulkan,
                                            {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    request.depth_attachment = &single_sample_depth;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_depth_attachment_dimensions_mismatch",
            "depth sample mismatch rejected");
}

void accepts_source_evidenced_wireframe_topology() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.raster.fill = PipelineFillMode::wireframe;
    DrawPacket packet = packet_fixture();
    packet.flags.wireframe = true;
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "source-evidenced indexed wireframe contract accepted");

    packet.flags.wireframe = false;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_fill_state_mismatch",
            "wireframe pipeline rejects a solid packet");
    packet.flags.wireframe = true;
    pipeline.raster.fill = PipelineFillMode::solid;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_fill_state_mismatch",
            "solid pipeline rejects a wireframe packet");
}

void validates_portable_diffuse_resource_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "txDiffuse"},
        {PipelineResourceKind::sampler, 0U, 1U, "txDiffuseSampler"},
    };
    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture sampled(Backend::Vulkan, sampled_description());
    FakeSampler sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&sampled, &sampler};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable diffuse texture and sampler contract accepted");

    request.resource_authority = IndexedResourceAuthority::packet_contract;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_resource_execution_staged",
            "resource declarations remain staged without explicit authority");

    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding.sampler = nullptr;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_resource_binding_missing",
            "partial diffuse resource binding rejected");

    request.sampled_binding = {&sampled, &sampler};
    pipeline.resources[1].binding = 0U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request,
            "duplicate resource binding rejected by bounded pipeline validation");
    pipeline.resources[1].binding = 2U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_resource_layout_unsupported",
            "non-portable sampler binding rejected");
    pipeline.resources[1].binding = 1U;

    FakeTexture missing_usage(Backend::Vulkan,
                              {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm,
                               TextureUsage::transfer_source, TextureMemory::device_local,
                               TextureMutability::immutable});
    request.sampled_binding.texture = &missing_usage;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_resource_texture_usage_invalid",
            "texture without sampled usage rejected");

    FakeTexture writable(Backend::Vulkan,
                         {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm,
                          TextureUsage::sampled | TextureUsage::storage,
                          TextureMemory::device_local, TextureMutability::immutable});
    request.sampled_binding.texture = &writable;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_resource_texture_usage_unsupported",
            "writable sampled texture rejected from the first resource baseline");

    request.sampled_binding.texture = &target;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_resource_feedback_loop",
            "color target sampling feedback loop rejected");

    request.sampled_binding.texture = &sampled;
    TextureDescription multisample_sampled_description = sampled_description();
    multisample_sampled_description.samples = 4U;
    FakeTexture multisample_sampled(Backend::Vulkan, multisample_sampled_description);
    request.sampled_binding.texture = &multisample_sampled;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_resource_texture_description_unsupported",
            "multisampled diffuse texture rejected");
    request.sampled_binding.texture = &sampled;
    FakeSampler foreign_sampler(Backend::D3D12);
    request.sampled_binding.sampler = &foreign_sampler;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_resource_backend_mismatch",
            "foreign sampler backend rejected");

    SamplerDescription malformed_sampler;
    malformed_sampler.min_lod = 4.0F;
    malformed_sampler.max_lod = 1.0F;
    FakeSampler malformed(Backend::Vulkan, malformed_sampler);
    request.sampled_binding.sampler = &malformed;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request,
            "malformed sampler metadata rejected");

    pipeline.resources.clear();
    request.sampled_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_resource_binding_unexpected",
            "explicit resource authority rejected for a resource-free pipeline");
}

void validates_portable_material_buffer_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
    };
    DrawPacket packet = packet_fixture();
    packet.resources.push_back({"txDiffuse", 21U, 0U, "body.dds"});
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeSampler sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    FakeBuffer material(Backend::Vulkan,
                        {512U, BufferUsage::uniform, BufferMemory::host_visible,
                         BufferMutability::mutable_data});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &sampler};
    request.material_binding = {&material, 256U,
                                portable_material_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable material constants buffer accepted");

    request.material_binding.range_bytes = portable_material_constant_bytes;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_material_buffer_alignment_invalid",
            "short D3D12-incompatible material view rejected");
    request.material_binding.range_bytes = portable_material_buffer_view_bytes;
    request.material_binding.offset_bytes = 1U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_material_buffer_alignment_invalid",
            "unaligned material view rejected");
    request.material_binding.offset_bytes = 512U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_material_buffer_range_invalid",
            "out-of-range material view rejected");
    request.material_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_material_buffer_missing",
            "missing material constants buffer rejected");
    FakeBuffer wrong_usage(Backend::Vulkan,
                           {256U, BufferUsage::vertex, BufferMemory::host_visible,
                            BufferMutability::immutable});
    request.material_binding = {&wrong_usage, 0U,
                                portable_material_buffer_view_bytes};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_material_buffer_usage_invalid",
            "non-uniform material buffer rejected");
    FakeBuffer foreign(Backend::D3D12,
                       {256U, BufferUsage::uniform, BufferMemory::host_visible,
                        BufferMutability::immutable});
    request.material_binding.buffer = &foreign;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_material_buffer_backend_mismatch",
            "foreign-backend material buffer rejected");
}

void validates_portable_frame_buffer_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
    };
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::diffuse_with_constants_and_frame,
            "portable material and frame resource layout classified");
    DrawPacket packet = packet_fixture();
    packet.resources.push_back({"txDiffuse", 21U, 0U, "body.dds"});
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeSampler sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    FakeBuffer material(Backend::Vulkan,
                        {256U, BufferUsage::uniform, BufferMemory::host_visible,
                         BufferMutability::mutable_data});
    FakeBuffer frame(Backend::Vulkan,
                     {256U, BufferUsage::uniform, BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &sampler};
    request.material_binding = {&material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable frame constants buffer accepted");

    request.frame_binding.range_bytes = portable_frame_constant_bytes;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_frame_buffer_alignment_invalid",
            "short D3D12-incompatible frame view rejected");
    request.frame_binding.range_bytes = portable_frame_buffer_view_bytes;
    request.frame_binding.offset_bytes = 1U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_frame_buffer_alignment_invalid",
            "unaligned frame view rejected");
    request.frame_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_frame_buffer_missing",
            "missing frame constants buffer rejected");
    FakeBuffer wrong_usage(Backend::Vulkan,
                           {256U, BufferUsage::vertex, BufferMemory::host_visible,
                            BufferMutability::immutable});
    request.frame_binding = {&wrong_usage, 0U, portable_frame_buffer_view_bytes};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_frame_buffer_usage_invalid",
            "non-uniform frame buffer rejected");
}

void validates_portable_normal_map_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
        {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
    };
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame,
            "portable normal-map resource layout classified");
    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeTexture normal(Backend::Vulkan, sampled_description());
    FakeSampler diffuse_sampler(Backend::Vulkan);
    FakeSampler normal_sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan,
                        {132U, BufferUsage::vertex, BufferMemory::device_local,
                         BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan,
                       {6U, BufferUsage::index, BufferMemory::device_local,
                        BufferMutability::immutable});
    FakeBuffer material(Backend::Vulkan,
                        {256U, BufferUsage::uniform, BufferMemory::host_visible,
                         BufferMutability::immutable});
    FakeBuffer frame(Backend::Vulkan,
                     {256U, BufferUsage::uniform, BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &diffuse_sampler};
    request.normal_binding = {&normal, &normal_sampler};
    request.material_binding = {&material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable tangent-space normal-map contract accepted");

    request.normal_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_binding_missing",
            "missing normal texture and sampler rejected");
    request.normal_binding = {&normal, nullptr};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_binding_missing",
            "partial normal binding rejected");

    FakeTexture missing_usage(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::transfer_source,
         TextureMemory::device_local, TextureMutability::immutable});
    request.normal_binding = {&missing_usage, &normal_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_texture_usage_invalid",
            "normal texture without sampled usage rejected");

    FakeSampler foreign_sampler(Backend::D3D12);
    request.normal_binding = {&normal, &foreign_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_normal_backend_mismatch",
            "foreign normal sampler rejected");

    request.normal_binding = {&target, &normal_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_feedback_loop",
            "normal-map render-target feedback rejected");

    request.normal_binding = {&normal, &normal_sampler};
    pipeline.resources.resize(4U);
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_binding_unexpected",
            "normal bindings are rejected from the diffuse-only constants layout");
}

void validates_portable_maps_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
        {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTexture"},
        {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
    };
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame,
            "portable maps resource layout classified");
    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeTexture normal(Backend::Vulkan, sampled_description());
    FakeTexture maps(Backend::Vulkan, sampled_description());
    FakeSampler diffuse_sampler(Backend::Vulkan);
    FakeSampler normal_sampler(Backend::Vulkan);
    FakeSampler maps_sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan,
                        {132U, BufferUsage::vertex, BufferMemory::device_local,
                         BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan,
                       {6U, BufferUsage::index, BufferMemory::device_local,
                        BufferMutability::immutable});
    FakeBuffer material(Backend::Vulkan,
                        {256U, BufferUsage::uniform, BufferMemory::host_visible,
                         BufferMutability::immutable});
    FakeBuffer frame(Backend::Vulkan,
                     {256U, BufferUsage::uniform, BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &diffuse_sampler};
    request.normal_binding = {&normal, &normal_sampler};
    request.maps_binding = {&maps, &maps_sampler};
    request.material_binding = {&material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable linear maps contract accepted");
    FakeTexture compressed_maps(
        Backend::Vulkan,
        {4U, 4U, 1U, 1U, TextureFormat::bc3_unorm, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    request.maps_binding = {&compressed_maps, &maps_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable linear maps contract accepts BC3 data");
    request.maps_binding = {&maps, &maps_sampler};

    request.maps_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_maps_binding_missing",
            "missing maps texture and sampler rejected");
    request.maps_binding = {&maps, nullptr};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_maps_binding_missing",
            "partial maps binding rejected");

    request.maps_binding = {&maps, &maps_sampler};
    pipeline.resources.resize(6U);
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_maps_binding_unexpected",
            "maps binding is rejected from the normal-only layout");

    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
        {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
        {PipelineResourceKind::sampler, 0U, 6U, "mapsTextureWrongKind"},
        {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
    };
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_resource_layout_unsupported",
            "wrong-kind maps declaration rejected");

    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
        {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTexture"},
        {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
    };
    FakeSampler foreign_sampler(Backend::D3D12);
    request.maps_binding = {&maps, &foreign_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_maps_backend_mismatch",
            "foreign maps sampler rejected");

    request.maps_binding = {&maps, &maps_sampler};
    FakeTexture missing_usage(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::transfer_source,
         TextureMemory::device_local, TextureMutability::immutable});
    request.maps_binding = {&missing_usage, &maps_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_maps_texture_usage_invalid",
            "maps texture without sampled usage rejected");

    FakeTexture srgb_maps(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_srgb, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    request.maps_binding = {&srgb_maps, &maps_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_maps_texture_description_unsupported",
            "sRGB maps texture rejected in favor of linear UNORM");

    request.maps_binding = {&target, &maps_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_maps_feedback_loop",
            "maps render-target alias rejected");

    pipeline.resources[7] =
        {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTextureAlias"};
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::unsupported,
            "duplicate maps sampled binding alias rejected");
}

void validates_portable_detail_stack_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
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
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame,
            "portable detail-stack resource layout classified");
    pipeline.resources.pop_back();
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::unsupported,
            "incomplete detail-stack resource pair rejected");
    pipeline.resources.push_back(
        {PipelineResourceKind::sampler, 0U, 11U, "normalDetailSampler"});

    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeTexture normal(Backend::Vulkan, sampled_description());
    FakeTexture maps(Backend::Vulkan, sampled_description());
    FakeTexture detail(Backend::Vulkan,
                       {2U, 2U, 1U, 1U, TextureFormat::rgba8_srgb, TextureUsage::sampled,
                        TextureMemory::device_local, TextureMutability::immutable});
    FakeTexture normal_detail(Backend::Vulkan, sampled_description());
    FakeSampler diffuse_sampler(Backend::Vulkan);
    FakeSampler normal_sampler(Backend::Vulkan);
    FakeSampler maps_sampler(Backend::Vulkan);
    FakeSampler detail_sampler(Backend::Vulkan);
    FakeSampler normal_detail_sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan,
                        {132U, BufferUsage::vertex, BufferMemory::device_local,
                         BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan,
                       {6U, BufferUsage::index, BufferMemory::device_local,
                        BufferMutability::immutable});
    FakeBuffer material(Backend::Vulkan,
                        {256U, BufferUsage::uniform, BufferMemory::host_visible,
                         BufferMutability::immutable});
    FakeBuffer frame(Backend::Vulkan,
                     {256U, BufferUsage::uniform, BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &diffuse_sampler};
    request.normal_binding = {&normal, &normal_sampler};
    request.maps_binding = {&maps, &maps_sampler};
    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};
    request.material_binding = {&material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable detail-stack contract accepts sRGB detail and linear normal-detail");
    FakeTexture compressed_detail(
        Backend::Vulkan,
        {4U, 4U, 1U, 1U, TextureFormat::bc1_srgb, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    request.detail_binding = {&compressed_detail, &detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable detail-stack contract accepts BC1 sRGB detail data");
    request.detail_binding = {&detail, &detail_sampler};

    request.detail_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_detail_binding_missing",
            "missing detail pair rejected");
    request.detail_binding = {&detail, nullptr};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_detail_binding_missing",
            "partial detail pair rejected");
    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_binding_missing",
            "missing normal-detail pair rejected");
    request.normal_detail_binding = {&normal_detail, nullptr};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_binding_missing",
            "partial normal-detail pair rejected");
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};

    pipeline.resources.resize(8U);
    request.detail_binding = {&detail, &detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_detail_binding_unexpected",
            "detail pair rejected by the maps-only layout");
    request.detail_binding = {};
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_binding_unexpected",
            "normal-detail pair rejected by the maps-only layout");

    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
        {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTexture"},
        {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
        {PipelineResourceKind::sampler, 0U, 8U, "detailTextureWrongKind"},
        {PipelineResourceKind::sampler, 0U, 9U, "detailSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 10U, "normalDetailTexture"},
        {PipelineResourceKind::sampler, 0U, 11U, "normalDetailSampler"},
    };
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::unsupported,
            "wrong-kind detail declaration rejected");
    pipeline.resources[8] =
        {PipelineResourceKind::sampled_texture, 0U, 8U, "detailTexture"};
    pipeline.resources[10] =
        {PipelineResourceKind::sampler, 0U, 10U, "normalDetailTextureWrongKind"};
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::unsupported,
            "wrong-kind normal-detail declaration rejected");
    pipeline.resources[10] =
        {PipelineResourceKind::sampled_texture, 0U, 10U, "normalDetailTexture"};

    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};
    FakeSampler foreign_sampler(Backend::D3D12);
    request.detail_binding = {&detail, &foreign_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_detail_backend_mismatch",
            "foreign detail sampler rejected");
    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {&normal_detail, &foreign_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_normal_detail_backend_mismatch",
            "foreign normal-detail sampler rejected");
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};

    FakeTexture missing_usage(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::transfer_source,
         TextureMemory::device_local, TextureMutability::immutable});
    request.detail_binding = {&missing_usage, &detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_detail_texture_usage_invalid",
            "detail texture without sampled usage rejected");
    FakeTexture writable_detail(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm,
         TextureUsage::sampled | TextureUsage::storage,
         TextureMemory::device_local, TextureMutability::immutable});
    request.detail_binding = {&writable_detail, &detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_detail_texture_usage_unsupported",
            "writable detail texture rejected");
    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {&missing_usage, &normal_detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_texture_usage_invalid",
            "normal-detail texture without sampled usage rejected");
    FakeTexture srgb_normal_detail(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_srgb, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    request.normal_detail_binding = {&srgb_normal_detail, &normal_detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_normal_detail_texture_description_unsupported",
            "sRGB normal-detail texture rejected in favor of linear UNORM");
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};

    request.detail_binding = {&target, &detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_detail_feedback_loop",
            "detail render-target feedback rejected");
    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {&target, &normal_detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_feedback_loop",
            "normal-detail render-target feedback rejected");
    request.normal_detail_binding = {&normal_detail, &normal_detail_sampler};

    SamplerDescription invalid_sampler;
    invalid_sampler.max_lod = -1.0F;
    FakeSampler bad_detail_sampler(Backend::Vulkan, invalid_sampler);
    request.detail_binding = {&detail, &bad_detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_detail_sampler_lod_invalid",
            "malformed detail sampler rejected");
    request.detail_binding = {&detail, &detail_sampler};
    request.normal_detail_binding = {&normal_detail, &bad_detail_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_sampler_lod_invalid",
            "malformed normal-detail sampler rejected");
}

void rejects_invalid_depth_attachment_descriptions() {
    Diagnostic diagnostic;
    DepthAttachmentDescription description;
    require(validate_depth_attachment_description(description, diagnostic) ==
                DepthAttachmentStatus::invalid_description &&
                diagnostic.code == "depth_attachment_dimensions_invalid",
            "zero-sized depth attachment rejected");

    description.width = 16U;
    description.height = 16U;
    description.samples = 2U;
    require(validate_depth_attachment_description(description, diagnostic) ==
                DepthAttachmentStatus::unsupported &&
                diagnostic.code == "depth_attachment_samples_unsupported",
            "unsupported multisample depth attachment rejected explicitly");

    description.samples = 4U;
    require(validate_depth_attachment_description(description, diagnostic) ==
                DepthAttachmentStatus::ready,
            "four-sample depth attachment accepted");

    description.samples = 1U;
    description.format = static_cast<DepthAttachmentFormat>(99);
    require(validate_depth_attachment_description(description, diagnostic) ==
                DepthAttachmentStatus::unsupported &&
                diagnostic.code == "depth_attachment_format_unsupported",
            "unknown depth attachment format rejected");
}

void rejects_invalid_depth_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = PipelineCompareOperation::less;
    DrawPacket packet = packet_fixture();
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_depth_attachment_missing",
            "depth-enabled draw requires an attachment");

    FakeDepthAttachment depth(Backend::Vulkan, {8U, 16U, 1U, DepthAttachmentFormat::d32_float});
    request.depth_attachment = &depth;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_depth_attachment_dimensions_mismatch",
            "depth dimensions must match color target");

    depth = FakeDepthAttachment(Backend::Vulkan, {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    request.depth_attachment = &depth;
    request.depth_clear_value = std::numeric_limits<float>::infinity();
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_depth_clear_invalid",
            "non-finite depth clear rejected");

    request.depth_clear_value = 1.0F;
    pipeline.depth.compare = PipelineCompareOperation::less_or_equal;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_depth_compare_unsupported",
            "non-source depth comparison rejected");

    pipeline.depth.compare = PipelineCompareOperation::less;
    packet.flags.depth_test = false;
    packet.flags.depth_write = true;
    request.depth_clear_value = 1.0F;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_depth_write_without_test",
            "depth write without test rejected");
}

void validates_ordered_indexed_batch_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    DrawPacket first_packet = packet_fixture();
    DrawPacket second_packet = packet_fixture();
    first_packet.world_matrix[12] = -0.25F;
    second_packet.world_matrix[12] = 0.25F;
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    std::array<IndexedStaticMeshDrawRequest, 2> requests = {
        request_fixture(first_packet, pipeline, vertices, indices),
        request_fixture(second_packet, pipeline, vertices, indices),
    };
    IndexedStaticMeshBatchDescription description;
    description.draws = requests;
    description.load_color = false;
    description.clear_color = {0.1F, 0.2F, 0.3F, 1.0F};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_batch_description(target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "ordered indexed batch preflight accepted");
    require(std::string(indexed_static_mesh_batch_status_name(IndexedStaticMeshBatchStatus::ready)) == "ready",
            "indexed batch status name");
    require(requests[0].packet == &first_packet && requests[1].packet == &second_packet &&
                requests[0].clear_color == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F} &&
                requests[1].clear_color == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F} &&
                !requests[0].load_color && !requests[1].load_color &&
                !requests[0].clear_depth && !requests[1].clear_depth &&
                requests[0].depth_attachment == nullptr && requests[1].depth_attachment == nullptr,
            "batch preflight preserves ordered requests and caller state");

    PipelineProgram multisample_pipeline = pipeline_fixture();
    multisample_pipeline.targets.colors.front().samples = 4U;
    multisample_pipeline.targets.has_depth = true;
    multisample_pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 4U};
    multisample_pipeline.depth.test_enabled = true;
    multisample_pipeline.depth.write_enabled = true;
    multisample_pipeline.depth.compare = PipelineCompareOperation::less;
    DrawPacket multisample_packet = packet_fixture();
    multisample_packet.flags.depth_test = true;
    multisample_packet.flags.depth_write = true;
    FakeTexture multisample_target(Backend::Vulkan, [&] {
        TextureDescription result = target_description();
        result.samples = 4U;
        return result;
    }());
    auto multisample_request = request_fixture(multisample_packet, multisample_pipeline,
                                               vertices, indices);
    const std::array<IndexedStaticMeshDrawRequest, 1> multisample_requests = {multisample_request};
    FakeDepthAttachment multisample_depth(Backend::Vulkan,
                                          {16U, 16U, 4U, DepthAttachmentFormat::d32_float});
    IndexedStaticMeshBatchDescription multisample_description;
    multisample_description.draws = multisample_requests;
    multisample_description.depth_attachment = &multisample_depth;
    require(validate_indexed_static_mesh_batch_description(multisample_target,
                                                           multisample_description,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "four-sample indexed batch accepted");
    FakeDepthAttachment mismatched_depth(Backend::Vulkan,
                                         {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    multisample_description.depth_attachment = &mismatched_depth;
    require(validate_indexed_static_mesh_batch_description(multisample_target,
                                                           multisample_description,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_batch_depth_dimensions_mismatch",
            "batch depth sample mismatch rejected");

    requests[1].packet = nullptr;
    require(validate_indexed_static_mesh_batch_description(target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_handle_missing",
            "invalid later draw rejects the complete batch before execution");
    requests[1] = request_fixture(second_packet, pipeline, vertices, indices);
    requests[1].load_color = true;
    require(validate_indexed_static_mesh_batch_description(target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_batch_draw_override",
            "per-draw load override rejected");

    description = {};
    require(validate_indexed_static_mesh_batch_description(target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_batch_empty",
            "empty indexed batch rejected");
    description.draws = requests;
    std::vector<IndexedStaticMeshDrawRequest> oversized(max_indexed_static_mesh_batch_draws + 1U,
                                                        requests[0]);
    description.draws = oversized;
    require(validate_indexed_static_mesh_batch_description(target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_batch_limit",
            "over-limit indexed batch rejected");
}

void rejects_static_indexed_limits_and_ownership() {
    PipelineProgram pipeline = pipeline_fixture();
    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    Diagnostic diagnostic;
    auto request = request_fixture(packet, pipeline, vertices, indices);

    request.camera_frame.reset();
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_camera_frame_missing",
            "missing camera frame rejected");
    request = request_fixture(packet, pipeline, vertices, indices);
    request.camera_frame->clip_space = CameraClipSpace::d3d12;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_camera_clip_space_mismatch",
            "wrong camera clip space rejected");
    request = request_fixture(packet, pipeline, vertices, indices);
    request.camera_frame->view_projection[0] = std::numeric_limits<float>::quiet_NaN();
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_camera_view_projection_non_finite",
            "non-finite camera matrix rejected");
    request = request_fixture(packet, pipeline, vertices, indices);
    packet.world_matrix[0] = std::numeric_limits<float>::infinity();
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_world_matrix_non_finite",
            "non-finite world matrix rejected");
    packet.world_matrix = apex::scene::identity_matrix;
    pipeline.transform_contract = PipelineTransformContract::none;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_transform_contract_required",
            "missing transform shader contract rejected");
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;

    request.packet = nullptr;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_handle_missing",
            "missing packet rejected");
    request.packet = &packet;
    packet.index_count = 4U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_range_invalid",
            "non-triangle index count rejected");
    packet.index_count = 3U;
    packet.vertex_stride_floats = 12U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_stride_mismatch",
            "float32 and byte stride mismatch rejected");
    packet.vertex_stride_floats = 11U;
    packet.vertex_offset = 2U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_buffer_range_invalid",
            "vertex element offset range rejected");
    packet.vertex_offset = 0U;
    vertices = FakeBuffer(Backend::Vulkan, {36U, BufferUsage::index, BufferMemory::device_local,
                                            BufferMutability::immutable});
    request.vertex_buffer = &vertices;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_vertex_usage_invalid",
            "vertex usage rejected");
    vertices = FakeBuffer(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                            BufferMutability::immutable});
    request.vertex_buffer = &vertices;
    FakeBuffer mutable_indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                                  BufferMutability::mutable_data});
    request.index_buffer = &mutable_indices;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_buffer_mutable",
            "mutable geometry buffer rejected");
    FakeBuffer foreign_indices(Backend::D3D12, {6U, BufferUsage::index, BufferMemory::device_local,
                                                 BufferMutability::immutable});
    request.index_buffer = &foreign_indices;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_backend_mismatch",
            "foreign backend rejected");
}

void rejects_staged_draw_packet() {
    apex::formats::Kn5File model;
    apex::formats::Kn5Material material;
    material.name = "body";
    material.shader = "ksPerPixel";
    model.materials.push_back(material);
    model.root.type = 1U;
    model.root.kind = "node";
    model.root.name = "ROOT";
    model.root.active = true;
    apex::formats::Kn5Node mesh;
    mesh.type = 2U;
    mesh.kind = "mesh";
    mesh.name = "BODY";
    mesh.active = true;
    mesh.renderable = true;
    mesh.materialId = 0U;
    mesh.vertexStride = 11U;
    mesh.vertices.resize(33U, 0.0F);
    mesh.indices = {0U, 1U, 2U};
    model.root.children.push_back(mesh);

    apex::scene::SceneSnapshot scene;
    (void)scene.add_material({"body", "ksPerPixel", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = scene.add_node(std::move(root));
    apex::scene::SceneNode scene_mesh;
    scene_mesh.name = "BODY";
    scene_mesh.kind = apex::scene::NodeKind::mesh;
    scene_mesh.material = 0U;
    scene_mesh.renderable = true;
    const auto mesh_id = scene.add_node(std::move(scene_mesh), root_id);
    RenderPlan plan;
    plan.items.push_back({mesh_id, 0U, 0U, 0.0F, false, true, {}, {}, {root_id, mesh_id}});
    const auto built = build_draw_packets(model, scene, plan);
    require(built.packets.size() == 1U && !built.packets.front().shader_execution_supported,
            "current packet builder marks shader execution staged");

    PipelineProgram pipeline = pipeline_fixture();
    pipeline.vertex_layout.stride = 11U * sizeof(float);
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    Diagnostic diagnostic;
    auto request = request_fixture(built.packets.front(), pipeline, vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_shader_execution_staged",
            "staged packet rejected before execution");

    DrawPacket executable_packet = built.packets.front();
    executable_packet.flags.depth_test = false;
    executable_packet.flags.depth_write = false;
    request = request_fixture(executable_packet, pipeline, vertices, indices);
    request.shader_authority = IndexedShaderAuthority::explicit_pipeline;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready &&
                !built.packets.front().shader_execution_supported &&
                !executable_packet.shader_execution_supported,
            "an explicit executable pipeline authorizes only its request");

    pipeline.transform_contract = PipelineTransformContract::none;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_transform_contract_required",
            "explicit pipeline authority still requires the transform contract");
}

} // namespace

int main() {
    try {
        accepts_bounded_static_indexed_contract();
        accepts_static_and_skinned_buffer_contracts();
        accepts_explicit_d32_depth_contract();
        accepts_source_evidenced_blend_state();
        validates_multisample_texture_contract();
        accepts_multisample_depth_and_indexed_contract();
        accepts_source_evidenced_wireframe_topology();
        validates_portable_diffuse_resource_contract();
        validates_portable_material_buffer_contract();
        validates_portable_frame_buffer_contract();
        validates_portable_normal_map_contract();
        validates_portable_maps_contract();
        validates_portable_detail_stack_contract();
        rejects_invalid_depth_attachment_descriptions();
        rejects_invalid_depth_contract();
        validates_ordered_indexed_batch_contract();
        rejects_static_indexed_limits_and_ownership();
        rejects_staged_draw_packet();
        std::cout << "indexed draw tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "indexed draw tests failed: " << error.what() << '\n';
        return 1;
    }
}
