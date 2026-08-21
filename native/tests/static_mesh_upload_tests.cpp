#include "apex/render/static_mesh_upload.hpp"

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
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const TextureInfo& info() const noexcept override { return info_; }

private:
    TextureInfo info_{};
};

class FakeSampler final : public Sampler {
public:
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const SamplerInfo& info() const noexcept override { return info_; }

private:
    SamplerInfo info_{};
};

class FakeShaderModule final : public ShaderModule {
public:
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const ShaderModuleInfo& info() const noexcept override { return info_; }

private:
    ShaderModuleInfo info_{};
};

class FakeDevice final : public Device {
public:
    const DeviceInfo& info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        descriptions.push_back(description);
        bytes.emplace_back(initial_data.begin(), initial_data.end());
        if (fail_status != BufferStatus::ready) {
            return {fail_status, {"fake_buffer_failure", "fake buffer failure"}, nullptr};
        }
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }

    BufferUpdateResult update_buffer(Buffer&, std::uint64_t,
                                     std::span<const std::byte>) override {
        return {BufferStatus::unsupported, {"fake_unsupported", "not used"}};
    }

    TextureResult create_texture(const TextureDescription&,
                                 const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"fake_unsupported", "not used"}, nullptr};
    }

    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"fake_unsupported", "not used"}};
    }

    TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"fake_unsupported", "not used"}, {}};
    }

    TriangleDrawResult draw_triangle_and_readback(Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"fake_unsupported", "not used"}, {}};
    }

    SamplerResult create_sampler(const SamplerDescription&) override {
        return {SamplerStatus::unsupported, {"fake_unsupported", "not used"}, nullptr};
    }

    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"fake_unsupported", "not used"}, nullptr};
    }

    void wait_idle() noexcept override {}

    BufferStatus fail_status = BufferStatus::ready;
    std::vector<BufferDescription> descriptions;
    std::vector<std::vector<std::byte>> bytes;

private:
    DeviceInfo info_{Backend::Vulkan, "fake", "test", 1U, 0U, 0U, 0U, 0U, true};
};

apex::formats::Kn5Node mesh_fixture() {
    apex::formats::Kn5Node mesh;
    mesh.type = 2U;
    mesh.kind = "mesh";
    mesh.vertexStride = 11U;
    mesh.vertices.resize(33U, 0.0F);
    mesh.vertices[0] = -0.75F;
    mesh.vertices[11] = 0.75F;
    mesh.vertices[22] = 0.0F;
    mesh.vertices[1] = -0.75F;
    mesh.vertices[12] = -0.75F;
    mesh.vertices[23] = 0.75F;
    mesh.indices = {0U, 1U, 2U};
    return mesh;
}

DrawPacket packet_fixture() {
    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.world_matrix = apex::scene::identity_matrix;
    return packet;
}

void accepts_valid_mesh_and_uploads_immutable_buffers() {
    const auto mesh = mesh_fixture();
    const auto packet = packet_fixture();
    Diagnostic diagnostic;
    require(validate_static_mesh_upload(mesh, packet, diagnostic) == StaticMeshUploadStatus::ready,
            "valid static KN5 geometry accepted");
    require(std::string(static_mesh_upload_status_name(StaticMeshUploadStatus::ready)) == "ready",
            "static mesh upload status name");

    FakeDevice device;
    const auto result = upload_static_mesh(device, mesh, packet);
    require(result.ok(), "static KN5 geometry upload succeeds");
    require(device.descriptions.size() == 2U && device.bytes.size() == 2U,
            "upload creates vertex and index buffers");
    require(device.descriptions[0].usage == BufferUsage::vertex &&
                device.descriptions[1].usage == BufferUsage::index &&
                device.descriptions[0].memory == BufferMemory::device_local &&
                device.descriptions[1].memory == BufferMemory::device_local &&
                device.descriptions[0].mutability == BufferMutability::immutable &&
                device.descriptions[1].mutability == BufferMutability::immutable,
            "upload buffers are immutable device-local resources");
    require(device.bytes[0].size() == mesh.vertices.size() * sizeof(float) &&
                device.bytes[1].size() == mesh.indices.size() * sizeof(std::uint16_t),
            "upload byte sizes match source geometry");

    PipelineProgram pipeline;
    const IndexedStaticMeshDrawRequest request = result.upload->make_request(pipeline);
    require(request.packet == &result.upload->packet && request.packet != &packet &&
                request.vertex_buffer == result.upload->vertex_buffer.get() &&
                request.index_buffer == result.upload->index_buffer.get() &&
                request.index_type == StaticMeshIndexType::uint16,
            "upload produces an indexed request-ready object");
}

void rejects_malformed_geometry_and_ranges() {
    auto mesh = mesh_fixture();
    const auto packet = packet_fixture();
    Diagnostic diagnostic;
    mesh.vertices.pop_back();
    require(validate_static_mesh_upload(mesh, packet, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_geometry_malformed",
            "truncated vertex payload rejected");

    mesh = mesh_fixture();
    mesh.indices[2] = 3U;
    require(validate_static_mesh_upload(mesh, packet, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_index_out_of_range",
            "out-of-range index rejected");

    mesh = mesh_fixture();
    mesh.vertices[4] = std::numeric_limits<float>::quiet_NaN();
    require(validate_static_mesh_upload(mesh, packet, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_vertex_non_finite",
            "non-finite vertex rejected");

    mesh = mesh_fixture();
    auto ranged_packet = packet_fixture();
    ranged_packet.vertex_offset = std::numeric_limits<std::uint32_t>::max();
    require(validate_static_mesh_upload(mesh, ranged_packet, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_packet_range_invalid",
            "overflowing vertex range rejected");
    ranged_packet = packet_fixture();
    ranged_packet.index_offset = 2U;
    require(validate_static_mesh_upload(mesh, ranged_packet, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_packet_range_invalid",
            "out-of-range index span rejected");
    ranged_packet = packet_fixture();
    ranged_packet.vertex_count = 2U;
    require(validate_static_mesh_upload(mesh, ranged_packet, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_packet_index_out_of_range",
            "index outside selected vertex span rejected");
}

void rejects_limits_and_backend_failures() {
    const auto mesh = mesh_fixture();
    const auto packet = packet_fixture();
    Diagnostic diagnostic;
    StaticMeshUploadLimits limits;
    limits.max_vertices = 2U;
    require(validate_static_mesh_upload(mesh, packet, limits, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_geometry_limit",
            "vertex count limit enforced");
    limits = {};
    limits.max_index_bytes = 2U;
    require(validate_static_mesh_upload(mesh, packet, limits, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_byte_limit",
            "index byte limit enforced");
    limits = {};
    limits.max_vertex_bytes = max_buffer_bytes + 1U;
    require(validate_static_mesh_upload(mesh, packet, limits, diagnostic) == StaticMeshUploadStatus::invalid_request &&
                diagnostic.code == "static_mesh_upload_limits_invalid",
            "invalid byte limits rejected");

    FakeDevice device;
    device.fail_status = BufferStatus::allocation_failed;
    const auto allocation_failure = upload_static_mesh(device, mesh, packet);
    require(allocation_failure.status == StaticMeshUploadStatus::allocation_failed &&
                allocation_failure.diagnostic.code == "fake_buffer_failure",
            "buffer allocation failure is propagated");
}

}  // namespace

int main() {
    try {
        accepts_valid_mesh_and_uploads_immutable_buffers();
        rejects_malformed_geometry_and_ranges();
        rejects_limits_and_backend_failures();
        std::cout << "Static mesh upload tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Static mesh upload tests failed: " << error.what() << '\n';
        return 1;
    }
}
