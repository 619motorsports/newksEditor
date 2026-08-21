#pragma once

#include "apex/render/material_binding.hpp"
#include "apex/render/static_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace apex::render {

// This component is an explicit handoff from validated KN5 material data to
// the already-executable static-scene adapter. It accepts caller-supplied
// SPIR-V or DXIL modules only; stock-container translation remains staged.
enum class StockMaterialShaderKeyKind : std::uint8_t {
    material_name,
    shader_family,
};

struct StockMaterialShaderModules {
    StockMaterialShaderKeyKind key_kind = StockMaterialShaderKeyKind::shader_family;
    std::string key;
    std::span<const PipelineShaderModule> modules{};
};

struct StockMaterialExecutionLimits {
    MaterialBindingLimits material{};
    StaticSceneResourceLimits scene{};
    std::size_t max_shader_sets = 4096U;
    std::size_t max_shader_key_bytes = 256U;
};

struct StockMaterialExecutionRequest {
    const formats::Kn5File* model = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    // Packets must already be validated by build_draw_packets. The handoff
    // copies them and never changes shader_execution_supported.
    std::span<const DrawPacket> packets{};
    // A set may be selected by exact material name or canonical shader name.
    // Material-name sets take precedence over shader-family sets.
    std::span<const StockMaterialShaderModules> shader_modules{};
    // Empty means no overrides. Otherwise this table must match model
    // material order. External/solid-color resource overrides are rejected
    // because StaticSceneResources resolves KN5 texture ownership only.
    std::span<const MaterialBindingOverrides> overrides_by_material{};
    PipelineRenderTargets targets{};
    bool wireframe = false;
    StaticSceneTextureAuthority texture_authority =
        StaticSceneTextureAuthority::caller_tables;
    StockMaterialExecutionLimits limits{};
};

struct StockMaterialExecutionResult {
    StaticSceneResourceStatus status = StaticSceneResourceStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<StaticSceneResources> resources;

    [[nodiscard]] bool ok() const noexcept {
        return status == StaticSceneResourceStatus::ready && resources != nullptr;
    }
};

// Build up to two explicit PipelineProgram values per used material. The two
// variants preserve opaque and transparent node state. Build one resolved
// 80-byte material record per used material, then synchronously prepare the
// static scene. Supported shader families are
// ksPerPixel, ksPerPixelNM, ksPerPixelMultiMap,
// ksPerPixelMultiMap_AT, ksPerPixelMultiMap_NMDetail, and
// ksPerPixelMultiMap_AT_NMDetail. The dirt-zero
// ksPerPixelMultiMap_damage_dirt branch is also supported. Both AT profiles
// retain their A2C state.
[[nodiscard]] StockMaterialExecutionResult prepare_stock_material_execution(
    Device& device, const StockMaterialExecutionRequest& request);

}  // namespace apex::render
