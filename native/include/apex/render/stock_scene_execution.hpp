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
// execute frame-plan effects such as shadows, reflections, sky, CSP lights,
// or post-processing. Per-node CSP mesh state is supported, while CSP shader,
// property, and resource changes remain explicit evidence in render_plan.
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
    std::span<const MaterialBindingOverrides> overrides_by_material{};
    // When enabled, resolve the source-evidenced F4 state at this facade
    // boundary. The resolver's complete material table is merged after any
    // caller-provided material overrides.
    bool evaluate_damage_preview = false;
    std::optional<bool> damage_broken_visible;
    PipelineRenderTargets targets{};
    bool wireframe = false;
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
