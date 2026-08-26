#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>

namespace apex::render {

// Raw bridge record for one ordinary native ksPerPixel indexed draw. It does
// not contain StockKsPerPixelNativeDrawBinding so a wrapper can assemble a
// batch without exposing its binding object to the executor.
struct D3D12StockKsPerPixelNativeBatchDraw {
  const DrawPacket *packet = nullptr;
  const PipelineProgram *pipeline = nullptr;
  StaticMeshIndexType index_type = StaticMeshIndexType::uint16;
  ID3D12Resource *vertex_buffer = nullptr;
  D3D12_RESOURCE_STATES vertex_state = D3D12_RESOURCE_STATE_COMMON;
  ID3D12Resource *index_buffer = nullptr;
  D3D12_RESOURCE_STATES index_state = D3D12_RESOURCE_STATE_COMMON;
  const StockKsPerPixelNativeShaderProgram *shader_program = nullptr;
  const StockKsPerPixelNativeConstantBuffers *constant_buffers = nullptr;
  const StockKsPerPixelNativeSamplers *samplers = nullptr;
  std::array<ID3D12Resource *, 5U> constant_buffer_resources{};
  ID3D12Resource *diffuse_texture = nullptr;
  DXGI_FORMAT diffuse_format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES *diffuse_state = nullptr;
  std::array<ID3D12Resource *, 3U> shadow_maps{};
  std::array<D3D12_RESOURCE_STATES *, 3U> shadow_states{};
};

// The bridge owns the target/depth state pointers and keeps every resource
// alive through this synchronous call. The allocator is idle and the list is
// closed on entry; the helper submits one command list and leaves it closed.
// Only homogeneous validated base or alpha-to-coverage ksPerPixel programs
// and ordinary static triangle draws are accepted. Overlay, selected,
// portable, and mixed-variant draws are intentionally rejected.
struct D3D12StockKsPerPixelNativeBatchContext {
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *command_list = nullptr;
  ID3D12Resource *target = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE target_rtv{};
  D3D12_RESOURCE_STATES *target_state = nullptr;
  DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
  std::uint32_t target_width = 0U;
  std::uint32_t target_height = 0U;
  bool load_color = false;
  std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
  bool *target_cleared = nullptr;
  ID3D12Resource *resolve_target = nullptr;
  D3D12_RESOURCE_STATES *resolve_target_state = nullptr;
  bool *resolve_target_cleared = nullptr;
  ID3D12Resource *depth = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE depth_write_dsv{};
  D3D12_CPU_DESCRIPTOR_HANDLE depth_read_dsv{};
  D3D12_RESOURCE_STATES *depth_state = nullptr;
  bool *depth_cleared = nullptr;
  bool clear_depth = false;
  float depth_clear_value = 1.0F;
  std::span<const D3D12StockKsPerPixelNativeBatchDraw> draws{};
  bool capture_rgba8 = true;
  std::vector<std::byte> *readback = nullptr;
};

[[nodiscard]] bool record_d3d12_stock_ks_per_pixel_native_batch(
    const D3D12StockKsPerPixelNativeBatchContext &context,
    Diagnostic &diagnostic);

} // namespace apex::render

#endif
