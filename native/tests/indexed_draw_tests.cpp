#include "apex/render/draw_packet.hpp"
#include "apex/render/device.hpp"
#include "apex/render/selected_mesh.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan_abi.hpp"
#include "../src/render/backend_internal.hpp"

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

PipelineResourceKind probe_resource_kind(StockShaderDescriptorKind kind) {
    switch (kind) {
    case StockShaderDescriptorKind::uniform_buffer:
        return PipelineResourceKind::uniform_buffer;
    case StockShaderDescriptorKind::sampled_image:
        return PipelineResourceKind::sampled_texture;
    case StockShaderDescriptorKind::sampler:
        return PipelineResourceKind::sampler;
    }
    return PipelineResourceKind::storage_buffer;
}

PipelineProgram vulkan_abi_probe_pipeline_fixture() {
    PipelineProgram pipeline;
    pipeline.name = "indexed-stock-vulkan-abi-probe";
    pipeline.targets.colors.push_back(
        {PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.depth.compare = PipelineCompareOperation::less;
    pipeline.transform_contract = PipelineTransformContract::none;
    pipeline.vertex_layout.stride = stock_ks_per_pixel_vertex_stride_bytes;
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0,
         PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent,
         PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    pipeline.shaders = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
         shader_fixture(), PipelineShaderProvenance::native_abi_probe},
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
         shader_fixture(), PipelineShaderProvenance::native_abi_probe},
    };
    for (const StockKsPerPixelBackendBinding& binding :
         stock_ks_per_pixel_vulkan_abi_manifest.bindings) {
        pipeline.resources.push_back(
            {probe_resource_kind(binding.descriptor_kind), binding.vulkan_set,
             binding.vulkan_binding, std::string(binding.name)});
    }
    return pipeline;
}

PipelineProgram overlay_pipeline_fixture() {
    PipelineProgram pipeline;
    pipeline.name = "overlay-line-contract";
    pipeline.targets.colors.push_back(
        {PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.raster.fill = PipelineFillMode::wireframe;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    pipeline.vertex_layout.stride = sizeof(OverlayLineVertex);
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::color,
         PipelineVertexAttributeFormat::float32x3, 1U,
         static_cast<std::uint32_t>(3U * sizeof(float))},
    };
    pipeline.shaders.push_back({PipelineShaderStage::vertex,
                                PipelineShaderFormat::spirv,
                                shader_fixture()});
    pipeline.shaders.push_back({PipelineShaderStage::fragment,
                                PipelineShaderFormat::spirv,
                                shader_fixture()});
    return pipeline;
}

PipelineProgram selected_mesh_pipeline_fixture() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.name = "selected-mesh-contract";
    pipeline.raster.cull = PipelineCullMode::front;
    pipeline.transform_contract = PipelineTransformContract::selected_mesh;
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

PipelineProgram depth_only_pipeline_fixture() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.targets.colors.clear();
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    pipeline.resources.clear();
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = PipelineCompareOperation::less;
    pipeline.shaders.pop_back();
    return pipeline;
}

DepthOnlyIndexedStaticMeshDrawRequest depth_only_request_fixture(
    const DrawPacket& packet, const PipelineProgram& pipeline,
    FakeBuffer& vertices, FakeBuffer& indices) {
    CameraFrame camera;
    camera.clip_space = vertices.backend() == Backend::Vulkan
                            ? CameraClipSpace::vulkan
                            : CameraClipSpace::d3d12;
    return {&packet, &pipeline, &vertices, &indices, StaticMeshIndexType::uint16,
            camera, false, 1.0F};
}

PipelineProgram alpha_tested_depth_only_pipeline_fixture() {
    PipelineProgram pipeline = depth_only_pipeline_fixture();
    pipeline.name = "indexed-alpha-tested-depth-contract";
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 3U, "samLinearShadow"},
        {PipelineResourceKind::uniform_buffer, 0U, 4U, "shadowMaterial"},
    };
    pipeline.shaders.push_back({PipelineShaderStage::fragment,
                                PipelineShaderFormat::spirv, shader_fixture()});
    return pipeline;
}

DepthOnlyIndexedStaticMeshDrawRequest alpha_tested_depth_only_request_fixture(
    const DrawPacket& packet, const PipelineProgram& pipeline,
    FakeBuffer& vertices, FakeBuffer& indices, FakeTexture& diffuse,
    FakeSampler& sampler, FakeBuffer& material) {
    auto request = depth_only_request_fixture(packet, pipeline, vertices, indices);
    request.material_mode =
        DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::stock_alpha_tested;
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.alpha_tested_diffuse_binding = {&diffuse, &sampler};
    request.alpha_tested_material_binding = {
        &material, 256U, stock_shadow_caster_material_bytes};
    return request;
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

void validates_directional_shadow_receiver_contract() {
    PipelineProgram pipeline = pipeline_fixture();
    pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 16U, "txShadow0"},
        {PipelineResourceKind::sampled_texture, 0U, 17U, "txShadow1"},
        {PipelineResourceKind::sampled_texture, 0U, 18U, "txShadow2"},
        {PipelineResourceKind::sampler, 0U, 19U, "shadowSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 20U, "shadowReceiver"},
    };
    require(pipeline_declares_directional_shadow_receiver(pipeline) &&
                classify_indexed_portable_resource_layout(pipeline) ==
                    IndexedPortableResourceLayout::diffuse_with_frame,
            "directional-shadow receiver extends the material layout orthogonally");

    DrawPacket packet = packet_fixture();
    packet.resources.push_back({"txDiffuse", 1U, 0U, "body.dds"});
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeSampler diffuse_sampler(Backend::Vulkan);
    SamplerDescription shadow_sampler_description;
    shadow_sampler_description.min_filter = SamplerFilter::nearest;
    shadow_sampler_description.mag_filter = SamplerFilter::nearest;
    shadow_sampler_description.mip_filter = SamplerFilter::nearest;
    shadow_sampler_description.address_u = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.address_v = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.address_w = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.compare = SamplerCompare::disabled;
    FakeSampler shadow_sampler(Backend::Vulkan, shadow_sampler_description);
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    FakeBuffer frame(Backend::Vulkan,
                     {256U, BufferUsage::uniform, BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    FakeBuffer shadow_constants(Backend::Vulkan,
                                {256U, BufferUsage::uniform,
                                 BufferMemory::host_visible,
                                 BufferMutability::mutable_data});
    FakeDepthAttachment shadow0(
        Backend::Vulkan,
        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    FakeDepthAttachment shadow1(
        Backend::Vulkan,
        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    FakeDepthAttachment shadow2(
        Backend::Vulkan,
        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &diffuse_sampler};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    request.directional_shadow_binding = {
        {&shadow0, &shadow1, &shadow2}, &shadow_sampler, &shadow_constants,
        0U, portable_directional_shadow_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "complete three-map directional-shadow receiver accepted");

    // Keep the receiver resources request-local, but verify that every
    // partial binding fails closed when the pipeline does not declare the
    // receiver extension. In particular, maps[0] alone must not be treated
    // as an ignorable staged resource.
    PipelineProgram non_receiver_pipeline = pipeline_fixture();
    non_receiver_pipeline.resources = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
        {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
    };
    auto non_receiver_request =
        request_fixture(packet, non_receiver_pipeline, vertices, indices);
    non_receiver_request.resource_authority =
        IndexedResourceAuthority::explicit_bindings;
    non_receiver_request.sampled_binding = {&diffuse, &diffuse_sampler};
    non_receiver_request.frame_binding = {
        &frame, 0U, portable_frame_buffer_view_bytes};
    const auto require_unexpected_shadow_binding =
        [&](const IndexedDirectionalShadowBinding& binding,
            std::string_view description) {
            non_receiver_request.directional_shadow_binding = binding;
            require(validate_indexed_static_mesh_draw_request(
                        target, non_receiver_request, diagnostic) ==
                        IndexedStaticMeshDrawStatus::invalid_request &&
                        diagnostic.code ==
                            "indexed_directional_shadow_binding_unexpected",
                    description);
        };
    IndexedDirectionalShadowBinding partial_shadow_binding{};
    partial_shadow_binding.maps[0] = &shadow0;
    require_unexpected_shadow_binding(
        partial_shadow_binding,
        "a non-receiver pipeline rejects only its first directional-shadow map");
    partial_shadow_binding = {};
    partial_shadow_binding.sampler = &shadow_sampler;
    require_unexpected_shadow_binding(
        partial_shadow_binding,
        "a non-receiver pipeline rejects an unexpected shadow sampler");
    partial_shadow_binding = {};
    partial_shadow_binding.constants = &shadow_constants;
    require_unexpected_shadow_binding(
        partial_shadow_binding,
        "a non-receiver pipeline rejects an unexpected shadow constants buffer");
    partial_shadow_binding = {};
    partial_shadow_binding.constants_range_bytes =
        portable_directional_shadow_buffer_view_bytes;
    require_unexpected_shadow_binding(
        partial_shadow_binding,
        "a non-receiver pipeline rejects an unexpected shadow constants range");

    request.directional_shadow_binding.maps[2] = nullptr;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_directional_shadow_binding_missing",
            "truncated three-map receiver rejected before execution");
    request.directional_shadow_binding.maps[2] = &shadow2;

    request.directional_shadow_binding.maps[2] = &shadow1;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_directional_shadow_map_duplicate",
            "duplicate receiver map rejected");
    request.directional_shadow_binding.maps[2] = &shadow2;

    FakeDepthAttachment non_sampled(
        Backend::Vulkan,
        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, false});
    request.directional_shadow_binding.maps[2] = &non_sampled;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_directional_shadow_description_unsupported",
            "non-sampled depth attachment rejected as a receiver map");
    request.directional_shadow_binding.maps[2] = &shadow2;

    FakeDepthAttachment wrong_size(
        Backend::Vulkan,
        {16U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    request.directional_shadow_binding.maps[2] = &wrong_size;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_directional_shadow_dimensions_mismatch",
            "mismatched receiver-map dimensions rejected");
    request.directional_shadow_binding.maps[2] = &shadow2;

    FakeSampler comparison_sampler(Backend::Vulkan, [&] {
        SamplerDescription description = shadow_sampler_description;
        description.compare = SamplerCompare::less_equal;
        return description;
    }());
    request.directional_shadow_binding.sampler = &comparison_sampler;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_directional_shadow_sampler_contract_invalid",
            "hardware comparison sampler rejected by explicit-PCF contract");
    request.directional_shadow_binding.sampler = &shadow_sampler;

    request.directional_shadow_binding.constants_range_bytes = 128U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_directional_shadow_constants_alignment_invalid",
            "truncated receiver constants rejected");

    pipeline.resources.pop_back();
    require(!pipeline_declares_directional_shadow_receiver(pipeline) &&
                classify_indexed_portable_resource_layout(pipeline) ==
                    IndexedPortableResourceLayout::unsupported,
            "partial receiver declaration is unsupported");
}

