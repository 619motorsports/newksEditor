#include "apex/render/skinned_mesh_upload.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace apex::render {
namespace {

static_assert(std::is_nothrow_move_assignable_v<DrawPacket>);

bool checked_multiply(std::size_t left, std::size_t right, std::uint64_t& output) noexcept {
    const auto left64 = static_cast<std::uint64_t>(left);
    const auto right64 = static_cast<std::uint64_t>(right);
    if (right64 != 0U && left64 > std::numeric_limits<std::uint64_t>::max() / right64) return false;
    output = left64 * right64;
    return true;
}

bool finite_matrix(const apex::scene::Matrix4& matrix) noexcept {
    for (const float value : matrix)
        if (!std::isfinite(value)) return false;
    return true;
}

SkinnedMeshUploadStatus invalid(Diagnostic& diagnostic, const char* code,
                                const char* message) noexcept {
    diagnostic = {code, message};
    return SkinnedMeshUploadStatus::invalid_request;
}

SkinnedMeshUploadStatus map_buffer_failure(const BufferResult& result,
                                           const char* resource,
                                           Diagnostic& diagnostic) {
    diagnostic = result.diagnostic;
    if (diagnostic.code.empty())
        diagnostic = {"skinned_mesh_buffer_create_failed",
                      std::string("Failed to create ") + resource + " buffer"};
    switch (result.status) {
    case BufferStatus::unsupported: return SkinnedMeshUploadStatus::unsupported;
    case BufferStatus::allocation_failed: return SkinnedMeshUploadStatus::allocation_failed;
    case BufferStatus::upload_failed: return SkinnedMeshUploadStatus::upload_failed;
    case BufferStatus::invalid_description: return SkinnedMeshUploadStatus::invalid_request;
    case BufferStatus::ready: break;
    }
    return SkinnedMeshUploadStatus::upload_failed;
}

SkinnedMeshUploadStatus map_update_failure(const BufferUpdateResult& result,
                                           Diagnostic& diagnostic) {
    diagnostic = result.diagnostic;
    if (diagnostic.code.empty())
        diagnostic = {"skinned_mesh_vertex_update_failed", "The skinned vertex buffer update failed"};
    switch (result.status) {
    case BufferStatus::unsupported: return SkinnedMeshUploadStatus::unsupported;
    case BufferStatus::allocation_failed: return SkinnedMeshUploadStatus::allocation_failed;
    case BufferStatus::upload_failed: return SkinnedMeshUploadStatus::upload_failed;
    case BufferStatus::invalid_description: return SkinnedMeshUploadStatus::invalid_request;
    case BufferStatus::ready: break;
    }
    return SkinnedMeshUploadStatus::upload_failed;
}

DrawPacketLimits draw_packet_limits(const SkinnedMeshUploadLimits& limits) noexcept {
    DrawPacketLimits output;
    output.max_vertices = limits.max_vertices;
    output.max_bones = limits.max_bones;
    output.max_cpu_skin_bytes = limits.max_cpu_skin_bytes;
    return output;
}

SkinnedMeshUploadStatus validate_packet_range(
    const formats::Kn5Node& mesh, const DrawPacket& packet,
    const SkinnedMeshUploadLimits& limits, Diagnostic& diagnostic) noexcept {
    if (limits.max_vertices == 0U || limits.max_indices == 0U || limits.max_bones == 0U ||
        limits.max_bone_name_bytes == 0U ||
        limits.max_vertex_bytes == 0U || limits.max_index_bytes == 0U ||
        limits.max_cpu_skin_bytes == 0U || limits.max_vertex_bytes > max_buffer_bytes ||
        limits.max_index_bytes > max_buffer_bytes)
        return invalid(diagnostic, "skinned_mesh_upload_limits_invalid",
                       "Skinned mesh upload limits are invalid");
    if (packet.primitive != DrawPrimitiveKind::skinned_mesh)
        return invalid(diagnostic, "skinned_mesh_primitive_required",
                       "The draw packet is not a skinned mesh");
    if (mesh.type != 3U || mesh.kind != "skinnedMesh")
        return invalid(diagnostic, "skinned_mesh_kind_invalid",
                       "The KN5 source is not a skinned mesh");
    if (mesh.vertexStride != 19U || packet.vertex_stride_floats != mesh.vertexStride)
        return invalid(diagnostic, "skinned_mesh_stride_invalid",
                       "The KN5 skinned mesh stride is not 19 float32 values");
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.bones.empty())
        return invalid(diagnostic, "skinned_mesh_geometry_empty",
                       "The KN5 skinned mesh has no geometry or bones");
    if (mesh.vertices.size() % 19U != 0U || mesh.indices.size() % 3U != 0U)
        return invalid(diagnostic, "skinned_mesh_geometry_malformed",
                       "The KN5 skinned mesh geometry is not stride or triangle aligned");

