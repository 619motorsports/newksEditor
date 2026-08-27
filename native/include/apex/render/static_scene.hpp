#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/render/kn5_scene_node_map.hpp"
#include "apex/render/directional_shadow.hpp"
#include "apex/render/skinned_mesh_upload.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan.hpp"
#include "apex/render/texture_payload_authority.hpp"
#include "apex/render/static_mesh_upload.hpp"
#include "apex/scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::render {

inline constexpr std::uint32_t invalid_static_scene_source_program_index =
    std::numeric_limits<std::uint32_t>::max();

struct StaticSceneResourceLimits {
    Kn5SceneNodeMapLimits node_map{};
    StaticMeshUploadLimits mesh{};
    SkinnedMeshUploadLimits skinned{};
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
    std::uint64_t max_total_multimap_reflection_constant_bytes =
        1ULL * 1024ULL * 1024ULL;
    // A selected Vulkan source-equivalent draw owns five aligned native
    // records. This limit is separate from the portable material table.
    std::uint64_t max_total_stock_vulkan_source_constant_bytes =
        32ULL * 1024ULL * 1024ULL;
    // Installed D3D12 native draws also own five aligned records per packet,
    // including a private mutable b4 allocation.
    std::uint64_t max_total_stock_d3d12_native_constant_bytes =
        32ULL * 1024ULL * 1024ULL;
    // One D3D12/Vulkan-aligned frame record is allocated when a prepared
    // pipeline declares the portable frame binding.
    std::uint64_t max_total_frame_constant_bytes = portable_frame_buffer_view_bytes;
    // One shared mutable receiver record is allocated when any prepared
    // pipeline declares the fixed three-cascade extension at bindings 16-20.
    std::uint64_t max_total_directional_shadow_constant_bytes =
        portable_directional_shadow_buffer_view_bytes;
    std::uint64_t max_total_update_bytes = 512ULL * 1024ULL * 1024ULL;
    // Counts full source payloads once per packet. This limit bounds repeated
    // validation work when many packets reference one large mesh.
    std::uint64_t max_validation_bytes = 1024ULL * 1024ULL * 1024ULL;
    // Bounds host-side tables and deep copies that preparation retains or
    // creates. Texture source bytes and decoded pixels have separate limits.
    std::uint64_t max_preparation_bytes = 512ULL * 1024ULL * 1024ULL;
};