void validates_recovered_stock_directional_shadow_abi() {
    std::array<apex::scene::Matrix4, indexed_directional_shadow_cascade_count>
        matrices{};
    matrices[0][0] = 1.0F;
    matrices[1][5] = 2.0F;
    matrices[2][10] = 3.0F;
    const std::array<float, indexed_directional_shadow_cascade_count> biases = {
        0.000002F, 0.000015F, 0.0003F};
    const StockDirectionalShadowReceiverConstants constants =
        make_stock_directional_shadow_receiver_constants(matrices, biases, 2048U);
    require(constants.shadow_matrices == matrices && constants.biases == biases,
            "stock receiver packing preserves matrices and bias order");
    require(constants.texture_size == 1.0F / 2048.0F,
            "stock receiver textureSize is the render-target reciprocal");

    const auto bytes = std::as_bytes(
        std::span<const StockDirectionalShadowReceiverConstants>(&constants, 1U));
    require(bytes.size() == stock_directional_shadow_buffer_view_bytes,
            "stock receiver packing is exactly 208 bytes");
    require(offsetof(StockDirectionalShadowReceiverConstants, biases) == 192U &&
                offsetof(StockDirectionalShadowReceiverConstants, texture_size) == 204U,
            "stock receiver offsets remain explicit for both backend handoffs");

    for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
        FakeBuffer native_constants(
            backend,
            {stock_directional_shadow_buffer_view_bytes, BufferUsage::uniform,
             BufferMemory::host_visible, BufferMutability::mutable_data});
        require(native_constants.backend() == backend &&
                    native_constants.info().description.size_bytes ==
                        stock_directional_shadow_buffer_view_bytes,
                "stock receiver byte contract is backend-neutral");
    }
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

    FakeTexture bc5_normal(
        Backend::Vulkan,
        {4U, 4U, 1U, 1U, TextureFormat::bc5_unorm, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    request.normal_binding = {&bc5_normal, &normal_sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_normal_texture_description_unsupported",
            "BC5 normal texture rejected because the normal ABI has no Z reconstruction");
    request.normal_binding = {&normal, &normal_sampler};

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

void validates_portable_multimap_reflection_contract() {
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
        {PipelineResourceKind::sampled_texture, 0U,
         portable_multimap_cube_texture_binding, "reflectionCube"},
        {PipelineResourceKind::sampler, 0U,
         portable_multimap_cube_sampler_binding, "reflectionSampler"},
        {PipelineResourceKind::uniform_buffer, 0U,
         portable_multimap_reflection_constants_binding,
         "reflectionConstants"},
    };
    require(pipeline_declares_multimap_reflection(pipeline),
            "complete portable MultiMap reflection extension recognized");
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::
                    diffuse_normal_maps_with_constants_and_frame,
            "reflection extension preserves the underlying maps layout");

    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeTexture normal(Backend::Vulkan, sampled_description());
    FakeTexture maps(Backend::Vulkan, sampled_description());
    TextureDescription cube_description = sampled_description();
    cube_description.width = 4U;
    cube_description.height = 4U;
    cube_description.mip_levels = 3U;
    cube_description.shape = TextureShape::texture_cube;
    FakeTexture cube(Backend::Vulkan, cube_description);
    FakeSampler diffuse_sampler(Backend::Vulkan);
    FakeSampler normal_sampler(Backend::Vulkan);
    FakeSampler maps_sampler(Backend::Vulkan);
    FakeSampler cube_sampler(Backend::Vulkan);
    FakeBuffer vertices(Backend::Vulkan,
                        {132U, BufferUsage::vertex,
                         BufferMemory::device_local,
                         BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan,
                       {6U, BufferUsage::index, BufferMemory::device_local,
                        BufferMutability::immutable});
    FakeBuffer material(Backend::Vulkan,
                        {256U, BufferUsage::uniform,
                         BufferMemory::host_visible,
                         BufferMutability::immutable});
    FakeBuffer frame(Backend::Vulkan,
                     {256U, BufferUsage::uniform, BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    FakeBuffer reflection(Backend::Vulkan,
                          {512U, BufferUsage::uniform,
                           BufferMemory::host_visible,
                           BufferMutability::mutable_data});
    auto request = request_fixture(packet, pipeline, vertices, indices);
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {&diffuse, &diffuse_sampler};
    request.normal_binding = {&normal, &normal_sampler};
    request.maps_binding = {&maps, &maps_sampler};
    request.material_binding = {
        &material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    request.multimap_reflection_binding = {
        {&cube, &cube_sampler},
        {&reflection, 256U,
         portable_multimap_reflection_buffer_view_bytes}};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "explicit cube and padded reflection constants accepted");

    request.multimap_reflection_binding.cube.texture = nullptr;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_multimap_reflection_binding_missing",
            "partial reflection request binding rejected");
    request.multimap_reflection_binding.cube.texture = &cube;

    TextureDescription array_description = cube_description;
    array_description.shape = TextureShape::texture_2d;
    array_description.array_layers = texture_cube_face_count;
    FakeTexture six_layer_array(Backend::Vulkan, array_description);
    request.multimap_reflection_binding.cube.texture = &six_layer_array;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code ==
                    "indexed_multimap_reflection_texture_description_unsupported",
            "ordinary six-layer arrays cannot masquerade as cubes");

    TextureDescription non_square_description = cube_description;
    non_square_description.height = 2U;
    FakeTexture non_square_cube(Backend::Vulkan, non_square_description);
    request.multimap_reflection_binding.cube.texture = &non_square_cube;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code ==
                    "indexed_multimap_reflection_texture_description_unsupported",
            "non-square reflection cubes fail closed");
    request.multimap_reflection_binding.cube.texture = &cube;

    SamplerDescription comparison_description;
    comparison_description.compare = SamplerCompare::less;
    FakeSampler comparison_sampler(Backend::Vulkan, comparison_description);
    request.multimap_reflection_binding.cube.sampler = &comparison_sampler;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_multimap_reflection_sampler_contract_invalid",
            "comparison reflection sampler rejected");
    request.multimap_reflection_binding.cube.sampler = &cube_sampler;

    request.multimap_reflection_binding.constants.range_bytes = 16U;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_multimap_reflection_constants_alignment_invalid",
            "unpadded reflection constants view rejected");
    request.multimap_reflection_binding.constants.range_bytes =
        portable_multimap_reflection_buffer_view_bytes;
    request.multimap_reflection_binding.constants.offset_bytes = 512U;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_multimap_reflection_constants_range_invalid",
            "out-of-bounds reflection constants view rejected");
    request.multimap_reflection_binding.constants.offset_bytes = 256U;

    FakeBuffer foreign_reflection(
        Backend::D3D12,
        {256U, BufferUsage::uniform, BufferMemory::host_visible,
         BufferMutability::immutable});
    request.multimap_reflection_binding.constants.buffer = &foreign_reflection;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code ==
                    "indexed_multimap_reflection_backend_mismatch",
            "foreign reflection constants buffer rejected");
    request.multimap_reflection_binding.constants.buffer = &reflection;

    PipelineProgram partial = pipeline;
    partial.resources.pop_back();
    require(!pipeline_declares_multimap_reflection(partial) &&
                classify_indexed_portable_resource_layout(partial) ==
                    IndexedPortableResourceLayout::unsupported,
            "partial reflection declaration rejected");
    PipelineProgram duplicate = pipeline;
    duplicate.resources.push_back(
        {PipelineResourceKind::sampled_texture, 0U,
         portable_multimap_cube_texture_binding, "duplicateReflectionCube"});
    require(!pipeline_declares_multimap_reflection(duplicate) &&
                classify_indexed_portable_resource_layout(duplicate) ==
                    IndexedPortableResourceLayout::unsupported,
            "duplicate reflection declaration rejected");

    PipelineProgram no_maps = pipeline;
    no_maps.resources.erase(no_maps.resources.begin() + 6U,
                            no_maps.resources.begin() + 8U);
    request.pipeline = &no_maps;
    require(validate_indexed_static_mesh_draw_request(target, request,
                                                      diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code ==
                    "indexed_multimap_reflection_layout_unsupported",
            "reflection extension cannot attach to a non-maps material layout");
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

void validates_portable_damage_stack_contract() {
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
        {PipelineResourceKind::sampled_texture, 0U, 12U, "damageTexture"},
        {PipelineResourceKind::sampler, 0U, 13U, "damageSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 14U, "damageMaskTexture"},
        {PipelineResourceKind::sampler, 0U, 15U, "damageMaskSampler"},
    };
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame,
            "portable damage-stack resource layout classified");
    pipeline.resources.pop_back();
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::unsupported,
            "incomplete damage-mask resource pair rejected");
    pipeline.resources.push_back(
        {PipelineResourceKind::sampler, 0U, 15U, "damageMaskSampler"});

    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeTexture normal(Backend::Vulkan, sampled_description());
    FakeTexture maps(Backend::Vulkan, sampled_description());
    FakeTexture damage(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_srgb, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    FakeTexture damage_mask(Backend::Vulkan, sampled_description());
    FakeSampler sampler(Backend::Vulkan);
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
    request.sampled_binding = {&diffuse, &sampler};
    request.normal_binding = {&normal, &sampler};
    request.maps_binding = {&maps, &sampler};
    request.damage_binding = {&damage, &sampler};
    request.damage_mask_binding = {&damage_mask, &sampler};
    request.material_binding = {&material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable damage-stack contract accepts sRGB damage and linear mask data");

    request.damage_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_damage_binding_missing",
            "missing damage pair rejected");
    request.damage_binding = {&damage, &sampler};
    request.damage_mask_binding = {};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_damage_mask_binding_missing",
            "missing damage-mask pair rejected");

    FakeTexture srgb_damage_mask(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_srgb, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    request.damage_mask_binding = {&srgb_damage_mask, &sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_damage_mask_texture_description_unsupported",
            "sRGB damage mask rejected in favor of linear UNORM");
}