    const std::size_t vertex_count = mesh.vertices.size() / 19U;
    const std::size_t index_count = mesh.indices.size();
    if (vertex_count > limits.max_vertices || index_count > limits.max_indices ||
        vertex_count > max_indexed_static_mesh_vertices || index_count > max_indexed_static_mesh_indices ||
        vertex_count > std::numeric_limits<std::uint32_t>::max() ||
        index_count > std::numeric_limits<std::uint32_t>::max() ||
        mesh.bones.size() > limits.max_bones)
        return invalid(diagnostic, "skinned_mesh_geometry_limit",
                       "The KN5 skinned mesh exceeds the upload limits");
    if (packet.vertex_count == 0U || packet.index_count == 0U || packet.index_count % 3U != 0U ||
        static_cast<std::size_t>(packet.vertex_offset) > vertex_count ||
        static_cast<std::size_t>(packet.vertex_count) > vertex_count - packet.vertex_offset ||
        static_cast<std::size_t>(packet.index_offset) > index_count ||
        static_cast<std::size_t>(packet.index_count) > index_count - packet.index_offset)
        return invalid(diagnostic, "skinned_mesh_packet_range_invalid",
                       "The draw packet range exceeds the KN5 skinned mesh");
    if (!finite_matrix(packet.world_matrix))
        return invalid(diagnostic, "skinned_mesh_world_non_finite",
                       "The skinned mesh world matrix is not finite");
    if (packet.bone_palette.size() != mesh.bones.size())
        return invalid(diagnostic, "skinned_mesh_palette_invalid",
                       "The draw packet bone palette does not match the KN5 bone table");
    for (const auto& bone : mesh.bones) {
        if (bone.name.size() > limits.max_bone_name_bytes)
            return invalid(diagnostic, "skinned_mesh_bone_name_limit",
                           "A KN5 bone name exceeds the upload limit");
        if (!finite_matrix(bone.transform))
            return invalid(diagnostic, "skinned_mesh_bone_non_finite",
                           "The KN5 skinned mesh contains a non-finite inverse-bind matrix");
    }
    for (const auto& palette : packet.bone_palette) {
        if (!finite_matrix(palette))
            return invalid(diagnostic, "skinned_mesh_palette_non_finite",
                           "The draw packet bone palette contains a non-finite matrix");
    }
    std::uint64_t vertex_bytes = 0U;
    std::uint64_t index_bytes = 0U;
    if (!checked_multiply(mesh.vertices.size(), sizeof(float), vertex_bytes) ||
        !checked_multiply(mesh.indices.size(), sizeof(std::uint16_t), index_bytes))
        return invalid(diagnostic, "skinned_mesh_byte_size_overflow",
                       "The KN5 skinned mesh byte size overflows");
    if (vertex_bytes == 0U || index_bytes == 0U || vertex_bytes > limits.max_vertex_bytes ||
        index_bytes > limits.max_index_bytes)
        return invalid(diagnostic, "skinned_mesh_byte_limit",
                       "The KN5 skinned mesh exceeds the byte limits");
    for (const float value : mesh.vertices) {
        if (!std::isfinite(value))
            return invalid(diagnostic, "skinned_mesh_vertex_non_finite",
                           "The KN5 skinned mesh contains a non-finite vertex value");
    }
    for (const std::uint16_t index : mesh.indices) {
        if (static_cast<std::size_t>(index) >= vertex_count)
            return invalid(diagnostic, "skinned_mesh_index_out_of_range",
                           "The KN5 skinned mesh contains an out-of-range index");
    }
    for (std::size_t offset = 0U; offset < mesh.vertices.size(); offset += 19U) {
        for (std::size_t influence = 0U; influence < 4U; ++influence) {
            const float weight = mesh.vertices[offset + 11U + influence];
            const float index = mesh.vertices[offset + 15U + influence];
            if (!std::isfinite(weight) || !std::isfinite(index) || index < 0.0F ||
                std::trunc(index) != index || static_cast<double>(index) >= static_cast<double>(mesh.bones.size()))
                return invalid(diagnostic, "skinned_mesh_influence_invalid",
                               "The KN5 skinned mesh contains an invalid bone influence");
        }
    }
    const auto selected_indices = std::span<const std::uint16_t>(mesh.indices).subspan(
        packet.index_offset, packet.index_count);
    for (const std::uint16_t index : selected_indices) {
        if (index >= packet.vertex_count)
            return invalid(diagnostic, "skinned_mesh_packet_index_out_of_range",
                           "The draw packet contains an index outside its selected vertex range");
    }
    try {
        // Run the complete bounded CPU path before any backend allocation.
        // This catches singular transforms and overflowed computed positions,
        // not only malformed source fields.
        (void)skin_vertices_reference(std::span<const float>(mesh.vertices),
                                      std::span<const apex::scene::Matrix4>(packet.bone_palette),
                                      packet.world_matrix, draw_packet_limits(limits));
    } catch (const DrawPacketError& error) {
        diagnostic = {error.code(), error.what()};
        return SkinnedMeshUploadStatus::invalid_request;
    } catch (const std::bad_alloc&) {
        diagnostic = {"skinned_mesh_skin_allocation_failed",
                      "CPU skinning output allocation failed"};
        return SkinnedMeshUploadStatus::allocation_failed;
    }
    return SkinnedMeshUploadStatus::ready;
}

