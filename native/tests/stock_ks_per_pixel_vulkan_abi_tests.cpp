#include "apex/render/stock_ks_per_pixel_vulkan_abi.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

PipelineResourceKind pipeline_kind(StockShaderDescriptorKind kind) {
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

PipelineProgram probe_program() {
  PipelineProgram program;
  program.name = "recovered-ks-per-pixel-native-abi-probe";
  program.shaders = {
      {PipelineShaderStage::vertex,
       PipelineShaderFormat::spirv,
       {0x03U, 0x02U, 0x23U, 0x07U},
       PipelineShaderProvenance::native_abi_probe},
      {PipelineShaderStage::fragment,
       PipelineShaderFormat::spirv,
       {0x03U, 0x02U, 0x23U, 0x07U},
       PipelineShaderProvenance::native_abi_probe},
  };
  for (const StockKsPerPixelBackendBinding &binding :
       stock_ks_per_pixel_vulkan_abi_manifest.bindings) {
    program.resources.push_back({pipeline_kind(binding.descriptor_kind),
                                 binding.vulkan_set, binding.vulkan_binding,
                                 std::string(binding.name)});
  }
  return program;
}

void acceptsRecoveredManifestAndProbe() {
  require(validate_stock_ks_per_pixel_backend_contract(
              stock_ks_per_pixel_vulkan_abi_manifest.bindings) ==
              StockKsPerPixelBindingStatus::ready,
          "Vulkan ABI manifest reuses the recovered register mapping");
  require(stock_ks_per_pixel_vulkan_abi_manifest.descriptor_set_count == 3U,
          "recovered Vulkan ABI has three descriptor namespaces");
  require(stock_ks_per_pixel_vulkan_abi_manifest.transform_contract ==
              PipelineTransformContract::none,
          "native ABI probe does not use portable draw-matrix push constants");
  require(pipeline_shader_provenance_name(
              PipelineShaderProvenance::native_abi_probe) ==
              std::string_view("native_abi_probe"),
          "probe provenance is explicit and distinct");
  require(validate_stock_ks_per_pixel_vulkan_abi(probe_program()) ==
              StockKsPerPixelVulkanAbiStatus::ready,
          "well-formed recovered ABI probe is accepted");
}

void rejectsPortableOrMismatchedDeclarations() {
  PipelineProgram portable = probe_program();
  portable.shaders[0].provenance = PipelineShaderProvenance::portable;
  require(validate_stock_ks_per_pixel_vulkan_abi(portable) ==
              StockKsPerPixelVulkanAbiStatus::shader_provenance_mismatch,
          "portable shader provenance cannot enter native ABI probe");

  PipelineProgram translated = probe_program();
  translated.shaders[1].provenance = PipelineShaderProvenance::translated;
  require(validate_stock_ks_per_pixel_vulkan_abi(translated) ==
              StockKsPerPixelVulkanAbiStatus::shader_provenance_mismatch,
          "translated label cannot enter native ABI probe");

  PipelineProgram native = probe_program();
  native.shaders[0].format = PipelineShaderFormat::dxbc;
  require(validate_stock_ks_per_pixel_vulkan_abi(native) ==
              StockKsPerPixelVulkanAbiStatus::shader_format_mismatch,
          "installed DXBC cannot enter native Vulkan ABI probe");

  PipelineProgram push_constants = probe_program();
  push_constants.transform_contract = PipelineTransformContract::draw_matrices;
  require(validate_stock_ks_per_pixel_vulkan_abi(push_constants) ==
              StockKsPerPixelVulkanAbiStatus::transform_contract_mismatch,
          "portable push constants cannot enter native ABI probe");
}

void rejectsResourceNamespaceDrift() {
  PipelineProgram missing = probe_program();
  missing.resources.pop_back();
  require(validate_stock_ks_per_pixel_vulkan_abi(missing) ==
              StockKsPerPixelVulkanAbiStatus::resource_count_mismatch,
          "missing recovered descriptor is rejected");

  PipelineProgram wrong_set = probe_program();
  wrong_set.resources[5].set = 0U;
  require(validate_stock_ks_per_pixel_vulkan_abi(wrong_set) ==
              StockKsPerPixelVulkanAbiStatus::resource_set_mismatch,
          "texture moved into constant-buffer set is rejected");

  PipelineProgram wrong_binding = probe_program();
  wrong_binding.resources[6].binding = 5U;
  require(validate_stock_ks_per_pixel_vulkan_abi(wrong_binding) ==
              StockKsPerPixelVulkanAbiStatus::resource_binding_mismatch,
          "shadow texture binding drift is rejected");

  PipelineProgram wrong_kind = probe_program();
  wrong_kind.resources[10].kind = PipelineResourceKind::sampled_texture;
  require(validate_stock_ks_per_pixel_vulkan_abi(wrong_kind) ==
              StockKsPerPixelVulkanAbiStatus::resource_kind_mismatch,
          "comparison sampler kind drift is rejected");
}

void rejectsManifestDrift() {
  std::array<StockKsPerPixelBackendBinding,
             stock_ks_per_pixel_backend_contract.size()>
      bindings = stock_ks_per_pixel_backend_contract;
  bindings[0].stages = StockShaderStageMask::fragment;
  const StockKsPerPixelVulkanAbiManifest drifted{
      bindings, 3U, PipelineTransformContract::none,
      PipelineShaderProvenance::native_abi_probe};
  require(validate_stock_ks_per_pixel_vulkan_abi(probe_program(), drifted) ==
              StockKsPerPixelVulkanAbiStatus::manifest_invalid,
          "stage visibility drift invalidates the recovered manifest");
}

} // namespace

int main() {
  try {
    acceptsRecoveredManifestAndProbe();
    rejectsPortableOrMismatchedDeclarations();
    rejectsResourceNamespaceDrift();
    rejectsManifestDrift();
    std::cout << "stock ksPerPixel Vulkan ABI tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "stock ksPerPixel Vulkan ABI tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
