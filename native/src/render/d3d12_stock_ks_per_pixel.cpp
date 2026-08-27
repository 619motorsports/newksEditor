#include "d3d12_stock_ks_per_pixel.hpp"
#include "apex/render/draw_packet.hpp"

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <wrl/client.h>

namespace apex::render {
namespace {

using Microsoft::WRL::ComPtr;

Diagnostic hresult_error(const char *operation, HRESULT result,
                         const char *code = "d3d12_stock_native_failed") {
  char text[9]{};
  std::snprintf(text, sizeof(text), "%08lX",
                static_cast<unsigned long>(result));
  return {code, std::string(operation) + " failed with HRESULT 0x" + text};
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (list == nullptr || resource == nullptr || before == after)
    return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  list->ResourceBarrier(1U, &barrier);
}

D3D12_BLEND blend_factor(PipelineBlendFactor factor) noexcept {
  switch (factor) {
  case PipelineBlendFactor::zero:
    return D3D12_BLEND_ZERO;
  case PipelineBlendFactor::one:
    return D3D12_BLEND_ONE;
  case PipelineBlendFactor::source_alpha:
    return D3D12_BLEND_SRC_ALPHA;
  case PipelineBlendFactor::one_minus_source_alpha:
    return D3D12_BLEND_INV_SRC_ALPHA;
  case PipelineBlendFactor::destination_color:
    return D3D12_BLEND_DEST_COLOR;
  case PipelineBlendFactor::destination_alpha:
    return D3D12_BLEND_DEST_ALPHA;
  case PipelineBlendFactor::one_minus_destination_alpha:
    return D3D12_BLEND_INV_DEST_ALPHA;
  }
  return D3D12_BLEND_ZERO;
}

D3D12_BLEND_OP blend_op(PipelineBlendOperation operation) noexcept {
  switch (operation) {
  case PipelineBlendOperation::add:
    return D3D12_BLEND_OP_ADD;
  case PipelineBlendOperation::subtract:
    return D3D12_BLEND_OP_SUBTRACT;
  case PipelineBlendOperation::reverse_subtract:
    return D3D12_BLEND_OP_REV_SUBTRACT;
  }
  return D3D12_BLEND_OP_ADD;
}

D3D12_COMPARISON_FUNC compare(PipelineCompareOperation operation) noexcept {
  switch (operation) {
  case PipelineCompareOperation::never:
    return D3D12_COMPARISON_FUNC_NEVER;
  case PipelineCompareOperation::less:
    return D3D12_COMPARISON_FUNC_LESS;
  case PipelineCompareOperation::equal:
    return D3D12_COMPARISON_FUNC_EQUAL;
  case PipelineCompareOperation::less_or_equal:
    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
  case PipelineCompareOperation::greater:
    return D3D12_COMPARISON_FUNC_GREATER;
  case PipelineCompareOperation::not_equal:
    return D3D12_COMPARISON_FUNC_NOT_EQUAL;
  case PipelineCompareOperation::greater_or_equal:
    return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
  case PipelineCompareOperation::always:
    return D3D12_COMPARISON_FUNC_ALWAYS;
  }
  return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_TEXTURE_ADDRESS_MODE address(SamplerAddressMode mode) noexcept {
  switch (mode) {
  case SamplerAddressMode::repeat:
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  case SamplerAddressMode::mirrored_repeat:
    return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
  case SamplerAddressMode::clamp_to_edge:
    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  case SamplerAddressMode::clamp_to_border:
    return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  }
  return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

bool native_sampler_descriptions_valid(
    const StockKsPerPixelNativeSamplers &samplers, Diagnostic &diagnostic) {
  if (!samplers.ready()) {
    diagnostic = {"d3d12_stock_native_samplers_missing",
                  "The native ksPerPixel sampler owner is incomplete"};
    return false;
  }
  const Sampler *linear =
      samplers.sampler(StockKsPerPixelNativeSamplerSlot::linear);
  const Sampler *shadow =
      samplers.sampler(StockKsPerPixelNativeSamplerSlot::shadow);
  if (linear == nullptr || shadow == nullptr ||
      linear->backend() != Backend::D3D12 ||
      shadow->backend() != Backend::D3D12) {
    diagnostic = {"d3d12_stock_native_sampler_backend_mismatch",
                  "Native ksPerPixel samplers must belong to D3D12"};
    return false;
  }
  const SamplerDescription &l = linear->info().description;
  const SamplerDescription &s = shadow->info().description;
  const bool linear_ok =
      l.min_filter == SamplerFilter::anisotropic &&
      l.mag_filter == SamplerFilter::anisotropic &&
      l.mip_filter == SamplerFilter::anisotropic &&
      l.address_u == SamplerAddressMode::repeat &&
      l.address_v == SamplerAddressMode::repeat &&
      l.address_w == SamplerAddressMode::repeat &&
      l.compare == SamplerCompare::disabled && std::isfinite(l.mip_lod_bias) &&
      std::isfinite(l.max_anisotropy) && l.max_anisotropy >= 2.0F &&
      l.max_anisotropy <= 16.0F &&
      std::trunc(l.max_anisotropy) == l.max_anisotropy && l.min_lod == 0.0F &&
      l.max_lod == std::numeric_limits<float>::max();
  const bool shadow_ok = s.min_filter == SamplerFilter::linear &&
                         s.mag_filter == SamplerFilter::linear &&
                         s.mip_filter == SamplerFilter::nearest &&
                         s.address_u == SamplerAddressMode::clamp_to_edge &&
                         s.address_v == SamplerAddressMode::clamp_to_edge &&
                         s.address_w == SamplerAddressMode::clamp_to_edge &&
                         s.compare == SamplerCompare::less &&
                         s.mip_lod_bias == 0.0F && s.max_anisotropy == 1.0F &&
                         s.min_lod == 0.0F &&
                         s.max_lod == std::numeric_limits<float>::max();
  if (!linear_ok || !shadow_ok) {
    diagnostic = {"d3d12_stock_native_sampler_contract_mismatch",
                  "Native ksPerPixel sampler descriptions do not match "
                  "samLinear/samShadow"};
    return false;
  }
  return true;
}

bool create_srv(ID3D12Device *device, ID3D12Resource *resource,
                DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                Diagnostic &diagnostic) {
  if (device == nullptr || resource == nullptr)
    return false;
  const D3D12_RESOURCE_DESC resource_description = resource->GetDesc();
  if (resource_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      resource_description.Width == 0U || resource_description.Height == 0U) {
    diagnostic = {
        "d3d12_stock_native_texture_invalid",
        "Native ksPerPixel sampled resources must be non-empty 2D textures"};
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = resource_description.MipLevels;
  srv.Texture2D.MostDetailedMip = 0U;
  srv.Texture2D.ResourceMinLODClamp = 0.0F;
  device->CreateShaderResourceView(resource, &srv, destination);
  return true;
}

bool drain_after_submit_failure(ID3D12Device *device,
                                ID3D12CommandQueue *queue) {
  if (device == nullptr || queue == nullptr)
    return false;
  ComPtr<ID3D12Fence> drain_fence;
  if (FAILED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&drain_fence))))
    return false;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr || FAILED(queue->Signal(drain_fence.Get(), 1U))) {
    if (event != nullptr)
      CloseHandle(event);
    return false;
  }
  const bool waited =
      drain_fence->GetCompletedValue() >= 1U ||
      (SUCCEEDED(drain_fence->SetEventOnCompletion(1U, event)) &&
       WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0);
  CloseHandle(event);
  return waited;
}

bool submit_and_wait(const D3D12StockKsPerPixelNativeDrawContext &context,
                     ID3D12Fence *fence, HANDLE fence_event,
                     Diagnostic &diagnostic) {
  if (fence == nullptr || fence_event == nullptr) {
    diagnostic = {"d3d12_stock_native_fence_failed",
                  "Native draw synchronization is incomplete"};
    return false;
  }
  HRESULT result = S_OK;
  result = context.command_list->Close();
  if (FAILED(result)) {
    diagnostic =
        hresult_error("ID3D12GraphicsCommandList::Close(stock native)", result);
    return false;
  }
  ID3D12CommandList *lists[] = {context.command_list};
  context.queue->ExecuteCommandLists(1U, lists);
  result = context.queue->Signal(fence, 1U);
  if (FAILED(result)) {
    const bool drained =
        drain_after_submit_failure(context.device, context.queue);
    diagnostic =
        hresult_error("ID3D12CommandQueue::Signal(stock native)", result);
    diagnostic.code = drained ? "d3d12_stock_native_submit_failed"
                              : "d3d12_stock_native_submit_undrained";
    return false;
  }
  if (fence->GetCompletedValue() < 1U &&
      (FAILED(fence->SetEventOnCompletion(1U, fence_event)) ||
       WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0)) {
    const bool drained =
        drain_after_submit_failure(context.device, context.queue);
    diagnostic = {"d3d12_stock_native_fence_failed",
                  "Waiting for native draw failed"};
    if (!drained)
      diagnostic.code = "d3d12_stock_native_submit_undrained";
    return false;
  }
  return true;
}

} // namespace

