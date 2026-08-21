#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/render/kn5_scene_node_map.hpp"
#include "apex/render/static_mesh_upload.hpp"
#include "apex/scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace apex::render {

struct StaticSceneResourceLimits {
    Kn5SceneNodeMapLimits node_map{};
    StaticMeshUploadLimits mesh{};
    PipelineLimits pipeline{};
    std::size_t max_draws = max_indexed_static_mesh_batch_draws;
    std::size_t max_materials = 4096U;
    std::size_t max_material_constant_buffers = 4096U;
    std::size_t max_textures = 65'536U;
    std::size_t max_resource_string_bytes = 1U << 20;
    apex::core::ParseLimits texture_decode{};
    std::uint64_t max_total_texture_source_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_decoded_texture_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_vertex_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_index_bytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_shader_bytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_material_constant_bytes = 1ULL * 1024ULL * 1024ULL;
    // Counts full source payloads once per packet. This limit bounds repeated
    // validation work when many packets reference one large mesh.
    std::uint64_t max_validation_bytes = 1024ULL * 1024ULL * 1024ULL;
};

enum class StaticSceneTextureAuthority : std::uint8_t {
    // The frame supplies non-owning tables in final KN5 texture order.
    caller_tables,
    // Preparation decodes used embedded KN5 DDS payloads and owns the created
    // texture resources plus one linear/repeat sampler.
    embedded_kn5,
};

struct StaticScenePrepareRequest {
    const formats::Kn5File* model = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    std::span<const DrawPacket> packets{};
    // Indexed by the final model's global MaterialId. Used programs are
    // copied into the returned resources.
    std::span<const PipelineProgram* const> pipelines_by_material{};
    // This optional table uses the same material order. A used pipeline that
    // declares the portable constants binding requires the complete table.
    // Preparation copies each used value into an owned 256-byte GPU record.
    std::span<const KsPerPixelMaterialConstants> material_constants_by_material{};
    StaticSceneTextureAuthority texture_authority =
        StaticSceneTextureAuthority::caller_tables;
    StaticSceneResourceLimits limits{};
};

enum class StaticSceneResourceStatus {
    ready,
    invalid_request,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct StaticSceneFrameDescription {
    CameraFrame camera{};
    DepthAttachment* depth_attachment = nullptr;
    bool load_color = false;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    bool clear_depth = false;
    float depth_clear_value = 1.0F;
    // For caller_tables authority, these non-owning tables use the final
    // Kn5File::textures ordering. When a prepared packet uses txDiffuse, both
    // lengths must equal the final texture count. Used entries must remain
    // alive through the synchronous draw; unused entries can be null. The
    // embedded_kn5 authority ignores these tables and uses owned resources.
    std::span<const Texture* const> textures_by_global_index{};
    std::span<const Sampler* const> samplers_by_global_index{};
};

struct StaticSceneResourceResult;

class StaticSceneResources final {
public:
    [[nodiscard]] Backend backend() const noexcept { return backend_; }
    [[nodiscard]] std::size_t draw_count() const noexcept { return packets_.size(); }
    [[nodiscard]] std::size_t unique_geometry_count() const noexcept { return uploads_.size(); }
    [[nodiscard]] std::size_t owned_texture_count() const noexcept;
    [[nodiscard]] std::size_t owned_material_constant_count() const noexcept {
        return owned_material_constants_.size();
    }

    // Keep the preparing device alive and use it for every draw. The call is
    // synchronous. The target and optional depth attachment must remain alive
    // until it returns.
    [[nodiscard]] IndexedStaticMeshBatchResult draw_and_readback(
        Device& device, Texture& target,
        const StaticSceneFrameDescription& frame) const;

private:
    Backend backend_ = Backend::Vulkan;
    // Non-owning. The preparing device must remain alive until the final draw.
    const Device* device_ = nullptr;
    std::vector<DrawPacket> packets_;
    std::vector<PipelineProgram> pipelines_;
    std::vector<std::unique_ptr<StaticMeshUpload>> uploads_;
    std::vector<std::size_t> upload_for_packet_;
    std::vector<std::size_t> pipeline_for_packet_;
    std::vector<std::uint32_t> texture_for_packet_;
    std::vector<std::size_t> material_constant_for_packet_;
    std::vector<std::unique_ptr<Texture>> owned_textures_;
    std::vector<std::unique_ptr<Buffer>> owned_material_constants_;
    std::unique_ptr<Sampler> owned_sampler_;
    std::size_t texture_count_ = 0U;
    bool has_texture_resources_ = false;
    StaticSceneTextureAuthority texture_authority_ =
        StaticSceneTextureAuthority::caller_tables;

    friend struct StaticSceneResourceResult;
    friend StaticSceneResourceResult prepare_static_scene_resources(
        Device&, const StaticScenePrepareRequest&);
};

struct StaticSceneResourceResult {
    StaticSceneResourceStatus status = StaticSceneResourceStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<StaticSceneResources> resources;

    [[nodiscard]] bool ok() const noexcept {
        return status == StaticSceneResourceStatus::ready && resources != nullptr;
    }
};

[[nodiscard]] const char* static_scene_resource_status_name(
    StaticSceneResourceStatus status) noexcept;

// Validate every source node, packet, pipeline, and aggregate limit before
// creating the first backend buffer. Geometry is uploaded once per NodeId;
// duplicate packets remain distinct ordered draw instances.
[[nodiscard]] StaticSceneResourceResult prepare_static_scene_resources(
    Device& device, const StaticScenePrepareRequest& request);

}  // namespace apex::render