void validates_portable_damage_dust_alpha_contract() {
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
        {PipelineResourceKind::sampled_texture, 0U, 8U, "dustTexture"},
        {PipelineResourceKind::sampler, 0U, 9U, "dustSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 12U, "damageTexture"},
        {PipelineResourceKind::sampler, 0U, 13U, "damageSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 14U, "damageMaskTexture"},
        {PipelineResourceKind::sampler, 0U, 15U, "damageMaskSampler"},
    };
    require(classify_indexed_portable_resource_layout(pipeline) ==
                IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame,
            "portable damage-dust resource layout classified");

    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeTexture normal(Backend::Vulkan, sampled_description());
    FakeTexture maps(Backend::Vulkan, sampled_description());
    FakeTexture dust(Backend::Vulkan, sampled_description());
    FakeTexture damage(Backend::Vulkan, sampled_description());
    FakeTexture damage_mask(Backend::Vulkan, sampled_description());
    FakeSampler sampler(Backend::Vulkan);
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
    request.sampled_binding = {&diffuse, &sampler};
    request.normal_binding = {&normal, &sampler};
    request.maps_binding = {&maps, &sampler};
    request.detail_binding = {&dust, &sampler};
    request.damage_binding = {&damage, &sampler};
    request.damage_mask_binding = {&damage_mask, &sampler};
    request.material_binding = {&material, 0U, portable_material_buffer_view_bytes};
    request.frame_binding = {&frame, 0U, portable_frame_buffer_view_bytes};
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "portable damage-dust contract accepts txDust alpha binding");

    request.normal_detail_binding = {&dust, &sampler};
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_normal_detail_binding_unexpected",
            "damage-dust layout keeps generic normal-detail binding exclusive");
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

    description.shader_readable = true;
    require(validate_depth_attachment_description(description, diagnostic) ==
                DepthAttachmentStatus::unsupported &&
                diagnostic.code == "depth_attachment_sampled_multisample_unsupported",
            "shader-readable multisample depth attachment rejected explicitly");

    description.samples = 1U;
    description.format = static_cast<DepthAttachmentFormat>(99);
    require(validate_depth_attachment_description(description, diagnostic) ==
                DepthAttachmentStatus::unsupported &&
                diagnostic.code == "depth_attachment_format_unsupported",
            "unknown depth attachment format rejected");
}

