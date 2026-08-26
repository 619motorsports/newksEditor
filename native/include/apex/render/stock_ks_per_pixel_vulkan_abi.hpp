#pragma once

#include "apex/render/pipeline.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"

#include <cstdint>
#include <span>

namespace apex::render {

// This manifest describes the recovered native register namespace after its
// backend-neutral Vulkan set mapping. It is an ABI transport contract only:
// it does not assert that a probe shader reproduces installed ksPerPixel
// equations, DXBC instructions, or native material behavior.
struct StockKsPerPixelVulkanAbiManifest {
  std::span<const StockKsPerPixelBackendBinding> bindings{};
  std::uint32_t descriptor_set_count = 0U;
  PipelineTransformContract transform_contract =
      PipelineTransformContract::none;
  PipelineShaderProvenance shader_provenance =
      PipelineShaderProvenance::native_abi_probe;
};

inline constexpr StockKsPerPixelVulkanAbiManifest
    stock_ks_per_pixel_vulkan_abi_manifest{
        stock_ks_per_pixel_backend_contract, 3U,
        PipelineTransformContract::none,
        PipelineShaderProvenance::native_abi_probe};

enum class StockKsPerPixelVulkanAbiStatus : std::uint8_t {
  ready,
  manifest_invalid,
  shader_count_mismatch,
  shader_stage_mismatch,
  shader_format_mismatch,
  shader_provenance_mismatch,
  transform_contract_mismatch,
  resource_count_mismatch,
  resource_kind_mismatch,
  resource_set_mismatch,
  resource_binding_mismatch,
  resource_name_mismatch,
};

[[nodiscard]] const char *stock_ks_per_pixel_vulkan_abi_status_name(
    StockKsPerPixelVulkanAbiStatus status) noexcept;

// Validate a pipeline declaration before a future multi-set Vulkan adapter
// allocates descriptors. This is allocation-free and does not create Vulkan
// objects or inspect shader instructions.
[[nodiscard]] StockKsPerPixelVulkanAbiStatus
validate_stock_ks_per_pixel_vulkan_abi(
    const PipelineProgram &program,
    const StockKsPerPixelVulkanAbiManifest &manifest =
        stock_ks_per_pixel_vulkan_abi_manifest) noexcept;

} // namespace apex::render
