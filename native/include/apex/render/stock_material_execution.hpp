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
// SPIR-V, DXBC, or DXIL modules and an explicit immutable Vulkan ksPerPixel
// source package. Stock-container translation remains staged.
enum class StockMaterialShaderKeyKind : std::uint8_t {
    material_name,
    shader_family,
};

enum class StockMaterialShaderVariant : std::uint8_t {
    standard,
    damage_dust,
};

struct StockMaterialShaderModules {
    StockMaterialShaderKeyKind key_kind = StockMaterialShaderKeyKind::shader_family;
    std::string key;
    std::span<const PipelineShaderModule> modules{};
    // The label is part of the executable shader contract. A packet that
    // binds txDust must not silently use bytecode that omits txDust.
    StockMaterialShaderVariant variant = StockMaterialShaderVariant::standard;
    // The receiver extension is an orthogonal shader-module contract. The
    // selector never mixes receiver and non-receiver modules for one request.
    bool directional_shadow_receiver = false;
    // Orthogonal portable bindings 21-23 variant. Only the four bounded
    // MultiMap families can select a module set with this flag.
    bool multimap_reflection = false;
};

struct StockMaterialExecutionLimits {
    MaterialBindingLimits material{};
    StaticSceneResourceLimits scene{};
    std::size_t max_shader_sets = 4096U;
    std::size_t max_shader_key_bytes = 256U;
};

enum class BuiltinVulkanStockSourceSelector : std::uint8_t {
    disabled,
    ks_per_pixel,
};

enum class BuiltinD3D12StockNativeSelector : std::uint8_t {
    disabled,
    ks_per_pixel_base,
    ks_per_pixel_alpha_to_coverage,
    ks_per_pixel_base_and_alpha_to_coverage,
};

struct StockMaterialD3D12NativeProgram {
    // Exact stock shader-family key. Native selection accepts only the
    // selector's ksPerPixel/ksPerPixelAT keys paired with matching validated
    // packages.
    std::string key;
    std::shared_ptr<const ValidatedStockKsPerPixelNativeProgram> program;
};

struct StockMaterialExecutionRequest {
    const formats::Kn5File* model = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    // Packets must already be validated by build_draw_packets. The handoff
    // copies them and never changes shader_execution_supported.
    std::span<const DrawPacket> packets{};
    // A set may be selected by exact material name or canonical shader name.
    // Material-name sets take precedence over shader-family sets. Skinned
    // packets always use the ksSkinnedMesh family because their vertex ABI
    // cannot safely inherit a static material-name override.
    std::span<const StockMaterialShaderModules> shader_modules{};
    // Opt-in fallback for exact static ksPerPixel packets on Vulkan. A
    // matching caller module set always remains authoritative. Other shader
    // families, skinned packets, D3D12, and portable receiver modules never
    // enter this source-equivalent path.
    BuiltinVulkanStockSourceSelector builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::disabled;
    StockKsPerPixelNativeSamplerSettings
        builtin_vulkan_source_sampler_settings{};
    // Opt-in installed-DXBC execution for opaque static ksPerPixel and
    // ksPerPixelAT scenes on D3D12. Matching caller modules remain
    // authoritative. Each validated package owner is cloned into one mutable
    // b0-b4 resource bundle per selected packet by StaticSceneResources.
    BuiltinD3D12StockNativeSelector builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::disabled;
    std::span<const StockMaterialD3D12NativeProgram>
        builtin_d3d12_native_programs{};
    StockKsPerPixelNativeSamplerSettings
        builtin_d3d12_native_sampler_settings{};
    // Empty means no overrides. Otherwise this table must match model
    // material order. External/solid-color resource overrides are rejected
    // because StaticSceneResources resolves KN5 texture ownership only.
    std::span<const MaterialBindingOverrides> overrides_by_material{};
    PipelineRenderTargets targets{};
    bool wireframe = false;
    // Selects shader modules and the matching set 0/bindings 16-20 receiver
    // declaration. The per-frame resource binding is supplied separately by
    // StaticSceneFrameDescription when the prepared scene is drawn.
    bool directional_shadow_receiver = false;
    // Select reflection-capable modules and the frame-owned cube extension
    // for the four bounded MultiMap families only.
    bool multimap_reflection = false;
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

// Build up to two executable pipeline owners per used material. The two
// variants preserve opaque and transparent node state. The optional built-in
// owner is restricted to exact static Vulkan ksPerPixel packets; all other
// paths use explicit PipelineProgram values. Build one resolved 80-byte
// material record per used material, then synchronously prepare the static
// scene. Supported shader families are
// ksPerPixel, ksPerPixelAT, ksSkinnedMesh, ksPerPixelNM, ksPerPixelMultiMap,
// ksPerPixelMultiMap_AT, ksPerPixelMultiMap_NMDetail, and
// ksPerPixelMultiMap_AT_NMDetail. A bounded dirt-zero stage from
// ksPerPixelMultiMap_damage_dirt is also supported. This stage does not claim
// parity for stock detail, sun-specular, Fresnel, or reflection branches. Both
// AT profiles retain their A2C state.
[[nodiscard]] StockMaterialExecutionResult prepare_stock_material_execution(
    Device& device, const StockMaterialExecutionRequest& request);

}  // namespace apex::render