SkinnedMeshUploadStatus validate_owned_packet(
    const SkinnedMeshUpload& upload, const DrawPacket& packet, Diagnostic& diagnostic) noexcept {
    if (upload.vertex_buffer == nullptr || upload.index_buffer == nullptr)
        return invalid(diagnostic, "skinned_mesh_upload_uninitialized",
                       "The skinned mesh upload has no geometry buffers");
    if (packet.primitive != DrawPrimitiveKind::skinned_mesh)
        return invalid(diagnostic, "skinned_mesh_primitive_required",
                       "The draw packet is not a skinned mesh");
    if (packet.node != upload.packet.node)
        return invalid(diagnostic, "skinned_mesh_packet_node_mismatch",
                       "The draw packet node does not own the uploaded geometry");
    if (packet.vertex_stride_floats != upload.source_vertex_stride_floats() ||
        packet.vertex_count == 0U || packet.index_count == 0U || packet.index_count % 3U != 0U ||
        static_cast<std::size_t>(packet.vertex_offset) > upload.source_vertex_count() ||
        static_cast<std::size_t>(packet.vertex_count) >
            upload.source_vertex_count() - packet.vertex_offset ||
        static_cast<std::size_t>(packet.index_offset) > upload.source_index_count() ||
        static_cast<std::size_t>(packet.index_count) >
            upload.source_index_count() - packet.index_offset)
        return invalid(diagnostic, "skinned_mesh_packet_range_invalid",
                       "The draw packet range exceeds the uploaded skinned geometry");
    if (!finite_matrix(packet.world_matrix))
        return invalid(diagnostic, "skinned_mesh_world_non_finite",
                       "The skinned mesh world matrix is not finite");
    if (packet.bone_palette.size() != upload.source_bone_count())
        return invalid(diagnostic, "skinned_mesh_palette_invalid",
                       "The draw packet bone palette does not match the uploaded bone table");
    for (const auto& palette : packet.bone_palette) {
        if (!finite_matrix(palette))
            return invalid(diagnostic, "skinned_mesh_palette_non_finite",
                           "The draw packet bone palette contains a non-finite matrix");
    }
    const auto selected_indices = upload.source_indices().subspan(
        packet.index_offset, packet.index_count);
    for (const std::uint16_t index : selected_indices) {
        if (index >= packet.vertex_count)
            return invalid(diagnostic, "skinned_mesh_packet_index_out_of_range",
                           "The draw packet contains an index outside its selected vertex range");
    }
    return SkinnedMeshUploadStatus::ready;
}

}  // namespace