struct StaticScenePrepareRequest {
    const formats::Kn5File* model = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    std::span<const DrawPacket> packets{};
    // Indexed by the final model's global MaterialId. Used programs are
    // copied into the returned resources. Supply this table or the per-packet
    // table below, but not both.
    std::span<const PipelineProgram* const> pipelines_by_material{};
    // Indexed by packet. This preserves source-evidenced per-node CSP render
    // state when meshes share one material but require different pipelines.
    std::span<const PipelineProgram* const> pipelines_by_packet{};
    // Optional retained Vulkan source-equivalent owners. The packet-index
    // table must be empty or match packets exactly. Portable packets use
    // invalid_static_scene_source_program_index. A selected packet's pipeline
    // pointer must be the exact pipeline owned by the indexed program.
    std::span<const std::shared_ptr<const
        ValidatedStockKsPerPixelVulkanSourceProgram>>
        stock_vulkan_source_programs{};
    std::span<const std::uint32_t>
        stock_vulkan_source_program_by_packet{};
    // Exact recovered 32-byte b4 records, indexed by final material ID.
    // Required as a complete table when any source program is selected.
    std::span<const StockKsPerPixelMaterialConstants>
        stock_vulkan_source_material_constants_by_material{};
    // Source evidence makes anisotropy and mip bias runtime settings. The
    // defaults match the native sampler allocator; callers can supply the
    // active video/render settings without changing shader authority.
    StockKsPerPixelNativeSamplerSettings
        stock_vulkan_source_sampler_settings{};
    // Optional retained installed-DXBC owners. The packet-index table follows
    // the Vulkan source table contract and uses the same invalid sentinel for
    // portable packets. Static-scene preparation clones one validated program
    // into a distinct mutable native resource bundle for every selected draw.
    std::span<const std::shared_ptr<const
        ValidatedStockKsPerPixelNativeProgram>>
        stock_d3d12_native_programs{};
    std::span<const std::uint32_t>
        stock_d3d12_native_program_by_packet{};
    std::span<const StockKsPerPixelMaterialConstants>
        stock_d3d12_native_material_constants_by_material{};
    StockKsPerPixelNativeSamplerSettings
        stock_d3d12_native_sampler_settings{};
    // This optional table uses the same material order. A used pipeline that
    // declares the portable constants binding requires the complete table.
    // Preparation copies each used value into an owned 256-byte GPU record.
    std::span<const KsPerPixelMaterialConstants> material_constants_by_material{};
    // Optional semantic reflection records in final material order. A used
    // pipeline that declares bindings 21-23 requires the complete table.
    std::span<const KsPerPixelMultiMapReflectionConstants>
        multimap_reflection_constants_by_material{};
    // Explicit resolved ksShadowGenAT material records, indexed by final
    // material ID. The source host record is 32 bytes; preparation owns one
    // 256-byte aligned uniform allocation for each used alpha caster.
    std::span<const StockShadowCasterMaterialConstants>
        stock_shadow_constants_by_material{};
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
    // Optional retained single-sample output for a four-sample target.
    Texture* resolve_target = nullptr;
    // Disable the synchronous CPU copy when only the retained image is used.
    bool capture_rgba8 = true;
    // Select one color-target subresource. Cube frames must name a face.
    TextureTargetSubresource target_subresource{};
    // Optional current packet states for animated scenes. The span must have
    // exactly the prepared packet count when supplied. Node/material,
    // primitive, ranges, pipeline flags, and resources must remain stable;
    // world matrices and bone palettes may change.
    std::span<const DrawPacket> refreshed_packets{};
    // Optional byte mask in prepared packet order. Empty means all packets
    // are visible. A supplied mask must match the prepared packet count and
    // contain only 0 (hidden) or 1 (visible). Hidden packets keep their stable
    // prepared contract but do not draw or update retained skinned geometry.
    // The mask cannot authorize an intrinsic shadow-only packet for color.
    std::span<const std::uint8_t> packet_visibility{};
    // Optional color-pass permutation in prepared packet-index space. Empty
    // preserves prepared order. A supplied span must contain every prepared
    // index exactly once. Visibility and refreshed state remain keyed by the
    // prepared index. Directional shadows have an independent order.
    std::span<const std::uint32_t> color_packet_order{};
    // When false, skinned uploads are restored to their bind-pose bytes. When
    // true, each skinned upload is CPU-skinned from refreshed_packets (or the
    // prepared packets when the span is empty) before the batch is submitted.
    bool apply_skinning = false;
    // Optional per-frame values for pipelines that declare set 0/binding 3.
    // The prepared scene validates and uploads one bounded record before the
    // ordered batch is recorded. It always derives the record's camera
    // position from camera.position.
    std::optional<KsPerPixelFrameConstants> frame_constants;
    // Frame-owned renderer cubemap. It remains outside KN5 texture tables.
    // Both handles are required exactly when a prepared pipeline declares
    // the portable MultiMap reflection extension.
    IndexedSampledTextureBinding multimap_reflection_cube{};
    struct StockNativeFrame {
        StockKsPerPixelCameraConstants camera{};
        StockKsPerPixelLightingConstants lighting{};
        StockDirectionalShadowReceiverConstants shadow_constants{};
        std::array<const DepthAttachment*,
                   stock_ks_per_pixel_shadow_cascade_count>
            shadow_maps{};
    };
    using StockVulkanSourceFrame = StockNativeFrame;
    // Complete recovered-native frame ABI for retained source-equivalent
    // packets. Portable frame constants are never reinterpreted as these
    // records.
    std::optional<StockVulkanSourceFrame> stock_vulkan_source_frame;
    // Complete recovered-native frame ABI for installed D3D12 packets. It is
    // separate from the Vulkan source field so backend authority cannot be
    // inferred or accidentally crossed.
    std::optional<StockNativeFrame> stock_d3d12_native_frame;
    // Optional retained maps for pipelines that declare the directional
    // receiver extension. The maps must come from the same device and remain
    // alive through this synchronous call. Caster execution remains an
    // explicit operation through draw_opaque_directional_shadows().
    DirectionalShadowMapResources* directional_shadow_maps = nullptr;
    // The portable 256-byte receiver record remains the default. Select the
    // recovered 208-byte stock cbShadowMaps layout only with matching caller
    // shader modules; this field never infers an ABI from opaque bytecode.
    DirectionalShadowReceiverConstantsLayout directional_shadow_constants_layout =
        DirectionalShadowReceiverConstantsLayout::portable;
    // For caller_tables authority, these non-owning tables use the final
    // Kn5File::textures ordering. When a prepared packet uses txDiffuse,
    // txNormal, txMaps, txDetail, txNormalDetail, txDamage, or txDamageMask,
    // both lengths must equal the final texture count.
    // Used entries must remain alive through the synchronous draw; unused
    // entries can be null. The owned-model authority ignores these tables
    // and uses owned resources.
    std::span<const Texture* const> textures_by_global_index{};
    std::span<const Sampler* const> samplers_by_global_index{};
    // Editor overlays are submitted after all retained scene packets in the
    // same render pass. The caller owns the referenced pipeline and buffer.
    std::span<const OverlayLineDrawRequest> overlay_draws{};
    // An optional portable selected-mesh pass uses the recovered shader and
    // state contract for one visible prepared static packet. This port draws
    // it once after opaque packets and before the view axis and transparent
    // packets. Both handles are required through this synchronous call.
    const PipelineProgram* selected_mesh_pipeline = nullptr;
    const Buffer* selected_mesh_color_buffer = nullptr;
    // Recovered SCENE_FINISHED callback lines execute after opaque packets
    // and the selected mesh, before the view axis and transparent packets.
    std::span<const OverlayLineDrawRequest> scene_finished_overlay_draws{};
    // Recovered world-origin axis draws execute after opaque packets and the
    // selected mesh, but before transparent packets. The caller owns the
    // referenced pipeline and buffer through this synchronous call.
    std::span<const OverlayLineDrawRequest> view_axis_draws{};
};