void validates_depth_only_indexed_contract() {
    PipelineProgram pipeline = depth_only_pipeline_fixture();
    DrawPacket packet = packet_fixture();
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    FakeDepthAttachment depth(Backend::Vulkan,
                              {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    auto request = depth_only_request_fixture(packet, pipeline, vertices, indices);
    Diagnostic diagnostic;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::ready,
            "valid depth-only indexed contract accepted");
    require(std::string(depth_only_indexed_static_mesh_draw_status_name(
                DepthOnlyIndexedStaticMeshDrawStatus::ready)) == "ready",
            "depth-only draw status name");

    request.clear_depth = true;
    request.depth_clear_value = std::numeric_limits<float>::infinity();
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_static_mesh_depth_clear_invalid",
            "non-finite depth-only clear rejected");
    request.clear_depth = false;
    request.depth_clear_value = 1.0F;

    packet.flags.depth_write = false;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "depth_only_indexed_packet_depth_state_mismatch",
            "depth-only packet and pipeline depth state must match");
    packet.flags.depth_write = true;

    FakeDepthAttachment multisample_depth(Backend::Vulkan,
                                          {16U, 16U, 4U, DepthAttachmentFormat::d32_float});
    require(validate_depth_only_indexed_static_mesh_draw_request(
                multisample_depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "depth_only_indexed_static_mesh_depth_target_unsupported",
            "multisample depth-only target rejected");

    FakeBuffer combined_vertices(Backend::Vulkan,
                                 {132U, BufferUsage::vertex | BufferUsage::transfer_destination,
                                  BufferMemory::device_local, BufferMutability::immutable});
    request = depth_only_request_fixture(packet, pipeline, combined_vertices, indices);
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_vertex_buffer_usage_invalid",
            "non-exclusive vertex usage rejected");

    vertices = FakeBuffer(Backend::Vulkan, {131U, BufferUsage::vertex,
                                            BufferMemory::device_local,
                                            BufferMutability::immutable});
    request = depth_only_request_fixture(packet, pipeline, vertices, indices);
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_static_mesh_buffer_range_invalid",
            "depth-only vertex range exceeding ownership rejected");

    vertices = FakeBuffer(Backend::Vulkan, {132U, BufferUsage::vertex,
                                            BufferMemory::device_local,
                                            BufferMutability::immutable});
    request = depth_only_request_fixture(packet, pipeline, vertices, indices);
    packet.world_matrix[0] = std::numeric_limits<float>::quiet_NaN();
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_static_mesh_world_matrix_non_finite",
            "non-finite depth-only world matrix rejected");

    packet = packet_fixture();
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    request = depth_only_request_fixture(packet, pipeline, vertices, indices);
    pipeline.shaders.push_back({PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
                                shader_fixture()});
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_shader_pair_invalid",
            "depth-only fragment shader rejected");

    pipeline = depth_only_pipeline_fixture();
    request = depth_only_request_fixture(packet, pipeline, vertices, indices);
    std::array<DepthOnlyIndexedStaticMeshDrawRequest, 2U> draws = {request, request};
    DepthOnlyIndexedStaticMeshBatchDescription batch;
    batch.draws = draws;
    batch.depth_attachment = &depth;
    batch.clear_depth = true;
    batch.depth_clear_value = 0.75F;
    require(validate_depth_only_indexed_static_mesh_batch_description(batch, diagnostic) ==
                DepthOnlyIndexedStaticMeshBatchStatus::ready,
            "valid depth-only indexed batch accepted");
    require(std::string(depth_only_indexed_static_mesh_batch_status_name(
                DepthOnlyIndexedStaticMeshBatchStatus::ready)) == "ready",
            "depth-only batch status name");
    draws[1].clear_depth = true;
    require(validate_depth_only_indexed_static_mesh_batch_description(batch, diagnostic) ==
                DepthOnlyIndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_static_mesh_batch_draw_override",
            "per-draw depth clear override rejected");
    batch = {};
    require(validate_depth_only_indexed_static_mesh_batch_description(batch, diagnostic) ==
                DepthOnlyIndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_static_mesh_batch_empty",
            "empty depth-only batch rejected");
    batch.depth_attachment = &depth;
    batch.clear_depth = true;
    require(validate_depth_only_indexed_static_mesh_batch_description(batch, diagnostic) ==
                DepthOnlyIndexedStaticMeshBatchStatus::ready,
            "clear-only depth batch initializes an empty caster map");
}

