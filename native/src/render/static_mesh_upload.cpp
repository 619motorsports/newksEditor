#include "apex/render/static_mesh_upload.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace apex::render {
namespace {

bool checked_multiply(std::size_t left, std::size_t right, std::uint64_t& output) noexcept {
    const auto left64 = static_cast<std::uint64_t>(left);
    const auto right64 = static_cast<std::uint64_t>(right);
    if (right64 != 0U && left64 > std::numeric_limits<std::uint64_t>::max() / right64) return false;
    output = left64 * right64;
    return true;
}

bool finite_matrix(const apex::scene::Matrix4& matrix) noexcept {
    for (const float value : matrix) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

StaticMeshUploadStatus invalid(Diagnostic& diagnostic, const char* code, const char* message) noexcept {
    diagnostic = {code, message};
    return StaticMeshUploadStatus::invalid_request;
}

StaticMeshUploadStatus map_buffer_failure(const BufferResult& result,
                                          const char* resource,
                                          Diagnostic& diagnostic) {
    diagnostic = result.diagnostic;
    if (diagnostic.code.empty()) {
        diagnostic = {"static_mesh_buffer_create_failed", std::string("Failed to create ") + resource + " buffer"};
    }
    switch (result.status) {
        case BufferStatus::unsupported: return StaticMeshUploadStatus::unsupported;
        case BufferStatus::allocation_failed: return StaticMeshUploadStatus::allocation_failed;
        case BufferStatus::upload_failed: return StaticMeshUploadStatus::upload_failed;
        case BufferStatus::invalid_description: return StaticMeshUploadStatus::invalid_request;
        case BufferStatus::ready: break;
    }
    return StaticMeshUploadStatus::upload_failed;
}

}  // namespace

IndexedStaticMeshDrawRequest StaticMeshUpload::make_request(
    const PipelineProgram& pipeline, const CameraFrame& camera_frame,
    std::uint32_t mip_level, std::uint32_t array_layer,
    std::array<float, 4> clear_color) const noexcept {
    return {&packet, &pipeline, vertex_buffer.get(), index_buffer.get(), StaticMeshIndexType::uint16,
            mip_level, array_layer, clear_color, camera_frame};
}

const char* static_mesh_upload_status_name(StaticMeshUploadStatus status) noexcept {
    switch (status) {
        case StaticMeshUploadStatus::ready: return "ready";
        case StaticMeshUploadStatus::invalid_request: return "invalid_request";
        case StaticMeshUploadStatus::unsupported: return "unsupported";
        case StaticMeshUploadStatus::allocation_failed: return "allocation_failed";
        case StaticMeshUploadStatus::upload_failed: return "upload_failed";
    }
    return "unknown";
}

StaticMeshUploadStatus validate_static_mesh_upload(
    const formats::Kn5Node& mesh, const DrawPacket& packet,
    const StaticMeshUploadLimits& limits, Diagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (limits.max_vertices == 0U || limits.max_indices == 0U || limits.max_vertex_bytes == 0U ||
        limits.max_index_bytes == 0U || limits.max_vertex_bytes > max_buffer_bytes ||
        limits.max_index_bytes > max_buffer_bytes) {
        return invalid(diagnostic, "static_mesh_upload_limits_invalid", "Static mesh upload limits are invalid");
    }
    if (packet.primitive != DrawPrimitiveKind::static_mesh) {
        return invalid(diagnostic, "static_mesh_primitive_required", "The draw packet is not a static mesh");
    }
    if (mesh.type != 2U || mesh.kind != "mesh") {
        return invalid(diagnostic, "static_mesh_kind_invalid", "The KN5 source is not a static mesh");
    }
    if (mesh.vertexStride == 0U || mesh.vertexStride > std::numeric_limits<std::uint32_t>::max() ||
        mesh.vertexStride != 11U || packet.vertex_stride_floats != mesh.vertexStride) {
        return invalid(diagnostic, "static_mesh_stride_invalid", "The KN5 static mesh stride is not 11 float32 values");
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return invalid(diagnostic, "static_mesh_geometry_empty", "The KN5 static mesh has no geometry");
    }
    if (mesh.vertices.size() % mesh.vertexStride != 0U || mesh.indices.size() % 3U != 0U) {
        return invalid(diagnostic, "static_mesh_geometry_malformed", "The KN5 static mesh geometry is not stride or triangle aligned");
    }

    const std::size_t vertex_count = mesh.vertices.size() / mesh.vertexStride;
    const std::size_t index_count = mesh.indices.size();
    if (vertex_count > limits.max_vertices || index_count > limits.max_indices ||
        vertex_count > max_indexed_static_mesh_vertices || index_count > max_indexed_static_mesh_indices ||
        vertex_count > std::numeric_limits<std::uint32_t>::max() ||
        index_count > std::numeric_limits<std::uint32_t>::max()) {
        return invalid(diagnostic, "static_mesh_geometry_limit", "The KN5 static mesh exceeds the upload limits");
    }
    if (packet.vertex_count == 0U || packet.index_count == 0U || packet.index_count % 3U != 0U) {
        return invalid(diagnostic, "static_mesh_packet_range_invalid", "The draw packet has an invalid triangle range");
    }
    if (static_cast<std::size_t>(packet.vertex_offset) > vertex_count ||
        static_cast<std::size_t>(packet.vertex_count) > vertex_count - packet.vertex_offset ||
        static_cast<std::size_t>(packet.index_offset) > index_count ||
        static_cast<std::size_t>(packet.index_count) > index_count - packet.index_offset) {
        return invalid(diagnostic, "static_mesh_packet_range_invalid", "The draw packet range exceeds the KN5 mesh");
    }
    if (!finite_matrix(packet.world_matrix)) {
        return invalid(diagnostic, "static_mesh_world_non_finite", "The draw packet world matrix is not finite");
    }

    std::uint64_t vertex_bytes = 0U;
    std::uint64_t index_bytes = 0U;
    if (!checked_multiply(mesh.vertices.size(), sizeof(float), vertex_bytes) ||
        !checked_multiply(mesh.indices.size(), sizeof(std::uint16_t), index_bytes)) {
        return invalid(diagnostic, "static_mesh_byte_size_overflow", "The KN5 static mesh byte size overflows");
    }
    if (vertex_bytes == 0U || index_bytes == 0U || vertex_bytes > limits.max_vertex_bytes ||
        index_bytes > limits.max_index_bytes || vertex_bytes > max_buffer_bytes || index_bytes > max_buffer_bytes) {
        return invalid(diagnostic, "static_mesh_byte_limit", "The KN5 static mesh exceeds the byte limits");
    }
    for (const float value : mesh.vertices) {
        if (!std::isfinite(value)) {
            return invalid(diagnostic, "static_mesh_vertex_non_finite", "The KN5 static mesh contains a non-finite vertex value");
        }
    }
    for (const std::uint16_t index : mesh.indices) {
        if (static_cast<std::size_t>(index) >= vertex_count) {
            return invalid(diagnostic, "static_mesh_index_out_of_range", "The KN5 static mesh contains an out-of-range index");
        }
    }
    const auto selected_indices = std::span<const std::uint16_t>(mesh.indices).subspan(
        packet.index_offset, packet.index_count);
    for (const std::uint16_t index : selected_indices) {
        if (index >= packet.vertex_count) {
            return invalid(diagnostic, "static_mesh_packet_index_out_of_range",
                           "The draw packet contains an index outside its selected vertex range");
        }
    }
    return StaticMeshUploadStatus::ready;
}

StaticMeshUploadResult upload_static_mesh(
    Device& device, const formats::Kn5Node& mesh, const DrawPacket& packet,
    const StaticMeshUploadLimits& limits) {
    Diagnostic diagnostic;
    const StaticMeshUploadStatus validation =
        validate_static_mesh_upload(mesh, packet, limits, diagnostic);
    if (validation != StaticMeshUploadStatus::ready) return {validation, std::move(diagnostic), nullptr};

    const auto vertex_bytes = std::as_bytes(std::span<const float>(mesh.vertices));
    const auto index_bytes = std::as_bytes(std::span<const std::uint16_t>(mesh.indices));
    const BufferDescription vertex_description{
        static_cast<std::uint64_t>(vertex_bytes.size()), BufferUsage::vertex,
        BufferMemory::device_local, BufferMutability::immutable};
    const BufferDescription index_description{
        static_cast<std::uint64_t>(index_bytes.size()), BufferUsage::index,
        BufferMemory::device_local, BufferMutability::immutable};

    BufferResult vertex_result = device.create_buffer(vertex_description, vertex_bytes);
    if (!vertex_result.ok()) {
        const StaticMeshUploadStatus status = map_buffer_failure(vertex_result, "vertex", diagnostic);
        return {status, std::move(diagnostic), nullptr};
    }
    BufferResult index_result = device.create_buffer(index_description, index_bytes);
    if (!index_result.ok()) {
        const StaticMeshUploadStatus status = map_buffer_failure(index_result, "index", diagnostic);
        return {status, std::move(diagnostic), nullptr};
    }

    auto upload = std::make_unique<StaticMeshUpload>();
    upload->packet = packet;
    upload->vertex_buffer = std::move(vertex_result.buffer);
    upload->index_buffer = std::move(index_result.buffer);
    return {StaticMeshUploadStatus::ready, {}, std::move(upload)};
}

}  // namespace apex::render