struct StaticSceneResourceResult;

class StaticSceneResources final {
public:
    [[nodiscard]] Backend backend() const noexcept { return backend_; }
    [[nodiscard]] std::size_t draw_count() const noexcept { return packets_.size(); }
    // The returned view remains valid for this resource object's lifetime.
    // Callers can inspect stable packet identity but must supply refreshed
    // frame state through draw_and_readback.
    [[nodiscard]] std::span<const DrawPacket> prepared_packets() const noexcept {
        return packets_;
    }
    [[nodiscard]] std::size_t unique_pipeline_count() const noexcept {
        return pipelines_.size() + stock_vulkan_source_programs_.size() +
               stock_d3d12_native_programs_.size();
    }
    [[nodiscard]] std::size_t stock_vulkan_source_program_count() const
        noexcept {
        return stock_vulkan_source_programs_.size();
    }
    [[nodiscard]] bool requires_stock_vulkan_source_frame() const noexcept {
        return !stock_vulkan_source_programs_.empty();
    }
    [[nodiscard]] std::size_t stock_d3d12_native_program_count() const
        noexcept {
        return stock_d3d12_native_programs_.size();
    }
    [[nodiscard]] bool requires_stock_d3d12_native_frame() const noexcept {
        return !stock_d3d12_native_programs_.empty();
    }
    [[nodiscard]] std::size_t unique_geometry_count() const noexcept {
        return uploads_.size() + skinned_uploads_.size();
    }
    [[nodiscard]] std::size_t owned_texture_count() const noexcept;
    [[nodiscard]] std::size_t owned_material_constant_count() const noexcept {
        return owned_material_constants_.size();
    }
    [[nodiscard]] std::size_t owned_multimap_reflection_constant_count() const
        noexcept {
        return owned_multimap_reflection_constants_.size();
    }
    [[nodiscard]] bool requires_multimap_reflection_cube() const noexcept {
        return !owned_multimap_reflection_constants_.empty();
    }
    [[nodiscard]] std::size_t owned_stock_shadow_constant_count() const noexcept {
        return owned_stock_shadow_constants_.size();
    }
    [[nodiscard]] bool owns_frame_constants() const noexcept {
        return owned_frame_constants_ != nullptr;
    }
    [[nodiscard]] bool owns_directional_shadow_receiver() const noexcept {
        return owned_directional_shadow_constants_ != nullptr &&
               owned_directional_shadow_sampler_ != nullptr;
    }

    // Validate mutable frame state against the prepared packet table without
    // changing retained resources. An empty span selects the prepared state.
    [[nodiscard]] bool validate_refreshed_packets(
        std::span<const DrawPacket> refreshed_packets,
        Diagnostic& output_diagnostic) const noexcept;

