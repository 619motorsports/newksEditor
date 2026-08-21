#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

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

// The buffers are immutable and retain the backend context through their
// public RAII handles. The source KN5 node is not retained. A request returned
// by make_request must not outlive this upload or its pipeline.
struct StaticMeshUpload {
    DrawPacket packet;
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;

    [[nodiscard]] IndexedStaticMeshDrawRequest make_request(
        const PipelineProgram& pipeline, std::uint32_t mip_level = 0U,
        std::uint32_t array_layer = 0U,
        std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F}) const noexcept;
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