const char* skinned_mesh_upload_status_name(SkinnedMeshUploadStatus status) noexcept {
    switch (status) {
    case SkinnedMeshUploadStatus::ready: return "ready";
    case SkinnedMeshUploadStatus::invalid_request: return "invalid_request";
    case SkinnedMeshUploadStatus::unsupported: return "unsupported";
    case SkinnedMeshUploadStatus::allocation_failed: return "allocation_failed";
    case SkinnedMeshUploadStatus::upload_failed: return "upload_failed";
    }
    return "unknown";
}

SkinnedMeshUploadStatus validate_skinned_mesh_upload(
    const formats::Kn5Node& mesh, const DrawPacket& packet,
    const SkinnedMeshUploadLimits& limits, Diagnostic& diagnostic) noexcept {
    return validate_packet_range(mesh, packet, limits, diagnostic);
}

std::optional<IndexedStaticMeshDrawRequest> SkinnedMeshUpload::make_request(
    const DrawPacket& draw_packet, const PipelineProgram& pipeline,
    const CameraFrame& camera_frame, std::uint32_t mip_level,
    std::uint32_t array_layer, std::array<float, 4> clear_color,
    Diagnostic& diagnostic) const noexcept {
    diagnostic = {};
    if (validate_owned_packet(*this, draw_packet, diagnostic) != SkinnedMeshUploadStatus::ready)
        return std::nullopt;
    return IndexedStaticMeshDrawRequest{&draw_packet, &pipeline, vertex_buffer.get(), index_buffer.get(),
                                        StaticMeshIndexType::uint16, mip_level, array_layer,
                                        clear_color, camera_frame};
}

IndexedStaticMeshDrawRequest SkinnedMeshUpload::make_request(
    const PipelineProgram& pipeline, const CameraFrame& camera_frame,
    std::uint32_t mip_level, std::uint32_t array_layer,
    std::array<float, 4> clear_color) const noexcept {
    Diagnostic diagnostic;
    const auto request = make_request(packet, pipeline, camera_frame, mip_level, array_layer,
                                      clear_color, diagnostic);
    return request.value_or(IndexedStaticMeshDrawRequest{});
}

SkinnedMeshPoseUpdateResult SkinnedMeshUpload::prepare_pose(
    const DrawPacket& draw_packet) const noexcept {
    Diagnostic diagnostic;
    if (validate_owned_packet(*this, draw_packet, diagnostic) != SkinnedMeshUploadStatus::ready)
        return {SkinnedMeshUploadStatus::invalid_request, std::move(diagnostic), {}};
    try {
        DrawPacketLimits limits = skin_limits_;
        return {SkinnedMeshUploadStatus::ready, {},
                skin_vertices_reference(bind_vertices_, draw_packet.bone_palette,
                                        draw_packet.world_matrix, limits)};
    } catch (const DrawPacketError& error) {
        return {SkinnedMeshUploadStatus::invalid_request,
                {error.code(), error.what()}, {}};
    } catch (const std::bad_alloc&) {
        return {SkinnedMeshUploadStatus::allocation_failed,
                {"skinned_mesh_skin_allocation_failed", "CPU skinning output allocation failed"}, {}};
    }
}

SkinnedMeshPoseUpdateResult SkinnedMeshUpload::commit_pose(
    Device& device, const DrawPacket& draw_packet,
    std::span<const float> skinned_vertices) {
    Diagnostic diagnostic;
    if (validate_owned_packet(*this, draw_packet, diagnostic) != SkinnedMeshUploadStatus::ready)
        return {SkinnedMeshUploadStatus::invalid_request, std::move(diagnostic), {}};
    if (skinned_vertices.size() != bind_vertices_.size())
        return {SkinnedMeshUploadStatus::invalid_request,
                {"skinned_mesh_output_size_invalid", "Skinned output does not match the bind-pose vertex stream"}, {}};
    for (const float value : skinned_vertices) {
        if (!std::isfinite(value))
            return {SkinnedMeshUploadStatus::invalid_request,
                    {"skinned_mesh_output_non_finite", "Skinned output contains a non-finite value"}, {}};
    }
    DrawPacket committed_packet;
    try {
        // Allocate the packet copy before changing backend state. Moving this
        // validated copy into place after a successful upload does not retain
        // caller-owned storage.
        committed_packet = draw_packet;
    } catch (const std::bad_alloc&) {
        return {SkinnedMeshUploadStatus::allocation_failed,
                {"skinned_mesh_packet_allocation_failed",
                 "The updated skinned packet could not be retained"}, {}};
    }
    const auto bytes = std::as_bytes(skinned_vertices);
    const BufferUpdateResult updated = device.update_buffer(*vertex_buffer, 0U, bytes);
    if (!updated.ok()) {
        const SkinnedMeshUploadStatus status = map_update_failure(updated, diagnostic);
        return {status, std::move(diagnostic), {}};
    }
    packet = std::move(committed_packet);
    return {SkinnedMeshUploadStatus::ready, {}, {}};
}

