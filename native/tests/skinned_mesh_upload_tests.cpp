#include "apex/render/skinned_mesh_upload.hpp"

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

class FakeDevice final : public Device {
public:
    const DeviceInfo& info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        descriptions.push_back(description);
        initial_bytes.emplace_back(initial_data.begin(), initial_data.end());
        if (create_status != BufferStatus::ready)
            return {create_status, {"fake_buffer_failure", "fake buffer failure"}, nullptr};
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }

    BufferUpdateResult update_buffer(Buffer&, std::uint64_t offset,
                                     std::span<const std::byte> data) override {
        ++update_calls;
        if (update_status != BufferStatus::ready)
            return {update_status, {"fake_update_failure", "fake update failure"}};
        if (offset != 0U) return {BufferStatus::invalid_description,
                                  {"fake_update_offset", "unexpected update offset"}};
        last_update.assign(data.begin(), data.end());
        return {BufferStatus::ready, {}};
    }

    TextureResult create_texture(const TextureDescription&, const TextureUploadPlan&) override {
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

    BufferStatus create_status = BufferStatus::ready;
    BufferStatus update_status = BufferStatus::ready;
    std::size_t update_calls = 0U;
    std::vector<BufferDescription> descriptions;
    std::vector<std::vector<std::byte>> initial_bytes;
    std::vector<std::byte> last_update;

private:
    DeviceInfo info_{Backend::Vulkan, "fake", "test", 1U, 0U, 0U, 0U, 0U, true};
};

apex::formats::Kn5Node mesh_fixture() {
    apex::formats::Kn5Node mesh;
    mesh.type = 3U;
    mesh.kind = "skinnedMesh";
    mesh.vertexStride = 19U;
    mesh.bones.push_back({"Bone", apex::scene::identity_matrix});
    mesh.vertices.resize(3U * 19U, 0.0F);
    const std::array<std::array<float, 3>, 3> positions = {{{-0.75F, -0.75F, 0.0F},
                                                             {0.75F, -0.75F, 0.0F},
                                                             {0.0F, 0.75F, 0.0F}}};
    for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
        const std::size_t offset = vertex * 19U;
        for (std::size_t axis = 0U; axis < 3U; ++axis) mesh.vertices[offset + axis] = positions[vertex][axis];
        mesh.vertices[offset + 5U] = 1.0F;
        mesh.vertices[offset + 8U] = 1.0F;
        mesh.vertices[offset + 12U] = 1.0F;
        mesh.vertices[offset + 15U] = 0.0F;
        mesh.vertices[offset + 16U] = 0.0F;
        mesh.vertices[offset + 17U] = 0.0F;
        mesh.vertices[offset + 18U] = 0.0F;
    }
    mesh.indices = {0U, 1U, 2U};
    return mesh;
}

DrawPacket packet_fixture(float translation = 2.0F) {
    DrawPacket packet;
    packet.node = 7U;
    packet.primitive = DrawPrimitiveKind::skinned_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 19U;
    packet.bone_palette.push_back(apex::scene::identity_matrix);
    packet.bone_palette.front()[12] = translation;
    packet.world_matrix = apex::scene::identity_matrix;
    return packet;
}