void validates_alpha_tested_depth_only_contract() {
    PipelineProgram pipeline = alpha_tested_depth_only_pipeline_fixture();
    DrawPacket packet = packet_fixture();
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    FakeDepthAttachment depth(Backend::Vulkan,
                              {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    FakeTexture diffuse(Backend::Vulkan, sampled_description());
    FakeSampler sampler(Backend::Vulkan);
    FakeBuffer material(Backend::Vulkan,
                        {512U, BufferUsage::uniform, BufferMemory::host_visible,
                         BufferMutability::mutable_data});
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex,
                                          BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index,
                                         BufferMemory::device_local,
                                         BufferMutability::immutable});
    auto request = alpha_tested_depth_only_request_fixture(
        packet, pipeline, vertices, indices, diffuse, sampler, material);
    Diagnostic diagnostic;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::ready,
            "valid alpha-tested depth-only contract accepted");

    request.resource_authority = IndexedResourceAuthority::packet_contract;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "depth_only_indexed_alpha_tested_resource_authority_required",
            "alpha-tested depth-only resources require explicit authority");
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;

    request.alpha_tested_diffuse_binding.texture = nullptr;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_alpha_tested_binding_missing",
            "missing alpha-tested diffuse binding rejected");
    request.alpha_tested_diffuse_binding = {&diffuse, &sampler};

    FakeTexture foreign_diffuse(Backend::D3D12, sampled_description());
    request.alpha_tested_diffuse_binding.texture = &foreign_diffuse;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "depth_only_indexed_alpha_tested_backend_mismatch",
            "foreign alpha-tested texture rejected");
    request.alpha_tested_diffuse_binding.texture = &diffuse;

    TextureDescription not_sampled = sampled_description();
    not_sampled.usage = TextureUsage::transfer_source;
    FakeTexture not_sampled_diffuse(Backend::Vulkan, not_sampled);
    request.alpha_tested_diffuse_binding.texture = &not_sampled_diffuse;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_alpha_tested_texture_usage_invalid",
            "non-sampled alpha-tested texture rejected");
    request.alpha_tested_diffuse_binding.texture = &diffuse;

    TextureDescription mutable_diffuse_description = sampled_description();
    mutable_diffuse_description.mutability = TextureMutability::mutable_data;
    FakeTexture mutable_diffuse(Backend::Vulkan, mutable_diffuse_description);
    request.alpha_tested_diffuse_binding.texture = &mutable_diffuse;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "depth_only_indexed_alpha_tested_texture_description_invalid",
            "mutable alpha-tested texture rejected");
    request.alpha_tested_diffuse_binding.texture = &diffuse;

    TextureDescription unsupported_format = sampled_description();
    unsupported_format.format = TextureFormat::r32_sfloat;
    FakeTexture unsupported_diffuse(Backend::Vulkan, unsupported_format);
    request.alpha_tested_diffuse_binding.texture = &unsupported_diffuse;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "depth_only_indexed_alpha_tested_texture_description_invalid",
            "unsupported alpha-tested texture format rejected");
    request.alpha_tested_diffuse_binding.texture = &diffuse;

    SamplerDescription invalid_sampler_description;
    invalid_sampler_description.max_lod = -1.0F;
    FakeSampler invalid_sampler(Backend::Vulkan, invalid_sampler_description);
    request.alpha_tested_diffuse_binding.sampler = &invalid_sampler;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "depth_only_indexed_alpha_tested_sampler_lod_invalid",
            "malformed alpha-tested sampler rejected");
    request.alpha_tested_diffuse_binding.sampler = &sampler;

    request.alpha_tested_material_binding.range_bytes = stock_shadow_caster_material_bytes - 1U;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_alpha_tested_material_alignment_invalid",
            "short alpha-tested material range rejected");
    request.alpha_tested_material_binding.range_bytes = stock_shadow_caster_material_bytes;
    request.alpha_tested_material_binding.offset_bytes = 1U;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_alpha_tested_material_alignment_invalid",
            "unaligned alpha-tested material offset rejected");
    request.alpha_tested_material_binding.offset_bytes = 256U;

    FakeBuffer short_material(Backend::Vulkan,
                              {256U, BufferUsage::uniform, BufferMemory::host_visible,
                               BufferMutability::mutable_data});
    request.alpha_tested_material_binding.buffer = &short_material;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_alpha_tested_material_range_invalid",
            "out-of-range alpha-tested material view rejected");
    request.alpha_tested_material_binding.buffer = &material;

    FakeBuffer wrong_material_usage(Backend::Vulkan,
                                    {512U, BufferUsage::vertex,
                                     BufferMemory::host_visible,
                                     BufferMutability::mutable_data});
    request.alpha_tested_material_binding.buffer = &wrong_material_usage;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_alpha_tested_material_usage_invalid",
            "non-uniform alpha-tested material buffer rejected");
    request.alpha_tested_material_binding.buffer = &material;

    pipeline.resources.pop_back();
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_pipeline_state_invalid",
            "incomplete alpha-tested resource declaration rejected");
    pipeline = alpha_tested_depth_only_pipeline_fixture();
    request.pipeline = &pipeline;

    pipeline.resources[1].binding = 1U;
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_pipeline_state_invalid",
            "obsolete s1 alpha-shadow sampler binding rejected");
    pipeline = alpha_tested_depth_only_pipeline_fixture();
    request.pipeline = &pipeline;

    pipeline.shaders.pop_back();
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_shader_pair_invalid",
            "alpha-tested vertex-only pipeline rejected");
    pipeline = alpha_tested_depth_only_pipeline_fixture();
    request.pipeline = &pipeline;

    DrawPacket skinned_packet = skinned_packet_fixture();
    skinned_packet.flags.depth_test = true;
    skinned_packet.flags.depth_write = true;
    PipelineProgram skinned_pipeline = alpha_tested_depth_only_pipeline_fixture();
    skinned_pipeline.vertex_layout.stride = 19U * sizeof(float);
    FakeBuffer skinned_vertices(Backend::Vulkan,
                                {228U, BufferUsage::vertex, BufferMemory::device_local,
                                 BufferMutability::mutable_data});
    auto skinned_request = alpha_tested_depth_only_request_fixture(
        skinned_packet, skinned_pipeline, skinned_vertices, indices, diffuse, sampler,
        material);
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, skinned_request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "depth_only_indexed_alpha_tested_static_only",
            "skinned alpha-tested depth-only draw rejected");

    request.material_mode = DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::opaque;
    pipeline = depth_only_pipeline_fixture();
    request.pipeline = &pipeline;
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.alpha_tested_diffuse_binding = {&diffuse, &sampler};
    request.alpha_tested_material_binding = {
        &material, 256U, stock_shadow_caster_material_bytes};
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "depth_only_indexed_resource_binding_unexpected",
            "opaque depth-only draw rejects alpha resource authority");
    request.resource_authority = IndexedResourceAuthority::packet_contract;
    request.alpha_tested_diffuse_binding = {};
    request.alpha_tested_material_binding = {};
    require(validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic) ==
                DepthOnlyIndexedStaticMeshDrawStatus::ready,
            "opaque depth-only contract remains resource-free");
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

    requests[1].shader_authority =
        IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
    requests[1].camera_frame.reset();
    require(validate_indexed_static_mesh_batch_description(
                target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_binding_missing",
            "mixed native and portable batch validates each authority binding");
    requests[1] = request_fixture(second_packet, pipeline, vertices, indices);

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
    FakeTexture resolve_target(Backend::Vulkan, target_description());
    multisample_description.resolve_target = &resolve_target;
    multisample_description.capture_rgba8 = false;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "four-sample batch accepts a retained matching resolve target without CPU capture");
    multisample_description.resolve_target = nullptr;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_resolve_target_missing",
            "four-sample batch without capture rejects a missing resolve target");
    multisample_description.resolve_target = &multisample_target;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_batch_resolve_alias",
            "batch resolve rejects source aliasing before execution");
    multisample_description.resolve_target = &resolve_target;
    TextureDescription malformed_resolve_description = target_description();
    malformed_resolve_description.width = 15U;
    FakeTexture malformed_resolve(Backend::Vulkan,
                                  malformed_resolve_description);
    multisample_description.resolve_target = &malformed_resolve;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_resolve_description_mismatch",
            "batch resolve rejects mismatched retained dimensions");
    malformed_resolve_description = target_description();
    malformed_resolve_description.usage = TextureUsage::color_attachment;
    FakeTexture malformed_usage(Backend::Vulkan,
                                malformed_resolve_description);
    multisample_description.resolve_target = &malformed_usage;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_resolve_usage_invalid",
            "batch resolve rejects a destination that cannot be presented");
    FakeTexture foreign_resolve(Backend::D3D12, target_description());
    multisample_description.resolve_target = &foreign_resolve;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_resolve_backend_mismatch",
            "batch resolve rejects a foreign backend before execution");
    IndexedStaticMeshBatchDescription one_sample_resolve = description;
    one_sample_resolve.resolve_target = &resolve_target;
    require(validate_indexed_static_mesh_batch_description(
                target, one_sample_resolve, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_resolve_source_samples_invalid",
            "one-sample source rejects an unexpected resolve target");
    TextureDescription unsupported_source_description = target_description();
    unsupported_source_description.samples = 4U;
    unsupported_source_description.format = TextureFormat::r32_sfloat;
    FakeTexture unsupported_source(Backend::Vulkan,
                                   unsupported_source_description);
    TextureDescription unsupported_destination_description =
        unsupported_source_description;
    unsupported_destination_description.samples = 1U;
    FakeTexture unsupported_destination(
        Backend::Vulkan, unsupported_destination_description);
    multisample_description.resolve_target = &unsupported_destination;
    require(validate_indexed_static_mesh_batch_description(
                unsupported_source, multisample_description, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_static_mesh_target_format_unsupported",
            "batch target validation rejects a non-color source before resolve work");
    multisample_description.resolve_target = &resolve_target;
    multisample_description.capture_rgba8 = true;
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
                IndexedStaticMeshBatchStatus::ready,
            "empty indexed batch is an explicit clear-only color frame");
    description.draws = requests;
    std::vector<IndexedStaticMeshDrawRequest> oversized(max_indexed_static_mesh_batch_draws + 1U,
                                                        requests[0]);
    description.draws = oversized;
    require(validate_indexed_static_mesh_batch_description(target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_batch_limit",
            "over-limit indexed batch rejected");
}

void validates_cube_target_subresource_contract() {
    FakeTexture target(Backend::Vulkan, target_description());
    IndexedStaticMeshBatchDescription description;
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_batch_description(
                target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "default 2D target subresource remains valid");

    TextureTargetSubresource unexpected_face;
    unexpected_face.cube_face = CubeFace::positive_x;
    description.target_subresource = unexpected_face;
    require(validate_indexed_static_mesh_batch_description(
                target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_cube_face_unexpected",
            "2D target rejects a cube face");

    description.target_subresource = {};
    description.target_subresource.array_layer = 1U;
    require(validate_indexed_static_mesh_batch_description(
                target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_target_subresource_layer_out_of_range",
            "2D target rejects an out-of-range layer");

    description.target_subresource = {};
    description.target_subresource.mip_level = 1U;
    require(validate_indexed_static_mesh_batch_description(
                target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_target_subresource_mip_out_of_range",
            "2D target rejects an out-of-range mip");

    TextureDescription cube_description = target_description();
    cube_description.array_layers = 2U;
    cube_description.shape = TextureShape::texture_cube;
    FakeTexture cube_target(Backend::Vulkan, cube_description);
    description.target_subresource = {0U, 1U, CubeFace::negative_z};
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "cube target accepts an explicit face and logical cube layer");
    require(texture_target_physical_array_layer(
                cube_description, description.target_subresource) == 11U,
            "cube target maps logical layer and face to its physical layer");

    description.target_subresource = {};
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_cube_face_missing",
            "cube target rejects a missing face");

    description.target_subresource = {0U, 0U, static_cast<CubeFace>(6U)};
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_cube_face_invalid",
            "cube target rejects an invalid face");

    description.target_subresource = {0U, 2U, CubeFace::positive_x};
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_cube_layer_out_of_range",
            "cube target rejects an out-of-range logical cube layer");

    description.target_subresource = {1U, 0U, CubeFace::positive_x};
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_cube_mip_out_of_range",
            "cube target rejects an out-of-range mip");

    TextureDescription mipped_cube_description = cube_description;
    mipped_cube_description.mip_levels = 2U;
    FakeTexture mipped_cube_target(Backend::Vulkan, mipped_cube_description);
    require(validate_indexed_static_mesh_batch_description(
                mipped_cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_target_cube_mip_unsupported",
            "cube target rejects a nonzero mip in the first slice");

    TextureDescription multisample_cube_description = cube_description;
    multisample_cube_description.samples = 4U;
    FakeTexture multisample_cube_target(Backend::Vulkan,
                                        multisample_cube_description);
    description.target_subresource = {0U, 0U, CubeFace::positive_x};
    require(validate_indexed_static_mesh_batch_description(
                multisample_cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_static_mesh_target_cube_samples_unsupported",
            "cube target requires one sample");

    description.load_color = true;
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code == "indexed_static_mesh_batch_cube_load_unsupported",
            "cube target rejects loading a prior color slice");

    description.load_color = false;
    FakeTexture resolve_target(Backend::Vulkan, target_description());
    description.resolve_target = &resolve_target;
    require(validate_indexed_static_mesh_batch_description(
                cube_target, description, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_cube_resolve_unsupported",
            "cube target rejects a resolve target");

    TextureTargetSubresource ordinary_array_subresource;
    ordinary_array_subresource.array_layer = 5U;
    TextureDescription ordinary_array = target_description();
    ordinary_array.array_layers = texture_cube_face_count;
    require(texture_target_physical_array_layer(
                ordinary_array, ordinary_array_subresource) == 5U,
            "2D arrays retain direct physical layer mapping");
}

void validates_overlay_line_batch_contract() {
    PipelineProgram pipeline = overlay_pipeline_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(
        Backend::Vulkan,
        {6U * sizeof(OverlayLineVertex), BufferUsage::vertex,
         BufferMemory::host_visible, BufferMutability::mutable_data});
    OverlayLineDrawRequest request;
    request.pipeline = &pipeline;
    request.vertex_buffer = &vertices;
    request.vertex_count = 6U;
    Diagnostic diagnostic;
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "valid RGB overlay line contract accepted");

    IndexedStaticMeshBatchDescription batch;
    const std::array overlays = {request};
    batch.overlay_draws = overlays;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "overlay-only batch accepted");

    TextureDescription invalid_target_description = target_description();
    invalid_target_description.usage = TextureUsage::transfer_source;
    FakeTexture invalid_target(Backend::Vulkan, invalid_target_description);
    require(validate_indexed_static_mesh_batch_description(
                invalid_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_usage_invalid",
            "overlay-only batch rejects a target without color-attachment usage");

    DrawPacket first_packet = packet_fixture();
    DrawPacket second_packet = packet_fixture();
    PipelineProgram scene_pipeline = pipeline_fixture();
    FakeBuffer scene_vertices(
        Backend::Vulkan,
        {132U, BufferUsage::vertex, BufferMemory::device_local,
         BufferMutability::immutable});
    FakeBuffer scene_indices(
        Backend::Vulkan,
        {6U, BufferUsage::index, BufferMemory::device_local,
         BufferMutability::immutable});
    const std::array scene_draws = {
        request_fixture(first_packet, scene_pipeline, scene_vertices,
                        scene_indices),
        request_fixture(second_packet, scene_pipeline, scene_vertices,
                        scene_indices),
    };
    batch.draws = scene_draws;
    request.scene_position = 0U;
    const std::array first_overlay = {request};
    batch.overlay_draws = first_overlay;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "overlay can execute before the first ordinary scene draw");
    request.scene_position = 1U;
    const std::array middle_overlay = {request};
    batch.overlay_draws = middle_overlay;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "overlay can execute at an interior scene position");
    request.scene_position = 3U;
    const std::array invalid_position = {request};
    batch.overlay_draws = invalid_position;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_scene_position_invalid",
            "overlay scene position is range checked");
    OverlayLineDrawRequest later = request;
    later.scene_position = 1U;
    OverlayLineDrawRequest earlier = request;
    earlier.scene_position = 0U;
    const std::array unsorted = {later, earlier};
    batch.overlay_draws = unsorted;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_scene_order_invalid",
            "overlay scene positions must be nondecreasing");
    request.scene_position = std::numeric_limits<std::uint32_t>::max();
    batch.draws = {};
    const std::array restored_overlays = {request};
    batch.overlay_draws = restored_overlays;

    request.vertex_count = 5U;
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_vertex_count_invalid",
            "incomplete line pair rejected");
    request.vertex_count = 6U;
    request.vertex_offset_bytes = sizeof(OverlayLineVertex);
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_vertex_range_invalid",
            "overlay buffer overrun rejected");
    request.vertex_offset_bytes = 0U;
    request.matrices.world[0] = std::numeric_limits<float>::quiet_NaN();
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_matrix_non_finite",
            "non-finite overlay matrix rejected");
    request.matrices.world = apex::scene::identity_matrix;

    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    pipeline.depth.test_enabled = true;
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_depth_state_invalid",
            "partial normal-depth editor overlay rejected");
    pipeline.depth.write_enabled = true;
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "normal-depth scene-finished line overlay accepted");
    pipeline.depth.compare = PipelineCompareOperation::less;
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                    IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_depth_state_invalid",
            "non-native depth comparison rejected for line overlays");
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.depth.compare = PipelineCompareOperation::less_or_equal;
    pipeline.targets.has_depth = false;
    pipeline.targets.depth = {};
    pipeline.raster.fill = PipelineFillMode::solid;
    require(validate_overlay_line_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_topology_invalid",
            "triangle topology rejected for line overlay");
    pipeline.raster.fill = PipelineFillMode::wireframe;

    FakeDepthAttachment depth(
        Backend::Vulkan,
        {16U, 16U, 1U, DepthAttachmentFormat::d32_float});
    batch.depth_attachment = &depth;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "overlay_line_pipeline_depth_target_mismatch",
            "overlay pipeline depth metadata must match the shared pass");
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "depth-off overlay can share a pass with a depth attachment");

    std::vector<OverlayLineDrawRequest> too_many(max_overlay_line_draws + 1U,
                                                 request);
    batch.overlay_draws = too_many;
    require(validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "overlay_line_batch_limit",
            "over-limit overlay batch rejected before execution");
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
    plan.items.push_back({mesh_id, 0U, 0U, 0.0F, false, false, true, {}, {},
                          {root_id, mesh_id}, {}});
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