SkinnedMeshPoseUpdateResult SkinnedMeshUpload::update_pose(
    Device& device, const DrawPacket& draw_packet) {
    SkinnedMeshPoseUpdateResult prepared = prepare_pose(draw_packet);
    if (!prepared.ok()) return prepared;
    std::vector<float> output = std::move(prepared.skinned_vertices);
    SkinnedMeshPoseUpdateResult committed = commit_pose(device, draw_packet, output);
    if (committed.ok()) committed.skinned_vertices = std::move(output);
    return committed;
}

SkinnedMeshUploadResult upload_skinned_mesh(
    Device& device, const formats::Kn5Node& mesh, const DrawPacket& packet,
    const SkinnedMeshUploadLimits& limits) {
    Diagnostic diagnostic;
    const SkinnedMeshUploadStatus validation =
        validate_skinned_mesh_upload(mesh, packet, limits, diagnostic);
    if (validation != SkinnedMeshUploadStatus::ready)
        return {validation, std::move(diagnostic), nullptr};
    try {
        const DrawPacketLimits skin_limits = draw_packet_limits(limits);
        // Validation already executed the complete CPU path. Preserve the
        // exact bind-pose bytes until animation is explicitly enabled.
        const auto vertex_bytes = std::as_bytes(std::span<const float>(mesh.vertices));
        const auto index_bytes = std::as_bytes(std::span<const std::uint16_t>(mesh.indices));
        const BufferDescription vertex_description{
            static_cast<std::uint64_t>(vertex_bytes.size()), BufferUsage::vertex,
            BufferMemory::device_local, BufferMutability::mutable_data};
        const BufferDescription index_description{
            static_cast<std::uint64_t>(index_bytes.size()), BufferUsage::index,
            BufferMemory::device_local, BufferMutability::immutable};
        BufferResult vertex_result = device.create_buffer(vertex_description, vertex_bytes);
        if (!vertex_result.ok()) {
            const SkinnedMeshUploadStatus status = map_buffer_failure(vertex_result, "vertex", diagnostic);
            return {status, std::move(diagnostic), nullptr};
        }
        BufferResult index_result = device.create_buffer(index_description, index_bytes);
        if (!index_result.ok()) {
            const SkinnedMeshUploadStatus status = map_buffer_failure(index_result, "index", diagnostic);
            return {status, std::move(diagnostic), nullptr};
        }
        auto upload = std::make_unique<SkinnedMeshUpload>();
        upload->packet = packet;
        upload->source_vertex_count_ = static_cast<std::uint32_t>(mesh.vertices.size() / 19U);
        upload->source_index_count_ = static_cast<std::uint32_t>(mesh.indices.size());
        upload->source_vertex_stride_floats_ = 19U;
        upload->source_bone_count_ = mesh.bones.size();
        upload->bind_vertices_ = mesh.vertices;
        upload->source_indices_ = mesh.indices;
        upload->skin_limits_ = skin_limits;
        upload->vertex_buffer = std::move(vertex_result.buffer);
        upload->index_buffer = std::move(index_result.buffer);
        return {SkinnedMeshUploadStatus::ready, {}, std::move(upload)};
    } catch (const DrawPacketError& error) {
        return {SkinnedMeshUploadStatus::invalid_request,
                {error.code(), error.what()}, nullptr};
    } catch (const std::bad_alloc&) {
        return {SkinnedMeshUploadStatus::allocation_failed,
                {"skinned_mesh_allocation_failed", "Skinned mesh upload allocation failed"}, nullptr};
    }
}

}  // namespace apex::render