void validates_geometry_and_influences() {
    const auto mesh = mesh_fixture();
    auto packet = packet_fixture();
    Diagnostic diagnostic;
    require(validate_skinned_mesh_upload(mesh, packet, diagnostic) == SkinnedMeshUploadStatus::ready,
            "valid skinned mesh accepted");

    auto malformed = mesh;
    malformed.vertices.pop_back();
    require(validate_skinned_mesh_upload(malformed, packet, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_geometry_malformed",
            "truncated skinned vertices rejected");

    malformed = mesh;
    malformed.vertices[15U] = 1.0F;
    require(validate_skinned_mesh_upload(malformed, packet, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_influence_invalid",
            "out-of-range bone influence rejected");

    malformed = mesh;
    malformed.vertices[18U] = 0.5F;
    require(validate_skinned_mesh_upload(malformed, packet, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_influence_invalid",
            "non-integral bone influence rejected");

    malformed = mesh;
    malformed.bones.front().transform[0] = std::numeric_limits<float>::infinity();
    require(validate_skinned_mesh_upload(malformed, packet, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_bone_non_finite",
            "non-finite inverse-bind matrices are rejected");

    malformed = mesh;
    SkinnedMeshUploadLimits short_name_limit;
    short_name_limit.max_bone_name_bytes = 3U;
    require(validate_skinned_mesh_upload(malformed, packet, short_name_limit, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_bone_name_limit",
            "oversized bone names are rejected");

    malformed = mesh;
    packet.bone_palette.clear();
    require(validate_skinned_mesh_upload(malformed, packet, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_palette_invalid",
            "missing bone palette rejected");
}

void uploads_mutable_vertices_and_immutable_indices() {
    const auto mesh = mesh_fixture();
    const auto packet = packet_fixture();
    FakeDevice device;
    const auto result = upload_skinned_mesh(device, mesh, packet);
    require(result.ok(), "skinned mesh upload succeeds");
    require(device.descriptions.size() == 2U &&
                device.descriptions[0].usage == BufferUsage::vertex &&
                device.descriptions[0].mutability == BufferMutability::mutable_data &&
                device.descriptions[0].memory == BufferMemory::device_local &&
                device.descriptions[1].usage == BufferUsage::index &&
                device.descriptions[1].mutability == BufferMutability::immutable,
            "skinned upload uses mutable vertex and immutable index buffers");
    require(result.upload->source_vertex_count() == 3U && result.upload->source_bone_count() == 1U &&
                result.upload->bind_vertices().size() == mesh.vertices.size(),
            "skinned upload owns bounded bind-pose data");
    require(device.initial_bytes[0].size() == mesh.vertices.size() * sizeof(float),
            "initial skinned vertex bytes are uploaded");
    require(std::memcmp(device.initial_bytes[0].data(), mesh.vertices.data(),
                         device.initial_bytes[0].size()) == 0,
            "initial upload preserves exact bind-pose bytes");

    PipelineProgram pipeline;
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    CameraFrame camera;
    camera.clip_space = CameraClipSpace::vulkan;
    const auto request = result.upload->make_request(pipeline, camera);
    require(request.packet == &result.upload->packet &&
                request.vertex_buffer == result.upload->vertex_buffer.get() &&
                request.index_buffer == result.upload->index_buffer.get(),
            "skinned upload creates an indexed request");
}

void prepares_commits_and_restores_bind_pose() {
    const auto mesh = mesh_fixture();
    const auto bind_packet = packet_fixture(0.0F);
    FakeDevice device;
    const auto result = upload_skinned_mesh(device, mesh, bind_packet);
    require(result.ok(), "bind-pose upload succeeds");
    const std::size_t initial_updates = device.update_calls;

    DrawPacket animated = packet_fixture(2.0F);
    const auto prepared = result.upload->prepare_pose(animated);
    require(prepared.ok() && prepared.skinned_vertices.size() == mesh.vertices.size() &&
                prepared.skinned_vertices[0] == mesh.vertices[0] + 2.0F &&
                device.update_calls == initial_updates,
            "pose preparation is backend-free and applies the palette");
    const auto committed = result.upload->commit_pose(device, animated, prepared.skinned_vertices);
    require(committed.ok() && device.update_calls == initial_updates + 1U &&
                result.upload->packet.bone_palette.front()[12] == 2.0F,
            "successful pose commit updates the buffer and stored packet");

    DrawPacket bind_pose = packet_fixture(0.0F);
    const auto restored = result.upload->update_pose(device, bind_pose);
    require(restored.ok() && restored.skinned_vertices[0] == mesh.vertices[0] &&
                result.upload->packet.bone_palette.front()[12] == 0.0F,
            "update_pose restores the no-animation bind pose");
}

void failed_commit_preserves_packet() {
    const auto mesh = mesh_fixture();
    const auto original = packet_fixture(2.0F);
    FakeDevice device;
    const auto result = upload_skinned_mesh(device, mesh, original);
    require(result.ok(), "upload for failed commit succeeds");
    DrawPacket replacement = packet_fixture(5.0F);
    const auto prepared = result.upload->prepare_pose(replacement);
    require(prepared.ok(), "replacement pose prepares");
    device.update_status = BufferStatus::upload_failed;
    const auto failed = result.upload->commit_pose(device, replacement, prepared.skinned_vertices);
    require(!failed.ok() && failed.status == SkinnedMeshUploadStatus::upload_failed &&
                result.upload->packet.bone_palette.front()[12] == 2.0F,
            "failed backend update does not replace stored packet");
}

void rejects_creation_failures_and_limits() {
    const auto mesh = mesh_fixture();
    const auto packet = packet_fixture();
    Diagnostic diagnostic;
    SkinnedMeshUploadLimits limits;
    limits.max_bones = 0U;
    require(validate_skinned_mesh_upload(mesh, packet, limits, diagnostic) ==
                SkinnedMeshUploadStatus::invalid_request &&
                diagnostic.code == "skinned_mesh_upload_limits_invalid",
            "invalid limits rejected");
    FakeDevice device;
    device.create_status = BufferStatus::allocation_failed;
    const auto failure = upload_skinned_mesh(device, mesh, packet);
    require(failure.status == SkinnedMeshUploadStatus::allocation_failed &&
                failure.diagnostic.code == "fake_buffer_failure",
            "buffer allocation failure propagated");

    auto singular_packet = packet;
    singular_packet.world_matrix[0] = 0.0F;
    singular_packet.world_matrix[5] = 0.0F;
    singular_packet.world_matrix[10] = 0.0F;
    FakeDevice preflight_device;
    const auto singular = upload_skinned_mesh(preflight_device, mesh, singular_packet);
    require(!singular.ok() && singular.status == SkinnedMeshUploadStatus::invalid_request &&
                singular.diagnostic.code == "NON_INVERTIBLE_MATRIX" &&
                preflight_device.descriptions.empty(),
            "computed skinning failure is rejected before backend allocation");
}

}  // namespace

int main() {
    try {
        validates_geometry_and_influences();
        uploads_mutable_vertices_and_immutable_indices();
        prepares_commits_and_restores_bind_pose();
        failed_commit_preserves_packet();
        rejects_creation_failures_and_limits();
        std::cout << "Skinned mesh upload tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Skinned mesh upload tests failed: " << error.what() << '\n';
        return 1;
    }
}
