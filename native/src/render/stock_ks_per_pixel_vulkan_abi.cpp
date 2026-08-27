#include "apex/render/stock_ks_per_pixel_vulkan_abi.hpp"

#include <cstddef>

namespace apex::render {
namespace {

[[nodiscard]] bool
valid_manifest(const StockKsPerPixelVulkanAbiManifest &manifest) noexcept {
  return manifest.descriptor_set_count == 3U &&
         manifest.transform_contract == PipelineTransformContract::none &&
         manifest.shader_provenance ==
             PipelineShaderProvenance::native_abi_probe &&
         validate_stock_ks_per_pixel_backend_contract(manifest.bindings) ==
             StockKsPerPixelBindingStatus::ready;
}

[[nodiscard]] PipelineResourceKind
pipeline_kind(StockShaderDescriptorKind kind) noexcept {
  switch (kind) {
  case StockShaderDescriptorKind::uniform_buffer:
    return PipelineResourceKind::uniform_buffer;
  case StockShaderDescriptorKind::sampled_image:
    return PipelineResourceKind::sampled_texture;
  case StockShaderDescriptorKind::sampler:
    return PipelineResourceKind::sampler;
  }
  return PipelineResourceKind::storage_buffer;
}

} // namespace

const char *stock_ks_per_pixel_vulkan_abi_status_name(
    StockKsPerPixelVulkanAbiStatus status) noexcept {
  switch (status) {
  case StockKsPerPixelVulkanAbiStatus::ready:
    return "ready";
  case StockKsPerPixelVulkanAbiStatus::manifest_invalid:
    return "manifest_invalid";
  case StockKsPerPixelVulkanAbiStatus::shader_count_mismatch:
    return "shader_count_mismatch";
  case StockKsPerPixelVulkanAbiStatus::shader_stage_mismatch:
    return "shader_stage_mismatch";
  case StockKsPerPixelVulkanAbiStatus::shader_format_mismatch:
    return "shader_format_mismatch";
  case StockKsPerPixelVulkanAbiStatus::shader_provenance_mismatch:
    return "shader_provenance_mismatch";
  case StockKsPerPixelVulkanAbiStatus::transform_contract_mismatch:
    return "transform_contract_mismatch";
  case StockKsPerPixelVulkanAbiStatus::resource_count_mismatch:
    return "resource_count_mismatch";
  case StockKsPerPixelVulkanAbiStatus::resource_kind_mismatch:
    return "resource_kind_mismatch";
  case StockKsPerPixelVulkanAbiStatus::resource_set_mismatch:
    return "resource_set_mismatch";
  case StockKsPerPixelVulkanAbiStatus::resource_binding_mismatch:
    return "resource_binding_mismatch";
  case StockKsPerPixelVulkanAbiStatus::resource_name_mismatch:
    return "resource_name_mismatch";
  }
  return "unknown";
}

StockKsPerPixelVulkanAbiStatus validate_stock_ks_per_pixel_vulkan_abi(
    const PipelineProgram &program,
    const StockKsPerPixelVulkanAbiManifest &manifest) noexcept {
  if (!valid_manifest(manifest))
    return StockKsPerPixelVulkanAbiStatus::manifest_invalid;
  if (program.shaders.size() != 2U)
    return StockKsPerPixelVulkanAbiStatus::shader_count_mismatch;
  if (program.shaders[0].stage != PipelineShaderStage::vertex ||
      program.shaders[1].stage != PipelineShaderStage::fragment)
    return StockKsPerPixelVulkanAbiStatus::shader_stage_mismatch;
  for (const PipelineShaderModule &shader : program.shaders) {
    if (shader.format != PipelineShaderFormat::spirv)
      return StockKsPerPixelVulkanAbiStatus::shader_format_mismatch;
    if (shader.provenance != manifest.shader_provenance)
      return StockKsPerPixelVulkanAbiStatus::shader_provenance_mismatch;
  }
  if (program.transform_contract != manifest.transform_contract)
    return StockKsPerPixelVulkanAbiStatus::transform_contract_mismatch;
  if (program.resources.size() != manifest.bindings.size())
    return StockKsPerPixelVulkanAbiStatus::resource_count_mismatch;

  for (std::size_t index = 0U; index < manifest.bindings.size(); ++index) {
    const StockKsPerPixelBackendBinding &expected = manifest.bindings[index];
    const PipelineResourceBinding &actual = program.resources[index];
    if (actual.kind != pipeline_kind(expected.descriptor_kind))
      return StockKsPerPixelVulkanAbiStatus::resource_kind_mismatch;
    if (actual.set != expected.vulkan_set)
      return StockKsPerPixelVulkanAbiStatus::resource_set_mismatch;
    if (actual.binding != expected.vulkan_binding)
      return StockKsPerPixelVulkanAbiStatus::resource_binding_mismatch;
    if (actual.name != expected.name)
      return StockKsPerPixelVulkanAbiStatus::resource_name_mismatch;
  }
  return StockKsPerPixelVulkanAbiStatus::ready;
}

} // namespace apex::render
