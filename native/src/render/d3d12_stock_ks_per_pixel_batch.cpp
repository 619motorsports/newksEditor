#include "d3d12_stock_ks_per_pixel_batch.hpp"

#include "apex/render/draw_packet.hpp"

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace apex::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t cbv_srv_descriptors_per_draw = 11U;
constexpr std::size_t sampler_descriptors_per_draw = 2U;

Diagnostic hresult_error(const char *operation, HRESULT result,
                         const char *code = "d3d12_stock_native_batch_failed") {
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

bool target_format_matches(PipelineRenderTargetFormat format,
                           DXGI_FORMAT dxgi) noexcept {
  switch (format) {
  case PipelineRenderTargetFormat::rgba8_unorm:
    return dxgi == DXGI_FORMAT_R8G8B8A8_UNORM;
  case PipelineRenderTargetFormat::rgba8_srgb:
    return dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case PipelineRenderTargetFormat::bgra8_unorm:
    return dxgi == DXGI_FORMAT_B8G8R8A8_UNORM;
  case PipelineRenderTargetFormat::bgra8_srgb:
    return dxgi == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  case PipelineRenderTargetFormat::rgba16_float:
    return dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT;
  default:
    return false;
  }
}

bool create_srv(ID3D12Device *device, ID3D12Resource *resource,
                DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                Diagnostic &diagnostic) {
  (void)diagnostic;
  const D3D12_RESOURCE_DESC description = resource->GetDesc();
  if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      description.Width == 0U || description.Height == 0U ||
      description.MipLevels == 0U || description.SampleDesc.Count != 1U)
    return false;
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = description.MipLevels;
  device->CreateShaderResourceView(resource, &view, destination);
  return true;
}

bool native_sampler_descriptions_valid(
    const StockKsPerPixelNativeSamplers &samplers) noexcept {
  if (!samplers.ready())
    return false;
  const Sampler *linear =
      samplers.sampler(StockKsPerPixelNativeSamplerSlot::linear);
  const Sampler *shadow =
      samplers.sampler(StockKsPerPixelNativeSamplerSlot::shadow);
  if (linear == nullptr || shadow == nullptr ||
      linear->backend() != Backend::D3D12 ||
      shadow->backend() != Backend::D3D12)
    return false;
  const SamplerDescription &l = linear->info().description;
  const SamplerDescription &s = shadow->info().description;
  return l.min_filter == SamplerFilter::anisotropic &&
         l.mag_filter == SamplerFilter::anisotropic &&
         l.mip_filter == SamplerFilter::anisotropic &&
         l.address_u == SamplerAddressMode::repeat &&
         l.address_v == SamplerAddressMode::repeat &&
         l.address_w == SamplerAddressMode::repeat &&
         l.compare == SamplerCompare::disabled &&
         std::isfinite(l.mip_lod_bias) && std::isfinite(l.max_anisotropy) &&
         l.max_anisotropy >= 2.0F && l.max_anisotropy <= 16.0F &&
         std::trunc(l.max_anisotropy) == l.max_anisotropy &&
         l.min_lod == 0.0F && l.max_lod == std::numeric_limits<float>::max() &&
         s.min_filter == SamplerFilter::linear &&
         s.mag_filter == SamplerFilter::linear &&
         s.mip_filter == SamplerFilter::nearest &&
         s.address_u == SamplerAddressMode::clamp_to_edge &&
         s.address_v == SamplerAddressMode::clamp_to_edge &&
         s.address_w == SamplerAddressMode::clamp_to_edge &&
         s.compare == SamplerCompare::less && s.mip_lod_bias == 0.0F &&
         s.max_anisotropy == 1.0F && s.min_lod == 0.0F &&
         s.max_lod == std::numeric_limits<float>::max();
}

bool drain_after_submit_failure(ID3D12Device *device,
                                ID3D12CommandQueue *queue) {
  if (device == nullptr || queue == nullptr)
    return false;
  ComPtr<ID3D12Fence> fence;
  if (FAILED(
          device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    return false;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr)
    return false;
  constexpr UINT64 value = 1U;
  const HRESULT signal = queue->Signal(fence.Get(), value);
  bool drained = false;
  if (SUCCEEDED(signal) && SUCCEEDED(fence->SetEventOnCompletion(value, event)))
    drained = WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0;
  CloseHandle(event);
  return drained;
}

bool submit_and_wait(const D3D12StockKsPerPixelNativeBatchContext &context,
                     ID3D12Fence *fence, HANDLE event, Diagnostic &diagnostic) {
  HRESULT result = context.command_list->Close();
  if (FAILED(result)) {
    diagnostic = hresult_error(
        "ID3D12GraphicsCommandList::Close(stock native batch)", result);
    return false;
  }
  ID3D12CommandList *lists[] = {context.command_list};
  context.queue->ExecuteCommandLists(1U, lists);
  constexpr UINT64 value = 1U;
  result = context.queue->Signal(fence, value);
  if (FAILED(result)) {
    const bool drained =
        drain_after_submit_failure(context.device, context.queue);
    diagnostic =
        hresult_error("ID3D12CommandQueue::Signal(stock native batch)", result,
                      drained ? "d3d12_stock_native_batch_submit_failed"
                              : "d3d12_stock_native_batch_submit_undrained");
    return false;
  }
  result = fence->SetEventOnCompletion(value, event);
  if (FAILED(result)) {
    const bool drained =
        drain_after_submit_failure(context.device, context.queue);
    diagnostic = hresult_error(
        "ID3D12Fence::SetEventOnCompletion(stock native batch)", result,
        drained ? "d3d12_stock_native_batch_submit_failed"
                : "d3d12_stock_native_batch_submit_undrained");
    return false;
  }
  if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
    const bool drained =
        drain_after_submit_failure(context.device, context.queue);
    diagnostic = {drained ? "d3d12_stock_native_batch_submit_failed"
                          : "d3d12_stock_native_batch_submit_undrained",
                  "Waiting for native batch completion failed"};
    return false;
  }
  return true;
}