void validates_selected_mesh_draw_contract() {
    DrawPacket packet = packet_fixture();
    packet.flags.selected = true;
    PipelineProgram pipeline = selected_mesh_pipeline_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan,
                        {132U, BufferUsage::vertex,
                         BufferMemory::device_local,
                         BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan,
                       {6U, BufferUsage::index,
                        BufferMemory::device_local,
                        BufferMutability::immutable});
    FakeBuffer color(Backend::Vulkan,
                     {selected_mesh_color_view_bytes, BufferUsage::uniform,
                      BufferMemory::host_visible,
                      BufferMutability::mutable_data});
    SelectedMeshDrawRequest selected;
    selected.packet = &packet;
    selected.pipeline = &pipeline;
    selected.vertex_buffer = &vertices;
    selected.index_buffer = &indices;
    selected.color_buffer = &color;
    selected.color_range_bytes = selected_mesh_color_view_bytes;
    Diagnostic diagnostic;
    require(validate_selected_mesh_draw_request(target, selected, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "recovered selected-mesh request accepted");

    const std::array selected_draws = {selected};
    IndexedStaticMeshBatchDescription batch;
    batch.selected_mesh_draws = selected_draws;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "append-position selected-mesh batch accepted");

    TextureDescription invalid_target_description = target_description();
    invalid_target_description.width = 0U;
    FakeTexture invalid_target(Backend::Vulkan, invalid_target_description);
    require(validate_indexed_static_mesh_batch_description(
                invalid_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_invalid",
            "selected-only batch rejects a zero-width color target");

    selected.scene_position = 1U;
    const std::array out_of_range = {selected};
    batch.selected_mesh_draws = out_of_range;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "selected_mesh_scene_position_invalid",
            "selected-mesh scene position is range checked");

    selected.scene_position = 0U;
    DrawPacket skinned = packet;
    skinned.primitive = DrawPrimitiveKind::skinned_mesh;
    skinned.vertex_stride_floats = 19U;
    selected.packet = &skinned;
    require(validate_selected_mesh_draw_request(target, selected, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code == "selected_mesh_static_contract_required",
            "selected-mesh shader rejects skinned input");

    selected.packet = &packet;
    packet.index_count = 2U;
    require(validate_selected_mesh_draw_request(target, selected, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "selected_mesh_geometry_count_invalid",
            "truncated selected triangle is rejected");
    packet.index_count = 3U;

    packet.vertex_offset = 1U;
    require(validate_selected_mesh_draw_request(target, selected, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "selected_mesh_buffer_range_invalid",
            "selected vertex span cannot exceed its buffer");
    packet.vertex_offset = 0U;

    selected.color_range_bytes = 16U;
    require(validate_selected_mesh_draw_request(target, selected, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "selected_mesh_color_buffer_invalid",
            "selected color rejects a truncated uniform view");
    selected.color_range_bytes = selected_mesh_color_view_bytes;

    pipeline.blend.enabled = true;
    require(validate_selected_mesh_draw_request(target, selected, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "selected_mesh_blend_state_invalid",
            "selected pass rejects an approximate blended state");
}

void validates_stock_vulkan_abi_probe_authority() {
    FakeTexture target(Backend::Vulkan, target_description());
    PipelineProgram pipeline = vulkan_abi_probe_pipeline_fixture();
    DrawPacket packet = packet_fixture();
    packet.shader_execution_supported = false;

    FakeBuffer vertices(
        Backend::Vulkan,
        {3U * stock_ks_per_pixel_vertex_stride_bytes, BufferUsage::vertex,
         BufferMemory::device_local, BufferMutability::immutable});
    FakeBuffer indices(
        Backend::Vulkan,
        {3U * sizeof(std::uint16_t), BufferUsage::index,
         BufferMemory::device_local, BufferMutability::immutable});
    std::array<FakeBuffer, 5U> uniforms = {
        FakeBuffer(Backend::Vulkan,
                   {256U, BufferUsage::uniform, BufferMemory::host_visible,
                    BufferMutability::mutable_data}),
        FakeBuffer(Backend::Vulkan,
                   {256U, BufferUsage::uniform, BufferMemory::host_visible,
                    BufferMutability::mutable_data}),
        FakeBuffer(Backend::Vulkan,
                   {256U, BufferUsage::uniform, BufferMemory::host_visible,
                    BufferMutability::mutable_data}),
        FakeBuffer(Backend::Vulkan,
                   {256U, BufferUsage::uniform, BufferMemory::host_visible,
                    BufferMutability::mutable_data}),
        FakeBuffer(Backend::Vulkan,
                   {256U, BufferUsage::uniform, BufferMemory::host_visible,
                    BufferMutability::mutable_data}),
    };
    FakeTexture diffuse(
        Backend::Vulkan,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm,
         TextureUsage::sampled | TextureUsage::transfer_destination,
         TextureMemory::device_local, TextureMutability::immutable});
    const DepthAttachmentDescription shadow_description{
        32U, 32U, 1U, DepthAttachmentFormat::d32_float, true};
    FakeDepthAttachment shadow0(Backend::Vulkan, shadow_description);
    FakeDepthAttachment shadow1(Backend::Vulkan, shadow_description);
    FakeDepthAttachment shadow2(Backend::Vulkan, shadow_description);

    SamplerDescription linear_description;
    linear_description.min_filter = SamplerFilter::anisotropic;
    linear_description.mag_filter = SamplerFilter::anisotropic;
    linear_description.mip_filter = SamplerFilter::anisotropic;
    linear_description.max_anisotropy = 4.0F;
    linear_description.max_lod = std::numeric_limits<float>::max();
    FakeSampler linear(Backend::Vulkan, linear_description);
    SamplerDescription shadow_sampler_description;
    shadow_sampler_description.min_filter = SamplerFilter::linear;
    shadow_sampler_description.mag_filter = SamplerFilter::linear;
    shadow_sampler_description.mip_filter = SamplerFilter::nearest;
    shadow_sampler_description.address_u = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.address_v = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.address_w = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.compare = SamplerCompare::less;
    shadow_sampler_description.max_lod = std::numeric_limits<float>::max();
    FakeSampler shadow_sampler(Backend::Vulkan, shadow_sampler_description);

    StockKsPerPixelVulkanAbiProbeDrawBinding binding;
    for (std::size_t index = 0U; index < uniforms.size(); ++index) {
        binding.uniform_buffers[index] = {&uniforms[index], 0U, 256U};
    }
    binding.diffuse_texture = &diffuse;
    binding.shadow_maps = {&shadow0, &shadow1, &shadow2};
    binding.linear_sampler = &linear;
    binding.shadow_sampler = &shadow_sampler;

    IndexedStaticMeshDrawRequest request;
    request.packet = &packet;
    request.pipeline = &pipeline;
    request.vertex_buffer = &vertices;
    request.index_buffer = &indices;
    request.shader_authority = IndexedShaderAuthority::
        explicit_stock_ks_per_pixel_vulkan_abi_probe;
    request.stock_ks_per_pixel_vulkan_abi_probe = &binding;

    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_draw_request(
                target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            diagnostic.code.empty()
                ? "complete Vulkan ABI probe passes common preflight"
                : diagnostic.code.c_str());
    require(!request.camera_frame.has_value(),
            "Vulkan ABI probe does not require portable draw matrices");

    const std::array draws = {request};
    IndexedStaticMeshBatchDescription batch;
    batch.draws = draws;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_batch_unsupported",
            "Vulkan ABI probe batch execution remains explicitly staged");

    auto missing_uniform = binding;
    missing_uniform.uniform_buffers[2U].buffer = nullptr;
    auto malformed_request = request;
    malformed_request.stock_ks_per_pixel_vulkan_abi_probe = &missing_uniform;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_uniform_missing",
            "Vulkan ABI probe rejects a missing recovered uniform slot");

    auto truncated_uniform = binding;
    truncated_uniform.uniform_buffers[4U].range_bytes = 32U;
    malformed_request.stock_ks_per_pixel_vulkan_abi_probe =
        &truncated_uniform;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_uniform_view_invalid",
            "Vulkan ABI probe rejects a truncated uniform view");

    FakeBuffer d3d_uniform(
        Backend::D3D12,
        {256U, BufferUsage::uniform, BufferMemory::host_visible,
         BufferMutability::mutable_data});
    auto foreign_uniform = binding;
    foreign_uniform.uniform_buffers[0U].buffer = &d3d_uniform;
    malformed_request.stock_ks_per_pixel_vulkan_abi_probe = &foreign_uniform;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_uniform_backend_mismatch",
            "Vulkan ABI probe rejects a non-Vulkan uniform buffer");

    PipelineProgram drifted_pipeline = pipeline;
    drifted_pipeline.resources[5U].binding = 5U;
    malformed_request = request;
    malformed_request.pipeline = &drifted_pipeline;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_pipeline_resource_binding_mismatch",
            "Vulkan ABI probe rejects recovered descriptor drift");

    malformed_request = request;
    malformed_request.sampled_binding.texture = &diffuse;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_portable_binding_overlap",
            "Vulkan ABI probe rejects portable binding overlap");

    PipelineProgram wrong_layout = pipeline;
    wrong_layout.vertex_layout.attributes[3U].location = 4U;
    malformed_request = request;
    malformed_request.pipeline = &wrong_layout;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_vertex_layout_invalid",
            "Vulkan ABI probe rejects an approximate vertex layout");

    StockKsPerPixelNativeDrawBinding native_overlap;
    malformed_request = request;
    malformed_request.stock_ks_per_pixel_native = &native_overlap;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_binding_unexpected",
            "Vulkan ABI probe rejects installed-native binding overlap");

    malformed_request = request;
    malformed_request.shader_authority =
        IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_binding_unexpected",
            "installed-native authority rejects Vulkan probe binding overlap");

    SamplerDescription wrong_shadow_description = shadow_sampler_description;
    wrong_shadow_description.compare = SamplerCompare::less_equal;
    FakeSampler wrong_shadow(Backend::Vulkan, wrong_shadow_description);
    auto wrong_samplers = binding;
    wrong_samplers.shadow_sampler = &wrong_shadow;
    malformed_request = request;
    malformed_request.stock_ks_per_pixel_vulkan_abi_probe = &wrong_samplers;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_abi_probe_sampler_contract_invalid",
            "Vulkan ABI probe rejects comparison-sampler drift");

    StockKsPerPixelVulkanSourceProgramResult source_program =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(
            StockKsPerPixelVariant::base);
    require(source_program.ok(),
            "built-in Vulkan source-equivalent program fixture");
    StockKsPerPixelVulkanSourceDrawBinding source_binding;
    source_binding.program = &*source_program.program;
    source_binding.resources = binding;
    IndexedStaticMeshDrawRequest source_request = request;
    source_request.pipeline = &source_program.program->pipeline();
    source_request.shader_authority = IndexedShaderAuthority::
        explicit_stock_ks_per_pixel_vulkan_source_equivalent;
    source_request.stock_ks_per_pixel_vulkan_abi_probe = nullptr;
    source_request.stock_ks_per_pixel_vulkan_source = &source_binding;
    require(validate_indexed_static_mesh_draw_request(
                target, source_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            diagnostic.code.empty()
                ? "complete Vulkan source-equivalent draw passes preflight"
                : diagnostic.code.c_str());

    DrawPacket source_packet_with_texture_identity = packet;
    source_packet_with_texture_identity.resources.push_back(
        {"txDiffuse", 0U, 0U, "body.dds"});
    source_request.packet = &source_packet_with_texture_identity;
    require(validate_indexed_static_mesh_draw_request(
                target, source_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            diagnostic.code.empty()
                ? "source-equivalent draw accepts declarative packet texture identity"
                : diagnostic.code.c_str());
    source_request.packet = &packet;

    const std::array source_draws = {source_request, source_request};
    batch.draws = source_draws;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            diagnostic.code.empty()
                ? "Vulkan source-equivalent batches pass common preflight"
                : diagnostic.code.c_str());

    PipelineProgram portable_pipeline = pipeline_fixture();
    IndexedStaticMeshDrawRequest portable_request =
        request_fixture(packet, portable_pipeline, vertices, indices);
    portable_request.shader_authority =
        IndexedShaderAuthority::explicit_pipeline;
    const std::array mixed_draws = {source_request, portable_request};
    batch.draws = mixed_draws;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            diagnostic.code.empty()
                ? "Vulkan source-equivalent and portable draws can share a batch"
                : diagnostic.code.c_str());

    PipelineProgram copied_source_pipeline =
        source_program.program->pipeline();
    malformed_request = source_request;
    malformed_request.pipeline = &copied_source_pipeline;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_source_owner_mismatch",
            "a copied pipeline cannot escape the immutable source owner");

    auto missing_source_uniform = source_binding;
    missing_source_uniform.resources.uniform_buffers[3U].buffer = nullptr;
    malformed_request = source_request;
    malformed_request.stock_ks_per_pixel_vulkan_source =
        &missing_source_uniform;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_source_uniform_missing",
            "source-equivalent draw rejects a missing native uniform slot");

    malformed_request = source_request;
    malformed_request.stock_ks_per_pixel_vulkan_abi_probe = &binding;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_source_binding_overlap",
            "source-equivalent authority rejects probe binding overlap");

    malformed_request = source_request;
    malformed_request.sampled_binding.texture = &diffuse;
    require(validate_indexed_static_mesh_draw_request(
                target, malformed_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_stock_vulkan_source_portable_binding_overlap",
            "source-equivalent authority rejects portable binding overlap");
}

