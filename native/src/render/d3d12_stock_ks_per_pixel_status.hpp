#pragma once

#include "apex/render/device.hpp"

#include <cstdint>
#include <string_view>

namespace apex::render {

// The native D3D12 bridge performs both request preflight and GPU work. Keep
// the distinction at the diagnostic boundary so a rejected request does not
// look like a device failure to callers.
enum class D3D12StockKsPerPixelFailureKind : std::uint8_t {
  invalid_request,
  unsupported,
  execution_failed,
};

[[nodiscard]] constexpr D3D12StockKsPerPixelFailureKind
classify_d3d12_stock_ks_per_pixel_failure(std::string_view code) noexcept {
  // These diagnostics mean that the caller supplied an incomplete,
  // inconsistent, or out-of-range native draw contract.
  if (code == "d3d12_stock_native_context_missing" ||
      code == "d3d12_stock_native_request_missing" ||
      code == "d3d12_stock_native_geometry_invalid" ||
      code == "d3d12_stock_native_shader_invalid" ||
      code == "d3d12_stock_native_samplers_missing" ||
      code == "d3d12_stock_native_sampler_contract_mismatch" ||
      code == "d3d12_stock_native_constants_missing" ||
      code == "d3d12_stock_native_target_invalid" ||
      code == "d3d12_stock_native_variant_state_mismatch" ||
      code == "d3d12_stock_native_a2c_target_samples_invalid" ||
      code == "d3d12_stock_native_target_shape_invalid" ||
      code == "d3d12_stock_native_target_scissor_limit" ||
      code == "d3d12_stock_native_diffuse_state_missing" ||
      code == "d3d12_stock_native_depth_clear_invalid" ||
      code == "d3d12_stock_native_depth_load_before_clear" ||
      code == "d3d12_stock_native_resource_alias" ||
      code == "d3d12_stock_native_geometry_range_invalid" ||
      code == "d3d12_stock_native_geometry_state_invalid" ||
      code == "d3d12_stock_native_shadow_missing" ||
      code == "d3d12_stock_native_depth_state_missing" ||
      code == "d3d12_stock_native_depth_view_missing" ||
      code == "d3d12_stock_native_texture_invalid" ||
      code == "d3d12_stock_native_readback_footprint_invalid" ||
      code == "indexed_stock_native_binding_missing" ||
      code == "indexed_stock_native_diffuse_uninitialized" ||
      code == "indexed_stock_native_shadow_uninitialized") {
    return D3D12StockKsPerPixelFailureKind::invalid_request;
  }

  // These cases cannot be represented by the native path. The resource may
  // still be valid for a portable renderer, so callers should report
  // unsupported rather than execution_failed.
  if (code == "d3d12_stock_native_sampler_backend_mismatch" ||
      code == "d3d12_stock_native_target_format_unsupported" ||
      code == "d3d12_stock_native_wireframe_unsupported" ||
      code == "indexed_stock_native_type_unsupported" ||
      code == "indexed_stock_native_context_mismatch") {
    return D3D12StockKsPerPixelFailureKind::unsupported;
  }

  // Batch preflight uses a separate diagnostic namespace but the same
  // semantics. Keep this list explicit: an unknown future diagnostic must be
  // conservative and remain an execution failure until classified.
  if (code == "d3d12_stock_native_batch_context_missing" ||
      code == "d3d12_stock_native_batch_empty" ||
      code == "d3d12_stock_native_batch_too_many_draws" ||
      code == "d3d12_stock_native_batch_sampler_heap_limit" ||
      code == "d3d12_stock_native_batch_target_invalid" ||
      code == "d3d12_stock_native_batch_target_state_missing" ||
      code == "d3d12_stock_native_batch_clear_color_invalid" ||
      code == "d3d12_stock_native_batch_clear_depth_invalid" ||
      code == "d3d12_stock_native_batch_color_load_before_clear" ||
      code == "d3d12_stock_native_batch_target_shape_invalid" ||
      code == "d3d12_stock_native_batch_depth_context_missing" ||
      code == "d3d12_stock_native_batch_depth_shape_invalid" ||
      code == "d3d12_stock_native_batch_draw_context_missing" ||
      code == "d3d12_stock_native_batch_geometry_invalid" ||
      code == "d3d12_stock_native_batch_depth_state_mismatch" ||
      code == "d3d12_stock_native_batch_pipeline_target_invalid" ||
      code == "d3d12_stock_native_batch_vertex_layout_invalid" ||
      code == "d3d12_stock_native_batch_shader_invalid" ||
      code == "d3d12_stock_native_batch_bindings_invalid" ||
      code == "d3d12_stock_native_batch_constants_missing" ||
      code == "d3d12_stock_native_batch_shadows_missing" ||
      code == "d3d12_stock_native_batch_depth_missing" ||
      code == "d3d12_stock_native_batch_geometry_resource_invalid" ||
      code == "d3d12_stock_native_batch_geometry_range_invalid" ||
      code == "d3d12_stock_native_batch_geometry_state_invalid" ||
      code == "d3d12_stock_native_batch_resolve_alias" ||
      code == "d3d12_stock_native_batch_resolve_context_missing" ||
      code == "d3d12_stock_native_batch_resolve_shape_invalid" ||
      code == "d3d12_stock_native_batch_resolve_unexpected" ||
      code == "d3d12_stock_native_batch_depth_clear_invalid" ||
      code == "d3d12_stock_native_batch_depth_load_before_clear" ||
      code == "d3d12_stock_native_batch_resource_alias" ||
      code == "d3d12_stock_native_batch_shadow_alias" ||
      code == "d3d12_stock_native_batch_diffuse_invalid" ||
      code == "d3d12_stock_native_batch_constant_resource_invalid" ||
      code == "d3d12_stock_native_batch_constant_alias" ||
      code == "d3d12_stock_native_batch_descriptor_count_invalid" ||
      code == "d3d12_stock_native_batch_diffuse_view_invalid" ||
      code == "d3d12_stock_native_batch_shadow_view_invalid" ||
      code == "d3d12_stock_native_batch_readback_invalid" ||
      code == "indexed_stock_native_batch_resource_uninitialized") {
    return D3D12StockKsPerPixelFailureKind::invalid_request;
  }
  if (code == "d3d12_stock_native_batch_target_format_unsupported" ||
      code == "d3d12_stock_native_batch_readback_format_unsupported" ||
      code == "d3d12_stock_native_batch_target_samples_invalid" ||
      code == "d3d12_stock_native_batch_pipeline_unsupported" ||
      code == "indexed_stock_native_batch_type_unsupported" ||
      code == "indexed_stock_native_batch_device_mismatch" ||
      code == "indexed_stock_native_batch_context_mismatch") {
    return D3D12StockKsPerPixelFailureKind::unsupported;
  }
  return D3D12StockKsPerPixelFailureKind::execution_failed;
}

[[nodiscard]] constexpr IndexedStaticMeshDrawStatus
indexed_static_mesh_draw_status(
    D3D12StockKsPerPixelFailureKind kind) noexcept {
  switch (kind) {
  case D3D12StockKsPerPixelFailureKind::invalid_request:
    return IndexedStaticMeshDrawStatus::invalid_request;
  case D3D12StockKsPerPixelFailureKind::unsupported:
    return IndexedStaticMeshDrawStatus::unsupported;
  case D3D12StockKsPerPixelFailureKind::execution_failed:
    return IndexedStaticMeshDrawStatus::execution_failed;
  }
  return IndexedStaticMeshDrawStatus::execution_failed;
}

[[nodiscard]] constexpr IndexedStaticMeshBatchStatus
indexed_static_mesh_batch_status(
    D3D12StockKsPerPixelFailureKind kind) noexcept {
  switch (kind) {
  case D3D12StockKsPerPixelFailureKind::invalid_request:
    return IndexedStaticMeshBatchStatus::invalid_request;
  case D3D12StockKsPerPixelFailureKind::unsupported:
    return IndexedStaticMeshBatchStatus::unsupported;
  case D3D12StockKsPerPixelFailureKind::execution_failed:
    return IndexedStaticMeshBatchStatus::execution_failed;
  }
  return IndexedStaticMeshBatchStatus::execution_failed;
}

} // namespace apex::render
