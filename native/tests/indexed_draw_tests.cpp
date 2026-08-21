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
    pipeline.vertex_layout.stride = 12U;
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
    packet.vertex_stride_floats = 3U;
    packet.shader_execution_supported = true;
    packet.flags = {false, false, false, false, false, false, false, false};
    return packet;
}

TextureDescription target_description() {
    return {16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
            TextureUsage::color_attachment | TextureUsage::transfer_source,
            TextureMemory::device_local, TextureMutability::mutable_data};
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
    FakeBuffer vertices(Backend::Vulkan, {36U, BufferUsage::vertex, BufferMemory::device_local,
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
    FakeBuffer d3d_vertices(Backend::D3D12, {36U, BufferUsage::vertex, BufferMemory::device_local,
                                             BufferMutability::immutable});
    FakeBuffer d3d_indices(Backend::D3D12, {6U, BufferUsage::index, BufferMemory::device_local,
                                            BufferMutability::immutable});
    const auto d3d_request = request_fixture(packet, pipeline, d3d_vertices, d3d_indices);
    require(validate_indexed_static_mesh_draw_request(d3d_target, d3d_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::ready,
            "D3D12 camera clip contract accepted");
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
    FakeBuffer vertices(Backend::Vulkan, {36U, BufferUsage::vertex, BufferMemory::device_local,
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
            "multisample depth attachment rejected explicitly");

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
    FakeBuffer vertices(Backend::Vulkan, {36U, BufferUsage::vertex, BufferMemory::device_local,
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

void rejects_static_indexed_limits_and_ownership() {
    PipelineProgram pipeline = pipeline_fixture();
    DrawPacket packet = packet_fixture();
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {36U, BufferUsage::vertex, BufferMemory::device_local,
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
    packet.vertex_stride_floats = 4U;
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_stride_mismatch",
            "float32 and byte stride mismatch rejected");
    packet.vertex_stride_floats = 3U;
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
    vertices = FakeBuffer(Backend::Vulkan, {36U, BufferUsage::vertex, BufferMemory::device_local,
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
    FakeTexture target(Backend::Vulkan, target_description());
    FakeBuffer vertices(Backend::Vulkan, {132U, BufferUsage::vertex, BufferMemory::device_local,
                                          BufferMutability::immutable});
    FakeBuffer indices(Backend::Vulkan, {6U, BufferUsage::index, BufferMemory::device_local,
                                         BufferMutability::immutable});
    Diagnostic diagnostic;
    const auto request = request_fixture(built.packets.front(), pipeline, vertices, indices);
    require(validate_indexed_static_mesh_draw_request(target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_shader_execution_staged",
            "staged packet rejected before execution");
}

} // namespace

int main() {
    try {
        accepts_bounded_static_indexed_contract();
        accepts_explicit_d32_depth_contract();
        rejects_invalid_depth_attachment_descriptions();
        rejects_invalid_depth_contract();
        rejects_static_indexed_limits_and_ownership();
        rejects_staged_draw_packet();
        std::cout << "indexed draw tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "indexed draw tests failed: " << error.what() << '\n';
        return 1;
    }
}
