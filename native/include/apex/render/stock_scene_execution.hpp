#pragma once

#include "apex/render/damage_preview.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/stock_material_execution.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace apex::render {

// This is the bounded main-color scene boundary. It deliberately does not
// execute most frame-plan effects such as reflections, sky, CSP lights, or
// post-processing. A caller can opt into the bounded retained directional-
// shadow receiver contract. Per-node CSP mesh state is supported. Callers must
// resolve external resources into owned model payloads before this boundary.
// Static Vulkan ksPerPixel packets can explicitly select the immutable
// source-equivalent package when no caller module set matches.
// Other CSP shader, property, and resource changes remain explicit evidence in
// render_plan.
struct StockSceneExecutionLimits {
    std::size_t max_scene_nodes = 1'000'000U;
    std::size_t max_scene_materials = 1'000'000U;
    std::size_t max_model_materials = 4096U;
    std::size_t max_model_textures = 65'536U;
    std::size_t max_plan_items = 4096U;
    std::uint64_t max_plan_bytes = 256ULL * 1024ULL * 1024ULL;
    DrawPacketLimits packets{};
    StockMaterialExecutionLimits material{};
    DamagePreviewLimits damage{};
};

struct StockSceneExecutionRequest {
    const formats::Kn5File* model = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    RenderPlanOptions render{};
    DrawPacketOptions packets{};
    std::span<const StockMaterialShaderModules> shader_modules{};
    BuiltinVulkanStockSourceSelector builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::disabled;
    StockKsPerPixelNativeSamplerSettings
        builtin_vulkan_source_sampler_settings{};
    BuiltinD3D12StockNativeSelector builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::disabled;
    std::span<const StockMaterialD3D12NativeProgram>
        builtin_d3d12_native_programs{};
    StockKsPerPixelNativeSamplerSettings
        builtin_d3d12_native_sampler_settings{};
    std::span<const MaterialBindingOverrides> overrides_by_material{};
    // When enabled, resolve the source-evidenced F4 state at this facade
    // boundary. The resolver's complete material table is merged after any
    // caller-provided material overrides.
    bool evaluate_damage_preview = false;
    std::optional<bool> damage_broken_visible;
    PipelineRenderTargets targets{};
    bool wireframe = false;
    // Selects receiver-capable stock shader modules and prepares the scene-
    // owned bindings used by StaticSceneFrameDescription.
    bool directional_shadow_receiver = false;
    StaticSceneTextureAuthority texture_authority =
        StaticSceneTextureAuthority::caller_tables;
    StockSceneExecutionLimits limits{};
};

struct StockSceneExecutionResult {
    StaticSceneResourceStatus status = StaticSceneResourceStatus::unsupported;
    Diagnostic diagnostic;
    // The plan and packet evidence are retained even when main-color
    // execution fails. StaticSceneResources owns its own packet copy, so this
    // result intentionally does not make a second retained packet copy.
    RenderPlan render_plan;
    // Complete prepared-packet permutation for native Shadowgen traversal.
    // Visibility and refreshed state remain keyed by prepared packet index.
    std::vector<std::uint32_t> shadow_packet_order;
    std::vector<DrawPacketDiagnostic> packet_diagnostics;
    std::vector<DrawPacketUnsupportedEffect> packet_unsupported_effects;
    // Retain the bounded F4 audit, including diagnostics, when requested.
    std::optional<DamagePreviewResult> damage_preview;
    std::unique_ptr<StaticSceneResources> resources;

    [[nodiscard]] bool ok() const noexcept {
        return status == StaticSceneResourceStatus::ready && resources != nullptr;
    }
};

[[nodiscard]] StockSceneExecutionResult prepare_stock_scene_execution(
    Device& device, const StockSceneExecutionRequest& request);

}  // namespace apex::render
