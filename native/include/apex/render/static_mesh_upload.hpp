#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace apex::render {

// Limits for the adapter from one validated static KN5 mesh to backend
// buffers. These limits are independent from the binary parser limits because
// callers can construct Kn5File and Kn5Node values directly.
struct StaticMeshUploadLimits {
    std::size_t max_vertices = max_indexed_static_mesh_vertices;
    std::size_t max_indices = max_indexed_static_mesh_indices;
    std::uint64_t max_vertex_bytes = max_buffer_bytes;
    std::uint64_t max_index_bytes = max_buffer_bytes;
};

enum class StaticMeshUploadStatus {
    ready,
    invalid_request,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct StaticMeshUploadResult;

// The buffers are immutable and retain the backend context through their
// public RAII handles. The source KN5 node is not retained. A request returned
// by make_request must not outlive this upload or its pipeline.
struct StaticMeshUpload {
    DrawPacket packet;
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;

    // These counts describe the complete source geometry in the immutable
    // buffers. They are kept separately from packet because callers may reuse
    // one upload for multiple packet ranges.
    [[nodiscard]] std::uint32_t source_vertex_count() const noexcept {
        return source_vertex_count_;
    }
    [[nodiscard]] std::uint32_t source_index_count() const noexcept {
        return source_index_count_;
    }
    [[nodiscard]] std::uint32_t source_vertex_stride_floats() const noexcept {
        return source_vertex_stride_floats_;
    }
    [[nodiscard]] std::span<const std::uint16_t> source_indices() const noexcept {
        return source_indices_;
    }

    // Validate a caller-owned packet and return a request that points at it.
    // The packet must remain alive, unchanged, and compatible with this
    // upload until the request has been consumed.
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

private:
    std::uint32_t source_vertex_count_ = 0U;
    std::uint32_t source_index_count_ = 0U;
    std::uint32_t source_vertex_stride_floats_ = 0U;
    // Retain bounded index values so alternate packet ranges can be checked
    // without retaining the untrusted KN5 source node.
    std::vector<std::uint16_t> source_indices_;

    friend StaticMeshUploadResult upload_static_mesh(
        Device&, const formats::Kn5Node&, const DrawPacket&,
        const StaticMeshUploadLimits&);
};

struct StaticMeshUploadResult {
    StaticMeshUploadStatus status = StaticMeshUploadStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<StaticMeshUpload> upload;

    [[nodiscard]] bool ok() const noexcept {
        return status == StaticMeshUploadStatus::ready && upload != nullptr;
    }
};

[[nodiscard]] const char* static_mesh_upload_status_name(
    StaticMeshUploadStatus status) noexcept;

// Validate the source mesh and packet before any backend allocation. The
// adapter uploads the complete source mesh, so every source vertex and index
// is checked even when the packet selects a sub-range.
[[nodiscard]] StaticMeshUploadStatus validate_static_mesh_upload(
    const formats::Kn5Node& mesh, const DrawPacket& packet,
    const StaticMeshUploadLimits& limits, Diagnostic& diagnostic) noexcept;

[[nodiscard]] inline StaticMeshUploadStatus validate_static_mesh_upload(
    const formats::Kn5Node& mesh, const DrawPacket& packet, Diagnostic& diagnostic) noexcept {
    return validate_static_mesh_upload(mesh, packet, StaticMeshUploadLimits{}, diagnostic);
}

// Upload one static mesh as immutable device-local vertex and uint16 index
// buffers. The packet's offsets remain element offsets into the complete
// uploaded mesh, as required by IndexedStaticMeshDrawRequest.
[[nodiscard]] StaticMeshUploadResult upload_static_mesh(
    Device& device, const formats::Kn5Node& mesh, const DrawPacket& packet,
    const StaticMeshUploadLimits& limits = {});

}  // namespace apex::render
