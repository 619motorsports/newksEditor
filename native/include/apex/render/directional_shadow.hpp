#pragma once

#include "apex/render/device.hpp"
#include "apex/render/lighting.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

inline constexpr std::size_t directional_shadow_cascade_count = 3U;
inline constexpr std::uint32_t max_directional_shadow_map_size = 4096U;
inline constexpr std::uint64_t max_directional_shadow_map_bytes =
    3ULL * 4096ULL * 4096ULL * sizeof(float);

// Evidence note: the recovered stock MaterialFilterSM::apply path uses
// back-face culling when doubleFaceShadow is false (the stock default) and
// disables culling when it is true. Alpha-tested execution remains an
// explicit caller-shader contract; this API never translates stock shader
// containers.
[[nodiscard]] constexpr PipelineCullMode stock_directional_shadow_cull_mode(
    bool double_face_shadow) noexcept {
    return double_face_shadow ? PipelineCullMode::none : PipelineCullMode::back;
}

// Source-evidenced hard cascade selection for the portable receiver. The
// input is camera-forward depth. A value beyond the third split is fully lit
// and therefore returns no cascade.
[[nodiscard]] std::optional<std::size_t>
select_directional_shadow_cascade(
    float camera_forward_depth,
    const std::array<float, directional_shadow_cascade_count>& splits) noexcept;

// Reference evaluator for the recovered explicit 3x3 comparison equation.
// The caller supplies the nine nearest-sampled D32 values. This keeps the
// testable fidelity rule independent from backend descriptor plumbing.
[[nodiscard]] std::optional<float> evaluate_directional_shadow_pcf(
    const std::array<float, 3>& projected_coordinate, float depth_bias,
    std::span<const float> sampled_depths) noexcept;

struct DirectionalShadowMapLimits {
    std::uint32_t max_map_size = max_directional_shadow_map_size;
    std::uint64_t max_total_bytes = max_directional_shadow_map_bytes;
};

struct DirectionalShadowMapRequest {
    DirectionalShadowInput lighting{};
    DirectionalShadowMapLimits limits{};
};

enum class DirectionalShadowMapStatus : std::uint8_t {
    ready,
    invalid_request,
    unsupported,
    allocation_failed,
};

struct DirectionalShadowMapResult;

// Owns one fixed, backend-neutral D32 attachment for each recovered
// directional cascade. It does not expose backend-native image handles.
class DirectionalShadowMapResources final {
public:
    [[nodiscard]] Backend backend() const noexcept { return backend_; }
    [[nodiscard]] std::uint32_t map_size() const noexcept { return metadata_.map_size; }
    [[nodiscard]] const DirectionalShadowResult& metadata() const noexcept {
        return metadata_;
    }
    [[nodiscard]] const CameraFrame& camera(std::size_t index) const;
    [[nodiscard]] DepthAttachment& attachment(std::size_t index);
    [[nodiscard]] const DepthAttachment& attachment(std::size_t index) const;

private:
    Backend backend_ = Backend::Vulkan;
    const Device* device_ = nullptr;
    DirectionalShadowResult metadata_;
    // Sanitized camera origin used to compute metadata_.cascades. The
    // retained receiver verifies that the main camera still matches it.
    apex::scene::Vector3 receiver_position_{};
    std::array<CameraFrame, directional_shadow_cascade_count> cameras_{};
    std::array<std::unique_ptr<DepthAttachment>, directional_shadow_cascade_count>
        attachments_{};

    friend struct DirectionalShadowMapResult;
    friend DirectionalShadowMapResult prepare_directional_shadow_maps(
        Device&, const DirectionalShadowMapRequest&);
    friend class StaticSceneResources;
};

struct DirectionalShadowMapResult {
    DirectionalShadowMapStatus status = DirectionalShadowMapStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<DirectionalShadowMapResources> resources;

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectionalShadowMapStatus::ready && resources != nullptr;
    }
};

[[nodiscard]] DirectionalShadowMapResult prepare_directional_shadow_maps(
    Device& device, const DirectionalShadowMapRequest& request);
[[nodiscard]] const char* directional_shadow_map_status_name(
    DirectionalShadowMapStatus status) noexcept;

struct DirectionalShadowCasterReport {
    std::string code;
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
};

struct StaticSceneDirectionalShadowFrameDescription {
    DirectionalShadowMapResources* maps = nullptr;
    const PipelineProgram* opaque_pipeline = nullptr;
    // Explicit caller-supplied ksShadowGenAT-compatible pipeline. It must
    // consume t0/s3/b4 as validated by the device depth contract.
    const PipelineProgram* alpha_static_pipeline = nullptr;
    std::span<const DrawPacket> refreshed_packets{};
    // Uses the same prepared-packet byte-mask contract as the color frame.
    // Hidden packets are not selected or skinned as shadow casters.
    std::span<const std::uint8_t> packet_visibility{};
    // Optional caller-supplied depth-only pipeline for already-retained CPU
    // skinned geometry. A missing pipeline keeps skinned casters staged; this
    // seam does not infer or synthesize a stock ksShadowGen shader.
    const PipelineProgram* skinned_pipeline = nullptr;
    // Non-owning tables are required only for caller_tables preparation. The
    // embedded_kn5 path uses the textures and sampler retained by the static
    // scene resources.
    std::span<const Texture* const> textures_by_global_index{};
    std::span<const Sampler* const> samplers_by_global_index{};
};

enum class StaticSceneDirectionalShadowStatus : std::uint8_t {
    ready,
    partial,
    invalid_request,
    unsupported,
    execution_failed,
};

struct StaticSceneDirectionalShadowResult {
    StaticSceneDirectionalShadowStatus status =
        StaticSceneDirectionalShadowStatus::unsupported;
    Diagnostic diagnostic;
    std::size_t selected_casters = 0U;
    std::size_t opaque_casters = 0U;
    std::size_t alpha_tested_casters = 0U;
    std::size_t skinned_casters = 0U;
    std::size_t staged_alpha_tested = 0U;
    std::size_t staged_skinned = 0U;
    std::size_t cascades_completed = 0U;
    std::vector<DirectionalShadowCasterReport> staged_casters;

    [[nodiscard]] bool ok() const noexcept {
        return status == StaticSceneDirectionalShadowStatus::ready ||
               status == StaticSceneDirectionalShadowStatus::partial;
    }
};

[[nodiscard]] const char* static_scene_directional_shadow_status_name(
    StaticSceneDirectionalShadowStatus status) noexcept;

} // namespace apex::render