    // Keep the preparing device alive and use it for every draw. The call is
    // synchronous. The target and optional depth attachment must remain alive
    // until it returns. Input validation is failure-atomic. Backend buffer
    // updates are sequential: an upload failure prevents batch submission but
    // can leave earlier successful mutable updates committed. Retry the full
    // frame after such a failure.
    [[nodiscard]] IndexedStaticMeshBatchResult draw_and_readback(
        Device& device, Texture& target,
        const StaticSceneFrameDescription& frame);

    // Execute retained opaque static casters and, when an explicit skinned
    // depth pipeline is supplied, CPU-skinned casters into the fixed
    // three-map directional-shadow set. Alpha-tested static casters execute
    // only with an explicit caller-supplied alpha pipeline and bindings;
    // otherwise they remain staged. The pass does not infer a stock shader.
    [[nodiscard]] StaticSceneDirectionalShadowResult
    draw_opaque_directional_shadows(
        Device& device,
        const StaticSceneDirectionalShadowFrameDescription& frame);

private:
    struct PacketTextureIndices {
        std::uint32_t diffuse = invalid_draw_texture_index;
        std::uint32_t normal = invalid_draw_texture_index;
        std::uint32_t maps = invalid_draw_texture_index;
        std::uint32_t detail = invalid_draw_texture_index;
        std::uint32_t normal_detail = invalid_draw_texture_index;
        // txDust uses the same portable binding pair as detail, but only for
        // the mutually exclusive damage-dust layout.
        std::uint32_t dust = invalid_draw_texture_index;
        std::uint32_t damage = invalid_draw_texture_index;
        std::uint32_t damage_mask = invalid_draw_texture_index;
    };

    Backend backend_ = Backend::Vulkan;
    // Non-owning. The preparing device must remain alive until the final draw.
    const Device* device_ = nullptr;
    std::vector<DrawPacket> packets_;
    std::vector<PipelineProgram> pipelines_;
    std::vector<std::unique_ptr<StaticMeshUpload>> uploads_;
    std::vector<std::unique_ptr<SkinnedMeshUpload>> skinned_uploads_;
    std::vector<std::size_t> upload_for_packet_;
    std::vector<std::size_t> skinned_upload_for_packet_;
    std::vector<std::size_t> pipeline_for_packet_;
    std::vector<std::shared_ptr<const
        ValidatedStockKsPerPixelVulkanSourceProgram>>
        stock_vulkan_source_programs_;
    std::vector<std::uint32_t> stock_vulkan_source_program_for_packet_;
    std::vector<std::unique_ptr<StockKsPerPixelNativeConstantBuffers>>
        stock_vulkan_source_constants_for_packet_;
    std::vector<std::shared_ptr<const
        ValidatedStockKsPerPixelNativeProgram>>
        stock_d3d12_native_programs_;
    std::vector<std::uint32_t> stock_d3d12_native_program_for_packet_;
    std::vector<std::unique_ptr<StockKsPerPixelNativeDrawResources>>
        stock_d3d12_native_resources_for_packet_;
    std::vector<PacketTextureIndices> textures_for_packet_;
    std::vector<std::size_t> material_constant_for_packet_;
    std::vector<std::size_t> multimap_reflection_constant_for_packet_;
    std::vector<std::size_t> stock_shadow_constant_for_material_;
    std::vector<std::unique_ptr<Texture>> owned_textures_;
    std::vector<std::unique_ptr<Buffer>> owned_material_constants_;
    std::vector<std::unique_ptr<Buffer>>
        owned_multimap_reflection_constants_;
    std::vector<std::unique_ptr<Buffer>> owned_stock_shadow_constants_;
    std::unique_ptr<Buffer> owned_frame_constants_;
    std::unique_ptr<Buffer> owned_directional_shadow_constants_;
    std::unique_ptr<Sampler> owned_directional_shadow_sampler_;
    std::unique_ptr<Sampler> owned_sampler_;
    std::unique_ptr<Sampler> stock_vulkan_source_linear_sampler_;
    std::unique_ptr<Sampler> stock_vulkan_source_shadow_sampler_;
    std::size_t texture_count_ = 0U;
    bool has_texture_resources_ = false;
    bool has_portable_texture_resources_ = false;
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