enum class ResourceRole : std::uint8_t { geometry, diffuse, shadow, constant };

struct TrackedResource {
  ID3D12Resource *resource = nullptr;
  D3D12_RESOURCE_STATES *state = nullptr;
  D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES required = D3D12_RESOURCE_STATE_COMMON;
  ResourceRole role = ResourceRole::geometry;
};

} // namespace

bool record_d3d12_stock_ks_per_pixel_native_batch(
    const D3D12StockKsPerPixelNativeBatchContext &context,
    Diagnostic &diagnostic) {
  diagnostic = {};
  const auto invalid = [&](const char *code, const char *message) {
    diagnostic = {code, message};
    return false;
  };
  if (context.device == nullptr || context.queue == nullptr ||
      context.allocator == nullptr || context.command_list == nullptr ||
      context.target == nullptr || context.target_state == nullptr ||
      context.target_rtv.ptr == 0U || context.readback == nullptr)
    return invalid("d3d12_stock_native_batch_context_missing",
                   "Native batch context is incomplete");
  if (context.draws.empty())
    return invalid("d3d12_stock_native_batch_empty",
                   "Native batch requires at least one indexed draw");
  if (context.draws.size() > 4096U)
    return invalid("d3d12_stock_native_batch_too_many_draws",
                   "Native batch exceeds the bounded draw count");
  if (context.draws.size() >
      D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE / sampler_descriptors_per_draw)
    return invalid("d3d12_stock_native_batch_sampler_heap_limit",
                   "Native batch exceeds the shader-visible sampler heap");
  const std::uint64_t target_bytes_per_pixel =
      context.target_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U;
  if (context.target_width == 0U || context.target_height == 0U ||
      context.target_width >
          static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
      context.target_height >
          static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
      static_cast<std::uint64_t>(context.target_width) >
          max_texture_readback_bytes /
              static_cast<std::uint64_t>(context.target_height) /
              target_bytes_per_pixel)
    return invalid("d3d12_stock_native_batch_target_invalid",
                   "Native batch target dimensions exceed bounded limits");
  if (context.target_cleared == nullptr)
    return invalid("d3d12_stock_native_batch_target_state_missing",
                   "Native batch target has no clear-state tracker");
  if (std::any_of(context.clear_color.begin(), context.clear_color.end(),
                  [](float value) { return !std::isfinite(value); }))
    return invalid("d3d12_stock_native_batch_clear_color_invalid",
                   "Native batch clear color must be finite");
  if (!std::isfinite(context.depth_clear_value) ||
      context.depth_clear_value < 0.0F || context.depth_clear_value > 1.0F)
    return invalid("d3d12_stock_native_batch_clear_depth_invalid",
                   "Native batch clear depth must be finite and normalized");
  if (context.load_color && !*context.target_cleared)
    return invalid("d3d12_stock_native_batch_color_load_before_clear",
                   "Native batch color load requires a cleared target");
  if (context.target_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
      context.target_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
      context.target_format != DXGI_FORMAT_B8G8R8A8_UNORM &&
      context.target_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
      context.target_format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    return invalid("d3d12_stock_native_batch_target_format_unsupported",
                   "Native batch rendering requires an RGBA8, BGRA8, or "
                   "RGBA16F target");
  if (context.capture_rgba8 &&
      context.target_format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    return invalid("d3d12_stock_native_batch_readback_format_unsupported",
                   "Native RGBA8 readback does not accept an RGBA16F target");
  const D3D12_RESOURCE_DESC target_description = context.target->GetDesc();
  if (target_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      target_description.Format != context.target_format ||
      target_description.Width != context.target_width ||
      target_description.Height != context.target_height ||
      target_description.MipLevels != 1U ||
      target_description.DepthOrArraySize != 1U ||
      (target_description.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ==
          0U ||
      (target_description.SampleDesc.Count != 1U &&
       target_description.SampleDesc.Count != 4U))
    return invalid("d3d12_stock_native_batch_target_shape_invalid",
                   "Native batch requires a one-sample or four-sample 2D target");

  const bool use_depth = context.depth != nullptr;
  if (use_depth &&
      (context.depth == context.target || context.depth_state == nullptr ||
       context.depth_write_dsv.ptr == 0U || context.depth_read_dsv.ptr == 0U))
    return invalid("d3d12_stock_native_batch_depth_context_missing",
                   "Native batch depth context is incomplete");
  if (use_depth) {
    const D3D12_RESOURCE_DESC depth_description = context.depth->GetDesc();
    if (depth_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        depth_description.Format != DXGI_FORMAT_D32_FLOAT ||
        depth_description.Width != context.target_width ||
        depth_description.Height != context.target_height ||
        depth_description.SampleDesc.Count !=
            target_description.SampleDesc.Count)
      return invalid("d3d12_stock_native_batch_depth_shape_invalid",
                     "Native batch depth must match the target dimensions and samples");
  }
  bool any_depth = false;
  bool has_alpha_to_coverage = false;
  for (const auto &draw : context.draws) {
    if (draw.packet == nullptr || draw.pipeline == nullptr ||
        draw.vertex_buffer == nullptr || draw.index_buffer == nullptr ||
        draw.shader_program == nullptr || draw.constant_buffers == nullptr ||
        draw.samplers == nullptr || draw.diffuse_texture == nullptr ||
        draw.diffuse_state == nullptr)
      return invalid("d3d12_stock_native_batch_draw_context_missing",
                     "Native batch draw context is incomplete");
    const DrawPacket &packet = *draw.packet;
    const PipelineProgram &pipeline = *draw.pipeline;
    const StockKsPerPixelVariant shader_variant =
        draw.shader_program->source().variant();
    const bool alpha_to_coverage =
        shader_variant == StockKsPerPixelVariant::alpha_to_coverage;
    has_alpha_to_coverage = has_alpha_to_coverage || alpha_to_coverage;
    if (packet.primitive != DrawPrimitiveKind::static_mesh ||
        draw.index_type != StaticMeshIndexType::uint16 ||
        packet.vertex_count == 0U || packet.index_count == 0U ||
        packet.index_count % 3U != 0U || packet.vertex_stride_floats != 11U)
      return invalid("d3d12_stock_native_batch_geometry_invalid",
                     "Native batch requires bounded static triangle draws");
    if (packet.flags.blend_enabled ||
        packet.flags.alpha_to_coverage != alpha_to_coverage ||
        packet.flags.wireframe || packet.flags.selected || packet.shadow_only ||
        pipeline.blend.enabled ||
        pipeline.blend.alpha_to_coverage != alpha_to_coverage ||
        pipeline.raster.fill == PipelineFillMode::wireframe)
      return invalid("d3d12_stock_native_batch_pipeline_unsupported",
                     "Native batch accepts only matching opaque solid base or alpha-to-coverage draws");
    if (packet.flags.depth_test != pipeline.depth.test_enabled ||
        packet.flags.depth_write != pipeline.depth.write_enabled ||
        (packet.flags.depth_write && !packet.flags.depth_test))
      return invalid("d3d12_stock_native_batch_depth_state_mismatch",
                     "Native batch packet and pipeline depth state differ");
    any_depth = any_depth || pipeline.depth.test_enabled;
    if (pipeline.targets.colors.size() != 1U ||
        !target_format_matches(pipeline.targets.colors[0].format,
                               context.target_format) ||
        pipeline.targets.colors[0].samples !=
            target_description.SampleDesc.Count)
      return invalid("d3d12_stock_native_batch_pipeline_target_invalid",
                     "Native batch pipelines must match the target samples");
    const auto &layout = pipeline.vertex_layout;
    const auto matches_layout =
        [&](std::size_t index, PipelineVertexSemantic semantic,
            PipelineVertexAttributeFormat format, std::uint32_t location,
            std::uint32_t offset) {
          if (index >= layout.attributes.size())
            return false;
          const auto &attribute = layout.attributes[index];
          return attribute.semantic == semantic && attribute.format == format &&
                 attribute.location == location && attribute.offset == offset;
        };
    if (layout.stride != stock_ks_per_pixel_vertex_stride_bytes ||
        layout.attributes.size() != 4U ||
        pipeline.transform_contract != PipelineTransformContract::none ||
        !pipeline.resources.empty() ||
        !matches_layout(0U, PipelineVertexSemantic::position,
                        PipelineVertexAttributeFormat::float32x3, 0U, 0U) ||
        !matches_layout(1U, PipelineVertexSemantic::normal,
                        PipelineVertexAttributeFormat::float32x3, 1U, 12U) ||
        !matches_layout(2U, PipelineVertexSemantic::texcoord0,
                        PipelineVertexAttributeFormat::float32x2, 2U, 24U) ||
        !matches_layout(3U, PipelineVertexSemantic::tangent,
                        PipelineVertexAttributeFormat::float32x3, 3U, 32U))
      return invalid("d3d12_stock_native_batch_vertex_layout_invalid",
                     "Native batch requires the exact 44-byte stock layout");
    if (!draw.shader_program->ready() ||
        draw.shader_program->source().validation_status() !=
            StockKsPerPixelNativeProgramStatus::ready ||
        (shader_variant != StockKsPerPixelVariant::base &&
         !alpha_to_coverage) ||
        draw.shader_program->vertex_shader().backend() != Backend::D3D12 ||
        draw.shader_program->pixel_shader().backend() != Backend::D3D12 ||
        draw.shader_program->vertex_shader().info().stage !=
            ShaderStage::vertex ||
        draw.shader_program->pixel_shader().info().stage !=
            ShaderStage::fragment ||
        draw.shader_program->vertex_shader().info().format !=
            ShaderBytecodeFormat::dxbc ||
        draw.shader_program->pixel_shader().info().format !=
            ShaderBytecodeFormat::dxbc)
      return invalid("d3d12_stock_native_batch_shader_invalid",
                     "Native batch requires validated base or alpha-to-coverage D3D12 shaders");
    if (!draw.constant_buffers->ready() ||
        !native_sampler_descriptions_valid(*draw.samplers))
      return invalid("d3d12_stock_native_batch_bindings_invalid",
                     "Native batch requires ready constants and samplers");
    for (ID3D12Resource *constant : draw.constant_buffer_resources)
      if (constant == nullptr)
        return invalid("d3d12_stock_native_batch_constants_missing",
                       "Native batch constant resource is missing");
    for (std::size_t i = 0U; i < draw.shadow_maps.size(); ++i)
      if (draw.shadow_maps[i] == nullptr || draw.shadow_states[i] == nullptr)
        return invalid("d3d12_stock_native_batch_shadows_missing",
                       "Native batch requires three shadow maps and states");
    if (pipeline.depth.test_enabled && !use_depth)
      return invalid("d3d12_stock_native_batch_depth_missing",
                     "A depth-tested native draw requires a depth attachment");
    const D3D12_RESOURCE_DESC vertex_description =
        draw.vertex_buffer->GetDesc();
    const D3D12_RESOURCE_DESC index_description = draw.index_buffer->GetDesc();
    if (vertex_description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        index_description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
      return invalid("d3d12_stock_native_batch_geometry_resource_invalid",
                     "Native batch geometry must use buffer resources");
    const UINT64 vertex_offset =
        static_cast<UINT64>(packet.vertex_offset) * 44U;
    const UINT64 index_offset =
        static_cast<UINT64>(packet.index_offset) * sizeof(std::uint16_t);
    const UINT64 vertex_bytes = static_cast<UINT64>(packet.vertex_count) * 44U;
    const UINT64 index_bytes =
        static_cast<UINT64>(packet.index_count) * sizeof(std::uint16_t);
    if (vertex_offset > vertex_description.Width ||
        vertex_bytes > vertex_description.Width - vertex_offset ||
        index_offset > index_description.Width ||
        index_bytes > index_description.Width - index_offset ||
        vertex_bytes > std::numeric_limits<UINT>::max() ||
        index_bytes > std::numeric_limits<UINT>::max())
      return invalid("d3d12_stock_native_batch_geometry_range_invalid",
                     "Native batch geometry range exceeds its buffers");
    if ((static_cast<UINT>(draw.vertex_state) &
         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) == 0U ||
        (static_cast<UINT>(draw.index_state) &
         D3D12_RESOURCE_STATE_INDEX_BUFFER) == 0U)
      return invalid("d3d12_stock_native_batch_geometry_state_invalid",
                     "Native batch geometry is not in a valid tracked state");
  }
  const UINT target_samples = target_description.SampleDesc.Count;
  const bool requires_resolve = target_samples == 4U;
  if ((has_alpha_to_coverage && target_samples != 4U) ||
      (!has_alpha_to_coverage && target_samples != 1U &&
       target_samples != 4U))
    return invalid("d3d12_stock_native_batch_target_samples_invalid",
                   has_alpha_to_coverage
                       ? "Native ksPerPixelAT batches require a four-sample target"
                       : "Native base ksPerPixel batches require a one-sample or four-sample target");
  if (requires_resolve) {
    if (context.resolve_target == nullptr ||
        context.resolve_target_state == nullptr ||
        context.resolve_target_cleared == nullptr)
      return invalid("d3d12_stock_native_batch_resolve_context_missing",
                     "Four-sample native ksPerPixel batches require a retained resolve target");
    if (context.resolve_target == context.target ||
        context.resolve_target == context.depth)
      return invalid("d3d12_stock_native_batch_resolve_alias",
                     "Native batch resolve target must not alias color or depth");
    const D3D12_RESOURCE_DESC resolve_description =
        context.resolve_target->GetDesc();
    if (resolve_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resolve_description.Format != context.target_format ||
        resolve_description.Width != context.target_width ||
        resolve_description.Height != context.target_height ||
        resolve_description.MipLevels != 1U ||
        resolve_description.DepthOrArraySize != 1U ||
        resolve_description.SampleDesc.Count != 1U ||
        (resolve_description.Flags &
         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0U)
      return invalid("d3d12_stock_native_batch_resolve_shape_invalid",
                     "Native batch resolve target must be matching single-sample 2D storage");
  } else if (context.resolve_target != nullptr ||
             context.resolve_target_state != nullptr ||
             context.resolve_target_cleared != nullptr) {
    return invalid("d3d12_stock_native_batch_resolve_unexpected",
                   "Native base ksPerPixel batches do not use a resolve target");
  }
  if (context.clear_depth &&
      (context.depth == nullptr || context.depth_cleared == nullptr))
    return invalid("d3d12_stock_native_batch_depth_clear_invalid",
                   "Native batch depth clear requires a depth state tracker");
  if (any_depth && !context.clear_depth &&
      (context.depth_cleared == nullptr || !*context.depth_cleared))
    return invalid("d3d12_stock_native_batch_depth_load_before_clear",
                   "Native batch depth load requires a cleared attachment");

  std::vector<TrackedResource> tracked;
  struct ResourceRoleEntry {
    ID3D12Resource *resource = nullptr;
    ResourceRole role = ResourceRole::geometry;
  };
  std::vector<ResourceRoleEntry> resource_roles;
  try {
    tracked.reserve(context.draws.size() * 8U);
    resource_roles.reserve(context.draws.size() * 13U);
  } catch (const std::bad_alloc &) {
    return invalid("d3d12_stock_native_batch_allocation_failed",
                   "Native batch resource tracking allocation failed");
  }
  const auto track = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES *state,
                         D3D12_RESOURCE_STATES required, ResourceRole role) {
    if (resource == nullptr || state == nullptr)
      return false;
    for (const TrackedResource &entry : tracked) {
      if (entry.resource != resource)
        continue;
      if (entry.role != role || entry.state != state ||
          entry.required != required)
        return false;
      return true;
    }
    tracked.push_back({resource, state, *state, required, role});
    return true;
  };
  const auto remember_role = [&](ID3D12Resource *resource, ResourceRole role) {
    if (resource == nullptr)
      return false;
    for (const ResourceRoleEntry &entry : resource_roles)
      if (entry.resource == resource)
        return entry.role == role;
    resource_roles.push_back({resource, role});
    return true;
  };
  for (const auto &draw : context.draws) {
    if (draw.vertex_buffer == context.target ||
        draw.index_buffer == context.target ||
        draw.diffuse_texture == context.target ||
        draw.vertex_buffer == context.resolve_target ||
        draw.index_buffer == context.resolve_target ||
        draw.diffuse_texture == context.resolve_target ||
        draw.vertex_buffer == context.depth ||
        draw.index_buffer == context.depth ||
        draw.diffuse_texture == context.depth)
      return invalid("d3d12_stock_native_batch_resource_alias",
                     "Native batch resources must not alias the target");
    if (!remember_role(draw.vertex_buffer, ResourceRole::geometry) ||
        !remember_role(draw.index_buffer, ResourceRole::geometry) ||
        !remember_role(draw.diffuse_texture, ResourceRole::diffuse) ||
        !track(draw.diffuse_texture, draw.diffuse_state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               ResourceRole::diffuse))
      return invalid("d3d12_stock_native_batch_resource_alias",
                     "Native batch resources have incompatible aliases");
    const D3D12_RESOURCE_DESC diffuse_description =
        draw.diffuse_texture->GetDesc();
    if (diffuse_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        diffuse_description.Format != draw.diffuse_format ||
        diffuse_description.Width == 0U || diffuse_description.Height == 0U ||
        diffuse_description.MipLevels == 0U ||
        diffuse_description.SampleDesc.Count != 1U)
      return invalid("d3d12_stock_native_batch_diffuse_invalid",
                     "Native batch diffuse must be a single-sample 2D texture");
    for (std::size_t i = 0U; i < draw.shadow_maps.size(); ++i) {
      if (draw.shadow_maps[i] == context.target ||
          draw.shadow_maps[i] == context.resolve_target ||
          draw.shadow_maps[i] == context.depth ||
          draw.shadow_maps[i] == draw.vertex_buffer ||
          draw.shadow_maps[i] == draw.index_buffer ||
          draw.shadow_maps[i] == draw.diffuse_texture)
        return invalid(
            "d3d12_stock_native_batch_resource_alias",
            "Native batch shadow maps must not alias draw resources");
      for (std::size_t j = i + 1U; j < draw.shadow_maps.size(); ++j)
        if (draw.shadow_maps[i] == draw.shadow_maps[j])
          return invalid("d3d12_stock_native_batch_shadow_alias",
                         "Native batch shadow slots require distinct maps");
      if (!remember_role(draw.shadow_maps[i], ResourceRole::shadow))
        return invalid("d3d12_stock_native_batch_resource_alias",
                       "Native batch resources have incompatible aliases");
      if (!track(draw.shadow_maps[i], draw.shadow_states[i],
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                 ResourceRole::shadow))
        return invalid("d3d12_stock_native_batch_resource_alias",
                       "Native batch shadow state tracking is incompatible");
    }
    for (ID3D12Resource *constant : draw.constant_buffer_resources) {
      const D3D12_RESOURCE_DESC constant_description = constant->GetDesc();
      if (constant_description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
          constant_description.Width < 256U ||
          (constant->GetGPUVirtualAddress() & 255U) != 0U)
        return invalid(
            "d3d12_stock_native_batch_constant_resource_invalid",
            "Native batch constants require aligned buffer resources");
      if (!remember_role(constant, ResourceRole::constant))
        return invalid("d3d12_stock_native_batch_resource_alias",
                       "Native batch resources have incompatible aliases");
      if (constant == context.target || constant == context.depth ||
          constant == context.resolve_target ||
          constant == draw.vertex_buffer || constant == draw.index_buffer ||
          constant == draw.diffuse_texture)
        return invalid("d3d12_stock_native_batch_resource_alias",
                       "Native batch constants must not alias draw resources");
      for (ID3D12Resource *shadow : draw.shadow_maps)
        if (constant == shadow)
          return invalid("d3d12_stock_native_batch_resource_alias",
                         "Native batch constants must not alias shadow maps");
    }
    for (std::size_t i = 0U; i < draw.constant_buffer_resources.size(); ++i)
      for (std::size_t j = i + 1U; j < draw.constant_buffer_resources.size();
           ++j)
        if (draw.constant_buffer_resources[i] ==
            draw.constant_buffer_resources[j])
          return invalid(
              "d3d12_stock_native_batch_constant_alias",
              "Native batch constant slots require distinct resources");
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
  if (context.draws.size() > std::numeric_limits<std::size_t>::max() /
                                 cbv_srv_descriptors_per_draw ||
      context.draws.size() * cbv_srv_descriptors_per_draw >
          std::numeric_limits<UINT>::max() ||
      context.draws.size() > std::numeric_limits<std::size_t>::max() /
                                 sampler_descriptors_per_draw ||
      context.draws.size() * sampler_descriptors_per_draw >
          std::numeric_limits<UINT>::max())
    return invalid("d3d12_stock_native_batch_descriptor_count_invalid",
                   "Native batch descriptor count exceeds D3D12 limits");
  ComPtr<ID3D12DescriptorHeap> cbv_srv_heap;
  heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_description.NumDescriptors =
      static_cast<UINT>(context.draws.size() * cbv_srv_descriptors_per_draw);
  heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  HRESULT result = context.device->CreateDescriptorHeap(
      &heap_description, IID_PPV_ARGS(&cbv_srv_heap));
  if (FAILED(result)) {
    diagnostic = hresult_error(
        "CreateDescriptorHeap(stock native batch CBV/SRV)", result);
    return false;
  }
  ComPtr<ID3D12DescriptorHeap> sampler_heap;
  heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  heap_description.NumDescriptors =
      static_cast<UINT>(context.draws.size() * sampler_descriptors_per_draw);
  result = context.device->CreateDescriptorHeap(&heap_description,
                                                IID_PPV_ARGS(&sampler_heap));
  if (FAILED(result)) {
    diagnostic = hresult_error(
        "CreateDescriptorHeap(stock native batch sampler)", result);
    return false;
  }
  const UINT cbv_stride = context.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sampler_stride = context.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  const auto cpu_handle = [](D3D12_CPU_DESCRIPTOR_HANDLE start, UINT stride,
                             std::size_t index) {
    start.ptr += static_cast<SIZE_T>(index) * stride;
    return start;
  };
  const auto gpu_handle = [](D3D12_GPU_DESCRIPTOR_HANDLE start, UINT stride,
                             std::size_t index) {
    start.ptr += static_cast<UINT64>(index) * stride;
    return start;
  };
  const D3D12_CPU_DESCRIPTOR_HANDLE cbv_start =
      cbv_srv_heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_GPU_DESCRIPTOR_HANDLE cbv_gpu_start =
      cbv_srv_heap->GetGPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE sampler_start =
      sampler_heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_start =
      sampler_heap->GetGPUDescriptorHandleForHeapStart();
  for (std::size_t draw_index = 0U; draw_index < context.draws.size();
       ++draw_index) {
    const auto &draw = context.draws[draw_index];
    const std::size_t base = draw_index * cbv_srv_descriptors_per_draw;
    for (UINT i = 0U; i < 4U; ++i) {
      D3D12_CONSTANT_BUFFER_VIEW_DESC view{
          draw.constant_buffer_resources[i]->GetGPUVirtualAddress(), 256U};
      context.device->CreateConstantBufferView(
          &view, cpu_handle(cbv_start, cbv_stride, base + i));
    }
    const std::array<UINT, 3U> pixel_sources = {2U, 3U, 4U};
    for (UINT i = 0U; i < 3U; ++i) {
      D3D12_CONSTANT_BUFFER_VIEW_DESC view{
          draw.constant_buffer_resources[pixel_sources[i]]
              ->GetGPUVirtualAddress(),
          256U};
      context.device->CreateConstantBufferView(
          &view, cpu_handle(cbv_start, cbv_stride, base + 4U + i));
    }
    if (!create_srv(context.device, draw.diffuse_texture, draw.diffuse_format,
                    cpu_handle(cbv_start, cbv_stride, base + 7U), diagnostic))
      return invalid("d3d12_stock_native_batch_diffuse_view_invalid",
                     "Native batch diffuse SRV creation requires a 2D texture");
    for (UINT i = 0U; i < 3U; ++i)
      if (!create_srv(
              context.device, draw.shadow_maps[i], DXGI_FORMAT_R32_FLOAT,
              cpu_handle(cbv_start, cbv_stride, base + 8U + i), diagnostic))
        return invalid(
            "d3d12_stock_native_batch_shadow_view_invalid",
            "Native batch shadow SRV creation requires a 2D texture");
    const auto &linear =
        draw.samplers->sampler(StockKsPerPixelNativeSamplerSlot::linear)
            ->info()
            .description;
    const auto &shadow =
        draw.samplers->sampler(StockKsPerPixelNativeSamplerSlot::shadow)
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
    context.device->CreateSampler(
        &linear_desc, cpu_handle(sampler_start, sampler_stride,
                                 draw_index * sampler_descriptors_per_draw));
    D3D12_SAMPLER_DESC shadow_desc{};
    shadow_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadow_desc.AddressU = address(shadow.address_u);
    shadow_desc.AddressV = address(shadow.address_v);
    shadow_desc.AddressW = address(shadow.address_w);
    shadow_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
    shadow_desc.MinLOD = shadow.min_lod;
    shadow_desc.MaxLOD = shadow.max_lod;
    context.device->CreateSampler(
        &shadow_desc,
        cpu_handle(sampler_start, sampler_stride,
                   draw_index * sampler_descriptors_per_draw + 1U));
  }

  std::array<D3D12_DESCRIPTOR_RANGE,
             stock_ks_per_pixel_d3d12_root_ranges.size()>
      ranges{};
  std::array<D3D12_ROOT_PARAMETER, stock_ks_per_pixel_d3d12_root_ranges.size()>
      parameters{};
  for (std::size_t index = 0U;
       index < stock_ks_per_pixel_d3d12_root_ranges.size(); ++index) {
    const auto &source = stock_ks_per_pixel_d3d12_root_ranges[index];
    auto &range = ranges[index];
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
    auto &parameter = parameters[index];
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
  root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> root_blob, root_error;
  result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
                                       &root_blob, &root_error);
  if (FAILED(result)) {
    diagnostic =
        hresult_error("D3D12SerializeRootSignature(stock native batch)", result,
                      "d3d12_stock_native_batch_pipeline_failed");
    return false;
  }
  ComPtr<ID3D12RootSignature> root_signature;
  result = context.device->CreateRootSignature(
      0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
      IID_PPV_ARGS(&root_signature));
  if (FAILED(result)) {
    diagnostic =
        hresult_error("CreateRootSignature(stock native batch)", result,
                      "d3d12_stock_native_batch_pipeline_failed");
    return false;
  }

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
  std::vector<ComPtr<ID3D12PipelineState>> pipeline_states;
  try {
    pipeline_states.resize(context.draws.size());
  } catch (const std::bad_alloc &) {
    return invalid("d3d12_stock_native_batch_allocation_failed",
                   "Native batch pipeline allocation failed");
  }
  for (std::size_t index = 0U; index < context.draws.size(); ++index) {
    const auto &draw = context.draws[index];
    const auto vs = draw.shader_program->source().vertex_shader();
    const auto ps = draw.shader_program->source().pixel_shader();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.InputLayout = {input, 4U};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode =
        draw.pipeline->raster.cull == PipelineCullMode::none
            ? D3D12_CULL_MODE_NONE
        : draw.pipeline->raster.cull == PipelineCullMode::front
            ? D3D12_CULL_MODE_FRONT
            : D3D12_CULL_MODE_BACK;
    pso.RasterizerState.FrontCounterClockwise =
        draw.pipeline->raster.front_face ==
        PipelineFrontFace::counter_clockwise;
    pso.RasterizerState.DepthClipEnable = TRUE;
    auto &blend = pso.BlendState.RenderTarget[0];
    pso.BlendState.AlphaToCoverageEnable =
        draw.pipeline->blend.alpha_to_coverage ? TRUE : FALSE;
    blend.SrcBlend = blend_factor(draw.pipeline->blend.source_color);
    blend.DestBlend = blend_factor(draw.pipeline->blend.destination_color);
    blend.BlendOp = blend_op(draw.pipeline->blend.color_operation);
    blend.SrcBlendAlpha = blend_factor(draw.pipeline->blend.source_alpha);
    blend.DestBlendAlpha = blend_factor(draw.pipeline->blend.destination_alpha);
    blend.BlendOpAlpha = blend_op(draw.pipeline->blend.alpha_operation);
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable =
        draw.pipeline->depth.test_enabled ? TRUE : FALSE;
    pso.DepthStencilState.DepthWriteMask = draw.pipeline->depth.write_enabled
                                               ? D3D12_DEPTH_WRITE_MASK_ALL
                                               : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = compare(draw.pipeline->depth.compare);
    pso.NumRenderTargets = 1U;
    pso.RTVFormats[0] = context.target_format;
    pso.DSVFormat = use_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    pso.SampleMask = std::numeric_limits<UINT>::max();
    pso.SampleDesc.Count = target_samples;
    pso.SampleDesc.Quality = target_description.SampleDesc.Quality;
    result = context.device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&pipeline_states[index]));
    if (FAILED(result)) {
      diagnostic =
          hresult_error("CreateGraphicsPipelineState(stock native batch)",
                        result, "d3d12_stock_native_batch_pipeline_failed");
      return false;
    }
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0U;
  UINT64 row_size = 0U;
  UINT64 readback_size = 0U;
  ComPtr<ID3D12Resource> readback_resource;
  if (context.capture_rgba8) {
    const D3D12_RESOURCE_DESC readback_source_description =
        requires_resolve ? context.resolve_target->GetDesc()
                         : target_description;
    context.device->GetCopyableFootprints(
        &readback_source_description, 0U, 1U, 0U, &footprint, &rows,
        &row_size, &readback_size);
    if (rows != context.target_height ||
        row_size < static_cast<UINT64>(context.target_width) * 4U ||
        footprint.Footprint.RowPitch < row_size || readback_size == 0U ||
        readback_size > max_texture_readback_bytes ||
        readback_size > std::numeric_limits<SIZE_T>::max() ||
        footprint.Offset > readback_size ||
        (context.target_height > 0U &&
         (static_cast<UINT64>(context.target_height - 1U) >
              (readback_size - footprint.Offset) /
                  footprint.Footprint.RowPitch ||
          static_cast<UINT64>(context.target_width) * 4U >
              readback_size - footprint.Offset -
                  static_cast<UINT64>(context.target_height - 1U) *
                      footprint.Footprint.RowPitch)))
      return invalid("d3d12_stock_native_batch_readback_invalid",
                     "Native batch readback footprint exceeds bounded limits");
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_description{};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = readback_size;
    readback_description.Height = 1U;
    readback_description.DepthOrArraySize = 1U;
    readback_description.MipLevels = 1U;
    readback_description.SampleDesc.Count = 1U;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result = context.device->CreateCommittedResource(
        &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&readback_resource));
    if (FAILED(result)) {
      diagnostic = hresult_error(
          "CreateCommittedResource(stock native batch readback)", result);
      return false;
    }
  }
  ComPtr<ID3D12Fence> fence;
  result = context.device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence));
  if (FAILED(result)) {
    diagnostic = hresult_error("CreateFence(stock native batch)", result);
    return false;
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr)
    return invalid("d3d12_stock_native_batch_fence_failed",
                   "CreateEventW failed for native batch");
  result = context.allocator->Reset();
  if (FAILED(result)) {
    CloseHandle(event);
    diagnostic = hresult_error(
        "ID3D12CommandAllocator::Reset(stock native batch)", result);
    return false;
  }
  result =
      context.command_list->Reset(context.allocator, pipeline_states[0].Get());
  if (FAILED(result)) {
    // Restore the closed-list entry contract after a failed reset.
    context.command_list->Close();
    CloseHandle(event);
    diagnostic = hresult_error(
        "ID3D12GraphicsCommandList::Reset(stock native batch)", result);
    return false;
  }

  const D3D12_RESOURCE_STATES target_before = *context.target_state;
  const D3D12_RESOURCE_STATES depth_before =
      use_depth ? *context.depth_state : D3D12_RESOURCE_STATE_COMMON;
  transition(context.command_list, context.target, target_before,
             D3D12_RESOURCE_STATE_RENDER_TARGET);
  for (TrackedResource &entry : tracked)
    transition(context.command_list, entry.resource, entry.before,
               entry.required);
  if (!context.load_color)
    context.command_list->ClearRenderTargetView(
        context.target_rtv, context.clear_color.data(), 0U, nullptr);
  D3D12_RESOURCE_STATES depth_current = depth_before;
  if (use_depth && context.clear_depth) {
    transition(context.command_list, context.depth, depth_current,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    depth_current = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    context.command_list->ClearDepthStencilView(
        context.depth_write_dsv, D3D12_CLEAR_FLAG_DEPTH,
        context.depth_clear_value, 0U, 0U, nullptr);
  }
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
  for (std::size_t index = 0U; index < context.draws.size(); ++index) {
    const auto &draw = context.draws[index];
    const auto &packet = *draw.packet;
    context.command_list->SetPipelineState(pipeline_states[index].Get());
    if (draw.pipeline->depth.test_enabled) {
      const D3D12_RESOURCE_STATES desired =
          draw.pipeline->depth.write_enabled ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                             : D3D12_RESOURCE_STATE_DEPTH_READ;
      transition(context.command_list, context.depth, depth_current, desired);
      depth_current = desired;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = draw.pipeline->depth.write_enabled
                                                ? context.depth_write_dsv
                                                : context.depth_read_dsv;
    context.command_list->OMSetRenderTargets(
        1U, &context.target_rtv, FALSE,
        draw.pipeline->depth.test_enabled ? &dsv : nullptr);
    const std::size_t cbv_base = index * cbv_srv_descriptors_per_draw;
    const std::size_t sampler_base = index * sampler_descriptors_per_draw;
    context.command_list->SetGraphicsRootDescriptorTable(
        0U, gpu_handle(cbv_gpu_start, cbv_stride, cbv_base));
    context.command_list->SetGraphicsRootDescriptorTable(
        1U, gpu_handle(cbv_gpu_start, cbv_stride, cbv_base + 4U));
    context.command_list->SetGraphicsRootDescriptorTable(
        2U, gpu_handle(cbv_gpu_start, cbv_stride, cbv_base + 7U));
    context.command_list->SetGraphicsRootDescriptorTable(
        3U, gpu_handle(cbv_gpu_start, cbv_stride, cbv_base + 8U));
    context.command_list->SetGraphicsRootDescriptorTable(
        4U, gpu_handle(sampler_gpu_start, sampler_stride, sampler_base));
    const UINT64 vertex_offset =
        static_cast<UINT64>(packet.vertex_offset) * 44U;
    const UINT64 index_offset =
        static_cast<UINT64>(packet.index_offset) * sizeof(std::uint16_t);
    const UINT vertex_bytes = packet.vertex_count * 44U;
    const UINT index_bytes = packet.index_count * sizeof(std::uint16_t);
    D3D12_VERTEX_BUFFER_VIEW vb{draw.vertex_buffer->GetGPUVirtualAddress() +
                                    vertex_offset,
                                vertex_bytes, 44U};
    D3D12_INDEX_BUFFER_VIEW ib{draw.index_buffer->GetGPUVirtualAddress() +
                                   index_offset,
                               index_bytes, DXGI_FORMAT_R16_UINT};
    context.command_list->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.command_list->IASetVertexBuffers(0U, 1U, &vb);
    context.command_list->IASetIndexBuffer(&ib);
    context.command_list->DrawIndexedInstanced(packet.index_count, 1U, 0U, 0,
                                               0U);
  }
  const D3D12_RESOURCE_STATES resolve_before =
      requires_resolve ? *context.resolve_target_state
                       : D3D12_RESOURCE_STATE_COMMON;
  ID3D12Resource *readback_source = context.target;
  if (requires_resolve) {
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    transition(context.command_list, context.resolve_target, resolve_before,
               D3D12_RESOURCE_STATE_RESOLVE_DEST);
    context.command_list->ResolveSubresource(
        context.resolve_target, 0U, context.target, 0U,
        context.target_format);
    readback_source = context.resolve_target;
    transition(context.command_list, context.resolve_target,
               D3D12_RESOURCE_STATE_RESOLVE_DEST,
               context.capture_rgba8 ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                     : resolve_before);
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE, target_before);
  } else if (context.capture_rgba8) {
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
  } else {
    transition(context.command_list, context.target,
               D3D12_RESOURCE_STATE_RENDER_TARGET, target_before);
  }
  if (context.capture_rgba8) {
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = readback_source;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0U;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback_resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    context.command_list->CopyTextureRegion(&destination, 0U, 0U, 0U,
                                            &source, nullptr);
    if (requires_resolve)
      transition(context.command_list, context.resolve_target,
                 D3D12_RESOURCE_STATE_COPY_SOURCE, resolve_before);
    else
      transition(context.command_list, context.target,
                 D3D12_RESOURCE_STATE_COPY_SOURCE, target_before);
  }
  for (TrackedResource &entry : tracked)
    transition(context.command_list, entry.resource, entry.required,
               entry.before);
  if (use_depth)
    transition(context.command_list, context.depth, depth_current,
               depth_before);
  if (!submit_and_wait(context, fence.Get(), event, diagnostic)) {
    CloseHandle(event);
    if (diagnostic.code == "d3d12_stock_native_batch_submit_undrained") {
      *context.target_state = D3D12_RESOURCE_STATE_COMMON;
      *context.target_cleared = false;
      if (requires_resolve) {
        *context.resolve_target_state = D3D12_RESOURCE_STATE_COMMON;
        *context.resolve_target_cleared = false;
      }
      if (use_depth) {
        *context.depth_state = D3D12_RESOURCE_STATE_COMMON;
        if (context.depth_cleared != nullptr)
          *context.depth_cleared = false;
      }
      for (TrackedResource &entry : tracked)
        *entry.state = D3D12_RESOURCE_STATE_COMMON;
    }
    return false;
  }
  CloseHandle(event);
  *context.target_state = target_before;
  if (!context.load_color)
    *context.target_cleared = true;
  if (requires_resolve) {
    *context.resolve_target_state = resolve_before;
    *context.resolve_target_cleared = true;
  }
  if (use_depth && context.clear_depth)
    *context.depth_cleared = true;
  if (!context.capture_rgba8) {
    context.readback->clear();
    return true;
  }
  void *mapped = nullptr;
  D3D12_RANGE range{0U, static_cast<SIZE_T>(readback_size)};
  result = readback_resource->Map(0U, &range, &mapped);
  if (FAILED(result)) {
    diagnostic = hresult_error("Map(stock native batch readback)", result);
    return false;
  }
  try {
    context.readback->resize(static_cast<std::size_t>(context.target_width) *
                             context.target_height * 4U);
    const auto *bytes = static_cast<const std::byte *>(mapped);
    for (UINT row = 0U; row < context.target_height; ++row)
      std::memcpy(context.readback->data() +
                      static_cast<std::size_t>(row) * context.target_width * 4U,
                  bytes + footprint.Offset +
                      static_cast<std::size_t>(row) *
                          footprint.Footprint.RowPitch,
                  static_cast<std::size_t>(context.target_width) * 4U);
  } catch (const std::bad_alloc &) {
    readback_resource->Unmap(0U, nullptr);
    diagnostic = {"d3d12_stock_native_batch_readback_allocation_failed",
                  "Native batch readback allocation failed"};
    return false;
  }
  readback_resource->Unmap(0U, nullptr);
  if (context.target_format == DXGI_FORMAT_B8G8R8A8_UNORM ||
      context.target_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    for (std::size_t i = 0U; i < context.readback->size(); i += 4U)
      std::swap((*context.readback)[i], (*context.readback)[i + 2U]);
  return true;
}

} // namespace apex::render

#endif
