#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>

namespace apex::render {

// Private wrapper-to-native bridge for the standalone exact stock draw.
// d3d12_device.cpp remains the owner of the concrete wrapper classes.
struct D3D12StockKsPerPixelNativeDrawContext {
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
  ID3D12Resource *depth = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE depth_write_dsv{};
  D3D12_CPU_DESCRIPTOR_HANDLE depth_read_dsv{};
  D3D12_RESOURCE_STATES *depth_state = nullptr;
  bool *depth_cleared = nullptr;
  ID3D12Resource *vertex_buffer = nullptr;
  D3D12_RESOURCE_STATES vertex_state = D3D12_RESOURCE_STATE_COMMON;
  ID3D12Resource *index_buffer = nullptr;
  D3D12_RESOURCE_STATES index_state = D3D12_RESOURCE_STATE_COMMON;
  ID3D12Resource *diffuse_texture = nullptr;
  DXGI_FORMAT diffuse_format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES *diffuse_state = nullptr;
  std::array<ID3D12Resource *, 3U> shadow_maps{};
  std::array<D3D12_RESOURCE_STATES *, 3U> shadow_states{};
  const StockKsPerPixelNativeShaderProgram *shader_program = nullptr;
  const StockKsPerPixelNativeConstantBuffers *constant_buffers = nullptr;
  const StockKsPerPixelNativeSamplers *samplers = nullptr;
  std::array<ID3D12Resource *, 5U> constant_buffer_resources{};
  const IndexedStaticMeshDrawRequest *request = nullptr;
  std::vector<std::byte> *readback = nullptr;
};

// The allocator and list must be idle and the list closed on entry.  The
// helper resets them, submits synchronously, and leaves the list closed.
[[nodiscard]] bool record_d3d12_stock_ks_per_pixel_native_draw(
    const D3D12StockKsPerPixelNativeDrawContext &context,
    Diagnostic &diagnostic);

} // namespace apex::render

#endif