void visits_generalized_batch_in_requested_phase_order() {
    std::array<IndexedStaticMeshDrawRequest, 2U> scene_draws{};
    SelectedMeshDrawRequest selected;
    selected.scene_position = 1U;
    const std::array selected_draws = {selected};
    OverlayLineDrawRequest first_axis;
    first_axis.scene_position = 1U;
    OverlayLineDrawRequest second_axis;
    second_axis.scene_position = 1U;
    OverlayLineDrawRequest late_axis;
    const std::array overlays = {first_axis, second_axis, late_axis};
    IndexedStaticMeshBatchDescription batch;
    batch.draws = scene_draws;
    batch.selected_mesh_draws = selected_draws;
    batch.overlay_draws = overlays;

    std::vector<std::string> order;
    std::size_t scene_index = 0U;
    std::size_t overlay_index = 0U;
    const bool visited = visit_indexed_static_mesh_batch_draws(
        batch,
        [&](const IndexedStaticMeshDrawRequest&) {
            order.push_back("scene-" + std::to_string(scene_index++));
            return true;
        },
        [&](const SelectedMeshDrawRequest&) {
            order.emplace_back("selected");
            return true;
        },
        [&](const OverlayLineDrawRequest&) {
            order.push_back("overlay-" + std::to_string(overlay_index++));
            return true;
        });
    require(visited && order == std::vector<std::string>({
                                   "scene-0", "selected", "overlay-0",
                                   "overlay-1", "scene-1", "overlay-2"}),
            "batch visitor preserves selected, view-axis, transparent, and late-overlay order");
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
        validates_directional_shadow_receiver_contract();
        validates_recovered_stock_directional_shadow_abi();
        validates_portable_normal_map_contract();
        validates_portable_maps_contract();
        validates_portable_multimap_reflection_contract();
        validates_portable_detail_stack_contract();
        validates_portable_damage_stack_contract();
        validates_portable_damage_dust_alpha_contract();
        rejects_invalid_depth_attachment_descriptions();
        validates_depth_only_indexed_contract();
        validates_alpha_tested_depth_only_contract();
        rejects_invalid_depth_contract();
        validates_ordered_indexed_batch_contract();
        validates_cube_target_subresource_contract();
        validates_overlay_line_batch_contract();
        rejects_static_indexed_limits_and_ownership();
        rejects_staged_draw_packet();
        validates_selected_mesh_draw_contract();
        validates_stock_vulkan_abi_probe_authority();
        visits_generalized_batch_in_requested_phase_order();
        std::cout << "indexed draw tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "indexed draw tests failed: " << error.what() << '\n';
        return 1;
    }
}