bool record_d3d12_stock_ks_per_pixel_native_draw(
    const D3D12StockKsPerPixelNativeDrawContext &context,
    Diagnostic &diagnostic) {
  diagnostic = {};
  const auto invalid = [&](const char *code, const char *message) {
    diagnostic = {code, message};
    return false;
  };
  if (context.device == nullptr || context.queue == nullptr ||
      context.allocator == nullptr || context.command_list == nullptr ||
      context.target == nullptr || context.target_state == nullptr ||
      context.vertex_buffer == nullptr || context.index_buffer == nullptr ||
      context.request == nullptr || context.readback == nullptr ||
      context.shader_program == nullptr ||
      context.constant_buffers == nullptr || context.samplers == nullptr ||
      context.diffuse_texture == nullptr || context.target_rtv.ptr == 0U)
    return invalid("d3d12_stock_native_context_missing",
                   "Native ksPerPixel draw context is incomplete");
  const IndexedStaticMeshDrawRequest &request = *context.request;
  if (request.packet == nullptr || request.pipeline == nullptr)
    return invalid("d3d12_stock_native_request_missing",
                   "Native ksPerPixel draw requires packet and pipeline");
  const DrawPacket &packet = *request.packet;
  if (packet.primitive != DrawPrimitiveKind::static_mesh ||
      packet.index_count == 0U || packet.index_count % 3U != 0U ||
      packet.vertex_count == 0U)
    return invalid("d3d12_stock_native_geometry_invalid",
                   "Native ksPerPixel draw requires bounded static triangles");
  if (!context.shader_program->ready() ||
      context.shader_program->source().validation_status() !=
          StockKsPerPixelNativeProgramStatus::ready)
    return invalid("d3d12_stock_native_shader_invalid",
                   "Native ksPerPixel shader owner is not validated");
  if (!context.constant_buffers->ready() ||
      !native_sampler_descriptions_valid(*context.samplers, diagnostic))
    return false;
  for (ID3D12Resource *buffer : context.constant_buffer_resources)
    if (buffer == nullptr)
      return invalid("d3d12_stock_native_constants_missing",
                     "Native constant resource is missing");
  if (context.target_width == 0U || context.target_height == 0U ||
      context.target_width > std::numeric_limits<std::uint32_t>::max() / 4U ||
      static_cast<std::uint64_t>(context.target_width) * context.target_height *
              4U >
          max_texture_readback_bytes)
    return invalid("d3d12_stock_native_target_invalid",
                   "Native target dimensions exceed bounded RGBA8 readback");
  if (context.target_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
      context.target_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
      context.target_format != DXGI_FORMAT_B8G8R8A8_UNORM &&
      context.target_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    return invalid("d3d12_stock_native_target_format_unsupported",
                   "Native ksPerPixel readback requires RGBA8 or BGRA8");
  const D3D12_RESOURCE_DESC target_description = context.target->GetDesc();
  const StockKsPerPixelVariant shader_variant =
      context.shader_program->source().variant();
  const bool alpha_to_coverage =
      shader_variant == StockKsPerPixelVariant::alpha_to_coverage;
  if (request.pipeline->blend.alpha_to_coverage != alpha_to_coverage ||
      packet.flags.alpha_to_coverage != alpha_to_coverage)
    return invalid(
        "d3d12_stock_native_variant_state_mismatch",
        "Native ksPerPixel shader, packet, and pipeline A2C state must match");
  const UINT required_samples = alpha_to_coverage ? 4U : 1U;
  if (target_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      target_description.Format != context.target_format ||
      target_description.Width != context.target_width ||
      target_description.Height != context.target_height ||
      target_description.SampleDesc.Count != required_samples) {
    if (alpha_to_coverage)
      return invalid("d3d12_stock_native_a2c_target_samples_invalid",
                     "Native alpha-to-coverage requires a 4x color target");
    return invalid("d3d12_stock_native_target_shape_invalid",
                   "Native target must be a single-sample 2D image");
  }
  if (context.target_width >
          static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
      context.target_height >
          static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()))
    return invalid("d3d12_stock_native_target_scissor_limit",
                   "Native target dimensions exceed D3D12 LONG scissor limits");
  if (context.diffuse_state == nullptr)
    return invalid("d3d12_stock_native_diffuse_state_missing",
                   "Native diffuse resource has no tracked state");
  if (request.clear_depth &&
      (context.depth == nullptr || !request.pipeline->depth.test_enabled))
    return invalid("d3d12_stock_native_depth_clear_invalid",
                   "Native depth clear requires an enabled depth attachment");
  if (!request.clear_depth && context.depth != nullptr &&
      (context.depth_cleared == nullptr || !*context.depth_cleared) &&
      request.pipeline->depth.test_enabled)
    return invalid(
        "d3d12_stock_native_depth_load_before_clear",
        "Native depth load requires a previously cleared depth attachment");
  const auto aliases = [](ID3D12Resource *left, ID3D12Resource *right) {
    return left != nullptr && left == right;
  };
  if (aliases(context.target, context.depth) ||
      aliases(context.target, context.vertex_buffer) ||
      aliases(context.target, context.index_buffer) ||
      aliases(context.target, context.diffuse_texture))
    return invalid("d3d12_stock_native_resource_alias",
                   "Native target resources must not alias one another");
  if (aliases(context.vertex_buffer, context.index_buffer) ||
      aliases(context.vertex_buffer, context.diffuse_texture) ||
      aliases(context.index_buffer, context.diffuse_texture))
    return invalid("d3d12_stock_native_resource_alias",
                   "Native geometry and diffuse resources must not alias");
  for (ID3D12Resource *shadow : context.shadow_maps)
    if (aliases(context.target, shadow) || aliases(context.depth, shadow) ||
        aliases(context.vertex_buffer, shadow) ||
        aliases(context.index_buffer, shadow) ||
        aliases(context.diffuse_texture, shadow))
      return invalid("d3d12_stock_native_resource_alias",
                     "Native shadow resources must not alias draw resources");
  for (ID3D12Resource *constant : context.constant_buffer_resources)
    if (aliases(context.target, constant) || aliases(context.depth, constant) ||
        aliases(context.vertex_buffer, constant) ||
        aliases(context.index_buffer, constant) ||
        aliases(context.diffuse_texture, constant))
      return invalid("d3d12_stock_native_resource_alias",
                     "Native constant resources must not alias draw resources");
  for (std::size_t i = 0U; i < context.constant_buffer_resources.size(); ++i)
    for (std::size_t j = i + 1U; j < context.constant_buffer_resources.size();
         ++j)
      if (aliases(context.constant_buffer_resources[i],
                  context.constant_buffer_resources[j]))
        return invalid(
            "d3d12_stock_native_resource_alias",
            "Native constant-buffer slots require distinct resources");
  for (std::size_t i = 0U; i < context.shadow_maps.size(); ++i)
    for (std::size_t j = i + 1U; j < context.shadow_maps.size(); ++j)
      if (aliases(context.shadow_maps[i], context.shadow_maps[j]))
        return invalid("d3d12_stock_native_resource_alias",
                       "Native shadow slots require distinct resources");
  const std::array<ID3D12Resource *, 13U> all_resources = {
      context.target,
      context.depth,
      context.vertex_buffer,
      context.index_buffer,
      context.diffuse_texture,
      context.shadow_maps[0],
      context.shadow_maps[1],
      context.shadow_maps[2],
      context.constant_buffer_resources[0],
      context.constant_buffer_resources[1],
      context.constant_buffer_resources[2],
      context.constant_buffer_resources[3],
      context.constant_buffer_resources[4]};
  for (std::size_t i = 0U; i < all_resources.size(); ++i)
    for (std::size_t j = i + 1U; j < all_resources.size(); ++j)
      if (aliases(all_resources[i], all_resources[j]))
        return invalid("d3d12_stock_native_resource_alias",
                       "Native draw resources must not alias");
  if (request.pipeline->raster.fill == PipelineFillMode::wireframe)
    return invalid(
        "d3d12_stock_native_wireframe_unsupported",
        "The native indexed helper accepts triangle-fill draws only");
  const UINT64 stride = 44U;
  const UINT64 vertex_offset =
      static_cast<UINT64>(packet.vertex_offset) * stride;
  const UINT64 index_offset =
      static_cast<UINT64>(packet.index_offset) * sizeof(std::uint16_t);
  const UINT64 vertex_bytes = static_cast<UINT64>(packet.vertex_count) * stride;
  const UINT64 index_bytes =
      static_cast<UINT64>(packet.index_count) * sizeof(std::uint16_t);
  const D3D12_RESOURCE_DESC vertex_description =
      context.vertex_buffer->GetDesc();
  const D3D12_RESOURCE_DESC index_description = context.index_buffer->GetDesc();
  if (vertex_offset > vertex_description.Width ||
      vertex_bytes > vertex_description.Width - vertex_offset ||
      index_offset > index_description.Width ||
      index_bytes > index_description.Width - index_offset ||
      vertex_bytes > std::numeric_limits<UINT>::max() ||
      index_bytes > std::numeric_limits<UINT>::max())
    return invalid("d3d12_stock_native_geometry_range_invalid",
                   "Native indexed range exceeds its D3D12 buffers");
  if ((static_cast<UINT>(context.vertex_state) &
       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) == 0U ||
      (static_cast<UINT>(context.index_state) &
       D3D12_RESOURCE_STATE_INDEX_BUFFER) == 0U)
    return invalid("d3d12_stock_native_geometry_state_invalid",
                   "Native vertex/index resources are not in draw state");
  for (std::size_t i = 0U; i < context.shadow_maps.size(); ++i)
    if (context.shadow_maps[i] == nullptr ||
        context.shadow_states[i] == nullptr)
      return invalid(
          "d3d12_stock_native_shadow_missing",
          "Native ksPerPixel requires three shadow resources and states");
  if (context.depth != nullptr && context.depth_state == nullptr)
    return invalid("d3d12_stock_native_depth_state_missing",
                   "Native depth resource has no tracked state");
  if (context.depth != nullptr &&
      (context.depth_write_dsv.ptr == 0U || context.depth_read_dsv.ptr == 0U))
    return invalid("d3d12_stock_native_depth_view_missing",
                   "Native depth resource has no complete DSV pair");

  ComPtr<ID3D12DescriptorHeap> cbv_srv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
  heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_description.NumDescriptors = 11U;
  heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  HRESULT result = context.device->CreateDescriptorHeap(
      &heap_description, IID_PPV_ARGS(&cbv_srv_heap));
  if (FAILED(result)) {
    diagnostic =
        hresult_error("CreateDescriptorHeap(stock native CBV/SRV)", result);
    return false;
  }
  ComPtr<ID3D12DescriptorHeap> sampler_heap;
  heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  heap_description.NumDescriptors = 2U;
  result = context.device->CreateDescriptorHeap(&heap_description,
                                                IID_PPV_ARGS(&sampler_heap));
  if (FAILED(result)) {
    diagnostic =
        hresult_error("CreateDescriptorHeap(stock native sampler)", result);
    return false;
  }
  const UINT cbv_stride = context.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sampler_stride = context.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  const auto cbv_cpu = [&](UINT i) {
    auto h = cbv_srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * cbv_stride;
    return h;
  };
  const auto sampler_cpu = [&](UINT i) {
    auto h = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * sampler_stride;
    return h;
  };
  const auto cbv_gpu = [&](UINT i) {
    auto h = cbv_srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(i) * cbv_stride;
    return h;
  };
  const auto sampler_gpu = [&](UINT i) {
    auto h = sampler_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(i) * sampler_stride;
    return h;
  };
  for (UINT i = 0U; i < 4U; ++i) {
    const UINT source = i;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{
        context.constant_buffer_resources[source]->GetGPUVirtualAddress(),
        stock_ks_per_pixel_native_constant_buffer_view_bytes};
    context.device->CreateConstantBufferView(&cbv, cbv_cpu(i));
  }
  const std::array<UINT, 3U> pixel_constant_sources = {2U, 3U, 4U};
  for (UINT i = 0U; i < 3U; ++i) {
    const auto source = pixel_constant_sources[i];
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{
        context.constant_buffer_resources[source]->GetGPUVirtualAddress(),
        stock_ks_per_pixel_native_constant_buffer_view_bytes};
    context.device->CreateConstantBufferView(&cbv, cbv_cpu(4U + i));
  }
  if (!create_srv(context.device, context.diffuse_texture,
                  context.diffuse_format, cbv_cpu(7U), diagnostic))
    return false;
  for (UINT i = 0U; i < 3U; ++i)
    if (!create_srv(context.device, context.shadow_maps[i],
                    DXGI_FORMAT_R32_FLOAT, cbv_cpu(8U + i), diagnostic))
      return false;
  const SamplerDescription &linear =
      context.samplers->sampler(StockKsPerPixelNativeSamplerSlot::linear)
          ->info()
          .description;
  const SamplerDescription &shadow =
      context.samplers->sampler(StockKsPerPixelNativeSamplerSlot::shadow)
          ->info()
          .description;
  D3D12_SAMPLER_DESC linear_desc{};
  linear_desc.Filter = D3D12_FILTER_ANISOTROPIC;
  linear_desc.AddressU = address(linear.address_u);
  linear_desc.AddressV = address(linear.address_v);
  linear_desc.AddressW = address(linear.address_w);
  linear_desc.MipLODBias = linear.mip_lod_bias;
  linear_desc.MaxAnisotropy = static_cast<UINT>(linear.max_anisotropy);
  linear_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  linear_desc.MinLOD = linear.min_lod;
  linear_desc.MaxLOD = linear.max_lod;
  context.device->CreateSampler(&linear_desc, sampler_cpu(0U));
  D3D12_SAMPLER_DESC shadow_desc{};
  shadow_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  shadow_desc.AddressU = address(shadow.address_u);
  shadow_desc.AddressV = address(shadow.address_v);
  shadow_desc.AddressW = address(shadow.address_w);
  shadow_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
  shadow_desc.MinLOD = shadow.min_lod;
  shadow_desc.MaxLOD = shadow.max_lod;
  context.device->CreateSampler(&shadow_desc, sampler_cpu(1U));

  std::array<D3D12_DESCRIPTOR_RANGE,
             stock_ks_per_pixel_d3d12_root_ranges.size()>
      ranges{};
  std::array<D3D12_ROOT_PARAMETER, stock_ks_per_pixel_d3d12_root_ranges.size()>
      parameters{};
  for (std::size_t index = 0U;
       index < stock_ks_per_pixel_d3d12_root_ranges.size(); ++index) {
    const StockKsPerPixelD3D12DescriptorRange &source =
        stock_ks_per_pixel_d3d12_root_ranges[index];
    D3D12_DESCRIPTOR_RANGE &range = ranges[index];
    range.RangeType =
        source.register_class == StockShaderRegisterClass::constant_buffer
            ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
        : source.register_class == StockShaderRegisterClass::sampled_texture
            ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
            : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range.NumDescriptors = source.descriptor_count;
    range.BaseShaderRegister = source.base_register;
    range.RegisterSpace = source.register_space;
    range.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER &parameter = parameters[index];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = source.stages == StockShaderStageMask::vertex
                                     ? D3D12_SHADER_VISIBILITY_VERTEX
                                     : D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_SIGNATURE_DESC root{};
  root.NumParameters = static_cast<UINT>(parameters.size());
  root.pParameters = parameters.data();
  root.NumStaticSamplers = 0U;
  root.pStaticSamplers = nullptr;
  root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> root_blob, root_error;
  result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
                                       &root_blob, &root_error);
  if (FAILED(result)) {
    diagnostic = hresult_error("D3D12SerializeRootSignature(stock native)",
                               result, "indexed_stock_native_pipeline_failed");
    return false;
  }
  ComPtr<ID3D12RootSignature> root_signature;
  result = context.device->CreateRootSignature(
      0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
      IID_PPV_ARGS(&root_signature));
  if (FAILED(result)) {
    diagnostic = hresult_error("CreateRootSignature(stock native)", result,
                               "indexed_stock_native_pipeline_failed");
    return false;
  }

  const auto vertex_shader_bytes =
      context.shader_program->source().vertex_shader();
  const auto pixel_shader_bytes =
      context.shader_program->source().pixel_shader();
  const D3D12_INPUT_ELEMENT_DESC input[] = {
      {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
      {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
      {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 24U,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
      {"TANGENT", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 32U,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = root_signature.Get();
  pso.VS = {vertex_shader_bytes.data(), vertex_shader_bytes.size()};
  pso.PS = {pixel_shader_bytes.data(), pixel_shader_bytes.size()};
  pso.InputLayout = {input, 4U};
  // Wireframe is rejected above because the recovered stock input is a
  // triangle-list draw.  Keep the native PSO topology unambiguously paired
  // with the indexed command below.
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode =
      request.pipeline->raster.cull == PipelineCullMode::none
          ? D3D12_CULL_MODE_NONE
      : request.pipeline->raster.cull == PipelineCullMode::front
          ? D3D12_CULL_MODE_FRONT
          : D3D12_CULL_MODE_BACK;
  pso.RasterizerState.FrontCounterClockwise =
      request.pipeline->raster.front_face ==
      PipelineFrontFace::counter_clockwise;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.BlendState.AlphaToCoverageEnable =
      request.pipeline->blend.alpha_to_coverage ? TRUE : FALSE;
  pso.BlendState.RenderTarget[0].BlendEnable =
      request.pipeline->blend.enabled ? TRUE : FALSE;
  auto &blend = pso.BlendState.RenderTarget[0];
  blend.SrcBlend = blend_factor(request.pipeline->blend.source_color);
  blend.DestBlend = blend_factor(request.pipeline->blend.destination_color);
  blend.BlendOp = blend_op(request.pipeline->blend.color_operation);
  blend.SrcBlendAlpha = blend_factor(request.pipeline->blend.source_alpha);
  blend.DestBlendAlpha =
      blend_factor(request.pipeline->blend.destination_alpha);
  blend.BlendOpAlpha = blend_op(request.pipeline->blend.alpha_operation);
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  const bool use_depth =
      context.depth != nullptr && request.pipeline->depth.test_enabled;
  pso.DepthStencilState.DepthEnable = use_depth ? TRUE : FALSE;
  pso.DepthStencilState.DepthWriteMask =
      use_depth && request.pipeline->depth.write_enabled
          ? D3D12_DEPTH_WRITE_MASK_ALL
          : D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc =
      use_depth ? compare(request.pipeline->depth.compare)
                : D3D12_COMPARISON_FUNC_ALWAYS;
  pso.NumRenderTargets = 1U;
  pso.RTVFormats[0] = context.target_format;
  pso.DSVFormat = use_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
  pso.SampleMask = std::numeric_limits<UINT>::max();
  pso.SampleDesc.Count = required_samples;
  pso.SampleDesc.Quality = target_description.SampleDesc.Quality;
  ComPtr<ID3D12PipelineState> pipeline_state;
  result = context.device->CreateGraphicsPipelineState(
      &pso, IID_PPV_ARGS(&pipeline_state));
  if (FAILED(result)) {
    diagnostic = hresult_error("CreateGraphicsPipelineState(stock native)",
                               result, "indexed_stock_native_pipeline_failed");
    return false;
  }
  // Allocate the single-sample resolve target, synchronization, and copy
  // resources before resetting or recording the command list. This keeps
  // failures before submission from leaving a partially-owned GPU operation
  // behind.
  ComPtr<ID3D12Resource> resolve_resource;
  D3D12_RESOURCE_DESC readback_source_description = target_description;
  if (alpha_to_coverage) {
    readback_source_description.Alignment = 0U;
    readback_source_description.SampleDesc.Count = 1U;
    readback_source_description.SampleDesc.Quality = 0U;
    D3D12_HEAP_PROPERTIES resolve_heap{D3D12_HEAP_TYPE_DEFAULT};
    result = context.device->CreateCommittedResource(
        &resolve_heap, D3D12_HEAP_FLAG_NONE, &readback_source_description,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
        IID_PPV_ARGS(&resolve_resource));
    if (FAILED(result)) {
      diagnostic = hresult_error(
          "CreateCommittedResource(stock native A2C resolve)", result,
          "d3d12_stock_native_a2c_resolve_allocation_failed");
      return false;
    }
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0U;
  UINT64 row_size = 0U;
  UINT64 readback_size = 0U;
  context.device->GetCopyableFootprints(&readback_source_description, 0U, 1U,
                                        0U, &footprint, &rows, &row_size,
                                        &readback_size);
  if (rows != context.target_height ||
      row_size < static_cast<UINT64>(context.target_width) * 4U ||
      readback_size == 0U || readback_size > max_texture_readback_bytes ||
      readback_size > std::numeric_limits<SIZE_T>::max()) {
    return invalid(
        "d3d12_stock_native_readback_footprint_invalid",
        "Native readback footprint exceeds the bounded RGBA8 output");
  }
  D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC readback_desc{};
  readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  readback_desc.Width = readback_size;
  readback_desc.Height = 1U;
  readback_desc.DepthOrArraySize = 1U;
  readback_desc.MipLevels = 1U;
  readback_desc.SampleDesc.Count = 1U;
  readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback_resource;
  result = context.device->CreateCommittedResource(
      &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&readback_resource));
  if (FAILED(result)) {
    diagnostic =
        hresult_error("CreateCommittedResource(stock native readback)", result);
    return false;
  }
  ComPtr<ID3D12Fence> fence;
  result = context.device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence));
  if (FAILED(result)) {
    diagnostic = hresult_error("CreateFence(stock native)", result);
    return false;
  }
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event == nullptr)
    return invalid("d3d12_stock_native_fence_failed",
                   "CreateEventW failed for native draw");
  result = context.allocator->Reset();
  if (FAILED(result)) {
    CloseHandle(fence_event);
    diagnostic =
        hresult_error("ID3D12CommandAllocator::Reset(stock native)", result);
    return false;
  }
  result = context.command_list->Reset(context.allocator, pipeline_state.Get());
  if (FAILED(result)) {
    CloseHandle(fence_event);
    diagnostic =
        hresult_error("ID3D12GraphicsCommandList::Reset(stock native)", result);
    return false;
  }
  const D3D12_RESOURCE_STATES target_before = *context.target_state;
  const D3D12_RESOURCE_STATES depth_before = context.depth_state != nullptr
                                                 ? *context.depth_state
                                                 : D3D12_RESOURCE_STATE_COMMON;
  const D3D12_RESOURCE_STATES diffuse_before = *context.diffuse_state;
  const std::array<D3D12_RESOURCE_STATES, 3U> shadows_before = {
      *context.shadow_states[0], *context.shadow_states[1],
      *context.shadow_states[2]};
  transition(context.command_list, context.target, target_before,
             D3D12_RESOURCE_STATE_RENDER_TARGET);
  for (UINT i = 0U; i < 3U; ++i)
    transition(context.command_list, context.shadow_maps[i], shadows_before[i],
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(context.command_list, context.diffuse_texture, diffuse_before,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  const D3D12_RESOURCE_STATES depth_draw_state =
      request.pipeline->depth.write_enabled ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                            : D3D12_RESOURCE_STATE_DEPTH_READ;
  if (use_depth)
    transition(context.command_list, context.depth, depth_before,
               request.clear_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                   : depth_draw_state);
  if (!request.load_color) {
    const float clear[4] = {request.clear_color[0], request.clear_color[1],
                            request.clear_color[2], request.clear_color[3]};
    context.command_list->ClearRenderTargetView(context.target_rtv, clear, 0U,
                                                nullptr);
  }
  if (use_depth && request.clear_depth)
    context.command_list->ClearDepthStencilView(
        context.depth_write_dsv, D3D12_CLEAR_FLAG_DEPTH,
        request.depth_clear_value, 0U, 0U, nullptr);
  if (use_depth && request.clear_depth &&
      depth_draw_state != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    transition(context.command_list, context.depth,
               D3D12_RESOURCE_STATE_DEPTH_WRITE, depth_draw_state);
  const D3D12_CPU_DESCRIPTOR_HANDLE depth_draw_dsv =
      request.pipeline->depth.write_enabled ? context.depth_write_dsv
                                            : context.depth_read_dsv;
  context.command_list->OMSetRenderTargets(
      1U, &context.target_rtv, FALSE, use_depth ? &depth_draw_dsv : nullptr);
  D3D12_VIEWPORT viewport{0.0F,
                          0.0F,
                          static_cast<float>(context.target_width),
                          static_cast<float>(context.target_height),
                          0.0F,
                          1.0F};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(context.target_width),
                     static_cast<LONG>(context.target_height)};
  context.command_list->RSSetViewports(1U, &viewport);
  context.command_list->RSSetScissorRects(1U, &scissor);
  ID3D12DescriptorHeap *heaps[] = {cbv_srv_heap.Get(), sampler_heap.Get()};
  context.command_list->SetDescriptorHeaps(2U, heaps);
  context.command_list->SetGraphicsRootSignature(root_signature.Get());
  context.command_list->SetGraphicsRootDescriptorTable(0U, cbv_gpu(0U));
  context.command_list->SetGraphicsRootDescriptorTable(1U, cbv_gpu(4U));
  context.command_list->SetGraphicsRootDescriptorTable(2U, cbv_gpu(7U));
  context.command_list->SetGraphicsRootDescriptorTable(3U, cbv_gpu(8U));
  context.command_list->SetGraphicsRootDescriptorTable(4U, sampler_gpu(0U));
  D3D12_VERTEX_BUFFER_VIEW vb{context.vertex_buffer->GetGPUVirtualAddress() +
                                  vertex_offset,
                              static_cast<UINT>(vertex_bytes), 44U};
  D3D12_INDEX_BUFFER_VIEW ib{
      context.index_buffer->GetGPUVirtualAddress() + index_offset,
      static_cast<UINT>(index_bytes), DXGI_FORMAT_R16_UINT};
  context.command_list->IASetPrimitiveTopology(
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context.command_list->IASetVertexBuffers(0U, 1U, &vb);
  context.command_list->IASetIndexBuffer(&ib);
  context.command_list->DrawIndexedInstanced(packet.index_count, 1U, 0U, 0, 0U);
  ID3D12Resource *readback_source = context.target;
  if (alpha_to_coverage) {
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    context.command_list->ResolveSubresource(
        resolve_resource.Get(), 0U, context.target, 0U, context.target_format);
    transition(context.command_list, resolve_resource.Get(),
               D3D12_RESOURCE_STATE_RESOLVE_DEST,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    readback_source = resolve_resource.Get();
  } else {
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  D3D12_TEXTURE_COPY_LOCATION source{readback_source,
                                     D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  source.SubresourceIndex = 0U;
  D3D12_TEXTURE_COPY_LOCATION destination{
      readback_resource.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  destination.PlacedFootprint = footprint;
  context.command_list->CopyTextureRegion(&destination, 0U, 0U, 0U, &source,
                                          nullptr);
  if (alpha_to_coverage) {
    transition(context.command_list, resolve_resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RESOLVE_DEST);
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE, target_before);
  } else {
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_COPY_SOURCE, target_before);
  }
  for (UINT i = 0U; i < 3U; ++i)
    transition(context.command_list, context.shadow_maps[i],
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, shadows_before[i]);
  transition(context.command_list, context.diffuse_texture,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, diffuse_before);
  if (use_depth)
    transition(context.command_list, context.depth, depth_draw_state,
               depth_before);
  if (!submit_and_wait(context, fence.Get(), fence_event, diagnostic)) {
    CloseHandle(fence_event);
    return false;
  }
  CloseHandle(fence_event);
  void *mapped = nullptr;
  D3D12_RANGE range{0U, static_cast<SIZE_T>(readback_size)};
  result = readback_resource->Map(0U, &range, &mapped);
  if (FAILED(result)) {
    diagnostic = hresult_error("Map(stock native readback)", result);
    return false;
  }
  try {
    context.readback->resize(static_cast<std::size_t>(context.target_width) *
                             context.target_height * 4U);
    const auto *bytes = static_cast<const std::byte *>(mapped);
    for (UINT y = 0U; y < context.target_height; ++y)
      std::memcpy(context.readback->data() +
                      static_cast<std::size_t>(y) * context.target_width * 4U,
                  bytes + footprint.Offset +
                      static_cast<std::size_t>(y) *
                          footprint.Footprint.RowPitch,
                  static_cast<std::size_t>(context.target_width) * 4U);
  } catch (const std::bad_alloc &) {
    readback_resource->Unmap(0U, nullptr);
    diagnostic = {"d3d12_stock_native_readback_allocation_failed",
                  "Native readback allocation failed"};
    return false;
  }
  readback_resource->Unmap(0U, nullptr);
  if (context.target_format == DXGI_FORMAT_B8G8R8A8_UNORM ||
      context.target_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    for (std::size_t i = 0U; i < context.readback->size(); i += 4U)
      std::swap((*context.readback)[i], (*context.readback)[i + 2U]);
  *context.target_state = target_before;
  *context.diffuse_state = diffuse_before;
  if (context.depth_state != nullptr)
    *context.depth_state = depth_before;
  for (UINT i = 0U; i < 3U; ++i)
    *context.shadow_states[i] = shadows_before[i];
  if (context.depth_cleared != nullptr && request.clear_depth)
    *context.depth_cleared = true;
  return true;
}

} // namespace apex::render

#endif
