#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::render {

// Limits for one validated type-3 KN5 mesh. The source bind pose is copied
// into the upload so later animation frames do not depend on Kn5File lifetime.
struct SkinnedMeshUploadLimits {
    std::size_t max_vertices = max_indexed_static_mesh_vertices;
    std::size_t max_indices = max_indexed_static_mesh_indices;
    std::size_t max_bones = 1'000'000U;
    std::size_t max_bone_name_bytes = 1U << 20;
    std::uint64_t max_vertex_bytes = max_buffer_bytes;
    std::uint64_t max_index_bytes = max_buffer_bytes;
    std::uint64_t max_cpu_skin_bytes = 256ULL * 1024ULL * 1024ULL;
};

enum class SkinnedMeshUploadStatus {
    ready,
    invalid_request,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct SkinnedMeshUploadResult;
struct SkinnedMeshPoseUpdateResult;

// Vertex data is CPU-skinned into a mutable device-local vertex buffer. The
// index buffer is immutable. Neither the source KN5 node nor caller spans are
// retained after upload or update_pose returns.
struct SkinnedMeshUpload {
    DrawPacket packet;
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;

    [[nodiscard]] std::uint32_t source_vertex_count() const noexcept {
        return source_vertex_count_;
    }
    [[nodiscard]] std::uint32_t source_index_count() const noexcept {
        return source_index_count_;
    }
    [[nodiscard]] std::uint32_t source_vertex_stride_floats() const noexcept {
        return source_vertex_stride_floats_;
    }
    [[nodiscard]] std::size_t source_bone_count() const noexcept {
        return source_bone_count_;
    }
    [[nodiscard]] std::span<const float> bind_vertices() const noexcept {
        return bind_vertices_;
    }
    [[nodiscard]] std::span<const std::uint16_t> source_indices() const noexcept {
        return source_indices_;
    }

    [[nodiscard]] std::optional<IndexedStaticMeshDrawRequest> make_request(
        const DrawPacket& draw_packet, const PipelineProgram& pipeline,
        const CameraFrame& camera_frame, std::uint32_t mip_level,
        std::uint32_t array_layer, std::array<float, 4> clear_color,
        Diagnostic& diagnostic) const noexcept;

    [[nodiscard]] IndexedStaticMeshDrawRequest make_request(
        const PipelineProgram& pipeline, const CameraFrame& camera_frame,
        std::uint32_t mip_level = 0U,
        std::uint32_t array_layer = 0U,
        std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F}) const noexcept;

    // Prepare and validate the next frame without touching the backend. This
    // supports scene-level preflight of every skinned output before the first
    // mutable-buffer update is issued.
    [[nodiscard]] SkinnedMeshPoseUpdateResult prepare_pose(
        const DrawPacket& draw_packet) const noexcept;

    // Upload a previously prepared output. The stored packet changes only
    // after the backend accepts the complete vertex update.
    [[nodiscard]] SkinnedMeshPoseUpdateResult commit_pose(
        Device& device, const DrawPacket& draw_packet,
        std::span<const float> skinned_vertices);

    // Convenience operation for callers that do not need a separate batch
    // preflight step.
    [[nodiscard]] SkinnedMeshPoseUpdateResult update_pose(
        Device& device, const DrawPacket& draw_packet);

private:
    std::uint32_t source_vertex_count_ = 0U;
    std::uint32_t source_index_count_ = 0U;
    std::uint32_t source_vertex_stride_floats_ = 0U;
    std::size_t source_bone_count_ = 0U;
    std::vector<float> bind_vertices_;
    std::vector<std::uint16_t> source_indices_;
    DrawPacketLimits skin_limits_{};

    friend SkinnedMeshUploadResult upload_skinned_mesh(
        Device&, const formats::Kn5Node&, const DrawPacket&,
        const SkinnedMeshUploadLimits&);
};

struct SkinnedMeshUploadResult {
    SkinnedMeshUploadStatus status = SkinnedMeshUploadStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<SkinnedMeshUpload> upload;

    [[nodiscard]] bool ok() const noexcept {
        return status == SkinnedMeshUploadStatus::ready && upload != nullptr;
    }
};

struct SkinnedMeshPoseUpdateResult {
    SkinnedMeshUploadStatus status = SkinnedMeshUploadStatus::unsupported;
    Diagnostic diagnostic;
    std::vector<float> skinned_vertices;

    [[nodiscard]] bool ok() const noexcept {
        return status == SkinnedMeshUploadStatus::ready;
    }
};

[[nodiscard]] const char* skinned_mesh_upload_status_name(
    SkinnedMeshUploadStatus status) noexcept;

[[nodiscard]] SkinnedMeshUploadStatus validate_skinned_mesh_upload(
    const formats::Kn5Node& mesh, const DrawPacket& packet,
    const SkinnedMeshUploadLimits& limits, Diagnostic& diagnostic) noexcept;

[[nodiscard]] inline SkinnedMeshUploadStatus validate_skinned_mesh_upload(
    const formats::Kn5Node& mesh, const DrawPacket& packet,
    Diagnostic& diagnostic) noexcept {
    return validate_skinned_mesh_upload(mesh, packet, SkinnedMeshUploadLimits{}, diagnostic);
}

[[nodiscard]] SkinnedMeshUploadResult upload_skinned_mesh(
    Device& device, const formats::Kn5Node& mesh, const DrawPacket& packet,
    const SkinnedMeshUploadLimits& limits = {});

}  // namespace apex::render
