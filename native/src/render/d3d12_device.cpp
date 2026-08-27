#include "backend_internal.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/lighting.hpp"
#include "apex/render/viewport_frame_border.hpp"
#include "d3d12_stock_ks_per_pixel.hpp"
#include "d3d12_stock_ks_per_pixel_batch.hpp"
#include "d3d12_stock_ks_per_pixel_status.hpp"
#include "half_float.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <limits>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#    include <d3d12.h>
#    include <d3dcompiler.h>
#    include <dxgi1_6.h>
#    include <wrl/client.h>
#    include "generated/d3d12_hdr_tone_map_dxbc.hpp"
#    include "generated/d3d12_fxaa_dxbc.hpp"
#    include "generated/d3d12_portable_clouds_dxbc.hpp"
#    include "generated/d3d12_portable_grass_dxbc.hpp"
#    include "generated/d3d12_portable_sky_dxbc.hpp"
#    include "generated/d3d12_texture_mip_dxbc.hpp"
#endif

namespace apex::render {

bool valid_d3d12_native_window(const apex::platform::NativeSurfaceSource& source,
                               Diagnostic& diagnostic) {
    if (source.win32Window == nullptr) {
        diagnostic = {"d3d12_native_window_required",
                      "D3D12 native presentation requires a Win32 window handle"};
        return false;
    }
#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)
    if (!IsWindow(static_cast<HWND>(source.win32Window))) {
        diagnostic = {"d3d12_native_window_invalid",
                      "D3D12 native presentation requires a live Win32 window handle"};
        return false;
    }
#endif
    return true;
}

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)

namespace {

using Microsoft::WRL::ComPtr;

// Keep the shader-visible sampler heap within the SDK's documented capacity.
inline constexpr UINT kD3d12SamplerDescriptorCapacity = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

Diagnostic hresult_error(const char* operation, HRESULT result) {
    return {"d3d12_initialization_failed",
            std::string(operation) + " failed with HRESULT 0x" +
                [&] {
                    char buffer[9]{};
                    std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(result));
                    return std::string(buffer);
                }()};
}

struct Adapter {
    ComPtr<IDXGIAdapter1> handle;
    DXGI_ADAPTER_DESC1 description{};
};

struct D3D12PortableSkyPipeline {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT samples = 1U;
    ComPtr<ID3D12PipelineState> pipeline;
};

struct D3D12PortableCloudPipeline {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT samples = 1U;
    ComPtr<ID3D12PipelineState> pipeline;
};

struct D3D12PortableGrassPipeline {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT samples = 1U;
    ComPtr<ID3D12PipelineState> pipeline;
};

bool prepare_validation(const DeviceOptions& options, Diagnostic& diagnostic) {
    if (!options.enable_validation) {
        return true;
    }
    ComPtr<ID3D12Debug> debug;
    const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(result) || !debug) {
        diagnostic = hresult_error("D3D12GetDebugInterface", result);
        diagnostic.code = "d3d12_validation_unavailable";
        return false;
    }
    debug->EnableDebugLayer();
    return true;
}

[[nodiscard]] bool supports_d3d12(const ComPtr<IDXGIAdapter1>& adapter) {
    return SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                       __uuidof(ID3D12Device), nullptr));
}

std::vector<Adapter> adapters(const DeviceOptions& options, ComPtr<IDXGIFactory6>& factory) {
    UINT flags = 0;
    if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)))) {
        return {};
    }
    std::vector<Adapter> result;
    if (options.prefer_software) {
        ComPtr<IDXGIAdapter1> warp;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) || !warp || !supports_d3d12(warp))
            return result;
        Adapter item;
        item.handle = std::move(warp);
        if (FAILED(item.handle->GetDesc1(&item.description))) return result;
        result.push_back(std::move(item));
        return result;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter || !supports_d3d12(adapter)) continue;
        Adapter item;
        item.handle = std::move(adapter);
        if (FAILED(item.handle->GetDesc1(&item.description))) continue;
        if ((item.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 && !options.allow_software) continue;
        result.push_back(std::move(item));
    }
    return result;
}

DeviceInfo info_from(const DXGI_ADAPTER_DESC1& description) {
    DeviceInfo info;
    info.backend = Backend::D3D12;
    info.name = [&] {
        char name[sizeof(description.Description) / sizeof(wchar_t)]{};
        (void)WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name,
                                  static_cast<int>(sizeof(name)), nullptr, nullptr);
        return std::string(name);
    }();
    info.driver = "DXGI";
    info.vendor_id = description.VendorId;
    info.device_id = description.DeviceId;
    info.software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    return info;
}

struct D3D12Context {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    UINT sampler_descriptor_size = 0;
    UINT next_sampler = 0;
    std::mutex sampler_mutex;
    std::mutex command_mutex;
    ComPtr<ID3D12RootSignature> mip_root_signature;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>>
        mip_pipelines;
    ComPtr<ID3D12RootSignature> hdr_tone_map_root_signature;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>>
        hdr_tone_map_pipelines;
    ComPtr<ID3D12RootSignature> fxaa_root_signature;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>>
        fxaa_pipelines;
    ComPtr<ID3D12RootSignature> hdr_bloom_root_signature;
    ComPtr<ID3DBlob> hdr_bloom_vertex_shader;
    ComPtr<ID3DBlob> hdr_bloom_pixel_shader;
    ComPtr<ID3D12PipelineState> hdr_bloom_generate_pipeline;
    ComPtr<ID3D12PipelineState> hdr_bloom_add_pipeline;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>>
        hdr_bloom_screen_pipelines;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>>
        hdr_bloom_tone_map_pipelines;
    ComPtr<ID3D12RootSignature> portable_sky_root_signature;
    std::vector<D3D12PortableSkyPipeline> portable_sky_pipelines;
    ComPtr<ID3D12RootSignature> portable_cloud_root_signature;
    std::vector<D3D12PortableCloudPipeline> portable_cloud_pipelines;
    ComPtr<ID3D12RootSignature> portable_grass_root_signature;
    std::vector<D3D12PortableGrassPipeline> portable_grass_pipelines;
    void* native_window = nullptr;
    bool presentation_target_active = false;
    DeviceInfo info;

    ~D3D12Context() { (void)wait_idle(); }

    bool wait_idle() noexcept {
        if (!queue || !device) return true;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        constexpr UINT64 value = 1;
        bool success = SUCCEEDED(queue->Signal(fence.Get(), value));
        if (success && fence->GetCompletedValue() < value)
            success = SUCCEEDED(fence->SetEventOnCompletion(value, event));
        if (success && fence->GetCompletedValue() < value) {
            const DWORD wait_result = WaitForSingleObject(event, INFINITE);
            success = wait_result == WAIT_OBJECT_0;
        }
        CloseHandle(event);
        return success;
    }
};

class D3D12DepthAttachment;
class D3D12Texture;
class D3D12Sampler;

struct D3D12PortableCloudTextureResource {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES* tracked_state = nullptr;
    D3D12_RESOURCE_STATES original_state = D3D12_RESOURCE_STATE_COMMON;
    TextureDescription description{};
    bool initialized = false;
};

using D3D12PortableGrassTextureResource = D3D12PortableCloudTextureResource;

[[nodiscard]] bool inspect_d3d12_portable_cloud_texture(
    const Texture* requested, const D3D12Context* context,
    D3D12PortableCloudTextureResource& output,
    Diagnostic& diagnostic);
[[nodiscard]] bool inspect_d3d12_portable_cloud_sampler(
    const Sampler* requested, const D3D12Context* context,
    D3D12_GPU_DESCRIPTOR_HANDLE& output,
    Diagnostic& diagnostic);
[[nodiscard]] bool inspect_d3d12_portable_grass_texture(
    const Texture* requested, const D3D12Context* context,
    D3D12PortableGrassTextureResource& output,
    Diagnostic& diagnostic);
[[nodiscard]] bool inspect_d3d12_portable_grass_sampler(
    const Sampler* requested, const D3D12Context* context,
    D3D12_GPU_DESCRIPTOR_HANDLE& output,
    Diagnostic& diagnostic);

[[nodiscard]] D3D12_RESOURCE_STATES d3d12_texture_state(
    const D3D12Texture& texture) noexcept;
[[nodiscard]] ID3D12Resource* d3d12_texture_resource(
    const D3D12Texture& texture) noexcept;
[[nodiscard]] const TextureDescription& d3d12_texture_description(
    const D3D12Texture& texture) noexcept;
[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE d3d12_sampler_gpu(
    const D3D12Sampler& sampler) noexcept;
void d3d12_texture_set_state(D3D12Texture& texture,
                             D3D12_RESOURCE_STATES state) noexcept;

struct D3D12MaterialDescriptorBinding {
    // Keep the per-draw SRV heap alive until the synchronous fence completes.
    // The sampler descriptor is persistent in the context-owned shader-visible
    // sampler heap, so only its GPU handle is needed here.
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE cbv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE frame_cbv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE normal_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE normal_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE maps_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE maps_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE detail_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE detail_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE normal_detail_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE normal_detail_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE damage_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE damage_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE damage_mask_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE damage_mask_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadow_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadow_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadow_cbv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE multimap_cube_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE multimap_cube_sampler_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE multimap_reflection_cbv_gpu{};
    std::array<D3D12DepthAttachment*, indexed_directional_shadow_cascade_count> shadow_attachments{};
    bool constant_enabled = false;
    bool frame_enabled = false;
    bool normal_enabled = false;
    bool maps_enabled = false;
    bool detail_enabled = false;
    bool normal_detail_enabled = false;
    bool damage_enabled = false;
    bool damage_mask_enabled = false;
    bool shadow_enabled = false;
    bool multimap_reflection_enabled = false;
    bool enabled = false;
};

[[nodiscard]] bool d3d12_pipeline_has_material_constants(const PipelineProgram& pipeline) noexcept {
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 2U &&
            resource.kind == PipelineResourceKind::uniform_buffer) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool d3d12_pipeline_has_frame_constants(const PipelineProgram& pipeline) noexcept {
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 3U &&
            resource.kind == PipelineResourceKind::uniform_buffer) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool d3d12_pipeline_has_normal_texture(const PipelineProgram& pipeline) noexcept {
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 4U &&
            resource.kind == PipelineResourceKind::sampled_texture)
            texture = true;
        if (resource.set == 0U && resource.binding == 5U &&
            resource.kind == PipelineResourceKind::sampler)
            sampler = true;
    }
    return texture && sampler;
}

[[nodiscard]] bool d3d12_pipeline_has_maps_texture(const PipelineProgram& pipeline) noexcept {
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 6U &&
            resource.kind == PipelineResourceKind::sampled_texture)
            texture = true;
        if (resource.set == 0U && resource.binding == 7U &&
            resource.kind == PipelineResourceKind::sampler)
            sampler = true;
    }
    return texture && sampler;
}

[[nodiscard]] bool d3d12_pipeline_has_detail_texture(const PipelineProgram& pipeline) noexcept {
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 8U &&
            resource.kind == PipelineResourceKind::sampled_texture)
            texture = true;
        if (resource.set == 0U && resource.binding == 9U &&
            resource.kind == PipelineResourceKind::sampler)
            sampler = true;
    }
    return texture && sampler;
}

[[nodiscard]] bool d3d12_pipeline_has_normal_detail_texture(const PipelineProgram& pipeline) noexcept {
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 10U &&
            resource.kind == PipelineResourceKind::sampled_texture)
            texture = true;
        if (resource.set == 0U && resource.binding == 11U &&
            resource.kind == PipelineResourceKind::sampler)
            sampler = true;
    }
    return texture && sampler;
}

[[nodiscard]] bool d3d12_pipeline_has_damage_texture(const PipelineProgram& pipeline) noexcept {
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 12U &&
            resource.kind == PipelineResourceKind::sampled_texture)
            texture = true;
        if (resource.set == 0U && resource.binding == 13U &&
            resource.kind == PipelineResourceKind::sampler)
            sampler = true;
    }
    return texture && sampler;
}

[[nodiscard]] bool d3d12_pipeline_has_damage_mask_texture(const PipelineProgram& pipeline) noexcept {
    bool texture = false;
    bool sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding == 14U &&
            resource.kind == PipelineResourceKind::sampled_texture)
            texture = true;
        if (resource.set == 0U && resource.binding == 15U &&
            resource.kind == PipelineResourceKind::sampler)
            sampler = true;
    }
    return texture && sampler;
}

[[nodiscard]] bool d3d12_pipeline_has_directional_shadow_receiver(const PipelineProgram& pipeline) noexcept {
    return pipeline_declares_directional_shadow_receiver(pipeline);
}

[[nodiscard]] bool d3d12_pipeline_has_multimap_reflection(
    const PipelineProgram& pipeline) noexcept {
    return pipeline_declares_multimap_reflection(pipeline);
}

[[nodiscard]] bool prepare_d3d12_material_binding(
    const std::shared_ptr<D3D12Context>& context,
    const IndexedStaticMeshDrawRequest& request,
    ID3D12DescriptorHeap* srv_heap,
    UINT srv_index,
    UINT cbv_index,
    UINT frame_cbv_index,
    UINT normal_srv_index,
    UINT maps_srv_index,
    UINT detail_srv_index,
    UINT normal_detail_srv_index,
    UINT damage_srv_index,
    UINT damage_mask_srv_index,
    UINT shadow_srv_index,
    UINT shadow_cbv_index,
    UINT multimap_cube_srv_index,
    UINT multimap_reflection_cbv_index,
    D3D12MaterialDescriptorBinding& output,
    Diagnostic& diagnostic);

[[nodiscard]] bool valid_d3d12_usage(BufferUsage usage, Diagnostic& diagnostic) noexcept {
    const auto raw = static_cast<std::uint32_t>(usage);
    const auto transfer_destination = static_cast<std::uint32_t>(BufferUsage::transfer_destination);
    const auto storage = static_cast<std::uint32_t>(BufferUsage::storage);
    if ((raw & transfer_destination) != 0U && raw != transfer_destination) {
        diagnostic = {"d3d12_transfer_destination_exclusive",
                      "D3D12 COPY_DEST cannot be combined with another buffer usage"};
        return false;
    }
    if ((raw & storage) != 0U && raw != storage) {
        diagnostic = {"d3d12_storage_usage_exclusive",
                      "D3D12 unordered-access storage cannot be combined with another buffer usage"};
        return false;
    }
    return true;
}

[[nodiscard]] D3D12_RESOURCE_STATES resource_state(BufferUsage usage) noexcept {
    const auto raw = static_cast<std::uint32_t>(usage);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const auto add = [&state](D3D12_RESOURCE_STATES bit) {
        state = static_cast<D3D12_RESOURCE_STATES>(static_cast<UINT>(state) | static_cast<UINT>(bit));
    };
    if ((raw & static_cast<std::uint32_t>(BufferUsage::transfer_source)) != 0U)
        add(D3D12_RESOURCE_STATE_COPY_SOURCE);
    if ((raw & static_cast<std::uint32_t>(BufferUsage::transfer_destination)) != 0U)
        add(D3D12_RESOURCE_STATE_COPY_DEST);
    if ((raw & static_cast<std::uint32_t>(BufferUsage::vertex)) != 0U ||
        (raw & static_cast<std::uint32_t>(BufferUsage::uniform)) != 0U)
        add(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if ((raw & static_cast<std::uint32_t>(BufferUsage::index)) != 0U)
        add(D3D12_RESOURCE_STATE_INDEX_BUFFER);
    if ((raw & static_cast<std::uint32_t>(BufferUsage::storage)) != 0U)
        add(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return state;
}

bool execute_buffer_copy(const std::shared_ptr<D3D12Context>& context,
                         ID3D12Resource* source,
                         ID3D12Resource* destination,
                         UINT64 destination_offset,
                         UINT64 size,
                         D3D12_RESOURCE_STATES& destination_state,
                         D3D12_RESOURCE_STATES final_state,
                         Diagnostic& diagnostic) {
    ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT result = context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                               IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandAllocator", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandList", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    if (destination_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = destination_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    list->CopyBufferRegion(destination, destination_offset, source, 0, size);
    if (final_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12GraphicsCommandList::Close", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateFence", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        diagnostic = {"buffer_upload_failed", "CreateEventW failed while waiting for a D3D12 buffer upload"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1, command_lists);
    constexpr UINT64 fence_value = 1;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (FAILED(result)) {
        context->wait_idle();
        CloseHandle(event);
        diagnostic = hresult_error("ID3D12CommandQueue::Signal", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    if (fence->GetCompletedValue() < fence_value) {
        result = fence->SetEventOnCompletion(fence_value, event);
        if (FAILED(result)) {
            context->wait_idle();
            CloseHandle(event);
            diagnostic = hresult_error("ID3D12Fence::SetEventOnCompletion", result);
            diagnostic.code = "buffer_upload_failed";
            return false;
        }
        const DWORD wait_result = WaitForSingleObject(event, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            context->wait_idle();
            CloseHandle(event);
            diagnostic = {"buffer_upload_failed", "Waiting for the D3D12 buffer upload fence failed"};
            return false;
        }
    }
    CloseHandle(event);
    destination_state = final_state;
    return true;
}

class D3D12Buffer final : public Buffer {
public:
    D3D12Buffer(std::shared_ptr<D3D12Context> context,
                ComPtr<ID3D12Resource> resource,
                BufferDescription description,
                D3D12_RESOURCE_STATES state)
        : context_(std::move(context)), resource_(std::move(resource)), info_({description}), state_(state) {}

    ~D3D12Buffer() override {
        if (context_) context_->wait_idle();
    }

    Backend backend() const noexcept override { return Backend::D3D12; }
    const BufferInfo& info() const noexcept override { return info_; }
    [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
    [[nodiscard]] D3D12_RESOURCE_STATES state() const noexcept { return state_; }
    [[nodiscard]] const D3D12Context* context() const noexcept { return context_.get(); }

    bool write_host(std::uint64_t offset, std::span<const std::byte> data, Diagnostic& diagnostic) {
        void* mapped = nullptr;
        const HRESULT result = resource_->Map(0, nullptr, &mapped);
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Resource::Map", result);
            diagnostic.code = "buffer_upload_failed";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(mapped) + static_cast<std::size_t>(offset), data.data(), data.size());
        D3D12_RANGE written{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + data.size())};
        resource_->Unmap(0, &written);
        return true;
    }

    bool write_device(std::uint64_t offset, std::span<const std::byte> data, Diagnostic& diagnostic) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = static_cast<UINT64>(data.size());
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> staging;
        HRESULT result = context_->device->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&staging));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateCommittedResource(upload)", result);
            diagnostic.code = "buffer_upload_failed";
            return false;
        }
        void* mapped = nullptr;
        result = staging->Map(0, nullptr, &mapped);
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Resource::Map(upload)", result);
            diagnostic.code = "buffer_upload_failed";
            return false;
        }
        std::memcpy(mapped, data.data(), data.size());
        staging->Unmap(0, nullptr);
        return execute_buffer_copy(context_, staging.Get(), resource_.Get(), static_cast<UINT64>(offset),
                                   static_cast<UINT64>(data.size()), state_, resource_state(info_.description.usage), diagnostic);
    }

private:
    std::shared_ptr<D3D12Context> context_;
    ComPtr<ID3D12Resource> resource_;
    BufferInfo info_;
    D3D12_RESOURCE_STATES state_;
};

class D3D12DepthAttachment final : public DepthAttachment {
public:
    D3D12DepthAttachment(std::shared_ptr<D3D12Context> context,
                         ComPtr<ID3D12Resource> resource,
                         ComPtr<ID3D12DescriptorHeap> dsv_heap,
                         ComPtr<ID3D12DescriptorHeap> srv_heap,
                         DepthAttachmentDescription description,
                         D3D12_RESOURCE_STATES state,
                         D3D12_CPU_DESCRIPTOR_HANDLE write_dsv,
                         D3D12_CPU_DESCRIPTOR_HANDLE read_dsv,
                         D3D12_CPU_DESCRIPTOR_HANDLE srv)
        : context_(std::move(context)),
          resource_(std::move(resource)),
          dsv_heap_(std::move(dsv_heap)),
          srv_heap_(std::move(srv_heap)),
          info_({description}),
          state_(state),
          write_dsv_(write_dsv),
          read_dsv_(read_dsv),
          srv_(srv) {}

    ~D3D12DepthAttachment() override {
        if (context_) context_->wait_idle();
    }

    Backend backend() const noexcept override { return Backend::D3D12; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }
    [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
    [[nodiscard]] D3D12_RESOURCE_STATES state() const noexcept { return state_; }
    [[nodiscard]] D3D12_RESOURCE_STATES* state_pointer() noexcept {
        return &state_;
    }
    [[nodiscard]] const D3D12Context* context() const noexcept { return context_.get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv(bool writable) const noexcept {
        return writable ? write_dsv_ : read_dsv_;
    }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE srv() const noexcept { return srv_; }
    [[nodiscard]] bool cleared() const noexcept { return cleared_; }
    [[nodiscard]] bool* cleared_pointer() noexcept { return &cleared_; }
    void set_state(D3D12_RESOURCE_STATES state) noexcept { state_ = state; }
    void set_cleared() noexcept { cleared_ = true; }

    bool readback(const DepthAttachmentReadbackRequest& request,
                  std::vector<float>& output,
                  Diagnostic& diagnostic) {
        if (!cleared_) {
            diagnostic = {"depth_attachment_uninitialized",
                          "D32 depth readback requires an initialized depth attachment"};
            return false;
        }

        const D3D12_RESOURCE_DESC resource_description = resource_->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT row_count = 0U;
        UINT64 row_size = 0U;
        UINT64 readback_size = 0U;
        context_->device->GetCopyableFootprints(&resource_description, 0U, 1U, 0U, &footprint,
                                                &row_count, &row_size, &readback_size);
        constexpr UINT64 max_size_t = static_cast<UINT64>(std::numeric_limits<std::size_t>::max());
        const UINT64 row_bytes = static_cast<UINT64>(request.output_width) * sizeof(float);
        bool footprint_valid = row_count >= request.output_height && row_size >= row_bytes &&
                               footprint.Footprint.RowPitch >= row_bytes &&
                               footprint.Offset <= max_size_t &&
                               footprint.Footprint.RowPitch <= max_size_t &&
                               readback_size <= max_texture_readback_bytes &&
                               readback_size <= max_size_t;
        if (footprint_valid) {
            const UINT64 rows_before_last = static_cast<UINT64>(request.output_height - 1U);
            if (rows_before_last > (std::numeric_limits<UINT64>::max() - footprint.Offset) /
                                       footprint.Footprint.RowPitch) {
                footprint_valid = false;
            } else {
                const UINT64 last_row = footprint.Offset +
                                         rows_before_last * footprint.Footprint.RowPitch;
                footprint_valid = row_bytes <= std::numeric_limits<UINT64>::max() - last_row &&
                                  last_row + row_bytes <= readback_size;
            }
        }
        if (!footprint_valid) {
            diagnostic = {"depth_attachment_readback_footprint_invalid",
                          "D3D12 D32 depth readback footprint exceeds the bounded output"};
            return false;
        }

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
        ComPtr<ID3D12Resource> readback_resource;
        HRESULT result = context_->device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_resource));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateCommittedResource(depth readback)", result);
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        result = context_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateCommandAllocator(depth readback)", result);
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }
        ComPtr<ID3D12GraphicsCommandList> list;
        result = context_->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&list));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateCommandList(depth readback)", result);
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }
        const D3D12_RESOURCE_STATES final_state = state_;
        if (state_ != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource_.Get();
            barrier.Transition.StateBefore = state_;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = resource_.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0U;
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = readback_resource.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = footprint;
        list->CopyTextureRegion(&target, 0U, 0U, 0U, &source, nullptr);
        if (final_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource_.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter = final_state;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &barrier);
        }
        result = list->Close();
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12GraphicsCommandList::Close(depth readback)", result);
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }
        ComPtr<ID3D12Fence> fence;
        result = context_->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateFence(depth readback)", result);
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            diagnostic = {"depth_attachment_readback_failed",
                          "CreateEventW failed while waiting for a D3D12 depth readback"};
            return false;
        }
        ID3D12CommandList* command_lists[] = {list.Get()};
        context_->queue->ExecuteCommandLists(1U, command_lists);
        constexpr UINT64 fence_value = 1U;
        result = context_->queue->Signal(fence.Get(), fence_value);
        if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
            result = fence->SetEventOnCompletion(fence_value, event);
        if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value) {
            const DWORD wait_result = WaitForSingleObject(event, INFINITE);
            if (wait_result != WAIT_OBJECT_0) result = E_FAIL;
        }
        CloseHandle(event);
        if (FAILED(result)) {
            const bool drained = context_->wait_idle();
            state_ = drained ? final_state : D3D12_RESOURCE_STATE_COMMON;
            diagnostic = hresult_error("D3D12 depth readback fence", result);
            diagnostic.code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                                  ? "d3d12_device_removed"
                                  : "depth_attachment_readback_failed";
            return false;
        }
        state_ = final_state;

        void* mapped = nullptr;
        D3D12_RANGE read_range{0U, static_cast<SIZE_T>(readback_size)};
        result = readback_resource->Map(0U, &read_range, &mapped);
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Resource::Map(depth readback)", result);
            diagnostic.code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                                  ? "d3d12_device_removed"
                                  : "depth_attachment_readback_failed";
            return false;
        }
        try {
            output.resize(static_cast<std::size_t>(request.output_width) *
                          static_cast<std::size_t>(request.output_height));
            const auto* source_bytes = static_cast<const std::byte*>(mapped) + footprint.Offset;
            for (std::uint32_t row = 0U; row < request.output_height; ++row) {
                std::memcpy(output.data() + static_cast<std::size_t>(row) * request.output_width,
                            source_bytes + static_cast<std::size_t>(row) *
                                               footprint.Footprint.RowPitch,
                            static_cast<std::size_t>(row_bytes));
            }
        } catch (const std::bad_alloc&) {
            result = E_OUTOFMEMORY;
        }
        readback_resource->Unmap(0U, nullptr);
        if (FAILED(result)) {
            output.clear();
            diagnostic = hresult_error("D3D12 depth readback allocation", result);
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<D3D12Context> context_;
    ComPtr<ID3D12Resource> resource_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    DepthAttachmentInfo info_;
    D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_CPU_DESCRIPTOR_HANDLE write_dsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE read_dsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_{};
    bool cleared_ = false;
};

[[nodiscard]] DXGI_FORMAT dxgi_texture_format(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::rgba8_unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::bgra8_unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::rgba8_srgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::bgra8_srgb:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case TextureFormat::r8_unorm:
        return DXGI_FORMAT_R8_UNORM;
    case TextureFormat::r32_sfloat:
        return DXGI_FORMAT_R32_FLOAT;
    case TextureFormat::r5g6b5_unorm:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case TextureFormat::rgba16_sfloat:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::bc1_unorm:
        return DXGI_FORMAT_BC1_UNORM;
    case TextureFormat::bc1_srgb:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case TextureFormat::bc2_unorm:
        return DXGI_FORMAT_BC2_UNORM;
    case TextureFormat::bc2_srgb:
        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case TextureFormat::bc3_unorm:
        return DXGI_FORMAT_BC3_UNORM;
    case TextureFormat::bc3_srgb:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case TextureFormat::bc4_unorm:
        return DXGI_FORMAT_BC4_UNORM;
    case TextureFormat::bc5_unorm:
        return DXGI_FORMAT_BC5_UNORM;
    case TextureFormat::bc5_snorm:
        return DXGI_FORMAT_BC5_SNORM;
    case TextureFormat::bc6h_ufloat:
        return DXGI_FORMAT_BC6H_UF16;
    case TextureFormat::bc6h_sfloat:
        return DXGI_FORMAT_BC6H_SF16;
    case TextureFormat::bc7_unorm:
        return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::bc7_srgb:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC
d3d12_texture_srv_description(const TextureDescription& description,
                              const DXGI_FORMAT format) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (texture_default_sample_view_kind(description)) {
    case TextureSampleViewKind::texture_2d:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MostDetailedMip = 0U;
        view.Texture2D.MipLevels = description.mip_levels;
        view.Texture2D.PlaneSlice = 0U;
        view.Texture2D.ResourceMinLODClamp = 0.0F;
        break;
    case TextureSampleViewKind::texture_2d_array:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MostDetailedMip = 0U;
        view.Texture2DArray.MipLevels = description.mip_levels;
        view.Texture2DArray.FirstArraySlice = 0U;
        view.Texture2DArray.ArraySize = description.array_layers;
        view.Texture2DArray.PlaneSlice = 0U;
        view.Texture2DArray.ResourceMinLODClamp = 0.0F;
        break;
    case TextureSampleViewKind::texture_cube:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.TextureCube.MostDetailedMip = 0U;
        view.TextureCube.MipLevels = description.mip_levels;
        view.TextureCube.ResourceMinLODClamp = 0.0F;
        break;
    case TextureSampleViewKind::texture_cube_array:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        view.TextureCubeArray.MostDetailedMip = 0U;
        view.TextureCubeArray.MipLevels = description.mip_levels;
        view.TextureCubeArray.First2DArrayFace = 0U;
        view.TextureCubeArray.NumCubes = description.array_layers;
        view.TextureCubeArray.ResourceMinLODClamp = 0.0F;
        break;
    }
    return view;
}

[[nodiscard]] bool is_d3d12_supported_block_format(TextureFormat format) noexcept {
    return format == TextureFormat::bc1_unorm || format == TextureFormat::bc1_srgb ||
           format == TextureFormat::bc2_unorm || format == TextureFormat::bc2_srgb ||
           format == TextureFormat::bc3_unorm || format == TextureFormat::bc3_srgb ||
           format == TextureFormat::bc4_unorm ||
           format == TextureFormat::bc5_unorm || format == TextureFormat::bc5_snorm ||
           format == TextureFormat::bc6h_ufloat || format == TextureFormat::bc6h_sfloat ||
           format == TextureFormat::bc7_unorm || format == TextureFormat::bc7_srgb;
}

[[nodiscard]] bool validate_d3d12_texture_format_support(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT format,
    std::uint32_t mip_levels, Diagnostic& diagnostic,
    TextureUsage usage = TextureUsage::sampled) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    const HRESULT result = context->device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CheckFeatureSupport(format)", result);
        diagnostic.code = "d3d12_texture_format_query_failed";
        return false;
    }
    UINT required = static_cast<UINT>(D3D12_FORMAT_SUPPORT1_TEXTURE2D);
    const auto raw_usage = static_cast<std::uint32_t>(usage);
    if ((raw_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) != 0U)
        required |= static_cast<UINT>(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
    if ((raw_usage & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        required |= static_cast<UINT>(D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    if ((raw_usage & static_cast<std::uint32_t>(TextureUsage::storage)) != 0U)
        required |= static_cast<UINT>(D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    const UINT support1 = static_cast<UINT>(support.Support1);
    if ((support1 & required) != required ||
        (mip_levels > 1U && (support1 & static_cast<UINT>(D3D12_FORMAT_SUPPORT1_MIP)) == 0U)) {
        diagnostic = {"d3d12_texture_format_unsupported",
                      "The D3D12 adapter does not support the requested texture format and usage"};
        return false;
    }
    return true;
}

[[nodiscard]] D3D12_RESOURCE_STATES texture_state(
    TextureUsage usage,
    TextureAccessPolicy access_policy = TextureAccessPolicy::fixed_usage) noexcept {
    if (access_policy == TextureAccessPolicy::render_then_sample)
        return static_cast<D3D12_RESOURCE_STATES>(
            static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
            static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto raw = static_cast<std::uint32_t>(usage);
    if ((raw & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const auto add = [&state](D3D12_RESOURCE_STATES bit) {
        state = static_cast<D3D12_RESOURCE_STATES>(static_cast<UINT>(state) | static_cast<UINT>(bit));
    };
    if ((raw & static_cast<std::uint32_t>(TextureUsage::sampled)) != 0U)
        add(static_cast<D3D12_RESOURCE_STATES>(static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                               static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
    if ((raw & static_cast<std::uint32_t>(TextureUsage::transfer_source)) != 0U)
        add(D3D12_RESOURCE_STATE_COPY_SOURCE);
    if ((raw & static_cast<std::uint32_t>(TextureUsage::transfer_destination)) != 0U)
        add(D3D12_RESOURCE_STATE_COPY_DEST);
    if ((raw & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        add(D3D12_RESOURCE_STATE_RENDER_TARGET);
    if ((raw & static_cast<std::uint32_t>(TextureUsage::storage)) != 0U)
        add(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return state;
}

[[nodiscard]] bool prepare_d3d12_mip_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT format,
    ID3D12PipelineState*& pipeline, Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context->mip_root_signature == nullptr) {
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1U;
        srv_range.BaseShaderRegister = 0U;
        srv_range.RegisterSpace = 0U;
        srv_range.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER srv_parameter{};
        srv_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        srv_parameter.DescriptorTable.pDescriptorRanges = &srv_range;
        srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.0F;
        sampler.MaxAnisotropy = 1U;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0F;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0U;
        sampler.RegisterSpace = 0U;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = 1U;
        root_description.pParameters = &srv_parameter;
        root_description.NumStaticSamplers = 1U;
        root_description.pStaticSamplers = &sampler;
        root_description.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(texture mip)", result);
            diagnostic.code = "d3d12_texture_mip_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->mip_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(texture mip)", result);
            diagnostic.code = "d3d12_texture_mip_pipeline_failed";
            return false;
        }
    }

    for (const auto& cached : context->mip_pipelines) {
        if (cached.first == format) {
            pipeline = cached.second.Get();
            return true;
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->mip_root_signature.Get();
    description.VS = {
        generated::d3d12_texture_mip_vertex_dxbc,
        generated::d3d12_texture_mip_vertex_dxbc_size};
    description.PS = {
        generated::d3d12_texture_mip_pixel_dxbc,
        generated::d3d12_texture_mip_pixel_dxbc_size};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.RasterizerState.DepthBiasClamp =
        D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.RasterizerState.MultisampleEnable = FALSE;
    description.RasterizerState.AntialiasedLineEnable = FALSE;
    description.RasterizerState.ForcedSampleCount = 0U;
    description.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_RENDER_TARGET_BLEND_DESC& blend =
        description.BlendState.RenderTarget[0U];
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask =
        D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    description.DepthStencilState.BackFace =
        description.DepthStencilState.FrontFace;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = format;
    description.SampleDesc.Count = 1U;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(texture mip)", result);
        diagnostic.code = "d3d12_texture_mip_pipeline_failed";
        return false;
    }
    pipeline = created.Get();
    context->mip_pipelines.emplace_back(format, std::move(created));
    return true;
}

[[nodiscard]] bool execute_texture_mip_generation(
    const std::shared_ptr<D3D12Context>& context, ID3D12Resource* texture,
    const TextureDescription& description,
    D3D12_RESOURCE_STATES& texture_resource_state,
    Diagnostic& diagnostic) {
    const std::uint64_t physical_layers =
        texture_physical_array_layers(description);
    const std::uint64_t operations =
        physical_layers * static_cast<std::uint64_t>(description.mip_levels - 1U);
    if (operations == 0U ||
        operations > std::numeric_limits<UINT>::max()) {
        diagnostic = {
            "d3d12_texture_mip_descriptor_limit",
            "Texture mip generation exceeds the D3D12 descriptor count limit"};
        return false;
    }

    std::lock_guard command_guard(context->command_mutex);
    ID3D12PipelineState* pipeline = nullptr;
    const DXGI_FORMAT format = dxgi_texture_format(description.format);
    if (!prepare_d3d12_mip_pipeline(context, format, pipeline, diagnostic))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
    srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_description.NumDescriptors = static_cast<UINT>(operations);
    srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    HRESULT result = context->device->CreateDescriptorHeap(
        &srv_heap_description, IID_PPV_ARGS(&srv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(texture mip SRV)", result);
        diagnostic.code = "d3d12_texture_mip_descriptor_failed";
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.NumDescriptors = static_cast<UINT>(operations);
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(
        &rtv_heap_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(texture mip RTV)", result);
        diagnostic.code = "d3d12_texture_mip_descriptor_failed";
        return false;
    }
    const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const std::uint64_t last_operation = operations - 1U;
    if (srv_stride == 0U || rtv_stride == 0U ||
        last_operation >
            std::numeric_limits<SIZE_T>::max() / srv_stride ||
        last_operation >
            std::numeric_limits<SIZE_T>::max() / rtv_stride ||
        last_operation >
            std::numeric_limits<UINT64>::max() / srv_stride) {
        diagnostic = {
            "d3d12_texture_mip_descriptor_failed",
            "Texture mip descriptor handles exceed the D3D12 address range"};
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE first_srv =
        srv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE first_rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t layer = 0U;
         layer < static_cast<std::uint32_t>(physical_layers); ++layer) {
        for (std::uint32_t mip = 1U; mip < description.mip_levels; ++mip) {
            const UINT operation =
                layer * (description.mip_levels - 1U) + (mip - 1U);
            D3D12_CPU_DESCRIPTOR_HANDLE srv = first_srv;
            srv.ptr += static_cast<SIZE_T>(operation) * srv_stride;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_description{};
            srv_description.Format = format;
            srv_description.ViewDimension =
                D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv_description.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_description.Texture2DArray.MostDetailedMip = mip - 1U;
            srv_description.Texture2DArray.MipLevels = 1U;
            srv_description.Texture2DArray.FirstArraySlice = layer;
            srv_description.Texture2DArray.ArraySize = 1U;
            srv_description.Texture2DArray.PlaneSlice = 0U;
            srv_description.Texture2DArray.ResourceMinLODClamp = 0.0F;
            context->device->CreateShaderResourceView(
                texture, &srv_description, srv);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = first_rtv;
            rtv.ptr += static_cast<SIZE_T>(operation) * rtv_stride;
            D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
            rtv_description.Format = format;
            rtv_description.ViewDimension =
                D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv_description.Texture2DArray.MipSlice = mip;
            rtv_description.Texture2DArray.FirstArraySlice = layer;
            rtv_description.Texture2DArray.ArraySize = 1U;
            rtv_description.Texture2DArray.PlaneSlice = 0U;
            context->device->CreateRenderTargetView(
                texture, &rtv_description, rtv);
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandAllocator(texture mip)", result);
        diagnostic.code = "d3d12_texture_mip_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline,
        IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandList(texture mip)", result);
        diagnostic.code = "d3d12_texture_mip_execution_failed";
        return false;
    }

    constexpr D3D12_RESOURCE_STATES source_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES final_state =
        texture_state(description.usage, description.access_policy);
    if (texture_resource_state != source_state) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = texture_resource_state;
        barrier.Transition.StateAfter = source_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1U, descriptor_heaps);
    list->SetGraphicsRootSignature(context->mip_root_signature.Get());
    list->SetPipelineState(pipeline);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_GPU_DESCRIPTOR_HANDLE first_srv_gpu =
        srv_heap->GetGPUDescriptorHandleForHeapStart();
    for (std::uint32_t layer = 0U;
         layer < static_cast<std::uint32_t>(physical_layers); ++layer) {
        for (std::uint32_t mip = 1U; mip < description.mip_levels; ++mip) {
            const UINT operation =
                layer * (description.mip_levels - 1U) + (mip - 1U);
            const UINT subresource = layer * description.mip_levels + mip;
            D3D12_RESOURCE_BARRIER to_render_target{};
            to_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_render_target.Transition.pResource = texture;
            to_render_target.Transition.StateBefore = source_state;
            to_render_target.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            to_render_target.Transition.Subresource = subresource;
            list->ResourceBarrier(1U, &to_render_target);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = first_rtv;
            rtv.ptr += static_cast<SIZE_T>(operation) * rtv_stride;
            list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
            const std::uint32_t width =
                std::max(1U, description.width >> mip);
            const std::uint32_t height =
                std::max(1U, description.height >> mip);
            const D3D12_VIEWPORT viewport = {
                0.0F, 0.0F, static_cast<float>(width),
                static_cast<float>(height), 0.0F, 1.0F};
            const D3D12_RECT scissor = {
                0L, 0L, static_cast<LONG>(width),
                static_cast<LONG>(height)};
            list->RSSetViewports(1U, &viewport);
            list->RSSetScissorRects(1U, &scissor);
            D3D12_GPU_DESCRIPTOR_HANDLE srv = first_srv_gpu;
            srv.ptr += static_cast<UINT64>(operation) * srv_stride;
            list->SetGraphicsRootDescriptorTable(0U, srv);
            list->DrawInstanced(3U, 1U, 0U, 0U);

            std::swap(to_render_target.Transition.StateBefore,
                      to_render_target.Transition.StateAfter);
            list->ResourceBarrier(1U, &to_render_target);
        }
    }
    if (final_state != source_state) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = source_state;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12GraphicsCommandList::Close(texture mip)", result);
        diagnostic.code = "d3d12_texture_mip_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(
        0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateFence(texture mip)", result);
        diagnostic.code = "d3d12_texture_mip_execution_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        diagnostic = {
            "d3d12_texture_mip_execution_failed",
            "CreateEventW failed while waiting for D3D12 mip generation"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    constexpr UINT64 fence_value = 1U;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
        result = fence->SetEventOnCompletion(fence_value, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        texture_resource_state =
            drained ? final_state : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 texture mip fence", result);
        diagnostic.code =
            result == DXGI_ERROR_DEVICE_REMOVED ||
                    result == DXGI_ERROR_DEVICE_RESET
                ? "d3d12_device_removed"
                : "d3d12_texture_mip_execution_failed";
        return false;
    }
    texture_resource_state = final_state;
    diagnostic = {};
    return true;
}

// The public tone-map contract intentionally keeps bloom handles private. The
// D3D12 implementation therefore builds a short-lived five-level chain here.
// This shader is source-equivalent to the production WebGL topology, not a
// claim of recovered Yebis bytecode. Runtime compilation is used only for this
// optional path so the existing checked-in tone-map artifacts remain stable.
constexpr char kD3d12BloomShader[] = R"HLSL(
// Portable five-level bloom and bloom-aware tone-map shader.
// Keep this source synchronized with kD3d12BloomShader in d3d12_device.cpp.
Texture2D<float4> source_texture : register(t0);
Texture2D<float4> bloom_texture : register(t1);
SamplerState linear_clamp : register(s0);

cbuffer BloomParameters : register(b0) {
    uint mode;
    float exposure;
    float threshold;
    float remap;
    float texel_x;
    float texel_y;
    float sigma;
    uint sample_count;
    float composite_scale;
    float gamma_value;
    float saturation;
    float curve_scale;
    float curve_shoulder;
    uint srgb_destination;
    float dither_scale;
};

cbuffer BloomKernel : register(b1) {
    float4 kernel_offsets[4];
    float4 kernel_weights[4];
};

static const float dither_rgba8[16] = {
    7.0F, 135.0F, 39.0F, 167.0F, 199.0F, 71.0F, 231.0F, 103.0F,
    55.0F, 183.0F, 23.0F, 151.0F, 247.0F, 119.0F, 215.0F, 87.0F};

float decode_srgb_component(float encoded) {
    return encoded <= 0.04045F ? encoded / 12.92F :
                                 pow((encoded + 0.055F) / 1.055F, 2.4F);
}

float3 decode_srgb(float3 encoded) {
    return float3(decode_srgb_component(encoded.r),
                  decode_srgb_component(encoded.g),
                  decode_srgb_component(encoded.b));
}

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertex_id << 1U) & 2U, vertex_id & 2U);
    output.position = float4(output.uv * float2(2.0F, -2.0F) +
                             float2(-1.0F, 1.0F), 0.0F, 1.0F);
    return output;
}

float3 bright_pass(float3 rgb) {
    return min(max(rgb * exposure - threshold.xxx, 0.0F) * remap.xxx,
               64000.0F.xxx);
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    if (mode == 0U) {
        return float4(bright_pass(source_texture.SampleLevel(
                                      linear_clamp, input.uv, 0.0F).rgb), 1.0F);
    }
    if (mode == 1U) {
        float2 offset = float2(texel_x, texel_y) * 0.5F;
        float3 value = source_texture.SampleLevel(linear_clamp,
                                                  input.uv + float2(-offset.x, -offset.y), 0.0F).rgb;
        value += source_texture.SampleLevel(linear_clamp,
                                            input.uv + float2(offset.x, -offset.y), 0.0F).rgb;
        value += source_texture.SampleLevel(linear_clamp,
                                            input.uv + float2(-offset.x, offset.y), 0.0F).rgb;
        value += source_texture.SampleLevel(linear_clamp,
                                            input.uv + offset, 0.0F).rgb;
        return float4(value * 0.25F, 1.0F);
    }
    if (mode == 2U) {
        float3 value = 0.0F.xxx;
        for (uint tap = 0U; tap < 15U; ++tap) {
            if (tap >= sample_count) continue;
            uint vector_index = tap >> 2U;
            uint component_index = tap & 3U;
            float distance = kernel_offsets[vector_index][component_index];
            float weight = kernel_weights[vector_index][component_index];
            value += source_texture.SampleLevel(
                         linear_clamp, input.uv + float2(texel_x, texel_y) * distance,
                         0.0F).rgb * weight;
        }
        return float4(value, 1.0F);
    }
    if (mode == 3U) {
        return float4(min(source_texture.SampleLevel(
                              linear_clamp, input.uv, 0.0F).rgb * composite_scale,
                          64000.0F.xxx), 1.0F);
    }
    if (mode == 5U) {
        float4 source = source_texture.SampleLevel(linear_clamp, input.uv, 0.0F);
        float3 rgb = max(source.rgb, 0.0F);
        float luminance = dot(rgb, float3(0.2126F, 0.7152F, 0.0722F));
        float3 value = max(1.0F / 16384.0F,
                           lerp(luminance.xxx, rgb, saturation) * exposure);
        float3 decay = exp(-value * curve_scale);
        float3 shoulder = 1.0F - decay * curve_shoulder;
        float3 curve = saturate((1.0F - decay) * shoulder * shoulder);
        float3 mapped = curve;
        float3 glare = saturate(bloom_texture.SampleLevel(
            linear_clamp, input.uv, 0.0F).rgb);
        float3 encoded = pow(min(1.0F, mapped + (1.0F - mapped) * glare +
                                      1.0F / 4194304.0F),
                             1.0F / gamma_value);
        uint2 pixel = uint2(input.position.xy);
        uint dither_index = (pixel.y & 3U) * 4U + (pixel.x & 3U);
        encoded += (dither_rgba8[dither_index] / 255.0F - 0.5F) * dither_scale;
        return float4(srgb_destination != 0U ? decode_srgb(encoded) : encoded,
                      source.a);
    }
    float3 glare = saturate(source_texture.SampleLevel(
                                linear_clamp, input.uv, 0.0F).rgb);
    return float4(glare, 1.0F);
}
)HLSL";

using D3DCompileProc = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR,
                                          const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT,
                                          UINT, ID3DBlob **, ID3DBlob **);

[[nodiscard]] bool compile_d3d12_bloom_shader(
    LPCSTR entry, LPCSTR target, ComPtr<ID3DBlob>& bytecode,
    Diagnostic& diagnostic) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr) {
        diagnostic = {"d3d12_hdr_bloom_shader_unavailable",
                      "D3D12 HDR bloom requires d3dcompiler_47.dll"};
        return false;
    }
    const auto compile = reinterpret_cast<D3DCompileProc>(
        GetProcAddress(compiler, "D3DCompile"));
    if (compile == nullptr) {
        FreeLibrary(compiler);
        diagnostic = {"d3d12_hdr_bloom_shader_unavailable",
                      "D3D12 HDR bloom could not locate D3DCompile"};
        return false;
    }
    ComPtr<ID3DBlob> errors;
    const HRESULT result = compile(
        kD3d12BloomShader, sizeof(kD3d12BloomShader) - 1U,
        "apex_d3d12_hdr_bloom.hlsl", nullptr, nullptr, entry, target, 0U,
        0U, &bytecode, &errors);
    if (FAILED(result)) {
        std::string message = "D3D12 HDR bloom shader compilation failed";
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
            message += ": " + std::string(
                static_cast<const char *>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        FreeLibrary(compiler);
        diagnostic = {"d3d12_hdr_bloom_shader_failed", std::move(message)};
        return false;
    }
    FreeLibrary(compiler);
    return true;
}

[[nodiscard]] bool prepare_d3d12_hdr_bloom_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT target_format,
    bool additive, bool screen, bool tone_map, ID3D12PipelineState *&pipeline,
    Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context == nullptr || context->device == nullptr ||
        target_format == DXGI_FORMAT_UNKNOWN) {
        diagnostic = {"d3d12_hdr_bloom_pipeline_failed",
                      "D3D12 HDR bloom received an invalid device or format"};
        return false;
    }
    if (context->hdr_bloom_root_signature == nullptr) {
        if (!compile_d3d12_bloom_shader("vertex_main", "vs_5_0",
                                        context->hdr_bloom_vertex_shader,
                                        diagnostic) ||
            !compile_d3d12_bloom_shader("pixel_main", "ps_5_0",
                                        context->hdr_bloom_pixel_shader,
                                        diagnostic))
            return false;
        D3D12_DESCRIPTOR_RANGE source_range{};
        source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        source_range.NumDescriptors = 2U;
        source_range.BaseShaderRegister = 0U;
        source_range.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER source_parameter{};
        source_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        source_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        source_parameter.DescriptorTable.pDescriptorRanges = &source_range;
        source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_PARAMETER constants_parameter{};
        constants_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants_parameter.Constants.ShaderRegister = 0U;
        constants_parameter.Constants.Num32BitValues = 16U;
        constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_PARAMETER kernel_parameter{};
        kernel_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        kernel_parameter.Descriptor.ShaderRegister = 1U;
        kernel_parameter.Descriptor.RegisterSpace = 0U;
        kernel_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1U;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0U;
        const std::array<D3D12_ROOT_PARAMETER, 3U> parameters = {
            source_parameter, constants_parameter, kernel_parameter};
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        root_description.NumStaticSamplers = 1U;
        root_description.pStaticSamplers = &sampler;
        root_description.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(HDR bloom)", result);
            diagnostic.code = "d3d12_hdr_bloom_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->hdr_bloom_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(HDR bloom)", result);
            diagnostic.code = "d3d12_hdr_bloom_pipeline_failed";
            return false;
        }
    }
    if (tone_map) {
        for (const auto& cached : context->hdr_bloom_tone_map_pipelines)
            if (cached.first == target_format) {
                pipeline = cached.second.Get();
                return true;
            }
    }
    if (!tone_map && !additive && !screen && context->hdr_bloom_generate_pipeline != nullptr) {
        pipeline = context->hdr_bloom_generate_pipeline.Get();
        return true;
    }
    if (!tone_map && additive && !screen && context->hdr_bloom_add_pipeline != nullptr) {
        pipeline = context->hdr_bloom_add_pipeline.Get();
        return true;
    }
    if (screen) {
        for (const auto& cached : context->hdr_bloom_screen_pipelines)
            if (cached.first == target_format) {
                pipeline = cached.second.Get();
                return true;
            }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->hdr_bloom_root_signature.Get();
    description.VS = {context->hdr_bloom_vertex_shader->GetBufferPointer(),
                      context->hdr_bloom_vertex_shader->GetBufferSize()};
    description.PS = {context->hdr_bloom_pixel_shader->GetBufferPointer(),
                      context->hdr_bloom_pixel_shader->GetBufferSize()};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = target_format;
    description.SampleDesc.Count = 1U;
    auto& blend = description.BlendState.RenderTarget[0U];
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.BlendEnable = screen || additive;
    if (screen) {
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    } else {
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ONE;
    }
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(HDR bloom)", result);
        diagnostic.code = "d3d12_hdr_bloom_pipeline_failed";
        return false;
    }
    pipeline = created.Get();
    if (tone_map)
        context->hdr_bloom_tone_map_pipelines.emplace_back(target_format,
                                                            std::move(created));
    else if (screen)
        context->hdr_bloom_screen_pipelines.emplace_back(target_format,
                                                          std::move(created));
    else if (!additive)
        context->hdr_bloom_generate_pipeline = std::move(created);
    else
        context->hdr_bloom_add_pipeline = std::move(created);
    return true;
}

struct D3D12BloomImage {
    ComPtr<ID3D12Resource> resource;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

[[nodiscard]] bool create_d3d12_bloom_image(
    const std::shared_ptr<D3D12Context>& context, std::uint32_t width,
    std::uint32_t height, D3D12BloomImage& image, Diagnostic& diagnostic) {
    if (width == 0U || height == 0U ||
        width > max_texture_dimension || height > max_texture_dimension) {
        diagnostic = {"d3d12_hdr_bloom_dimensions_invalid",
                      "D3D12 HDR bloom dimensions are outside the bounded range"};
        return false;
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = description.Format;
    const HRESULT result = context->device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&image.resource));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommittedResource(HDR bloom)", result);
        diagnostic.code = "d3d12_hdr_bloom_allocation_failed";
        return false;
    }
    image.width = width;
    image.height = height;
    return true;
}

[[nodiscard]] bool execute_d3d12_hdr_bloom(
    const std::shared_ptr<D3D12Context>& context, ID3D12Resource* source,
    D3D12_RESOURCE_STATES& source_state, const TextureDescription& source_description,
    float exposure, const HdrBloomParameters& parameters,
    ComPtr<ID3D12Resource>& composite,
    Diagnostic& diagnostic) {
    if (context == nullptr || source == nullptr || context->device == nullptr ||
        context->queue == nullptr || source_description.width == 0U ||
        source_description.height == 0U) {
        diagnostic = {"d3d12_hdr_bloom_execution_failed",
                      "D3D12 HDR bloom received an invalid resource or context"};
        return false;
    }
    constexpr std::size_t kLevels = 5U;
    std::array<D3D12BloomImage, kLevels> horizontal{};
    std::array<D3D12BloomImage, kLevels> vertical{};
    if (!validate_hdr_bloom_resource_budget(
            source_description.width, source_description.height, 0U,
            diagnostic)) {
        diagnostic.message = "D3D12 " + diagnostic.message;
        return false;
    }
    const std::uint64_t composite_bytes =
        static_cast<std::uint64_t>(source_description.width) *
        source_description.height * 4ULL * sizeof(std::uint16_t);
    if (!validate_hdr_bloom_resource_budget(
            source_description.width, source_description.height,
            composite_bytes, diagnostic)) {
        diagnostic.message =
            "D3D12 " + diagnostic.message;
        return false;
    }
    std::uint32_t width = std::max(1U, (source_description.width + 3U) / 4U);
    std::uint32_t height = std::max(1U, (source_description.height + 3U) / 4U);
    for (std::size_t level = 0U; level < kLevels; ++level) {
        if (!create_d3d12_bloom_image(context, width, height, horizontal[level],
                                       diagnostic) ||
            !create_d3d12_bloom_image(context, width, height, vertical[level],
                                       diagnostic))
            return false;
        width = std::max(1U, (width + 1U) / 2U);
        height = std::max(1U, (height + 1U) / 2U);
    }
    D3D12_RESOURCE_DESC composite_description{};
    composite_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    composite_description.Width = source_description.width;
    composite_description.Height = source_description.height;
    composite_description.DepthOrArraySize = 1U;
    composite_description.MipLevels = 1U;
    composite_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    composite_description.SampleDesc.Count = 1U;
    composite_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    composite_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = composite_description.Format;
    HRESULT result = context->device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &composite_description,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&composite));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommittedResource(HDR bloom composite)", result);
        diagnostic.code = "d3d12_hdr_bloom_allocation_failed";
        return false;
    }

    std::lock_guard command_guard(context->command_mutex);
    ID3D12PipelineState* generate_pipeline = nullptr;
    if (!prepare_d3d12_hdr_bloom_pipeline(
            context, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, false,
            generate_pipeline, diagnostic))
        return false;
    ID3D12PipelineState* add_pipeline = nullptr;
    if (!prepare_d3d12_hdr_bloom_pipeline(
            context, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, false,
            add_pipeline, diagnostic))
        return false;

    constexpr UINT kGenerationOperations = 15U;
    constexpr UINT kCompositeOperations = 5U;
    constexpr UINT kSrvPerOperation = 2U;
    D3D12_DESCRIPTOR_HEAP_DESC srv_description{};
    srv_description.NumDescriptors =
        (kGenerationOperations + kCompositeOperations) * kSrvPerOperation;
    srv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    result = context->device->CreateDescriptorHeap(&srv_description,
                                                     IID_PPV_ARGS(&srv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(HDR bloom)", result);
        diagnostic.code = "d3d12_hdr_bloom_descriptor_failed";
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.NumDescriptors = kGenerationOperations + kCompositeOperations;
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(&rtv_description,
                                                    IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(HDR bloom RTV)", result);
        diagnostic.code = "d3d12_hdr_bloom_descriptor_failed";
        return false;
    }
    const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (srv_stride == 0U || rtv_stride == 0U) {
        diagnostic = {"d3d12_hdr_bloom_descriptor_failed",
                      "D3D12 returned a zero HDR bloom descriptor stride"};
        return false;
    }
    const auto srv_at = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * srv_stride;
        return handle;
    };
    const auto rtv_at = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
        return handle;
    };
    const auto make_srv = [&](ID3D12Resource* resource, UINT index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1U;
        context->device->CreateShaderResourceView(resource, &view,
                                                   srv_at(index * kSrvPerOperation));
        context->device->CreateShaderResourceView(resource, &view,
                                                   srv_at(index * kSrvPerOperation + 1U));
    };
    const auto make_rtv = [&](ID3D12Resource* resource, UINT index) {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        context->device->CreateRenderTargetView(resource, &view, rtv_at(index));
    };
    // Descriptor zero is the source for the bright pass. The remaining
    // generation descriptors follow the exact recording order: four
    // downsample operations, then two blur operations per level.
    make_srv(source, 0U);
    make_rtv(horizontal[0U].resource.Get(), 0U);
    UINT operation = 1U;
    for (std::size_t level = 1U; level < kLevels; ++level) {
        make_srv(horizontal[level - 1U].resource.Get(), operation);
        make_rtv(horizontal[level].resource.Get(), operation);
        ++operation;
    }
    for (std::size_t level = 0U; level < kLevels; ++level) {
        make_srv(horizontal[level].resource.Get(), operation);
        make_rtv(vertical[level].resource.Get(), operation);
        ++operation;
        make_srv(vertical[level].resource.Get(), operation);
        make_rtv(horizontal[level].resource.Get(), operation);
        ++operation;
    }
    operation = kGenerationOperations;
    for (std::size_t level = 0U; level < kLevels; ++level) {
        make_srv(horizontal[level].resource.Get(), operation);
        make_rtv(composite.Get(), operation);
        ++operation;
    }
    constexpr UINT kConstantBufferStride = 256U;
    constexpr UINT kConstantBufferCount = kGenerationOperations + kCompositeOperations;
    D3D12_HEAP_PROPERTIES constants_heap{};
    constants_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC constants_description{};
    constants_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    constants_description.Width = static_cast<UINT64>(kConstantBufferStride) *
                                  kConstantBufferCount;
    constants_description.Height = 1U;
    constants_description.DepthOrArraySize = 1U;
    constants_description.MipLevels = 1U;
    constants_description.SampleDesc.Count = 1U;
    constants_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> constants_upload;
    result = context->device->CreateCommittedResource(
        &constants_heap, D3D12_HEAP_FLAG_NONE, &constants_description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&constants_upload));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommittedResource(HDR bloom kernel)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    void* constants_mapping = nullptr;
    result = constants_upload->Map(0U, nullptr, &constants_mapping);
    if (FAILED(result) || constants_mapping == nullptr) {
        diagnostic = hresult_error("ID3D12Resource::Map(HDR bloom kernel)",
                                   FAILED(result) ? result : E_FAIL);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    std::memset(constants_mapping, 0U,
                static_cast<std::size_t>(constants_description.Width));
    const auto write_kernel = [&](UINT operation_index, std::size_t level) {
        const auto kernel = ksEditorBloomGaussianKernel(
            static_cast<std::int32_t>(level), parameters.kernel_threshold,
            parameters.radius_scale, parameters.source_level,
            parameters.display_scale);
        auto* bytes = static_cast<std::byte *>(constants_mapping) +
                      static_cast<std::size_t>(operation_index) *
                          kConstantBufferStride;
        std::memcpy(bytes, kernel.offsets.data(), 15U * sizeof(float));
        std::memcpy(bytes + 64U, kernel.weights.data(), 15U * sizeof(float));
    };
    for (std::size_t level = 0U; level < kLevels; ++level) {
        write_kernel(5U + static_cast<UINT>(level) * 2U, level);
        write_kernel(6U + static_cast<UINT>(level) * 2U, level);
    }
    constants_upload->Unmap(0U, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandAllocator(HDR bloom)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
        generate_pipeline, IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandList(HDR bloom)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    };
    transition(source, source_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1U, heaps);
    list->SetGraphicsRootSignature(context->hdr_bloom_root_signature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto set_constants = [&](std::uint32_t mode, float texel_x,
                                   float texel_y, float sigma,
                                   std::uint32_t sample_count,
                                   float composite_scale) {
        std::array<std::uint32_t, 16U> constants{};
        constants[0U] = mode;
        const auto set_float = [&](std::size_t index, float value) {
            std::memcpy(&constants[index], &value, sizeof(value));
        };
        set_float(1U, exposure);
        set_float(2U, parameters.threshold);
        set_float(3U, parameters.remap);
        set_float(4U, texel_x);
        set_float(5U, texel_y);
        set_float(6U, sigma);
        constants[7U] = sample_count;
        set_float(8U, composite_scale);
        list->SetGraphicsRoot32BitConstants(1U, 16U, constants.data(), 0U);
    };
    const auto draw = [&](ID3D12Resource* destination, UINT srv_index,
                          UINT rtv_index, std::uint32_t draw_width,
                          std::uint32_t draw_height,
                          UINT constants_index,
                          std::uint32_t mode, float texel_x, float texel_y,
                          float sigma, std::uint32_t sample_count,
                          float composite_scale) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_at(rtv_index);
        list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport = {0.0F, 0.0F,
                                         static_cast<float>(draw_width),
                                         static_cast<float>(draw_height), 0.0F,
                                         1.0F};
        const D3D12_RECT scissor = {0L, 0L, static_cast<LONG>(draw_width),
                                    static_cast<LONG>(draw_height)};
        list->RSSetViewports(1U, &viewport);
        list->RSSetScissorRects(1U, &scissor);
        D3D12_GPU_DESCRIPTOR_HANDLE srv =
            srv_heap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<UINT64>(srv_index * kSrvPerOperation) * srv_stride;
        list->SetGraphicsRootDescriptorTable(0U, srv);
        list->SetGraphicsRootConstantBufferView(
            2U, constants_upload->GetGPUVirtualAddress() +
                    static_cast<UINT64>(constants_index) * kConstantBufferStride);
        set_constants(mode, texel_x, texel_y, sigma, sample_count,
                      composite_scale);
        list->DrawInstanced(3U, 1U, 0U, 0U);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1U, &barrier);
    };
    // Bright pass.
    list->SetPipelineState(generate_pipeline);
    draw(horizontal[0U].resource.Get(), 0U, 0U, horizontal[0U].width,
         horizontal[0U].height, 0U, 0U, 0.0F, 0.0F, 1.0F, 1U, 0.0F);
    // Downsample and separable blur. The source and destination images are
    // single-mip resources, so each operation has an explicit state edge.
    for (std::size_t level = 1U; level < kLevels; ++level) {
        list->SetPipelineState(generate_pipeline);
        const UINT operation_index = static_cast<UINT>(level);
        draw(horizontal[level].resource.Get(), operation_index, operation_index,
             horizontal[level].width,
             horizontal[level].height, operation_index, 1U,
             1.0F / static_cast<float>(horizontal[level - 1U].width),
             1.0F / static_cast<float>(horizontal[level - 1U].height), 1.0F, 1U,
             0.0F);
    }
    list->SetPipelineState(generate_pipeline);
    for (std::size_t level = 0U; level < kLevels; ++level) {
        const auto kernel = ksEditorBloomGaussianKernel(
            static_cast<std::int32_t>(level), parameters.kernel_threshold,
            parameters.radius_scale, parameters.source_level,
            parameters.display_scale);
        const UINT horizontal_operation = 5U + static_cast<UINT>(level) * 2U;
        draw(vertical[level].resource.Get(), horizontal_operation,
             horizontal_operation, vertical[level].width, vertical[level].height,
             horizontal_operation, 2U,
             1.0F / static_cast<float>(horizontal[level].width), 0.0F,
             kernel.sigma, kernel.sample_count, 0.0F);
        draw(horizontal[level].resource.Get(), horizontal_operation + 1U,
             horizontal_operation + 1U, horizontal[level].width,
             horizontal[level].height, horizontal_operation + 1U, 2U, 0.0F,
             1.0F / static_cast<float>(vertical[level].height), kernel.sigma,
             kernel.sample_count, 0.0F);
    }
    list->SetPipelineState(add_pipeline);
    transition(composite.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE composite_rtv = rtv_at(kGenerationOperations);
    list->OMSetRenderTargets(1U, &composite_rtv, FALSE, nullptr);
    const float clear_color[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    list->ClearRenderTargetView(composite_rtv, clear_color, 0U, nullptr);
    transition(composite.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (std::size_t level = 0U; level < kLevels; ++level) {
        list->SetPipelineState(add_pipeline);
        draw(composite.Get(), kGenerationOperations + static_cast<UINT>(level),
             kGenerationOperations + static_cast<UINT>(level),
             source_description.width, source_description.height,
             kGenerationOperations + static_cast<UINT>(level), 3U,
             0.0F, 0.0F, 1.0F, 1U, parameters.composite_scale);
    }
    transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               source_state);
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12GraphicsCommandList::Close(HDR bloom)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&fence));
    HANDLE event = result == S_OK ? CreateEventW(nullptr, FALSE, FALSE, nullptr)
                                  : nullptr;
    if (event == nullptr) {
        diagnostic = {"d3d12_hdr_bloom_execution_failed",
                      "D3D12 HDR bloom could not create its completion event"};
        return false;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, lists);
    result = context->queue->Signal(fence.Get(), 1U);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < 1U)
        result = fence->SetEventOnCompletion(1U, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < 1U &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        source_state = drained ? source_state : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 HDR bloom fence", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    diagnostic = {};
    return true;
}

[[nodiscard]] bool execute_d3d12_hdr_bloom_tone_map(
    const std::shared_ptr<D3D12Context>& context, ID3D12Resource* source,
    D3D12_RESOURCE_STATES& source_state, ID3D12Resource* bloom,
    ID3D12Resource* destination, D3D12_RESOURCE_STATES& destination_state,
    const TextureDescription& destination_description,
    const HdrToneMapParameters& parameters, Diagnostic& diagnostic) {
    if (context == nullptr || source == nullptr || bloom == nullptr ||
        destination == nullptr || context->device == nullptr ||
        context->queue == nullptr) {
        diagnostic = {"d3d12_hdr_bloom_execution_failed",
                      "D3D12 HDR bloom tone mapping received an invalid resource or context"};
        return false;
    }
    std::lock_guard command_guard(context->command_mutex);
    ID3D12PipelineState* pipeline = nullptr;
    const DXGI_FORMAT destination_format =
        dxgi_texture_format(destination_description.format);
    if (!prepare_d3d12_hdr_bloom_pipeline(context, destination_format, false,
                                           false, true, pipeline, diagnostic))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC srv_description{};
    srv_description.NumDescriptors = 2U;
    srv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    HRESULT result = context->device->CreateDescriptorHeap(
        &srv_description, IID_PPV_ARGS(&srv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(HDR bloom tone map)", result);
        diagnostic.code = "d3d12_hdr_bloom_descriptor_failed";
        return false;
    }
    const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (srv_stride == 0U || rtv_stride == 0U) {
        diagnostic = {"d3d12_hdr_bloom_descriptor_failed",
                      "D3D12 returned a zero bloom tone-map descriptor stride"};
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC source_view{};
    source_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    source_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    source_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_view.Texture2D.MipLevels = 1U;
    const D3D12_CPU_DESCRIPTOR_HANDLE first_srv =
        srv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateShaderResourceView(source, &source_view, first_srv);
    D3D12_CPU_DESCRIPTOR_HANDLE second_srv = first_srv;
    second_srv.ptr += srv_stride;
    context->device->CreateShaderResourceView(bloom, &source_view, second_srv);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.NumDescriptors = 1U;
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(&rtv_description,
                                                    IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(HDR bloom tone map RTV)", result);
        diagnostic.code = "d3d12_hdr_bloom_descriptor_failed";
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC destination_view{};
    destination_view.Format = destination_format;
    destination_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE destination_rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateRenderTargetView(destination, &destination_view,
                                               destination_rtv);
    D3D12_HEAP_PROPERTIES constants_heap{};
    constants_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC constants_description{};
    constants_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    constants_description.Width = 256U;
    constants_description.Height = 1U;
    constants_description.DepthOrArraySize = 1U;
    constants_description.MipLevels = 1U;
    constants_description.SampleDesc.Count = 1U;
    constants_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> constants_upload;
    result = context->device->CreateCommittedResource(
        &constants_heap, D3D12_HEAP_FLAG_NONE, &constants_description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&constants_upload));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommittedResource(HDR bloom tone-map constants)",
            result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandAllocator(HDR bloom tone map)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline,
        IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandList(HDR bloom tone map)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    const auto transition = [&](ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES before,
                                D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    };
    const D3D12_RESOURCE_STATES shader_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES original_source_state = source_state;
    const D3D12_RESOURCE_STATES original_destination_state = destination_state;
    transition(source, original_source_state, shader_state);
    transition(bloom, shader_state, shader_state);
    transition(destination, original_destination_state,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1U, heaps);
    list->SetGraphicsRootSignature(context->hdr_bloom_root_signature.Get());
    list->SetPipelineState(pipeline);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1U, &destination_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {0.0F, 0.0F,
                                     static_cast<float>(destination_description.width),
                                     static_cast<float>(destination_description.height),
                                     0.0F, 1.0F};
    const D3D12_RECT scissor = {0L, 0L,
                                static_cast<LONG>(destination_description.width),
                                static_cast<LONG>(destination_description.height)};
    list->RSSetViewports(1U, &viewport);
    list->RSSetScissorRects(1U, &scissor);
    list->SetGraphicsRootDescriptorTable(
        0U, srv_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRootConstantBufferView(2U,
                                             constants_upload->GetGPUVirtualAddress());
    std::array<std::uint32_t, 16U> constants{};
    const auto set_float = [&](std::size_t index, float value) {
        std::memcpy(&constants[index], &value, sizeof(value));
    };
    constants[0U] = 5U;
    set_float(1U, parameters.exposure);
    set_float(2U, parameters.bloom.threshold);
    set_float(3U, parameters.bloom.remap);
    set_float(8U, parameters.bloom.composite_scale);
    set_float(9U, parameters.gamma);
    set_float(10U, parameters.saturation);
    set_float(11U, parameters.curve_scale);
    set_float(12U, parameters.curve_shoulder);
    constants[13U] = destination_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                             destination_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                         ? 1U
                         : 0U;
    set_float(14U, parameters.dither_scale);
    list->SetGraphicsRoot32BitConstants(1U, 16U, constants.data(), 0U);
    list->DrawInstanced(3U, 1U, 0U, 0U);
    transition(destination, D3D12_RESOURCE_STATE_RENDER_TARGET,
               original_destination_state);
    transition(source, shader_state, original_source_state);
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12GraphicsCommandList::Close(HDR bloom tone map)", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&fence));
    HANDLE event = result == S_OK ? CreateEventW(nullptr, FALSE, FALSE, nullptr)
                                  : nullptr;
    if (event == nullptr) {
        diagnostic = {"d3d12_hdr_bloom_execution_failed",
                      "D3D12 HDR bloom tone mapping could not create its event"};
        return false;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, lists);
    result = context->queue->Signal(fence.Get(), 1U);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < 1U)
        result = fence->SetEventOnCompletion(1U, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < 1U &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        source_state = drained ? original_source_state
                               : D3D12_RESOURCE_STATE_COMMON;
        destination_state = drained ? original_destination_state
                                    : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 HDR bloom tone-map fence", result);
        diagnostic.code = "d3d12_hdr_bloom_execution_failed";
        return false;
    }
    source_state = original_source_state;
    destination_state = original_destination_state;
    diagnostic = {};
    return true;
}

[[nodiscard]] bool prepare_d3d12_fxaa_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT format,
    ID3D12PipelineState*& pipeline, Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context == nullptr || context->device == nullptr ||
        format == DXGI_FORMAT_UNKNOWN) {
        diagnostic = {"d3d12_fxaa_pipeline_failed",
                      "D3D12 FXAA received an invalid device or format"};
        return false;
    }
    if (context->fxaa_root_signature == nullptr) {
        D3D12_DESCRIPTOR_RANGE source_range{};
        source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        source_range.NumDescriptors = 1U;
        source_range.BaseShaderRegister = 0U;
        source_range.RegisterSpace = 0U;
        source_range.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER source_parameter{};
        source_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        source_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        source_parameter.DescriptorTable.pDescriptorRanges = &source_range;
        source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_PARAMETER constants_parameter{};
        constants_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants_parameter.Constants.ShaderRegister = 0U;
        constants_parameter.Constants.RegisterSpace = 0U;
        constants_parameter.Constants.Num32BitValues = 3U;
        constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MipLODBias = 0.0F;
        sampler.MaxAnisotropy = 1U;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0F;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0U;
        sampler.RegisterSpace = 0U;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        const std::array<D3D12_ROOT_PARAMETER, 2U> parameters = {
            source_parameter, constants_parameter};
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters =
            static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        root_description.NumStaticSamplers = 1U;
        root_description.pStaticSamplers = &sampler;
        root_description.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(FXAA)", result);
            diagnostic.code = "d3d12_fxaa_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->fxaa_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(FXAA)", result);
            diagnostic.code = "d3d12_fxaa_pipeline_failed";
            return false;
        }
    }

    for (const auto& cached : context->fxaa_pipelines) {
        if (cached.first == format) {
            pipeline = cached.second.Get();
            return true;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->fxaa_root_signature.Get();
    description.VS = {generated::d3d12_fxaa_vertex_dxbc,
                      generated::d3d12_fxaa_vertex_dxbc_size};
    description.PS = {generated::d3d12_fxaa_pixel_dxbc,
                      generated::d3d12_fxaa_pixel_dxbc_size};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.RasterizerState.MultisampleEnable = FALSE;
    description.RasterizerState.AntialiasedLineEnable = FALSE;
    description.RasterizerState.ForcedSampleCount = 0U;
    description.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_RENDER_TARGET_BLEND_DESC& blend =
        description.BlendState.RenderTarget[0U];
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask =
        D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask =
        D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    description.DepthStencilState.BackFace =
        description.DepthStencilState.FrontFace;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = format;
    description.SampleDesc.Count = 1U;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(FXAA)", result);
        diagnostic.code = "d3d12_fxaa_pipeline_failed";
        return false;
    }
    pipeline = created.Get();
    context->fxaa_pipelines.emplace_back(format, std::move(created));
    return true;
}

[[nodiscard]] bool execute_d3d12_fxaa(
    const std::shared_ptr<D3D12Context>& context, ID3D12Resource* source,
    D3D12_RESOURCE_STATES& source_state, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES& destination_state,
    const TextureDescription& destination_description, Diagnostic& diagnostic) {
    if (context == nullptr || source == nullptr || destination == nullptr ||
        context->device == nullptr || context->queue == nullptr ||
        destination_description.width == 0U ||
        destination_description.height == 0U) {
        diagnostic = {"d3d12_fxaa_execution_failed",
                      "D3D12 FXAA received an invalid resource or context"};
        return false;
    }
    const D3D12_RESOURCE_STATES original_source_state = source_state;
    const D3D12_RESOURCE_STATES original_destination_state = destination_state;
    const DXGI_FORMAT destination_format =
        dxgi_texture_format(destination_description.format);
    ID3D12PipelineState* pipeline = nullptr;

    std::lock_guard command_guard(context->command_mutex);
    if (!prepare_d3d12_fxaa_pipeline(context, destination_format, pipeline,
                                      diagnostic))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
    srv_heap_description.NumDescriptors = 1U;
    srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    HRESULT result = context->device->CreateDescriptorHeap(
        &srv_heap_description, IID_PPV_ARGS(&srv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error("D3D12 FXAA SRV descriptor heap", result);
        diagnostic.code = "d3d12_fxaa_descriptor_failed";
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.NumDescriptors = 1U;
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(
        &rtv_heap_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error("D3D12 FXAA RTV descriptor heap", result);
        diagnostic.code = "d3d12_fxaa_descriptor_failed";
        return false;
    }
    const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (srv_stride == 0U || rtv_stride == 0U) {
        diagnostic = {"d3d12_fxaa_descriptor_failed",
                      "D3D12 returned a zero descriptor stride for FXAA"};
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC source_view{};
    source_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    source_view.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_view.Texture2D.MostDetailedMip = 0U;
    source_view.Texture2D.MipLevels = 1U;
    source_view.Texture2D.PlaneSlice = 0U;
    source_view.Texture2D.ResourceMinLODClamp = 0.0F;
    const D3D12_CPU_DESCRIPTOR_HANDLE source_descriptor =
        srv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateShaderResourceView(source, &source_view,
                                               source_descriptor);

    D3D12_RENDER_TARGET_VIEW_DESC destination_view{};
    destination_view.Format = destination_format;
    destination_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    destination_view.Texture2D.MipSlice = 0U;
    destination_view.Texture2D.PlaneSlice = 0U;
    const D3D12_CPU_DESCRIPTOR_HANDLE destination_descriptor =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateRenderTargetView(destination, &destination_view,
                                             destination_descriptor);

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("D3D12 FXAA command allocator", result);
        diagnostic.code = "d3d12_fxaa_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline,
        IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("D3D12 FXAA command list", result);
        diagnostic.code = "d3d12_fxaa_execution_failed";
        return false;
    }

    const auto transition = [&](ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES before,
                                D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    };
    constexpr D3D12_RESOURCE_STATES shader_source_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES render_target_state =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    transition(source, original_source_state, shader_source_state);
    transition(destination, original_destination_state, render_target_state);

    ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1U, descriptor_heaps);
    list->SetGraphicsRootSignature(context->fxaa_root_signature.Get());
    list->SetPipelineState(pipeline);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1U, &destination_descriptor, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {
        0.0F, 0.0F, static_cast<float>(destination_description.width),
        static_cast<float>(destination_description.height), 0.0F, 1.0F};
    const D3D12_RECT scissor = {
        0L, 0L, static_cast<LONG>(destination_description.width),
        static_cast<LONG>(destination_description.height)};
    list->RSSetViewports(1U, &viewport);
    list->RSSetScissorRects(1U, &scissor);
    list->SetGraphicsRootDescriptorTable(
        0U, srv_heap->GetGPUDescriptorHandleForHeapStart());
    std::array<std::uint32_t, 3U> constants{};
    const auto set_float = [](std::uint32_t& destination_value, float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::memcpy(&destination_value, &value, sizeof(value));
    };
    set_float(constants[0U], 1.0F /
                                static_cast<float>(destination_description.width));
    set_float(constants[1U], 1.0F /
                                static_cast<float>(destination_description.height));
    constants[2U] = destination_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                            destination_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                        ? 1U
                        : 0U;
    list->SetGraphicsRoot32BitConstants(
        1U, static_cast<UINT>(constants.size()), constants.data(), 0U);
    list->DrawInstanced(3U, 1U, 0U, 0U);
    transition(destination, render_target_state, original_destination_state);
    transition(source, shader_source_state, original_source_state);

    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12GraphicsCommandList::Close(FXAA)",
                                   result);
        diagnostic.code = "d3d12_fxaa_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("D3D12 FXAA fence", result);
        diagnostic.code = "d3d12_fxaa_execution_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        diagnostic = {"d3d12_fxaa_execution_failed",
                      "CreateEventW failed while waiting for FXAA"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    constexpr UINT64 fence_value = 1U;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
        result = fence->SetEventOnCompletion(fence_value, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        source_state = drained ? original_source_state
                               : D3D12_RESOURCE_STATE_COMMON;
        destination_state = drained ? original_destination_state
                                    : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 FXAA fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED ||
                                  result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed"
                              : "d3d12_fxaa_execution_failed";
        return false;
    }
    source_state = original_source_state;
    destination_state = original_destination_state;
    diagnostic = {};
    return true;
}

[[nodiscard]] bool prepare_d3d12_hdr_tone_map_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT format,
    ID3D12PipelineState*& pipeline, Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context == nullptr || context->device == nullptr ||
        format == DXGI_FORMAT_UNKNOWN) {
        diagnostic = {"d3d12_hdr_tone_map_pipeline_failed",
                      "D3D12 HDR tone mapping received an invalid device or format"};
        return false;
    }
    if (context->hdr_tone_map_root_signature == nullptr) {
        D3D12_DESCRIPTOR_RANGE source_range{};
        source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        source_range.NumDescriptors = 1U;
        source_range.BaseShaderRegister = 0U;
        source_range.RegisterSpace = 0U;
        source_range.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER source_parameter{};
        source_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        source_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        source_parameter.DescriptorTable.pDescriptorRanges = &source_range;
        source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_PARAMETER constants_parameter{};
        constants_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants_parameter.Constants.ShaderRegister = 0U;
        constants_parameter.Constants.RegisterSpace = 0U;
        constants_parameter.Constants.Num32BitValues = 7U;
        constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.0F;
        sampler.MaxAnisotropy = 1U;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0F;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0U;
        sampler.RegisterSpace = 0U;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        const std::array<D3D12_ROOT_PARAMETER, 2U> parameters = {
            source_parameter, constants_parameter};
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters =
            static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        root_description.NumStaticSamplers = 1U;
        root_description.pStaticSamplers = &sampler;
        root_description.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(HDR tone map)", result);
            diagnostic.code = "d3d12_hdr_tone_map_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->hdr_tone_map_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(HDR tone map)", result);
            diagnostic.code = "d3d12_hdr_tone_map_pipeline_failed";
            return false;
        }
    }

    for (const auto& cached : context->hdr_tone_map_pipelines) {
        if (cached.first == format) {
            pipeline = cached.second.Get();
            return true;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->hdr_tone_map_root_signature.Get();
    description.VS = {generated::d3d12_hdr_tone_map_vertex_dxbc,
                      generated::d3d12_hdr_tone_map_vertex_dxbc_size};
    description.PS = {generated::d3d12_hdr_tone_map_pixel_dxbc,
                      generated::d3d12_hdr_tone_map_pixel_dxbc_size};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.RasterizerState.MultisampleEnable = FALSE;
    description.RasterizerState.AntialiasedLineEnable = FALSE;
    description.RasterizerState.ForcedSampleCount = 0U;
    description.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_RENDER_TARGET_BLEND_DESC& blend =
        description.BlendState.RenderTarget[0U];
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask =
        D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask =
        D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    description.DepthStencilState.BackFace =
        description.DepthStencilState.FrontFace;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = format;
    description.SampleDesc.Count = 1U;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(HDR tone map)", result);
        diagnostic.code = "d3d12_hdr_tone_map_pipeline_failed";
        return false;
    }
    pipeline = created.Get();
    context->hdr_tone_map_pipelines.emplace_back(format, std::move(created));
    return true;
}

[[nodiscard]] bool execute_d3d12_hdr_tone_map(
    const std::shared_ptr<D3D12Context>& context, ID3D12Resource* source,
    D3D12_RESOURCE_STATES& source_state, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES& destination_state,
    const TextureDescription& destination_description,
    const HdrToneMapParameters& parameters, Diagnostic& diagnostic) {
    if (context == nullptr || source == nullptr || destination == nullptr ||
        context->device == nullptr || context->queue == nullptr) {
        diagnostic = {"d3d12_hdr_tone_map_execution_failed",
                      "D3D12 HDR tone mapping received an invalid resource or context"};
        return false;
    }
    const D3D12_RESOURCE_STATES original_source_state = source_state;
    const D3D12_RESOURCE_STATES original_destination_state = destination_state;
    const DXGI_FORMAT destination_format =
        dxgi_texture_format(destination_description.format);
    ID3D12PipelineState* pipeline = nullptr;

    std::lock_guard command_guard(context->command_mutex);
    if (!prepare_d3d12_hdr_tone_map_pipeline(context, destination_format,
                                              pipeline, diagnostic))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
    srv_heap_description.NumDescriptors = 1U;
    srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    HRESULT result = context->device->CreateDescriptorHeap(
        &srv_heap_description, IID_PPV_ARGS(&srv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(HDR tone map)", result);
        diagnostic.code = "d3d12_hdr_tone_map_descriptor_failed";
        return false;
    }
    const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_stride = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (srv_stride == 0U || rtv_stride == 0U) {
        diagnostic = {"d3d12_hdr_tone_map_descriptor_failed",
                      "D3D12 returned a zero descriptor stride for HDR tone mapping"};
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC source_view{};
    source_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    source_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    source_view.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    source_view.Texture2D.MostDetailedMip = 0U;
    source_view.Texture2D.MipLevels = 1U;
    source_view.Texture2D.PlaneSlice = 0U;
    source_view.Texture2D.ResourceMinLODClamp = 0.0F;
    const D3D12_CPU_DESCRIPTOR_HANDLE source_descriptor =
        srv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateShaderResourceView(source, &source_view,
                                               source_descriptor);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.NumDescriptors = 1U;
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(
        &rtv_heap_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateDescriptorHeap(HDR tone map RTV)", result);
        diagnostic.code = "d3d12_hdr_tone_map_descriptor_failed";
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC destination_view{};
    destination_view.Format = destination_format;
    destination_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    destination_view.Texture2D.MipSlice = 0U;
    destination_view.Texture2D.PlaneSlice = 0U;
    const D3D12_CPU_DESCRIPTOR_HANDLE destination_descriptor =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateRenderTargetView(destination, &destination_view,
                                             destination_descriptor);

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandAllocator(HDR tone map)", result);
        diagnostic.code = "d3d12_hdr_tone_map_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline,
        IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateCommandList(HDR tone map)", result);
        diagnostic.code = "d3d12_hdr_tone_map_execution_failed";
        return false;
    }

    const auto record_transition = [&](ID3D12Resource* resource,
                                       D3D12_RESOURCE_STATES before,
                                       D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    };
    constexpr D3D12_RESOURCE_STATES shader_source_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES render_target_state =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    record_transition(source, original_source_state, shader_source_state);
    record_transition(destination, original_destination_state,
                      render_target_state);

    ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1U, descriptor_heaps);
    list->SetGraphicsRootSignature(context->hdr_tone_map_root_signature.Get());
    list->SetPipelineState(pipeline);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1U, &destination_descriptor, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {
        0.0F, 0.0F, static_cast<float>(destination_description.width),
        static_cast<float>(destination_description.height), 0.0F, 1.0F};
    const D3D12_RECT scissor = {
        0L, 0L, static_cast<LONG>(destination_description.width),
        static_cast<LONG>(destination_description.height)};
    list->RSSetViewports(1U, &viewport);
    list->RSSetScissorRects(1U, &scissor);
    list->SetGraphicsRootDescriptorTable(
        0U, srv_heap->GetGPUDescriptorHandleForHeapStart());
    std::array<std::uint32_t, 7U> constants{};
    const auto set_float = [&](std::size_t index, float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::memcpy(&constants[index], &value, sizeof(value));
    };
    set_float(0U, parameters.exposure);
    set_float(1U, parameters.gamma);
    set_float(2U, parameters.saturation);
    set_float(3U, parameters.curve_scale);
    set_float(4U, parameters.curve_shoulder);
    constants[5U] = destination_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                            destination_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                        ? 1U
                        : 0U;
    set_float(6U, parameters.dither_scale);
    list->SetGraphicsRoot32BitConstants(
        1U, static_cast<UINT>(constants.size()), constants.data(), 0U);
    list->DrawInstanced(3U, 1U, 0U, 0U);
    record_transition(destination, render_target_state,
                      original_destination_state);
    record_transition(source, shader_source_state, original_source_state);

    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12GraphicsCommandList::Close(HDR tone map)", result);
        diagnostic.code = "d3d12_hdr_tone_map_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateFence(HDR tone map)", result);
        diagnostic.code = "d3d12_hdr_tone_map_execution_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        diagnostic = {"d3d12_hdr_tone_map_execution_failed",
                      "CreateEventW failed while waiting for HDR tone mapping"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    const bool submitted = true;
    constexpr UINT64 fence_value = 1U;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
        result = fence->SetEventOnCompletion(fence_value, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = submitted && context->wait_idle();
        source_state = drained ? original_source_state
                               : D3D12_RESOURCE_STATE_COMMON;
        destination_state = drained ? original_destination_state
                                    : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 HDR tone-map fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED ||
                                  result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed"
                              : "d3d12_hdr_tone_map_execution_failed";
        return false;
    }
    source_state = original_source_state;
    destination_state = original_destination_state;
    diagnostic = {};
    return true;
}

[[nodiscard]] bool validate_d3d12_sample_count(const std::shared_ptr<D3D12Context>& context,
                                               DXGI_FORMAT format,
                                               std::uint32_t samples,
                                               Diagnostic& diagnostic) {
    if (samples == 1U) return true;
    if (samples != 4U) {
        diagnostic = {"d3d12_multisample_count_unsupported",
                      "D3D12 multisample targets support only one or four samples"};
        return false;
    }
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
    quality.Format = format;
    quality.SampleCount = samples;
    quality.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    const HRESULT result = context->device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CheckFeatureSupport(MSAA)", result);
        diagnostic.code = "d3d12_multisample_query_failed";
        return false;
    }
    if (quality.NumQualityLevels == 0U) {
        diagnostic = {"d3d12_multisample_unsupported",
                      "The D3D12 adapter does not support the requested multisample format"};
        return false;
    }
    return true;
}

[[nodiscard]] D3D12_RESOURCE_FLAGS texture_flags(TextureUsage usage) noexcept {
    const auto raw = static_cast<std::uint32_t>(usage);
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::storage)) != 0U)
        flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(flags) |
                                                  static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    if ((raw & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(flags) |
                                                  static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    return flags;
}

[[nodiscard]] bool valid_d3d12_texture_usage(
    const TextureDescription& description, Diagnostic& diagnostic) noexcept {
    if (description.access_policy == TextureAccessPolicy::render_then_sample)
        return true;
    const auto raw = static_cast<std::uint32_t>(description.usage);
    const auto transfer_destination = static_cast<std::uint32_t>(TextureUsage::transfer_destination);
    const auto storage = static_cast<std::uint32_t>(TextureUsage::storage);
    const auto color_attachment = static_cast<std::uint32_t>(TextureUsage::color_attachment);
    if ((raw & transfer_destination) != 0U && raw != transfer_destination) {
        diagnostic = {"d3d12_transfer_destination_texture_exclusive",
                      "D3D12 COPY_DEST cannot be combined with another texture usage"};
        return false;
    }
    if ((raw & storage) != 0U && raw != storage) {
        diagnostic = {"d3d12_storage_texture_exclusive",
                      "D3D12 unordered-access texture state cannot be combined with another texture usage"};
        return false;
    }
    const auto allowed_with_color = color_attachment | static_cast<std::uint32_t>(TextureUsage::transfer_source);
    if ((raw & color_attachment) != 0U && (raw & ~allowed_with_color) != 0U) {
        diagnostic = {"d3d12_render_target_texture_exclusive",
                      "D3D12 render-target texture state cannot be combined with another texture usage"};
        return false;
    }
    return true;
}

bool execute_texture_upload(const std::shared_ptr<D3D12Context>& context,
                            ID3D12Resource* destination,
                            const TextureDescription& description,
                            const TextureUploadPlan& uploads,
                            D3D12_RESOURCE_STATES& destination_state,
                            D3D12_RESOURCE_STATES final_state,
                            Diagnostic& diagnostic) {
    const bool compressed = texture_format_is_compressed(description.format);
    if (compressed && !is_d3d12_supported_block_format(description.format)) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "D3D12 texture uploads support only BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 block-compressed formats"};
        return false;
    }
    if (!compressed && !texture_format_cpu_upload_supported(description.format)) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "Block-compressed texture uploads require a dedicated GPU upload path"};
        return false;
    }
    const auto bytes_per_pixel = texture_format_bytes_per_pixel(description.format);
    const TextureFormatInfo format_info = texture_format_info(description.format);
    if ((!compressed && bytes_per_pixel == 0U) ||
        (compressed && (format_info.block_width == 0U || format_info.block_height == 0U ||
                        format_info.block_bytes == 0U))) {
        diagnostic = {"texture_format_unknown", "Texture upload format has no byte layout"};
        return false;
    }
    const UINT subresource_count = static_cast<UINT>(
        static_cast<std::uint64_t>(description.mip_levels) *
        texture_physical_array_layers(description));
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
    std::vector<UINT> row_counts(subresource_count);
    std::vector<UINT64> row_sizes(subresource_count);
    UINT64 upload_size = 0;
    D3D12_RESOURCE_DESC resource_description = destination->GetDesc();
    context->device->GetCopyableFootprints(&resource_description, 0, subresource_count, 0,
                                           footprints.data(), row_counts.data(), row_sizes.data(), &upload_size);
    constexpr auto max_size_t = static_cast<UINT64>(std::numeric_limits<std::size_t>::max());
    if (upload_size > max_size_t) {
        diagnostic = {"texture_upload_size_platform_limit",
                      "D3D12 texture upload footprint exceeds the host size type"};
        return false;
    }
    for (const auto& footprint : footprints) {
        if (footprint.Offset > max_size_t || footprint.Footprint.RowPitch > max_size_t) {
            diagnostic = {"texture_upload_size_platform_limit",
                          "D3D12 texture footprint cannot be represented by the host size type"};
            return false;
        }
    }
    ComPtr<ID3D12Resource> upload_resource;
    if (!uploads.subresources.empty()) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC upload_description{};
        upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_description.Width = upload_size;
        upload_description.Height = 1;
        upload_description.DepthOrArraySize = 1;
        upload_description.MipLevels = 1;
        upload_description.SampleDesc.Count = 1;
        upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT result = context->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &upload_description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&upload_resource));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateCommittedResource(texture upload)", result);
            diagnostic.code = "texture_upload_failed";
            return false;
        }
        void* mapped = nullptr;
        result = upload_resource->Map(0, nullptr, &mapped);
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Resource::Map(texture upload)", result);
            diagnostic.code = "texture_upload_failed";
            return false;
        }
        for (const TextureUpload& upload : uploads.subresources) {
            const UINT subresource = static_cast<UINT>(
                texture_upload_physical_array_layer(description, upload) *
                    description.mip_levels +
                upload.mip_level);
            const auto& footprint = footprints[subresource];
            const std::size_t row_bytes = compressed
                                              ? ((static_cast<std::size_t>(upload.width) +
                                                  format_info.block_width - 1U) /
                                                 format_info.block_width) *
                                                    format_info.block_bytes
                                              : static_cast<std::size_t>(upload.width) * bytes_per_pixel;
            const std::size_t row_count = compressed
                                              ? (static_cast<std::size_t>(upload.height) +
                                                 format_info.block_height - 1U) /
                                                    format_info.block_height
                                              : upload.height;
            if (row_bytes > footprint.Footprint.RowPitch || row_count > row_counts[subresource] ||
                static_cast<std::uint64_t>(upload.row_pitch) * row_count > upload.data.size()) {
                diagnostic = {"texture_upload_footprint_invalid",
                              "D3D12 texture upload data does not fit its block or texel footprint"};
                upload_resource->Unmap(0, nullptr);
                return false;
            }
            auto* destination_bytes = static_cast<std::byte*>(mapped) + static_cast<std::size_t>(footprint.Offset);
            for (std::size_t row = 0; row < row_count; ++row) {
                std::memcpy(destination_bytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                            upload.data.data() + row * upload.row_pitch, row_bytes);
            }
        }
        upload_resource->Unmap(0, nullptr);
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT result = context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                               IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandAllocator(texture)", result);
        diagnostic.code = "texture_upload_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandList(texture)", result);
        diagnostic.code = "texture_upload_failed";
        return false;
    }
    if (destination_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = destination_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    if (upload_resource) {
        for (const TextureUpload& upload : uploads.subresources) {
            const UINT subresource = static_cast<UINT>(
                texture_upload_physical_array_layer(description, upload) *
                    description.mip_levels +
                upload.mip_level);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload_resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[subresource];
            D3D12_TEXTURE_COPY_LOCATION target{};
            target.pResource = destination;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = subresource;
            list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
    }
    if (final_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12GraphicsCommandList::Close(texture)", result);
        diagnostic.code = "texture_upload_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateFence(texture)", result);
        diagnostic.code = "texture_upload_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        diagnostic = {"texture_upload_failed", "CreateEventW failed while waiting for a D3D12 texture upload"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1, command_lists);
    constexpr UINT64 fence_value = 1;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (FAILED(result)) {
        context->wait_idle();
        CloseHandle(event);
        diagnostic = hresult_error("ID3D12CommandQueue::Signal(texture)", result);
        diagnostic.code = "texture_upload_failed";
        destination_state = final_state;
        return false;
    }
    if (fence->GetCompletedValue() < fence_value) {
        result = fence->SetEventOnCompletion(fence_value, event);
        if (FAILED(result)) {
            context->wait_idle();
            CloseHandle(event);
            diagnostic = hresult_error("ID3D12Fence::SetEventOnCompletion(texture)", result);
            diagnostic.code = "texture_upload_failed";
            destination_state = final_state;
            return false;
        }
        const DWORD wait_result = WaitForSingleObject(event, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            context->wait_idle();
            CloseHandle(event);
            diagnostic = {"texture_upload_failed", "Waiting for the D3D12 texture upload fence failed"};
            destination_state = final_state;
            return false;
        }
    }
    CloseHandle(event);
    destination_state = final_state;
    return true;
}

bool clear_texture_readback(const std::shared_ptr<D3D12Context>& context,
                            ID3D12Resource* destination,
                            const TextureDescription& description,
                            const TextureClearReadbackRequest& request,
                            D3D12_RESOURCE_STATES& destination_state,
                            std::vector<std::byte>& output,
                            Diagnostic& diagnostic) {
    const UINT subresource = request.array_layer * description.mip_levels + request.mip_level;
    const UINT64 row_bytes = static_cast<UINT64>(request.output_width) * 4U;
    D3D12_RESOURCE_DESC resource_description = destination->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 readback_size = 0;
    context->device->GetCopyableFootprints(&resource_description, subresource, 1, 0, &footprint,
                                           &row_count, &row_size, &readback_size);
    constexpr UINT64 max_size_t = static_cast<UINT64>(std::numeric_limits<std::size_t>::max());
    const UINT64 row_pitch = footprint.Footprint.RowPitch;
    bool footprint_valid = row_count >= request.output_height && row_size >= row_bytes &&
                           row_pitch >= row_bytes && footprint.Offset <= max_size_t &&
                           row_pitch <= max_size_t && readback_size <= max_texture_readback_bytes;
    if (footprint_valid) {
        const UINT64 rows_before_last = static_cast<UINT64>(request.output_height - 1U);
        if (rows_before_last > (std::numeric_limits<UINT64>::max() - footprint.Offset) / row_pitch) {
            footprint_valid = false;
        } else {
            const UINT64 last_row = footprint.Offset + rows_before_last * row_pitch;
            footprint_valid = row_bytes <= std::numeric_limits<UINT64>::max() - last_row &&
                              last_row + row_bytes <= readback_size;
        }
    }
    if (!footprint_valid) {
        diagnostic = {"texture_readback_footprint_invalid", "D3D12 readback footprint exceeds the bounded output"};
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_description{};
    buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_description.Width = readback_size;
    buffer_description.Height = 1;
    buffer_description.DepthOrArraySize = 1;
    buffer_description.MipLevels = 1;
    buffer_description.SampleDesc.Count = 1;
    buffer_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    HRESULT result = context->device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer_description, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommittedResource(readback)", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.NumDescriptors = 1;
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateDescriptorHeap(readback RTV)", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = dxgi_texture_format(description.format);
    if (description.array_layers == 1U) {
        rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv_description.Texture2D.MipSlice = request.mip_level;
        rtv_description.Texture2D.PlaneSlice = 0;
    } else {
        rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv_description.Texture2DArray.MipSlice = request.mip_level;
        rtv_description.Texture2DArray.FirstArraySlice = request.array_layer;
        rtv_description.Texture2DArray.ArraySize = 1;
        rtv_description.Texture2DArray.PlaneSlice = 0;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateRenderTargetView(destination, &rtv_description, rtv);

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandAllocator(readback)", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandList(readback)", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    if (destination_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = destination_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float clear_color[4] = {request.clear_color[0], request.clear_color[1],
                                  request.clear_color[2], request.clear_color[3]};
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = destination;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    if (texture_state(description.usage, description.access_policy) !=
        D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter =
            texture_state(description.usage, description.access_policy);
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12GraphicsCommandList::Close(readback)", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateFence(readback)", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        diagnostic = {"texture_readback_failed", "CreateEventW failed while waiting for a D3D12 readback"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1, command_lists);
    const D3D12_RESOURCE_STATES final_state =
        texture_state(description.usage, description.access_policy);
    bool submitted = true;
    constexpr UINT64 fence_value = 1;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
        result = fence->SetEventOnCompletion(fence_value, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value) {
        const DWORD wait_result = WaitForSingleObject(event, INFINITE);
        if (wait_result != WAIT_OBJECT_0) result = E_FAIL;
    }
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        destination_state = drained && submitted ? final_state : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 texture readback fence", result);
        diagnostic.code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                              ? "d3d12_device_removed"
                              : "texture_readback_failed";
        return false;
    }
    // The GPU has completed the transition before any host-visible work below.
    // Keep the tracked state correct even if Map or output allocation fails.
    destination_state = final_state;

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
    result = readback->Map(0, &read_range, &mapped);
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Resource::Map(readback)", result);
        diagnostic.code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                              ? "d3d12_device_removed"
                              : "texture_readback_failed";
        return false;
    }
    try {
        output.resize(static_cast<std::size_t>(request.output_width) * request.output_height * 4U);
        const auto* source_bytes = static_cast<const std::byte*>(mapped) + footprint.Offset;
        for (std::uint32_t row = 0; row < request.output_height; ++row)
            std::memcpy(output.data() + static_cast<std::size_t>(row) * request.output_width * 4U,
                        source_bytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                        static_cast<std::size_t>(row_bytes));
    } catch (const std::bad_alloc&) {
        result = E_OUTOFMEMORY;
    }
    readback->Unmap(0, nullptr);
    if (FAILED(result)) {
        output.clear();
        diagnostic = hresult_error("D3D12 texture readback allocation", result);
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    if (description.format == TextureFormat::bgra8_unorm || description.format == TextureFormat::bgra8_srgb) {
        for (std::size_t index = 0; index < output.size(); index += 4U)
            std::swap(output[index], output[index + 2U]);
    }
    return true;
}

[[nodiscard]] bool read_d3d12_hdr_luminance(
    const std::shared_ptr<D3D12Context>& context,
    ID3D12Resource* source,
    const TextureDescription& description,
    D3D12_RESOURCE_STATES& source_state,
    float& luminance,
    Diagnostic& diagnostic) {
    luminance = 0.0F;
    if (context == nullptr || context->device == nullptr || context->queue == nullptr ||
        source == nullptr) {
        diagnostic = {"d3d12_hdr_luminance_uninitialized",
                      "D3D12 HDR luminance measurement requires an initialized device and texture"};
        return false;
    }
    if (description.format != TextureFormat::rgba16_sfloat ||
        description.shape != TextureShape::texture_2d || description.samples != 1U ||
        description.array_layers != 1U || description.mip_levels == 0U) {
        diagnostic = {"hdr_luminance_request_invalid",
                      "HDR luminance measurement requires a single-sample, one-layer RGBA16F mip chain"};
        return false;
    }
    const std::uint32_t last_mip = description.mip_levels - 1U;
    const std::uint32_t last_width = std::max(1U, description.width >> last_mip);
    const std::uint32_t last_height = std::max(1U, description.height >> last_mip);
    if (last_width != 1U || last_height != 1U) {
        diagnostic = {"hdr_luminance_mip_chain_incomplete",
                      "HDR luminance measurement requires a final one-by-one mip level"};
        return false;
    }

    const UINT subresource = last_mip;
    const D3D12_RESOURCE_DESC resource_description = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0U;
    UINT64 row_size = 0U;
    UINT64 readback_size = 0U;
    context->device->GetCopyableFootprints(
        &resource_description, subresource, 1U, 0U, &footprint, &row_count,
        &row_size, &readback_size);
    constexpr UINT64 row_bytes = 4U * sizeof(std::uint16_t);
    constexpr UINT64 max_size_t = static_cast<UINT64>(std::numeric_limits<std::size_t>::max());
    if (row_count == 0U || row_size < row_bytes || footprint.Footprint.RowPitch < row_bytes ||
        footprint.Offset > max_size_t || readback_size == 0U ||
        readback_size > max_size_t ||
        readback_size > max_texture_readback_bytes) {
        diagnostic = {"hdr_luminance_readback_footprint_invalid",
                      "D3D12 HDR luminance readback footprint exceeds the bounded output"};
        return false;
    }
    if (footprint.Offset > readback_size ||
        row_bytes > readback_size - footprint.Offset) {
        diagnostic = {"hdr_luminance_readback_footprint_invalid",
                      "D3D12 HDR luminance readback row exceeds the bounded footprint"};
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_description{};
    buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_description.Width = readback_size;
    buffer_description.Height = 1U;
    buffer_description.DepthOrArraySize = 1U;
    buffer_description.MipLevels = 1U;
    buffer_description.SampleDesc.Count = 1U;
    buffer_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    HRESULT result = context->device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer_description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommittedResource(HDR luminance readback)", result);
        diagnostic.code = "d3d12_hdr_luminance_readback_failed";
        return false;
    }

    std::lock_guard command_guard(context->command_mutex);
    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandAllocator(HDR luminance readback)", result);
        diagnostic.code = "d3d12_hdr_luminance_readback_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateCommandList(HDR luminance readback)", result);
        diagnostic.code = "d3d12_hdr_luminance_readback_failed";
        return false;
    }
    const D3D12_RESOURCE_STATES restore_state = source_state;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.StateBefore = source_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // The D3D12 texture wrapper tracks one state for the whole image, so
        // transition all mips before copying the terminal subresource.
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = source;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION target_location{};
    target_location.pResource = readback.Get();
    target_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target_location.PlacedFootprint = footprint;
    list->CopyTextureRegion(&target_location, 0U, 0U, 0U, &source_location, nullptr);
    if (restore_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = restore_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12GraphicsCommandList::Close(HDR luminance readback)", result);
        diagnostic.code = "d3d12_hdr_luminance_readback_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                          IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CreateFence(HDR luminance readback)", result);
        diagnostic.code = "d3d12_hdr_luminance_readback_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        diagnostic = {"d3d12_hdr_luminance_readback_failed",
                      "CreateEventW failed while waiting for a D3D12 HDR luminance readback"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    constexpr UINT64 fence_value = 1U;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
        result = fence->SetEventOnCompletion(fence_value, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        source_state = drained ? restore_state : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 HDR luminance readback fence", result);
        diagnostic.code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                              ? "d3d12_device_removed"
                              : "d3d12_hdr_luminance_readback_failed";
        return false;
    }
    source_state = restore_state;

    void* mapped = nullptr;
    D3D12_RANGE read_range{footprint.Offset,
                           footprint.Offset + static_cast<SIZE_T>(row_bytes)};
    result = readback->Map(0U, &read_range, &mapped);
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Resource::Map(HDR luminance readback)", result);
        diagnostic.code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                              ? "d3d12_device_removed"
                              : "d3d12_hdr_luminance_readback_failed";
        return false;
    }
    const auto* bytes = static_cast<const std::byte*>(mapped) + footprint.Offset;
    std::uint16_t channels[3U]{};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
        std::memcpy(&channels[channel], bytes + channel * sizeof(std::uint16_t),
                    sizeof(std::uint16_t));
    readback->Unmap(0U, nullptr);
    const float red = detail::half_to_float(channels[0U]);
    const float green = detail::half_to_float(channels[1U]);
    const float blue = detail::half_to_float(channels[2U]);
    luminance = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
    if (!std::isfinite(luminance)) luminance = 0.0F;
    diagnostic = {};
    return true;
}

DXGI_FORMAT d3d12_pipeline_vertex_format(PipelineVertexAttributeFormat format) noexcept {
    switch (format) {
    case PipelineVertexAttributeFormat::float32: return DXGI_FORMAT_R32_FLOAT;
    case PipelineVertexAttributeFormat::float32x2: return DXGI_FORMAT_R32G32_FLOAT;
    case PipelineVertexAttributeFormat::float32x3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case PipelineVertexAttributeFormat::float32x4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case PipelineVertexAttributeFormat::uint32x4: return DXGI_FORMAT_R32G32B32A32_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

const char* d3d12_pipeline_semantic(PipelineVertexSemantic semantic) noexcept {
    switch (semantic) {
    case PipelineVertexSemantic::position: return "POSITION";
    case PipelineVertexSemantic::normal: return "NORMAL";
    case PipelineVertexSemantic::texcoord0: return "TEXCOORD";
    case PipelineVertexSemantic::texcoord1: return "TEXCOORD";
    case PipelineVertexSemantic::tangent: return "TANGENT";
    case PipelineVertexSemantic::color: return "COLOR";
    case PipelineVertexSemantic::bone_weights: return "BLENDWEIGHT";
    case PipelineVertexSemantic::bone_indices: return "BLENDINDICES";
    }
    return "POSITION";
}

[[nodiscard]] D3D12_COMPARISON_FUNC d3d12_pipeline_compare(PipelineCompareOperation compare) noexcept {
    switch (compare) {
    case PipelineCompareOperation::never: return D3D12_COMPARISON_FUNC_NEVER;
    case PipelineCompareOperation::less: return D3D12_COMPARISON_FUNC_LESS;
    case PipelineCompareOperation::equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case PipelineCompareOperation::less_or_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case PipelineCompareOperation::greater: return D3D12_COMPARISON_FUNC_GREATER;
    case PipelineCompareOperation::not_equal: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case PipelineCompareOperation::greater_or_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case PipelineCompareOperation::always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE d3d12_pipeline_topology_type(
    PipelineFillMode fill) noexcept {
    return fill == PipelineFillMode::wireframe
               ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
               : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY d3d12_pipeline_topology(
    PipelineFillMode fill) noexcept {
    return fill == PipelineFillMode::wireframe
               ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
               : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

[[nodiscard]] D3D12_BLEND d3d12_pipeline_blend_factor(PipelineBlendFactor factor) noexcept {
    switch (factor) {
    case PipelineBlendFactor::zero: return D3D12_BLEND_ZERO;
    case PipelineBlendFactor::one: return D3D12_BLEND_ONE;
    case PipelineBlendFactor::source_alpha: return D3D12_BLEND_SRC_ALPHA;
    case PipelineBlendFactor::one_minus_source_alpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case PipelineBlendFactor::destination_color: return D3D12_BLEND_DEST_COLOR;
    case PipelineBlendFactor::destination_alpha: return D3D12_BLEND_DEST_ALPHA;
    case PipelineBlendFactor::one_minus_destination_alpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ZERO;
}

[[nodiscard]] D3D12_BLEND_OP d3d12_pipeline_blend_operation(
    PipelineBlendOperation operation) noexcept {
    switch (operation) {
    case PipelineBlendOperation::add: return D3D12_BLEND_OP_ADD;
    case PipelineBlendOperation::subtract: return D3D12_BLEND_OP_SUBTRACT;
    case PipelineBlendOperation::reverse_subtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    }
    return D3D12_BLEND_OP_ADD;
}

void configure_d3d12_blend_state(const PipelineBlendState& source,
                                 D3D12_BLEND_DESC& destination) noexcept {
    destination.AlphaToCoverageEnable = source.alpha_to_coverage ? TRUE : FALSE;
    destination.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& target = destination.RenderTarget[0];
    target.BlendEnable = source.enabled ? TRUE : FALSE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = d3d12_pipeline_blend_factor(source.source_color);
    target.DestBlend = d3d12_pipeline_blend_factor(source.destination_color);
    target.BlendOp = d3d12_pipeline_blend_operation(source.color_operation);
    target.SrcBlendAlpha = d3d12_pipeline_blend_factor(source.source_alpha);
    target.DestBlendAlpha = d3d12_pipeline_blend_factor(source.destination_alpha);
    target.BlendOpAlpha = d3d12_pipeline_blend_operation(source.alpha_operation);
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
}

[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE d3d12_batch_sampler_address(
    SamplerAddressMode mode) noexcept {
    switch (mode) {
    case SamplerAddressMode::repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case SamplerAddressMode::mirrored_repeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case SamplerAddressMode::clamp_to_edge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case SamplerAddressMode::clamp_to_border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

[[nodiscard]] bool create_d3d12_batch_srv(ID3D12Device* device,
                                           ID3D12Resource* resource,
                                           DXGI_FORMAT format,
                                           D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept {
    if (device == nullptr || resource == nullptr) return false;
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

struct D3D12GeometryDescriptor {
    ID3D12Resource* vertex_resource = nullptr;
    UINT64 vertex_offset = 0U;
    UINT vertex_size = 0U;
    UINT vertex_stride = 0U;
    ID3D12Resource* index_resource = nullptr;
    UINT64 index_offset = 0U;
    UINT index_size = 0U;
    UINT vertex_count = 0U;
    UINT index_count = 0U;
    bool indexed = false;
};

struct D3D12IndexedBatchDraw {
    D3D12GeometryDescriptor geometry{};
    const PipelineProgram* pipeline = nullptr;
    const IndexedStaticMeshDrawRequest* scene_request = nullptr;
    // Native stock ksPerPixel draws use a different root signature and
    // descriptor ABI from the portable indexed path.  Keep the raw bridge
    // record alongside the common geometry so a hybrid batch can select the
    // correct pipeline at the original visitor position.
    bool native_stock_ks_per_pixel = false;
    D3D12StockKsPerPixelNativeBatchDraw native_stock_draw{};
    DrawMatrices matrices{};
    const D3D12Buffer* selected_color = nullptr;
    UINT64 selected_color_offset = 0U;
    bool alpha_tested = false;
    D3D12Texture* alpha_texture = nullptr;
    D3D12Sampler* alpha_sampler = nullptr;
    D3D12Buffer* alpha_material = nullptr;
    UINT64 alpha_material_offset = 0U;
    D3D12_GPU_DESCRIPTOR_HANDLE alpha_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE alpha_sampler_gpu{};
    bool transparent = false;
};

bool draw_graphics_and_readback(const std::shared_ptr<D3D12Context>& context,
                                ID3D12Resource* destination,
                                const TextureDescription& description,
                                const TriangleDrawRequest& request,
                                D3D12_RESOURCE_STATES& destination_state,
                                std::vector<std::byte>& output,
                                Diagnostic& diagnostic,
                                const D3D12GeometryDescriptor* geometry_override = nullptr,
                                const DrawMatrices* draw_matrices = nullptr,
                                D3D12DepthAttachment* depth_attachment = nullptr,
                                bool clear_depth = false,
                                float depth_clear_value = 1.0F,
                                bool load_color = false,
                                const IndexedStaticMeshDrawRequest* indexed_request = nullptr) {
    const auto& shaders = request.pipeline->shaders;
    const PipelineShaderModule* vertex_shader = nullptr;
    const PipelineShaderModule* fragment_shader = nullptr;
    for (const PipelineShaderModule& shader : shaders) {
        if (shader.format != PipelineShaderFormat::dxbc &&
            shader.format != PipelineShaderFormat::dxil) {
            diagnostic = {"triangle_shader_format_unsupported", "D3D12 triangle execution requires DXIL/DXBC shaders"};
            return false;
        }
        if (shader.stage == PipelineShaderStage::vertex) vertex_shader = &shader;
        if (shader.stage == PipelineShaderStage::fragment) fragment_shader = &shader;
    }
    if (vertex_shader == nullptr || fragment_shader == nullptr) {
        diagnostic = {"triangle_shader_pair_invalid", "D3D12 triangle execution requires vertex and fragment shaders"};
        return false;
    }
    const bool use_depth = depth_attachment != nullptr;
    const bool depth_test = use_depth && request.pipeline->depth.test_enabled;
    const bool depth_write = use_depth && request.pipeline->depth.write_enabled;
    if (request.pipeline->targets.colors.empty() ||
        request.pipeline->targets.colors.front().samples != description.samples) {
        diagnostic = {"d3d12_sample_count_mismatch",
                      "D3D12 color texture and pipeline sample counts must match"};
        return false;
    }
    if (!validate_d3d12_sample_count(context, dxgi_texture_format(description.format),
                                     description.samples, diagnostic))
        return false;
    if (use_depth && depth_attachment->info().description.samples != description.samples) {
        diagnostic = {"d3d12_depth_sample_count_mismatch",
                      "D3D12 color and depth attachment sample counts must match"};
        return false;
    }
    if (use_depth && !validate_d3d12_sample_count(context, DXGI_FORMAT_D32_FLOAT,
                                                   depth_attachment->info().description.samples, diagnostic))
        return false;
    const D3D12_RESOURCE_STATES depth_draw_state = depth_write ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                : D3D12_RESOURCE_STATE_DEPTH_READ;
    if (use_depth && !clear_depth && !depth_attachment->cleared()) {
        diagnostic = {"indexed_depth_load_before_clear",
                      "A D3D12 depth attachment must be cleared before it is loaded"};
        return false;
    }
    if (use_depth && (!std::isfinite(depth_clear_value) || depth_clear_value < 0.0F || depth_clear_value > 1.0F)) {
        diagnostic = {"d3d12_depth_clear_value_invalid", "D3D12 depth clear value must be finite and in [0, 1]"};
        return false;
    }
    D3D12GeometryDescriptor geometry;
    ComPtr<ID3D12Resource> vertices;
    HRESULT result = S_OK;
    if (geometry_override != nullptr) {
        geometry = *geometry_override;
    } else {
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC vertex_description{};
        vertex_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vertex_description.Width = request.vertex_data.size();
        vertex_description.Height = 1U;
        vertex_description.DepthOrArraySize = 1U;
        vertex_description.MipLevels = 1U;
        vertex_description.SampleDesc.Count = 1U;
        vertex_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = context->device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&vertices));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateCommittedResource(triangle vertices)", result);
            diagnostic.code = "triangle_execution_failed";
            return false;
        }
        void* mapped_vertices = nullptr;
        result = vertices->Map(0U, nullptr, &mapped_vertices);
        if (FAILED(result)) {
            diagnostic = hresult_error("Map(triangle vertices)", result);
            diagnostic.code = "triangle_execution_failed";
            return false;
        }
        std::memcpy(mapped_vertices, request.vertex_data.data(), request.vertex_data.size());
        vertices->Unmap(0U, nullptr);
        geometry.vertex_resource = vertices.Get();
        geometry.vertex_size = static_cast<UINT>(request.vertex_data.size());
        geometry.vertex_stride = request.pipeline->vertex_layout.stride;
    }

    D3D12MaterialDescriptorBinding material_binding;
    if (!request.pipeline->resources.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
        if (indexed_request == nullptr) {
            diagnostic = {"indexed_resource_request_missing",
                          "D3D12 resource-enabled indexed drawing requires its indexed request"};
            return false;
        }
        const bool has_material_constants = d3d12_pipeline_has_material_constants(*request.pipeline);
        const bool has_frame_constants = d3d12_pipeline_has_frame_constants(*request.pipeline);
        const bool has_normal_texture = d3d12_pipeline_has_normal_texture(*request.pipeline);
        const bool has_maps_texture = d3d12_pipeline_has_maps_texture(*request.pipeline);
        const bool has_detail_texture = d3d12_pipeline_has_detail_texture(*request.pipeline);
        const bool has_normal_detail_texture = d3d12_pipeline_has_normal_detail_texture(*request.pipeline);
        const bool has_damage_texture = d3d12_pipeline_has_damage_texture(*request.pipeline);
        const bool has_damage_mask_texture = d3d12_pipeline_has_damage_mask_texture(*request.pipeline);
        const bool has_shadow_receiver = d3d12_pipeline_has_directional_shadow_receiver(*request.pipeline);
        const bool has_multimap_reflection =
            d3d12_pipeline_has_multimap_reflection(*request.pipeline);
        srv_heap_description.NumDescriptors = 1U + static_cast<UINT>(has_material_constants) +
                                              static_cast<UINT>(has_frame_constants) +
                                              static_cast<UINT>(has_normal_texture) +
                                              static_cast<UINT>(has_maps_texture) +
                                              static_cast<UINT>(has_detail_texture) +
                                              static_cast<UINT>(has_normal_detail_texture) +
                                              static_cast<UINT>(has_damage_texture) +
                                              static_cast<UINT>(has_damage_mask_texture) +
                                              3U * static_cast<UINT>(has_shadow_receiver) +
                                              static_cast<UINT>(has_shadow_receiver) +
                                              2U * static_cast<UINT>(has_multimap_reflection);
        const UINT shadow_base = 1U + static_cast<UINT>(has_material_constants) +
                                 static_cast<UINT>(has_frame_constants) + static_cast<UINT>(has_normal_texture) +
                                 static_cast<UINT>(has_maps_texture) + static_cast<UINT>(has_detail_texture) +
                                 static_cast<UINT>(has_normal_detail_texture) + static_cast<UINT>(has_damage_texture) +
                                 static_cast<UINT>(has_damage_mask_texture);
        const UINT multimap_reflection_base =
            shadow_base + 4U * static_cast<UINT>(has_shadow_receiver);
        srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> srv_heap;
        result = context->device->CreateDescriptorHeap(&srv_heap_description, IID_PPV_ARGS(&srv_heap));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateDescriptorHeap(indexed resource SRV)", result);
            diagnostic.code = "indexed_resource_descriptor_heap_failed";
            return false;
        }
        if (!prepare_d3d12_material_binding(context, *indexed_request, srv_heap.Get(), 0U,
                                             has_material_constants ? 1U : 0U,
                                             has_frame_constants ? 1U + static_cast<UINT>(has_material_constants) : 0U,
                                             has_normal_texture
                                                 ? 1U + static_cast<UINT>(has_material_constants) +
                                                       static_cast<UINT>(has_frame_constants)
                                                 : 0U,
                                             has_maps_texture
                                                 ? 1U + static_cast<UINT>(has_material_constants) +
                                                       static_cast<UINT>(has_frame_constants) +
                                                       static_cast<UINT>(has_normal_texture)
                                                 : 0U,
                                             has_detail_texture
                                                 ? 1U + static_cast<UINT>(has_material_constants) +
                                                       static_cast<UINT>(has_frame_constants) +
                                                       static_cast<UINT>(has_normal_texture) +
                                                       static_cast<UINT>(has_maps_texture)
                                                 : 0U,
                                             has_normal_detail_texture
                                                 ? 1U + static_cast<UINT>(has_material_constants) +
                                                       static_cast<UINT>(has_frame_constants) +
                                                       static_cast<UINT>(has_normal_texture) +
                                                       static_cast<UINT>(has_maps_texture) +
                                                       static_cast<UINT>(has_detail_texture)
                                                 : 0U,
                                             has_damage_texture
                                                 ? 1U + static_cast<UINT>(has_material_constants) +
                                                       static_cast<UINT>(has_frame_constants) +
                                                       static_cast<UINT>(has_normal_texture) +
                                                       static_cast<UINT>(has_maps_texture) +
                                                       static_cast<UINT>(has_detail_texture) +
                                                       static_cast<UINT>(has_normal_detail_texture)
                                                 : 0U,
                                             has_damage_mask_texture
                                                 ? 1U + static_cast<UINT>(has_material_constants) +
                                                       static_cast<UINT>(has_frame_constants) +
                                                       static_cast<UINT>(has_normal_texture) +
                                                       static_cast<UINT>(has_maps_texture) +
                                                       static_cast<UINT>(has_detail_texture) +
                                                       static_cast<UINT>(has_normal_detail_texture) +
                                                       static_cast<UINT>(has_damage_texture)
                                                 : 0U,
                                             has_shadow_receiver ? shadow_base : 0U,
                                             has_shadow_receiver ? shadow_base + 3U : 0U,
                                             has_multimap_reflection
                                                 ? multimap_reflection_base
                                                 : 0U,
                                             has_multimap_reflection
                                                 ? multimap_reflection_base + 1U
                                                 : 0U,
                                             material_binding, diagnostic))
            return false;
    }

    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_ROOT_PARAMETER transform_parameter{};
    D3D12_DESCRIPTOR_RANGE srv_range{};
    D3D12_DESCRIPTOR_RANGE sampler_range{};
    D3D12_ROOT_PARAMETER srv_parameter{};
    D3D12_ROOT_PARAMETER sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE cbv_range{};
    D3D12_ROOT_PARAMETER cbv_parameter{};
    D3D12_DESCRIPTOR_RANGE frame_cbv_range{};
    D3D12_ROOT_PARAMETER frame_cbv_parameter{};
    D3D12_DESCRIPTOR_RANGE normal_srv_range{};
    D3D12_ROOT_PARAMETER normal_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE normal_sampler_range{};
    D3D12_ROOT_PARAMETER normal_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE maps_srv_range{};
    D3D12_ROOT_PARAMETER maps_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE maps_sampler_range{};
    D3D12_ROOT_PARAMETER maps_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE detail_srv_range{};
    D3D12_ROOT_PARAMETER detail_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE detail_sampler_range{};
    D3D12_ROOT_PARAMETER detail_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE normal_detail_srv_range{};
    D3D12_ROOT_PARAMETER normal_detail_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE normal_detail_sampler_range{};
    D3D12_ROOT_PARAMETER normal_detail_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE damage_srv_range{};
    D3D12_ROOT_PARAMETER damage_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE damage_sampler_range{};
    D3D12_ROOT_PARAMETER damage_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE damage_mask_srv_range{};
    D3D12_ROOT_PARAMETER damage_mask_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE damage_mask_sampler_range{};
    D3D12_ROOT_PARAMETER damage_mask_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE shadow_srv_range{};
    D3D12_ROOT_PARAMETER shadow_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE shadow_sampler_range{};
    D3D12_ROOT_PARAMETER shadow_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE shadow_cbv_range{};
    D3D12_ROOT_PARAMETER shadow_cbv_parameter{};
    D3D12_DESCRIPTOR_RANGE multimap_cube_srv_range{};
    D3D12_ROOT_PARAMETER multimap_cube_srv_parameter{};
    D3D12_DESCRIPTOR_RANGE multimap_cube_sampler_range{};
    D3D12_ROOT_PARAMETER multimap_cube_sampler_parameter{};
    D3D12_DESCRIPTOR_RANGE multimap_reflection_cbv_range{};
    D3D12_ROOT_PARAMETER multimap_reflection_cbv_parameter{};
    std::array<D3D12_ROOT_PARAMETER, 23> root_parameters{};
    UINT root_parameter_count = 0U;
    UINT shadow_srv_root_index = std::numeric_limits<UINT>::max();
    UINT shadow_sampler_root_index = std::numeric_limits<UINT>::max();
    UINT shadow_cbv_root_index = std::numeric_limits<UINT>::max();
    UINT multimap_cube_srv_root_index = std::numeric_limits<UINT>::max();
    UINT multimap_cube_sampler_root_index = std::numeric_limits<UINT>::max();
    UINT multimap_reflection_cbv_root_index =
        std::numeric_limits<UINT>::max();
    if (draw_matrices != nullptr) {
        transform_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transform_parameter.Constants.ShaderRegister = 0U;
        transform_parameter.Constants.RegisterSpace = 0U;
        transform_parameter.Constants.Num32BitValues =
            static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t));
        transform_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        root_parameters[root_parameter_count++] = transform_parameter;
    }
    if (!request.pipeline->resources.empty()) {
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1U;
        srv_range.BaseShaderRegister = 0U;
        srv_range.RegisterSpace = 0U;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        srv_parameter.DescriptorTable.pDescriptorRanges = &srv_range;
        srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_parameters[root_parameter_count++] = srv_parameter;

        sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.NumDescriptors = 1U;
        sampler_range.BaseShaderRegister = 1U;
        sampler_range.RegisterSpace = 0U;
        sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        sampler_parameter.DescriptorTable.pDescriptorRanges = &sampler_range;
        sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_parameters[root_parameter_count++] = sampler_parameter;
        if (d3d12_pipeline_has_material_constants(*request.pipeline)) {
            cbv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            cbv_range.NumDescriptors = 1U;
            cbv_range.BaseShaderRegister = 2U;
            cbv_range.RegisterSpace = 0U;
            cbv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            cbv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            cbv_parameter.DescriptorTable.pDescriptorRanges = &cbv_range;
            cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = cbv_parameter;
        }
        if (d3d12_pipeline_has_frame_constants(*request.pipeline)) {
            frame_cbv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            frame_cbv_range.NumDescriptors = 1U;
            frame_cbv_range.BaseShaderRegister = 3U;
            frame_cbv_range.RegisterSpace = 0U;
            frame_cbv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            frame_cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            frame_cbv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            frame_cbv_parameter.DescriptorTable.pDescriptorRanges = &frame_cbv_range;
            frame_cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = frame_cbv_parameter;
        }
        if (d3d12_pipeline_has_normal_texture(*request.pipeline)) {
            normal_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            normal_srv_range.NumDescriptors = 1U;
            normal_srv_range.BaseShaderRegister = 4U;
            normal_srv_range.RegisterSpace = 0U;
            normal_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            normal_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            normal_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            normal_srv_parameter.DescriptorTable.pDescriptorRanges = &normal_srv_range;
            normal_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = normal_srv_parameter;

            normal_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            normal_sampler_range.NumDescriptors = 1U;
            normal_sampler_range.BaseShaderRegister = 5U;
            normal_sampler_range.RegisterSpace = 0U;
            normal_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            normal_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            normal_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            normal_sampler_parameter.DescriptorTable.pDescriptorRanges = &normal_sampler_range;
            normal_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = normal_sampler_parameter;
        }
        if (d3d12_pipeline_has_maps_texture(*request.pipeline)) {
            maps_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            maps_srv_range.NumDescriptors = 1U;
            maps_srv_range.BaseShaderRegister = 6U;
            maps_srv_range.RegisterSpace = 0U;
            maps_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            maps_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            maps_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            maps_srv_parameter.DescriptorTable.pDescriptorRanges = &maps_srv_range;
            maps_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = maps_srv_parameter;

            maps_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            maps_sampler_range.NumDescriptors = 1U;
            maps_sampler_range.BaseShaderRegister = 7U;
            maps_sampler_range.RegisterSpace = 0U;
            maps_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            maps_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            maps_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            maps_sampler_parameter.DescriptorTable.pDescriptorRanges = &maps_sampler_range;
            maps_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = maps_sampler_parameter;
        }
        if (d3d12_pipeline_has_detail_texture(*request.pipeline)) {
            detail_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            detail_srv_range.NumDescriptors = 1U;
            detail_srv_range.BaseShaderRegister = 8U;
            detail_srv_range.RegisterSpace = 0U;
            detail_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            detail_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            detail_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            detail_srv_parameter.DescriptorTable.pDescriptorRanges = &detail_srv_range;
            detail_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = detail_srv_parameter;

            detail_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            detail_sampler_range.NumDescriptors = 1U;
            detail_sampler_range.BaseShaderRegister = 9U;
            detail_sampler_range.RegisterSpace = 0U;
            detail_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            detail_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            detail_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            detail_sampler_parameter.DescriptorTable.pDescriptorRanges = &detail_sampler_range;
            detail_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = detail_sampler_parameter;
        }
        if (d3d12_pipeline_has_normal_detail_texture(*request.pipeline)) {
            normal_detail_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            normal_detail_srv_range.NumDescriptors = 1U;
            normal_detail_srv_range.BaseShaderRegister = 10U;
            normal_detail_srv_range.RegisterSpace = 0U;
            normal_detail_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            normal_detail_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            normal_detail_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            normal_detail_srv_parameter.DescriptorTable.pDescriptorRanges = &normal_detail_srv_range;
            normal_detail_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = normal_detail_srv_parameter;

            normal_detail_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            normal_detail_sampler_range.NumDescriptors = 1U;
            normal_detail_sampler_range.BaseShaderRegister = 11U;
            normal_detail_sampler_range.RegisterSpace = 0U;
            normal_detail_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            normal_detail_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            normal_detail_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            normal_detail_sampler_parameter.DescriptorTable.pDescriptorRanges = &normal_detail_sampler_range;
            normal_detail_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = normal_detail_sampler_parameter;
        }
        if (d3d12_pipeline_has_damage_texture(*request.pipeline)) {
            damage_srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1U, 12U, 0U,
                                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            damage_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            damage_srv_parameter.DescriptorTable = {1U, &damage_srv_range};
            damage_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = damage_srv_parameter;
            damage_sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U, 13U, 0U,
                                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            damage_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            damage_sampler_parameter.DescriptorTable = {1U, &damage_sampler_range};
            damage_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = damage_sampler_parameter;
        }
        if (d3d12_pipeline_has_damage_mask_texture(*request.pipeline)) {
            damage_mask_srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1U, 14U, 0U,
                                     D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            damage_mask_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            damage_mask_srv_parameter.DescriptorTable = {1U, &damage_mask_srv_range};
            damage_mask_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = damage_mask_srv_parameter;
            damage_mask_sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U, 15U, 0U,
                                         D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            damage_mask_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            damage_mask_sampler_parameter.DescriptorTable = {1U, &damage_mask_sampler_range};
            damage_mask_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = damage_mask_sampler_parameter;
        }
        if (d3d12_pipeline_has_directional_shadow_receiver(*request.pipeline)) {
            shadow_srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3U, 16U, 0U,
                                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            shadow_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            shadow_srv_parameter.DescriptorTable = {1U, &shadow_srv_range};
            shadow_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            shadow_srv_root_index = root_parameter_count;
            root_parameters[root_parameter_count++] = shadow_srv_parameter;
            shadow_sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U, 19U, 0U,
                                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            shadow_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            shadow_sampler_parameter.DescriptorTable = {1U, &shadow_sampler_range};
            shadow_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            shadow_sampler_root_index = root_parameter_count;
            root_parameters[root_parameter_count++] = shadow_sampler_parameter;
            shadow_cbv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1U, 20U, 0U,
                                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            shadow_cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            shadow_cbv_parameter.DescriptorTable = {1U, &shadow_cbv_range};
            shadow_cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            shadow_cbv_root_index = root_parameter_count;
            root_parameters[root_parameter_count++] = shadow_cbv_parameter;
        }
        if (d3d12_pipeline_has_multimap_reflection(*request.pipeline)) {
            multimap_cube_srv_range = {
                D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1U,
                portable_multimap_cube_texture_binding, 0U,
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            multimap_cube_srv_parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            multimap_cube_srv_parameter.DescriptorTable = {
                1U, &multimap_cube_srv_range};
            multimap_cube_srv_parameter.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
            multimap_cube_srv_root_index = root_parameter_count;
            root_parameters[root_parameter_count++] =
                multimap_cube_srv_parameter;
            multimap_cube_sampler_range = {
                D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U,
                portable_multimap_cube_sampler_binding, 0U,
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            multimap_cube_sampler_parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            multimap_cube_sampler_parameter.DescriptorTable = {
                1U, &multimap_cube_sampler_range};
            multimap_cube_sampler_parameter.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
            multimap_cube_sampler_root_index = root_parameter_count;
            root_parameters[root_parameter_count++] =
                multimap_cube_sampler_parameter;
            multimap_reflection_cbv_range = {
                D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1U,
                portable_multimap_reflection_constants_binding, 0U,
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            multimap_reflection_cbv_parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            multimap_reflection_cbv_parameter.DescriptorTable = {
                1U, &multimap_reflection_cbv_range};
            multimap_reflection_cbv_parameter.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
            multimap_reflection_cbv_root_index = root_parameter_count;
            root_parameters[root_parameter_count++] =
                multimap_reflection_cbv_parameter;
        }
    }
    root_description.NumParameters = root_parameter_count;
    root_description.pParameters = root_parameters.data();
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    result = D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &root_blob, &root_error);
    if (FAILED(result)) {
        diagnostic = hresult_error("D3D12SerializeRootSignature(triangle)", result);
        diagnostic.code = "triangle_pipeline_failed";
        return false;
    }
    ComPtr<ID3D12RootSignature> root_signature;
    result = context->device->CreateRootSignature(0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root_signature));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateRootSignature(triangle)", result);
        diagnostic.code = "triangle_pipeline_failed";
        return false;
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
    input_elements.reserve(request.pipeline->vertex_layout.attributes.size());
    for (const PipelineVertexAttribute& attribute : request.pipeline->vertex_layout.attributes) {
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = d3d12_pipeline_semantic(attribute.semantic);
        element.SemanticIndex = attribute.semantic == PipelineVertexSemantic::texcoord1 ? 1U : 0U;
        element.Format = d3d12_pipeline_vertex_format(attribute.format);
        element.InputSlot = 0U;
        element.AlignedByteOffset = attribute.offset;
        element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        input_elements.push_back(element);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_description{};
    pipeline_description.pRootSignature = root_signature.Get();
    pipeline_description.VS = {vertex_shader->bytes.data(), vertex_shader->bytes.size()};
    pipeline_description.PS = {fragment_shader->bytes.data(), fragment_shader->bytes.size()};
    pipeline_description.InputLayout = {input_elements.data(), static_cast<UINT>(input_elements.size())};
    pipeline_description.PrimitiveTopologyType =
        d3d12_pipeline_topology_type(request.pipeline->raster.fill);
    pipeline_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline_description.RasterizerState.CullMode = request.pipeline->raster.cull == PipelineCullMode::none ? D3D12_CULL_MODE_NONE
                                             : request.pipeline->raster.cull == PipelineCullMode::front ? D3D12_CULL_MODE_FRONT
                                                                                                         : D3D12_CULL_MODE_BACK;
    pipeline_description.RasterizerState.FrontCounterClockwise = request.pipeline->raster.front_face == PipelineFrontFace::counter_clockwise;
    pipeline_description.RasterizerState.DepthClipEnable = TRUE;
    configure_d3d12_blend_state(request.pipeline->blend, pipeline_description.BlendState);
    pipeline_description.DepthStencilState.DepthEnable = depth_test ? TRUE : FALSE;
    pipeline_description.DepthStencilState.DepthWriteMask = depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                         : D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline_description.DepthStencilState.DepthFunc =
        use_depth ? d3d12_pipeline_compare(request.pipeline->depth.compare) : D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline_description.DepthStencilState.StencilEnable = FALSE;
    pipeline_description.NumRenderTargets = 1U;
    pipeline_description.RTVFormats[0] = dxgi_texture_format(description.format);
    pipeline_description.DSVFormat = use_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    pipeline_description.SampleMask = std::numeric_limits<UINT>::max();
    pipeline_description.SampleDesc.Count = description.samples;
    pipeline_description.SampleDesc.Quality = 0U;
    ComPtr<ID3D12PipelineState> pipeline;
    result = context->device->CreateGraphicsPipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateGraphicsPipelineState(triangle)", result);
        diagnostic.code = "triangle_pipeline_failed";
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.NumDescriptors = 1U;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateDescriptorHeap(triangle RTV)", result);
        diagnostic.code = "triangle_pipeline_failed";
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = dxgi_texture_format(description.format);
    rtv_description.ViewDimension = description.samples > 1U ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                                               : D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_description.Texture2D.MipSlice = 0U;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateRenderTargetView(destination, &rtv_description, rtv);
    D3D12_RESOURCE_DESC resource_description = destination->GetDesc();
    ComPtr<ID3D12Resource> resolve_resource;
    ID3D12Resource* readback_source = destination;
    if (description.samples > 1U) {
        D3D12_RESOURCE_DESC resolve_description = resource_description;
        resolve_description.SampleDesc.Count = 1U;
        resolve_description.SampleDesc.Quality = 0U;
        D3D12_HEAP_PROPERTIES resolve_heap{};
        resolve_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        result = context->device->CreateCommittedResource(
            &resolve_heap, D3D12_HEAP_FLAG_NONE, &resolve_description,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolve_resource));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateCommittedResource(MSAA resolve)", result);
            diagnostic.code = "triangle_execution_failed";
            return false;
        }
        readback_source = resolve_resource.Get();
        resource_description = resolve_description;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0U;
    UINT64 row_size = 0U;
    UINT64 readback_size = 0U;
    context->device->GetCopyableFootprints(&resource_description, 0U, 1U, 0U, &footprint,
                                           &row_count, &row_size, &readback_size);
    constexpr UINT64 max_size_t = static_cast<UINT64>(std::numeric_limits<std::size_t>::max());
    const UINT64 row_bytes = static_cast<UINT64>(description.width) * 4U;
    if (description.height == 0U || row_bytes > max_size_t / description.height) {
        diagnostic = {"triangle_readback_size_invalid", "D3D12 triangle output size exceeds the platform limit"};
        return false;
    }
    const std::size_t output_size =
        static_cast<std::size_t>(row_bytes * static_cast<UINT64>(description.height));
    if (row_count < description.height || row_size < row_bytes || footprint.Footprint.RowPitch < row_bytes ||
        footprint.Offset > max_size_t || readback_size > max_texture_readback_bytes ||
        readback_size > max_size_t) {
        diagnostic = {"triangle_readback_footprint_invalid", "D3D12 triangle readback footprint is out of bounds"};
        return false;
    }
    const UINT64 last_row = footprint.Offset + static_cast<UINT64>(description.height - 1U) * footprint.Footprint.RowPitch;
    if (last_row < footprint.Offset || last_row > readback_size || row_bytes > readback_size - last_row) {
        diagnostic = {"triangle_readback_footprint_invalid", "D3D12 triangle readback rows exceed the bounded footprint"};
        return false;
    }
    D3D12_RESOURCE_DESC readback_description{};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = readback_size;
    readback_description.Height = 1U;
    readback_description.DepthOrArraySize = 1U;
    readback_description.MipLevels = 1U;
    readback_description.SampleDesc.Count = 1U;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    result = context->device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                        &readback_description, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommittedResource(triangle readback)", result);
        diagnostic.code = "triangle_execution_failed";
        return false;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandAllocator(triangle)", result);
        diagnostic.code = "triangle_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandList(triangle)", result);
        diagnostic.code = "triangle_execution_failed";
        return false;
    }
    std::array<D3D12_RESOURCE_STATES, indexed_directional_shadow_cascade_count> shadow_states{};
    if (material_binding.shadow_enabled) {
        for (std::size_t cascade = 0U; cascade < indexed_directional_shadow_cascade_count; ++cascade) {
            D3D12DepthAttachment* attachment = material_binding.shadow_attachments[cascade];
            shadow_states[cascade] = attachment->state();
            if (shadow_states[cascade] != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = attachment->resource();
                barrier.Transition.StateBefore = shadow_states[cascade];
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1U, &barrier);
            }
        }
    }
    if (destination_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = destination_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    if (use_depth) {
        D3D12_RESOURCE_STATES depth_state = depth_attachment->state();
        if (clear_depth && depth_state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depth_attachment->resource();
            barrier.Transition.StateBefore = depth_state;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &barrier);
            depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        if (!clear_depth && depth_state != depth_draw_state) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depth_attachment->resource();
            barrier.Transition.StateBefore = depth_state;
            barrier.Transition.StateAfter = depth_draw_state;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &barrier);
            depth_state = depth_draw_state;
        }
        if (clear_depth) {
            const D3D12_CPU_DESCRIPTOR_HANDLE clear_dsv = depth_attachment->dsv(true);
            list->ClearDepthStencilView(clear_dsv, D3D12_CLEAR_FLAG_DEPTH, depth_clear_value, 0U, 0U, nullptr);
            if (depth_state != depth_draw_state) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = depth_attachment->resource();
                barrier.Transition.StateBefore = depth_state;
                barrier.Transition.StateAfter = depth_draw_state;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1U, &barrier);
                depth_state = depth_draw_state;
            }
        }
    }
    const float clear_color[4] = {request.clear_color[0], request.clear_color[1], request.clear_color[2], request.clear_color[3]};
    D3D12_CPU_DESCRIPTOR_HANDLE depth_dsv{};
    if (use_depth) depth_dsv = depth_attachment->dsv(depth_write);
    list->OMSetRenderTargets(1U, &rtv, FALSE, use_depth ? &depth_dsv : nullptr);
    if (!load_color) list->ClearRenderTargetView(rtv, clear_color, 0U, nullptr);
    list->SetGraphicsRootSignature(root_signature.Get());
    if (material_binding.enabled) {
        ID3D12DescriptorHeap* descriptor_heaps[] = {material_binding.srv_heap.Get(), context->sampler_heap.Get()};
        list->SetDescriptorHeaps(2U, descriptor_heaps);
        const UINT resource_root_index = draw_matrices != nullptr ? 1U : 0U;
        list->SetGraphicsRootDescriptorTable(resource_root_index, material_binding.srv_gpu);
        list->SetGraphicsRootDescriptorTable(resource_root_index + 1U, material_binding.sampler_gpu);
        UINT constant_root_index = resource_root_index + 2U;
        if (material_binding.constant_enabled)
            list->SetGraphicsRootDescriptorTable(constant_root_index++, material_binding.cbv_gpu);
        if (material_binding.frame_enabled)
            list->SetGraphicsRootDescriptorTable(constant_root_index, material_binding.frame_cbv_gpu);
        if (material_binding.normal_enabled) {
            const UINT normal_root_index = constant_root_index +
                                           static_cast<UINT>(material_binding.frame_enabled);
            list->SetGraphicsRootDescriptorTable(normal_root_index, material_binding.normal_srv_gpu);
            list->SetGraphicsRootDescriptorTable(normal_root_index + 1U, material_binding.normal_sampler_gpu);
        }
        if (material_binding.maps_enabled) {
            const UINT maps_root_index = constant_root_index +
                                         static_cast<UINT>(material_binding.frame_enabled) +
                                         2U * static_cast<UINT>(material_binding.normal_enabled);
            list->SetGraphicsRootDescriptorTable(maps_root_index, material_binding.maps_srv_gpu);
            list->SetGraphicsRootDescriptorTable(maps_root_index + 1U, material_binding.maps_sampler_gpu);
        }
        if (material_binding.detail_enabled) {
            const UINT detail_root_index = constant_root_index +
                                           static_cast<UINT>(material_binding.frame_enabled) +
                                           2U * static_cast<UINT>(material_binding.normal_enabled) +
                                           2U * static_cast<UINT>(material_binding.maps_enabled);
            list->SetGraphicsRootDescriptorTable(detail_root_index, material_binding.detail_srv_gpu);
            list->SetGraphicsRootDescriptorTable(detail_root_index + 1U, material_binding.detail_sampler_gpu);
        }
        if (material_binding.normal_detail_enabled) {
            const UINT normal_detail_root_index = constant_root_index +
                                                  static_cast<UINT>(material_binding.frame_enabled) +
                                                  2U * static_cast<UINT>(material_binding.normal_enabled) +
                                                  2U * static_cast<UINT>(material_binding.maps_enabled) +
                                                  2U * static_cast<UINT>(material_binding.detail_enabled);
            list->SetGraphicsRootDescriptorTable(normal_detail_root_index,
                                                  material_binding.normal_detail_srv_gpu);
            list->SetGraphicsRootDescriptorTable(normal_detail_root_index + 1U,
                                                  material_binding.normal_detail_sampler_gpu);
        }
        if (material_binding.damage_enabled) {
            const UINT damage_root_index = constant_root_index +
                                           static_cast<UINT>(material_binding.frame_enabled) +
                                           2U * static_cast<UINT>(material_binding.normal_enabled) +
                                           2U * static_cast<UINT>(material_binding.maps_enabled) +
                                           2U * static_cast<UINT>(material_binding.detail_enabled) +
                                           2U * static_cast<UINT>(material_binding.normal_detail_enabled);
            list->SetGraphicsRootDescriptorTable(damage_root_index,
                                                  material_binding.damage_srv_gpu);
            list->SetGraphicsRootDescriptorTable(damage_root_index + 1U,
                                                  material_binding.damage_sampler_gpu);
        }
        if (material_binding.damage_mask_enabled) {
            const UINT damage_mask_root_index = constant_root_index +
                                                static_cast<UINT>(material_binding.frame_enabled) +
                                                2U * static_cast<UINT>(material_binding.normal_enabled) +
                                                2U * static_cast<UINT>(material_binding.maps_enabled) +
                                                2U * static_cast<UINT>(material_binding.detail_enabled) +
                                                2U * static_cast<UINT>(material_binding.normal_detail_enabled) +
                                                2U * static_cast<UINT>(material_binding.damage_enabled);
            list->SetGraphicsRootDescriptorTable(damage_mask_root_index,
                                                  material_binding.damage_mask_srv_gpu);
            list->SetGraphicsRootDescriptorTable(damage_mask_root_index + 1U,
                                                  material_binding.damage_mask_sampler_gpu);
        }
        if (material_binding.shadow_enabled &&
            shadow_srv_root_index != std::numeric_limits<UINT>::max() &&
            shadow_sampler_root_index != std::numeric_limits<UINT>::max() &&
            shadow_cbv_root_index != std::numeric_limits<UINT>::max()) {
            list->SetGraphicsRootDescriptorTable(shadow_srv_root_index, material_binding.shadow_srv_gpu);
            list->SetGraphicsRootDescriptorTable(shadow_sampler_root_index,
                                                  material_binding.shadow_sampler_gpu);
            list->SetGraphicsRootDescriptorTable(shadow_cbv_root_index, material_binding.shadow_cbv_gpu);
        }
        if (material_binding.multimap_reflection_enabled &&
            multimap_cube_srv_root_index !=
                std::numeric_limits<UINT>::max() &&
            multimap_cube_sampler_root_index !=
                std::numeric_limits<UINT>::max() &&
            multimap_reflection_cbv_root_index !=
                std::numeric_limits<UINT>::max()) {
            list->SetGraphicsRootDescriptorTable(
                multimap_cube_srv_root_index,
                material_binding.multimap_cube_srv_gpu);
            list->SetGraphicsRootDescriptorTable(
                multimap_cube_sampler_root_index,
                material_binding.multimap_cube_sampler_gpu);
            list->SetGraphicsRootDescriptorTable(
                multimap_reflection_cbv_root_index,
                material_binding.multimap_reflection_cbv_gpu);
        }
    }
    if (draw_matrices != nullptr) {
        list->SetGraphicsRoot32BitConstants(
            0U, static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t)),
            draw_matrices, 0U);
    }
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(d3d12_pipeline_topology(request.pipeline->raster.fill));
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    vertex_view.BufferLocation = geometry.vertex_resource->GetGPUVirtualAddress() + geometry.vertex_offset;
    vertex_view.SizeInBytes = geometry.vertex_size;
    vertex_view.StrideInBytes = geometry.vertex_stride;
    list->IASetVertexBuffers(0U, 1U, &vertex_view);
    D3D12_INDEX_BUFFER_VIEW index_view{};
    if (geometry.indexed) {
        index_view.BufferLocation = geometry.index_resource->GetGPUVirtualAddress() + geometry.index_offset;
        index_view.SizeInBytes = geometry.index_size;
        index_view.Format = DXGI_FORMAT_R16_UINT;
        list->IASetIndexBuffer(&index_view);
    }
    D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(description.width), static_cast<float>(description.height), 0.0F, 1.0F};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(description.width), static_cast<LONG>(description.height)};
    list->RSSetViewports(1U, &viewport);
    list->RSSetScissorRects(1U, &scissor);
    if (geometry.indexed) list->DrawIndexedInstanced(geometry.index_count, 1U, 0U, 0, 0U);
    else list->DrawInstanced(request.vertex_count, 1U, 0U, 0U);
    if (material_binding.shadow_enabled) {
        for (std::size_t cascade = 0U; cascade < indexed_directional_shadow_cascade_count; ++cascade) {
            if (shadow_states[cascade] != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = material_binding.shadow_attachments[cascade]->resource();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.StateAfter = shadow_states[cascade];
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1U, &barrier);
            }
        }
    }
    const D3D12_RESOURCE_STATES final_state =
        texture_state(description.usage, description.access_policy);
    if (description.samples > 1U) {
        D3D12_RESOURCE_BARRIER resolve_source{};
        resolve_source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resolve_source.Transition.pResource = destination;
        resolve_source.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        resolve_source.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        resolve_source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &resolve_source);
        list->ResolveSubresource(resolve_resource.Get(), 0U, destination, 0U,
                                 dxgi_texture_format(description.format));
        D3D12_RESOURCE_BARRIER resolve_copy{};
        resolve_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resolve_copy.Transition.pResource = resolve_resource.Get();
        resolve_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        resolve_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        resolve_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &resolve_copy);
    } else {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = destination;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &to_copy);
    }
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = readback_source;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0U;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    list->CopyTextureRegion(&target, 0U, 0U, 0U, &source, nullptr);
    if (description.samples > 1U) {
        D3D12_RESOURCE_BARRIER destination_final{};
        destination_final.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        destination_final.Transition.pResource = destination;
        destination_final.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        destination_final.Transition.StateAfter = final_state;
        destination_final.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &destination_final);
    } else if (final_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("Close(triangle)", result);
        diagnostic.code = "triangle_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateFence(triangle)", result);
        diagnostic.code = "triangle_execution_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        diagnostic = {"triangle_execution_failed", "CreateEventW failed for triangle execution"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    result = context->queue->Signal(fence.Get(), 1U);
    if (SUCCEEDED(result)) result = fence->SetEventOnCompletion(1U, event);
    if (SUCCEEDED(result) && WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        destination_state = drained ? final_state : D3D12_RESOURCE_STATE_COMMON;
        if (use_depth)
            depth_attachment->set_state(drained ? depth_draw_state : D3D12_RESOURCE_STATE_COMMON);
        if (material_binding.shadow_enabled) {
            for (std::size_t cascade = 0U; cascade < indexed_directional_shadow_cascade_count; ++cascade)
                material_binding.shadow_attachments[cascade]->set_state(
                    drained ? shadow_states[cascade] : D3D12_RESOURCE_STATE_COMMON);
        }
        diagnostic = hresult_error("D3D12 triangle fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed" : "triangle_execution_failed";
        return false;
    }
    destination_state = final_state;
    if (use_depth) {
        depth_attachment->set_state(depth_draw_state);
        if (clear_depth) depth_attachment->set_cleared();
    }
    if (material_binding.shadow_enabled) {
        for (std::size_t cascade = 0U; cascade < indexed_directional_shadow_cascade_count; ++cascade)
            material_binding.shadow_attachments[cascade]->set_state(shadow_states[cascade]);
    }
    void* mapped = nullptr;
    D3D12_RANGE read_range{0U, static_cast<SIZE_T>(readback_size)};
    result = readback->Map(0U, &read_range, &mapped);
    if (SUCCEEDED(result)) {
        try {
            output.resize(output_size);
            const auto* source_bytes = static_cast<const std::byte*>(mapped) + footprint.Offset;
            for (std::uint32_t row = 0; row < description.height; ++row)
                std::memcpy(output.data() + static_cast<std::size_t>(row) * description.width * 4U,
                            source_bytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                            static_cast<std::size_t>(row_bytes));
        } catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
        readback->Unmap(0U, nullptr);
    }
    if (FAILED(result)) {
        diagnostic = hresult_error("Map(triangle readback)", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed" : "triangle_execution_failed";
        output.clear();
        return false;
    }
    if (description.format == TextureFormat::bgra8_unorm || description.format == TextureFormat::bgra8_srgb)
        for (std::size_t index = 0; index < output.size(); index += 4U) std::swap(output[index], output[index + 2U]);
    return true;
}

[[nodiscard]] bool prepare_d3d12_portable_sky_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT target_format,
    UINT sample_count, UINT sample_quality,
    ID3D12PipelineState*& pipeline, Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context == nullptr || context->device == nullptr ||
        target_format == DXGI_FORMAT_UNKNOWN ||
        (sample_count != 1U && sample_count != 4U)) {
        diagnostic = {"d3d12_portable_sky_pipeline_failed",
                      "D3D12 portable sky received an invalid device, format, or sample count"};
        return false;
    }
    if (context->portable_sky_root_signature == nullptr) {
        D3D12_ROOT_PARAMETER constants{};
        constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.Constants.ShaderRegister = 0U;
        constants.Constants.RegisterSpace = 0U;
        constants.Constants.Num32BitValues =
            static_cast<UINT>(sizeof(PortableSkyShaderConstants) /
                              sizeof(std::uint32_t));
        constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = 1U;
        root_description.pParameters = &constants;
        root_description.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(portable sky)", result);
            diagnostic.code = "d3d12_portable_sky_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->portable_sky_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(portable sky)", result);
            diagnostic.code = "d3d12_portable_sky_pipeline_failed";
            return false;
        }
    }
    for (const D3D12PortableSkyPipeline& cached :
         context->portable_sky_pipelines) {
        if (cached.format == target_format && cached.samples == sample_count) {
            pipeline = cached.pipeline.Get();
            return true;
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->portable_sky_root_signature.Get();
    description.VS = {generated::d3d12_portable_sky_vertex_dxbc,
                      generated::d3d12_portable_sky_vertex_dxbc_size};
    description.PS = {generated::d3d12_portable_sky_pixel_dxbc,
                      generated::d3d12_portable_sky_pixel_dxbc_size};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.BlendState.RenderTarget[0U].BlendEnable = FALSE;
    description.BlendState.RenderTarget[0U].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = target_format;
    description.SampleDesc.Count = sample_count;
    description.SampleDesc.Quality = sample_quality;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(portable sky)", result);
        diagnostic.code = "d3d12_portable_sky_pipeline_failed";
        return false;
    }
    D3D12PortableSkyPipeline cached;
    cached.format = target_format;
    cached.samples = sample_count;
    cached.pipeline = std::move(created);
    pipeline = cached.pipeline.Get();
    context->portable_sky_pipelines.push_back(std::move(cached));
    return true;
}

[[nodiscard]] bool prepare_d3d12_portable_cloud_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT target_format,
    UINT sample_count, UINT sample_quality,
    ID3D12PipelineState*& pipeline, Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context == nullptr || context->device == nullptr ||
        target_format == DXGI_FORMAT_UNKNOWN ||
        (sample_count != 1U && sample_count != 4U)) {
        diagnostic = {"d3d12_portable_cloud_pipeline_failed",
                      "D3D12 portable clouds received an invalid device, format, or sample count"};
        return false;
    }
    if (context->portable_cloud_root_signature == nullptr) {
        D3D12_ROOT_PARAMETER parameters[3U]{};
        parameters[0U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0U].Descriptor.ShaderRegister = 0U;
        parameters[0U].Descriptor.RegisterSpace = 0U;
        parameters[0U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1U;
        srv_range.BaseShaderRegister = 0U;
        srv_range.RegisterSpace = 0U;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        parameters[1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1U].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[1U].DescriptorTable.pDescriptorRanges = &srv_range;
        parameters[1U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE sampler_range{};
        sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.NumDescriptors = 1U;
        sampler_range.BaseShaderRegister = 0U;
        sampler_range.RegisterSpace = 0U;
        sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        parameters[2U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2U].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[2U].DescriptorTable.pDescriptorRanges = &sampler_range;
        parameters[2U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = 3U;
        root_description.pParameters = parameters;
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(portable clouds)", result);
            diagnostic.code = "d3d12_portable_cloud_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->portable_cloud_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(portable clouds)", result);
            diagnostic.code = "d3d12_portable_cloud_pipeline_failed";
            return false;
        }
    }
    for (const D3D12PortableCloudPipeline& cached :
         context->portable_cloud_pipelines) {
        if (cached.format == target_format && cached.samples == sample_count) {
            pipeline = cached.pipeline.Get();
            return true;
        }
    }
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 0U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 1U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 8U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 2U, DXGI_FORMAT_R32G32_FLOAT, 0U, 20U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 3U, DXGI_FORMAT_R32_FLOAT, 0U, 28U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->portable_cloud_root_signature.Get();
    description.VS = {generated::d3d12_portable_clouds_vertex_dxbc,
                      generated::d3d12_portable_clouds_vertex_dxbc_size};
    description.PS = {generated::d3d12_portable_clouds_pixel_dxbc,
                      generated::d3d12_portable_clouds_pixel_dxbc_size};
    description.InputLayout = {input, static_cast<UINT>(std::size(input))};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    auto& blend = description.BlendState.RenderTarget[0U];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = target_format;
    description.SampleDesc.Count = sample_count;
    description.SampleDesc.Quality = sample_quality;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(portable clouds)", result);
        diagnostic.code = "d3d12_portable_cloud_pipeline_failed";
        return false;
    }
    D3D12PortableCloudPipeline cached;
    cached.format = target_format;
    cached.samples = sample_count;
    cached.pipeline = std::move(created);
    pipeline = cached.pipeline.Get();
    context->portable_cloud_pipelines.push_back(std::move(cached));
    return true;
}

[[nodiscard]] bool prepare_d3d12_portable_grass_pipeline(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT target_format,
    UINT sample_count, UINT sample_quality,
    ID3D12PipelineState*& pipeline, Diagnostic& diagnostic) {
    pipeline = nullptr;
    if (context == nullptr || context->device == nullptr ||
        target_format == DXGI_FORMAT_UNKNOWN ||
        (sample_count != 1U && sample_count != 4U)) {
        diagnostic = {"d3d12_portable_grass_pipeline_failed",
                      "D3D12 portable grass received an invalid device, format, or sample count"};
        return false;
    }
    if (context->portable_grass_root_signature == nullptr) {
        D3D12_ROOT_PARAMETER parameters[3U]{};
        parameters[0U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0U].Descriptor.ShaderRegister = 0U;
        parameters[0U].Descriptor.RegisterSpace = 0U;
        parameters[0U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1U;
        srv_range.BaseShaderRegister = 0U;
        srv_range.RegisterSpace = 0U;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        parameters[1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1U].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[1U].DescriptorTable.pDescriptorRanges = &srv_range;
        parameters[1U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE sampler_range{};
        sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.NumDescriptors = 1U;
        sampler_range.BaseShaderRegister = 0U;
        sampler_range.RegisterSpace = 0U;
        sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        parameters[2U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2U].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[2U].DescriptorTable.pDescriptorRanges = &sampler_range;
        parameters[2U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = 3U;
        root_description.pParameters = parameters;
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_errors;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_errors);
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "D3D12SerializeRootSignature(portable grass)", result);
            diagnostic.code = "d3d12_portable_grass_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&context->portable_grass_root_signature));
        if (FAILED(result)) {
            diagnostic = hresult_error(
                "ID3D12Device::CreateRootSignature(portable grass)", result);
            diagnostic.code = "d3d12_portable_grass_pipeline_failed";
            return false;
        }
    }
    for (const D3D12PortableGrassPipeline& cached :
         context->portable_grass_pipelines) {
        if (cached.format == target_format && cached.samples == sample_count) {
            pipeline = cached.pipeline.Get();
            return true;
        }
    }
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 24U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 32U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 1U, DXGI_FORMAT_R32_FLOAT, 0U, 48U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 2U, DXGI_FORMAT_R32_FLOAT, 0U, 52U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = context->portable_grass_root_signature.Get();
    description.VS = {generated::d3d12_portable_grass_vertex_dxbc,
                      generated::d3d12_portable_grass_vertex_dxbc_size};
    description.PS = {generated::d3d12_portable_grass_pixel_dxbc,
                      generated::d3d12_portable_grass_pixel_dxbc_size};
    description.InputLayout = {input, static_cast<UINT>(std::size(input))};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.DepthStencilState.StencilEnable = FALSE;
    description.BlendState.RenderTarget[0U].BlendEnable = FALSE;
    description.BlendState.RenderTarget[0U].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = std::numeric_limits<UINT>::max();
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0U] = target_format;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = sample_count;
    description.SampleDesc.Quality = sample_quality;
    description.RasterizerState.MultisampleEnable = sample_count > 1U ? TRUE : FALSE;
    description.BlendState.AlphaToCoverageEnable = sample_count == 4U ? TRUE : FALSE;
    ComPtr<ID3D12PipelineState> created;
    const HRESULT result = context->device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&created));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "ID3D12Device::CreateGraphicsPipelineState(portable grass)", result);
        diagnostic.code = "d3d12_portable_grass_pipeline_failed";
        return false;
    }
    D3D12PortableGrassPipeline cached;
    cached.format = target_format;
    cached.samples = sample_count;
    cached.pipeline = std::move(created);
    pipeline = cached.pipeline.Get();
    context->portable_grass_pipelines.push_back(std::move(cached));
    return true;
}

bool draw_indexed_static_mesh_batch_and_readback(
    const std::shared_ptr<D3D12Context>& context,
    ID3D12Resource* destination,
    const TextureDescription& description,
    const IndexedStaticMeshBatchDescription& batch,
    std::span<const D3D12IndexedBatchDraw> draws,
    D3D12DepthAttachment* depth_attachment,
    bool color_initialized,
    D3D12_RESOURCE_STATES& destination_state,
    ID3D12Resource* retained_resolve_resource,
    D3D12_RESOURCE_STATES* retained_resolve_state,
    bool* retained_resolve_initialized,
    D3D12_RESOURCE_STATES retained_resolve_final_state,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    if (destination == nullptr) {
        diagnostic = {"indexed_static_mesh_batch_target_missing",
                      "D3D12 indexed static-mesh batch has no color target"};
        return false;
    }
    const UINT target_physical_layer = static_cast<UINT>(
        texture_target_physical_array_layer(description,
                                            batch.target_subresource));
    const UINT target_subresource =
        target_physical_layer * description.mip_levels +
        batch.target_subresource.mip_level;
    if (batch.load_color && !color_initialized) {
        diagnostic = {"indexed_color_load_before_clear",
                      "A color load was requested before the D3D12 color attachment was cleared"};
        return false;
    }
    const bool use_depth = depth_attachment != nullptr;
    if (!validate_d3d12_sample_count(context, dxgi_texture_format(description.format),
                                     description.samples, diagnostic))
        return false;
    for (const D3D12IndexedBatchDraw& draw : draws) {
        if (draw.pipeline == nullptr || draw.pipeline->targets.colors.empty() ||
            draw.pipeline->targets.colors.front().samples != description.samples) {
            diagnostic = {"d3d12_sample_count_mismatch",
                          "D3D12 color texture and every batch pipeline must use the same sample count"};
            return false;
        }
    }
    if (use_depth && depth_attachment->info().description.samples != description.samples) {
        diagnostic = {"d3d12_depth_sample_count_mismatch",
                      "D3D12 color and depth attachment sample counts must match"};
        return false;
    }
    if (use_depth && !validate_d3d12_sample_count(context, DXGI_FORMAT_D32_FLOAT,
                                                   depth_attachment->info().description.samples, diagnostic))
        return false;
    if (use_depth && !batch.clear_depth && !depth_attachment->cleared()) {
        diagnostic = {"indexed_depth_load_before_clear",
                      "A depth load was requested before the D3D12 depth attachment was cleared"};
        return false;
    }
    if (description.width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        description.height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())) {
        diagnostic = {"indexed_static_mesh_batch_uint_limit",
                      "D3D12 indexed static-mesh batch dimensions exceed LONG limits"};
        return false;
    }

    HRESULT result = S_OK;
    bool has_material_resources = false;
    bool has_material_constants = false;
    bool has_frame_constants = false;
    bool has_normal_texture = false;
    bool has_maps_texture = false;
    bool has_detail_texture = false;
    bool has_normal_detail_texture = false;
    bool has_damage_texture = false;
    bool has_damage_mask_texture = false;
    bool has_shadow_receiver = false;
    bool has_multimap_reflection = false;
    for (const D3D12IndexedBatchDraw& draw : draws) {
        if (draw.native_stock_ks_per_pixel || draw.scene_request == nullptr)
            continue;
        const IndexedStaticMeshDrawRequest& request = *draw.scene_request;
        if (!request.pipeline->resources.empty()) {
            has_material_resources = true;
            has_material_constants = has_material_constants ||
                                     d3d12_pipeline_has_material_constants(*request.pipeline);
            has_frame_constants = has_frame_constants ||
                                  d3d12_pipeline_has_frame_constants(*request.pipeline);
            has_normal_texture = has_normal_texture ||
                                 d3d12_pipeline_has_normal_texture(*request.pipeline);
            has_maps_texture = has_maps_texture ||
                               d3d12_pipeline_has_maps_texture(*request.pipeline);
            has_detail_texture = has_detail_texture ||
                                 d3d12_pipeline_has_detail_texture(*request.pipeline);
            has_normal_detail_texture = has_normal_detail_texture ||
                                        d3d12_pipeline_has_normal_detail_texture(*request.pipeline);
            has_damage_texture = has_damage_texture ||
                                 d3d12_pipeline_has_damage_texture(*request.pipeline);
            has_damage_mask_texture = has_damage_mask_texture ||
                                      d3d12_pipeline_has_damage_mask_texture(*request.pipeline);
            has_shadow_receiver = has_shadow_receiver ||
                                  d3d12_pipeline_has_directional_shadow_receiver(*request.pipeline);
            has_multimap_reflection = has_multimap_reflection ||
                                      d3d12_pipeline_has_multimap_reflection(*request.pipeline);
        }
    }
    const std::size_t material_descriptor_stride =
        1U + static_cast<std::size_t>(has_material_constants) +
        static_cast<std::size_t>(has_frame_constants) +
        static_cast<std::size_t>(has_normal_texture) +
        static_cast<std::size_t>(has_maps_texture) +
        static_cast<std::size_t>(has_detail_texture) +
        static_cast<std::size_t>(has_normal_detail_texture) +
        static_cast<std::size_t>(has_damage_texture) +
        static_cast<std::size_t>(has_damage_mask_texture) +
        4U * static_cast<std::size_t>(has_shadow_receiver) +
        2U * static_cast<std::size_t>(has_multimap_reflection);
    const std::size_t shadow_descriptor_base =
        1U + static_cast<std::size_t>(has_material_constants) +
        static_cast<std::size_t>(has_frame_constants) + static_cast<std::size_t>(has_normal_texture) +
        static_cast<std::size_t>(has_maps_texture) + static_cast<std::size_t>(has_detail_texture) +
        static_cast<std::size_t>(has_normal_detail_texture) + static_cast<std::size_t>(has_damage_texture) +
        static_cast<std::size_t>(has_damage_mask_texture);
    const std::size_t multimap_reflection_descriptor_base =
        shadow_descriptor_base +
        4U * static_cast<std::size_t>(has_shadow_receiver);
    if (has_material_resources &&
        (draws.size() > std::numeric_limits<std::size_t>::max() / material_descriptor_stride ||
         draws.size() * material_descriptor_stride > static_cast<std::size_t>(std::numeric_limits<UINT>::max()))) {
        diagnostic = {"indexed_resource_descriptor_heap_size_invalid",
                      "D3D12 material descriptor heap size exceeds the descriptor-count limit"};
        return false;
    }
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    std::vector<D3D12MaterialDescriptorBinding> material_bindings;
    if (has_material_resources) {
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
        srv_heap_description.NumDescriptors = static_cast<UINT>(draws.size() * material_descriptor_stride);
        srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = context->device->CreateDescriptorHeap(&srv_heap_description, IID_PPV_ARGS(&srv_heap));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateDescriptorHeap(indexed batch SRV)", result);
            diagnostic.code = "indexed_resource_descriptor_heap_failed";
            return false;
        }
        material_bindings.resize(draws.size());
        for (std::size_t index = 0; index < draws.size(); ++index) {
            if (draws[index].scene_request == nullptr ||
                draws[index].native_stock_ks_per_pixel)
                continue;
            const std::size_t descriptor_index = index * material_descriptor_stride;
            if (!prepare_d3d12_material_binding(context, *draws[index].scene_request, srv_heap.Get(),
                                                 static_cast<UINT>(descriptor_index),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U) +
                                                                   (has_normal_texture ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U) +
                                                                   (has_normal_texture ? 1U : 0U) +
                                                                   (has_maps_texture ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U) +
                                                                   (has_normal_texture ? 1U : 0U) +
                                                                   (has_maps_texture ? 1U : 0U) +
                                                                   (has_detail_texture ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U) +
                                                                   (has_normal_texture ? 1U : 0U) +
                                                                   (has_maps_texture ? 1U : 0U) +
                                                                   (has_detail_texture ? 1U : 0U) +
                                                                   (has_normal_detail_texture ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U) +
                                                                   (has_normal_texture ? 1U : 0U) +
                                                                   (has_maps_texture ? 1U : 0U) +
                                                                   (has_detail_texture ? 1U : 0U) +
                                                                   (has_normal_detail_texture ? 1U : 0U) +
                                                                   (has_damage_texture ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_material_constants ? 1U : 0U) +
                                                                   (has_frame_constants ? 1U : 0U) +
                                                                   (has_normal_texture ? 1U : 0U) +
                                                                   (has_maps_texture ? 1U : 0U) +
                                                                   (has_detail_texture ? 1U : 0U) +
                                                                   (has_normal_detail_texture ? 1U : 0U) +
                                                                   (has_damage_texture ? 1U : 0U) +
                                                                   (has_damage_mask_texture ? 1U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_shadow_receiver ? shadow_descriptor_base : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_shadow_receiver ? shadow_descriptor_base + 3U : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_multimap_reflection
                                                                        ? multimap_reflection_descriptor_base
                                                                        : 0U)),
                                                 static_cast<UINT>(descriptor_index +
                                                                   (has_multimap_reflection
                                                                        ? multimap_reflection_descriptor_base + 1U
                                                                        : 0U)),
                                                 material_bindings[index], diagnostic))
                return false;
        }
    }

    constexpr std::size_t native_cbv_srv_descriptors_per_draw = 11U;
    constexpr std::size_t native_sampler_descriptors_per_draw = 2U;
    std::vector<UINT> native_descriptor_bases(draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> native_sampler_bases(draws.size(), std::numeric_limits<UINT>::max());
    std::size_t native_draw_count = 0U;
    for (const D3D12IndexedBatchDraw& draw : draws)
        native_draw_count += draw.native_stock_ks_per_pixel ? 1U : 0U;
    ComPtr<ID3D12DescriptorHeap> native_srv_heap;
    ComPtr<ID3D12DescriptorHeap> native_sampler_heap;
    if (native_draw_count != 0U) {
        if (native_draw_count > 4096U ||
            native_draw_count > std::numeric_limits<std::size_t>::max() /
                                    native_cbv_srv_descriptors_per_draw ||
            native_draw_count * native_cbv_srv_descriptors_per_draw >
                static_cast<std::size_t>(std::numeric_limits<UINT>::max()) ||
            native_draw_count > D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE /
                                    native_sampler_descriptors_per_draw) {
            diagnostic = {"indexed_stock_native_batch_descriptor_count_invalid",
                          "Hybrid D3D12 batch descriptor count exceeds bounded limits"};
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.NumDescriptors = static_cast<UINT>(
            native_draw_count * native_cbv_srv_descriptors_per_draw);
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = context->device->CreateDescriptorHeap(&heap_description,
                                                        IID_PPV_ARGS(&native_srv_heap));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateDescriptorHeap(hybrid native CBV/SRV)", result);
            diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
            return false;
        }
        heap_description.NumDescriptors = static_cast<UINT>(
            native_draw_count * native_sampler_descriptors_per_draw);
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        result = context->device->CreateDescriptorHeap(&heap_description,
                                                        IID_PPV_ARGS(&native_sampler_heap));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateDescriptorHeap(hybrid native sampler)", result);
            diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
            return false;
        }
        const UINT cbv_stride = context->device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT sampler_stride = context->device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        const auto cpu_handle = [](D3D12_CPU_DESCRIPTOR_HANDLE start, UINT stride,
                                   std::size_t index_value) {
            start.ptr += static_cast<SIZE_T>(index_value) * stride;
            return start;
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE cbv_start =
            native_srv_heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE sampler_start =
            native_sampler_heap->GetCPUDescriptorHandleForHeapStart();
        std::size_t native_index = 0U;
        for (std::size_t index = 0U; index < draws.size(); ++index) {
            const D3D12IndexedBatchDraw& draw = draws[index];
            if (!draw.native_stock_ks_per_pixel) continue;
            const D3D12StockKsPerPixelNativeBatchDraw& native = draw.native_stock_draw;
            if (native.shader_program == nullptr || native.constant_buffers == nullptr ||
                native.samplers == nullptr || native.diffuse_texture == nullptr ||
                native.diffuse_format == DXGI_FORMAT_UNKNOWN ||
                std::any_of(native.constant_buffer_resources.begin(),
                            native.constant_buffer_resources.end(),
                            [](ID3D12Resource* value) { return value == nullptr; }) ||
                std::any_of(native.shadow_maps.begin(), native.shadow_maps.end(),
                            [](ID3D12Resource* value) { return value == nullptr; })) {
                diagnostic = {"indexed_stock_native_batch_binding_missing",
                              "Hybrid native draw binding is incomplete"};
                return false;
            }
            const std::size_t base = native_index * native_cbv_srv_descriptors_per_draw;
            native_descriptor_bases[index] = static_cast<UINT>(base);
            native_sampler_bases[index] = static_cast<UINT>(native_index * native_sampler_descriptors_per_draw);
            for (std::size_t constant_index = 0U; constant_index < 4U; ++constant_index) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC view{
                    native.constant_buffer_resources[constant_index]->GetGPUVirtualAddress(), 256U};
                context->device->CreateConstantBufferView(
                    &view, cpu_handle(cbv_start, cbv_stride, base + constant_index));
            }
            constexpr std::array<std::size_t, 3U> pixel_sources = {2U, 3U, 4U};
            for (std::size_t constant_index = 0U; constant_index < pixel_sources.size(); ++constant_index) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC view{
                    native.constant_buffer_resources[pixel_sources[constant_index]]->GetGPUVirtualAddress(), 256U};
                context->device->CreateConstantBufferView(
                    &view, cpu_handle(cbv_start, cbv_stride, base + 4U + constant_index));
            }
            bool views_valid = create_d3d12_batch_srv(
                context->device.Get(), native.diffuse_texture, native.diffuse_format,
                cpu_handle(cbv_start, cbv_stride, base + 7U));
            for (std::size_t shadow_index = 0U;
                 shadow_index < native.shadow_maps.size() && views_valid; ++shadow_index) {
                views_valid = create_d3d12_batch_srv(
                    context->device.Get(), native.shadow_maps[shadow_index],
                    DXGI_FORMAT_R32_FLOAT,
                    cpu_handle(cbv_start, cbv_stride, base + 8U + shadow_index));
            }
            if (!views_valid) {
                diagnostic = {"indexed_stock_native_batch_view_invalid",
                              "Hybrid native draw SRV creation requires 2D textures"};
                return false;
            }
            const Sampler* linear_sampler = native.samplers->sampler(
                StockKsPerPixelNativeSamplerSlot::linear);
            const Sampler* shadow_sampler = native.samplers->sampler(
                StockKsPerPixelNativeSamplerSlot::shadow);
            if (linear_sampler == nullptr || shadow_sampler == nullptr) {
                diagnostic = {"indexed_stock_native_batch_sampler_missing",
                              "Hybrid native draw samplers are incomplete"};
                return false;
            }
            const SamplerDescription& linear = linear_sampler->info().description;
            const SamplerDescription& shadow = shadow_sampler->info().description;
            D3D12_SAMPLER_DESC linear_desc{};
            linear_desc.Filter = D3D12_FILTER_ANISOTROPIC;
            linear_desc.AddressU = d3d12_batch_sampler_address(linear.address_u);
            linear_desc.AddressV = d3d12_batch_sampler_address(linear.address_v);
            linear_desc.AddressW = d3d12_batch_sampler_address(linear.address_w);
            linear_desc.MipLODBias = linear.mip_lod_bias;
            linear_desc.MaxAnisotropy = static_cast<UINT>(linear.max_anisotropy);
            linear_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            linear_desc.MinLOD = linear.min_lod;
            linear_desc.MaxLOD = linear.max_lod;
            context->device->CreateSampler(
                &linear_desc, cpu_handle(sampler_start, sampler_stride,
                                          native_index * native_sampler_descriptors_per_draw));
            D3D12_SAMPLER_DESC shadow_desc{};
            shadow_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            shadow_desc.AddressU = d3d12_batch_sampler_address(shadow.address_u);
            shadow_desc.AddressV = d3d12_batch_sampler_address(shadow.address_v);
            shadow_desc.AddressW = d3d12_batch_sampler_address(shadow.address_w);
            shadow_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
            shadow_desc.MinLOD = shadow.min_lod;
            shadow_desc.MaxLOD = shadow.max_lod;
            context->device->CreateSampler(
                &shadow_desc, cpu_handle(sampler_start, sampler_stride,
                                          native_index * native_sampler_descriptors_per_draw + 1U));
            ++native_index;
        }
    }

    std::vector<ComPtr<ID3D12RootSignature>> root_signatures;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    std::vector<UINT> shadow_srv_root_indices(draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> shadow_sampler_root_indices(draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> shadow_cbv_root_indices(draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> multimap_cube_srv_root_indices(
        draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> multimap_cube_sampler_root_indices(
        draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> multimap_reflection_cbv_root_indices(
        draws.size(), std::numeric_limits<UINT>::max());
    std::vector<UINT> selected_color_root_indices(draws.size(),
                                                   std::numeric_limits<UINT>::max());
    root_signatures.resize(draws.size());
    pipelines.resize(draws.size());
    const UINT target_sample_quality = destination->GetDesc().SampleDesc.Quality;
    for (std::size_t index = 0; index < draws.size(); ++index) {
        if (draws[index].native_stock_ks_per_pixel) {
            const PipelineProgram& program = *draws[index].pipeline;
            const auto& native = draws[index].native_stock_draw;
            std::array<D3D12_DESCRIPTOR_RANGE,
                       stock_ks_per_pixel_d3d12_root_ranges.size()> ranges{};
            std::array<D3D12_ROOT_PARAMETER,
                       stock_ks_per_pixel_d3d12_root_ranges.size()> parameters{};
            for (std::size_t range_index = 0U;
                 range_index < stock_ks_per_pixel_d3d12_root_ranges.size(); ++range_index) {
                const auto& source = stock_ks_per_pixel_d3d12_root_ranges[range_index];
                auto& range = ranges[range_index];
                range.RangeType = source.register_class == StockShaderRegisterClass::constant_buffer
                                       ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                                   : source.register_class == StockShaderRegisterClass::sampled_texture
                                       ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                       : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                range.NumDescriptors = source.descriptor_count;
                range.BaseShaderRegister = source.base_register;
                range.RegisterSpace = source.register_space;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                parameters[range_index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[range_index].DescriptorTable.NumDescriptorRanges = 1U;
                parameters[range_index].DescriptorTable.pDescriptorRanges = &range;
                parameters[range_index].ShaderVisibility = source.stages == StockShaderStageMask::vertex
                                                                 ? D3D12_SHADER_VISIBILITY_VERTEX
                                                                 : D3D12_SHADER_VISIBILITY_PIXEL;
            }
            D3D12_ROOT_SIGNATURE_DESC root_description{};
            root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            root_description.NumParameters = static_cast<UINT>(parameters.size());
            root_description.pParameters = parameters.data();
            ComPtr<ID3DBlob> root_blob;
            ComPtr<ID3DBlob> root_error;
            result = D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &root_blob, &root_error);
            if (FAILED(result)) {
                diagnostic = hresult_error("D3D12SerializeRootSignature(hybrid native)", result);
                diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
                return false;
            }
            result = context->device->CreateRootSignature(0U, root_blob->GetBufferPointer(),
                                                           root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signatures[index]));
            if (FAILED(result)) {
                diagnostic = hresult_error("CreateRootSignature(hybrid native)", result);
                diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
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
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U}};
            const auto vs = native.shader_program->source().vertex_shader();
            const auto ps = native.shader_program->source().pixel_shader();
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_description{};
            pipeline_description.pRootSignature = root_signatures[index].Get();
            pipeline_description.VS = {vs.data(), vs.size()};
            pipeline_description.PS = {ps.data(), ps.size()};
            pipeline_description.InputLayout = {input, 4U};
            pipeline_description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pipeline_description.RasterizerState.CullMode =
                program.raster.cull == PipelineCullMode::none ? D3D12_CULL_MODE_NONE
                : program.raster.cull == PipelineCullMode::front ? D3D12_CULL_MODE_FRONT
                : D3D12_CULL_MODE_BACK;
            pipeline_description.RasterizerState.FrontCounterClockwise =
                program.raster.front_face == PipelineFrontFace::counter_clockwise;
            pipeline_description.RasterizerState.DepthClipEnable = TRUE;
            pipeline_description.BlendState.AlphaToCoverageEnable =
                program.blend.alpha_to_coverage ? TRUE : FALSE;
            pipeline_description.BlendState.RenderTarget[0].SrcBlend =
                d3d12_pipeline_blend_factor(program.blend.source_color);
            pipeline_description.BlendState.RenderTarget[0].DestBlend =
                d3d12_pipeline_blend_factor(program.blend.destination_color);
            pipeline_description.BlendState.RenderTarget[0].BlendOp =
                d3d12_pipeline_blend_operation(program.blend.color_operation);
            pipeline_description.BlendState.RenderTarget[0].SrcBlendAlpha =
                d3d12_pipeline_blend_factor(program.blend.source_alpha);
            pipeline_description.BlendState.RenderTarget[0].DestBlendAlpha =
                d3d12_pipeline_blend_factor(program.blend.destination_alpha);
            pipeline_description.BlendState.RenderTarget[0].BlendOpAlpha =
                d3d12_pipeline_blend_operation(program.blend.alpha_operation);
            pipeline_description.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pipeline_description.DepthStencilState.DepthEnable =
                use_depth && program.depth.test_enabled ? TRUE : FALSE;
            pipeline_description.DepthStencilState.DepthWriteMask =
                use_depth && program.depth.write_enabled ? D3D12_DEPTH_WRITE_MASK_ALL
                                                         : D3D12_DEPTH_WRITE_MASK_ZERO;
            pipeline_description.DepthStencilState.DepthFunc = use_depth
                ? d3d12_pipeline_compare(program.depth.compare) : D3D12_COMPARISON_FUNC_ALWAYS;
            pipeline_description.NumRenderTargets = 1U;
            pipeline_description.RTVFormats[0] = dxgi_texture_format(description.format);
            pipeline_description.DSVFormat = use_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
            pipeline_description.SampleMask = std::numeric_limits<UINT>::max();
            pipeline_description.SampleDesc.Count = description.samples;
            pipeline_description.SampleDesc.Quality = target_sample_quality;
            result = context->device->CreateGraphicsPipelineState(
                &pipeline_description, IID_PPV_ARGS(&pipelines[index]));
            if (FAILED(result)) {
                diagnostic = hresult_error("CreateGraphicsPipelineState(hybrid native)", result);
                diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
                return false;
            }
            continue;
        }
        const PipelineProgram& program = *draws[index].pipeline;
        const PipelineShaderModule* vertex_shader = nullptr;
        const PipelineShaderModule* fragment_shader = nullptr;
        for (const PipelineShaderModule& shader : program.shaders) {
            if (shader.format != PipelineShaderFormat::dxbc &&
                shader.format != PipelineShaderFormat::dxil) {
                diagnostic = {"indexed_static_mesh_batch_shader_format_unsupported",
                              "D3D12 indexed static-mesh batch requires DXIL/DXBC shaders"};
                return false;
            }
            if (shader.stage == PipelineShaderStage::vertex) vertex_shader = &shader;
            if (shader.stage == PipelineShaderStage::fragment) fragment_shader = &shader;
        }
        if (vertex_shader == nullptr || fragment_shader == nullptr) {
            diagnostic = {"indexed_static_mesh_batch_shader_pair_invalid",
                          "D3D12 indexed static-mesh batch requires vertex and fragment shaders"};
            return false;
        }
        D3D12_ROOT_PARAMETER transform_parameter{};
        transform_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transform_parameter.Constants.ShaderRegister = 0U;
        transform_parameter.Constants.RegisterSpace = 0U;
        transform_parameter.Constants.Num32BitValues = static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t));
        transform_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        D3D12_DESCRIPTOR_RANGE sampler_range{};
        D3D12_ROOT_PARAMETER srv_parameter{};
        D3D12_ROOT_PARAMETER sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE cbv_range{};
        D3D12_ROOT_PARAMETER cbv_parameter{};
        D3D12_DESCRIPTOR_RANGE frame_cbv_range{};
        D3D12_ROOT_PARAMETER frame_cbv_parameter{};
        D3D12_DESCRIPTOR_RANGE normal_srv_range{};
        D3D12_ROOT_PARAMETER normal_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE normal_sampler_range{};
        D3D12_ROOT_PARAMETER normal_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE maps_srv_range{};
        D3D12_ROOT_PARAMETER maps_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE maps_sampler_range{};
        D3D12_ROOT_PARAMETER maps_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE detail_srv_range{};
        D3D12_ROOT_PARAMETER detail_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE detail_sampler_range{};
        D3D12_ROOT_PARAMETER detail_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE normal_detail_srv_range{};
        D3D12_ROOT_PARAMETER normal_detail_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE normal_detail_sampler_range{};
        D3D12_ROOT_PARAMETER normal_detail_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE damage_srv_range{};
        D3D12_ROOT_PARAMETER damage_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE damage_sampler_range{};
        D3D12_ROOT_PARAMETER damage_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE damage_mask_srv_range{};
        D3D12_ROOT_PARAMETER damage_mask_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE damage_mask_sampler_range{};
        D3D12_ROOT_PARAMETER damage_mask_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE shadow_srv_range{};
        D3D12_ROOT_PARAMETER shadow_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE shadow_sampler_range{};
        D3D12_ROOT_PARAMETER shadow_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE shadow_cbv_range{};
        D3D12_ROOT_PARAMETER shadow_cbv_parameter{};
        D3D12_DESCRIPTOR_RANGE multimap_cube_srv_range{};
        D3D12_ROOT_PARAMETER multimap_cube_srv_parameter{};
        D3D12_DESCRIPTOR_RANGE multimap_cube_sampler_range{};
        D3D12_ROOT_PARAMETER multimap_cube_sampler_parameter{};
        D3D12_DESCRIPTOR_RANGE multimap_reflection_cbv_range{};
        D3D12_ROOT_PARAMETER multimap_reflection_cbv_parameter{};
        std::array<D3D12_ROOT_PARAMETER, 24> root_parameters{};
        UINT root_parameter_count = 1U;
        root_parameters[0] = transform_parameter;
        D3D12_ROOT_PARAMETER selected_color_parameter{};
        if (program.transform_contract ==
            PipelineTransformContract::selected_mesh) {
            selected_color_parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_CBV;
            selected_color_parameter.Descriptor.ShaderRegister = 5U;
            selected_color_parameter.Descriptor.RegisterSpace = 0U;
            selected_color_parameter.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
            selected_color_root_indices[index] = root_parameter_count;
            root_parameters[root_parameter_count++] = selected_color_parameter;
        }
        if (!program.resources.empty()) {
            srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srv_range.NumDescriptors = 1U;
            srv_range.BaseShaderRegister = 0U;
            srv_range.RegisterSpace = 0U;
            srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            srv_parameter.DescriptorTable.pDescriptorRanges = &srv_range;
            srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = srv_parameter;

            sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            sampler_range.NumDescriptors = 1U;
            sampler_range.BaseShaderRegister = 1U;
            sampler_range.RegisterSpace = 0U;
            sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            sampler_parameter.DescriptorTable.pDescriptorRanges = &sampler_range;
            sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[root_parameter_count++] = sampler_parameter;
            if (d3d12_pipeline_has_material_constants(program)) {
                cbv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                cbv_range.NumDescriptors = 1U;
                cbv_range.BaseShaderRegister = 2U;
                cbv_range.RegisterSpace = 0U;
                cbv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                cbv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                cbv_parameter.DescriptorTable.pDescriptorRanges = &cbv_range;
                cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = cbv_parameter;
            }
            if (d3d12_pipeline_has_frame_constants(program)) {
                frame_cbv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                frame_cbv_range.NumDescriptors = 1U;
                frame_cbv_range.BaseShaderRegister = 3U;
                frame_cbv_range.RegisterSpace = 0U;
                frame_cbv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                frame_cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                frame_cbv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                frame_cbv_parameter.DescriptorTable.pDescriptorRanges = &frame_cbv_range;
                frame_cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = frame_cbv_parameter;
            }
            if (d3d12_pipeline_has_normal_texture(program)) {
                normal_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                normal_srv_range.NumDescriptors = 1U;
                normal_srv_range.BaseShaderRegister = 4U;
                normal_srv_range.RegisterSpace = 0U;
                normal_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                normal_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                normal_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                normal_srv_parameter.DescriptorTable.pDescriptorRanges = &normal_srv_range;
                normal_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = normal_srv_parameter;

                normal_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                normal_sampler_range.NumDescriptors = 1U;
                normal_sampler_range.BaseShaderRegister = 5U;
                normal_sampler_range.RegisterSpace = 0U;
                normal_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                normal_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                normal_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                normal_sampler_parameter.DescriptorTable.pDescriptorRanges = &normal_sampler_range;
                normal_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = normal_sampler_parameter;
            }
            if (d3d12_pipeline_has_maps_texture(program)) {
                maps_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                maps_srv_range.NumDescriptors = 1U;
                maps_srv_range.BaseShaderRegister = 6U;
                maps_srv_range.RegisterSpace = 0U;
                maps_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                maps_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                maps_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                maps_srv_parameter.DescriptorTable.pDescriptorRanges = &maps_srv_range;
                maps_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = maps_srv_parameter;

                maps_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                maps_sampler_range.NumDescriptors = 1U;
                maps_sampler_range.BaseShaderRegister = 7U;
                maps_sampler_range.RegisterSpace = 0U;
                maps_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                maps_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                maps_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                maps_sampler_parameter.DescriptorTable.pDescriptorRanges = &maps_sampler_range;
                maps_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = maps_sampler_parameter;
            }
            if (d3d12_pipeline_has_detail_texture(program)) {
                detail_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                detail_srv_range.NumDescriptors = 1U;
                detail_srv_range.BaseShaderRegister = 8U;
                detail_srv_range.RegisterSpace = 0U;
                detail_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                detail_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                detail_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                detail_srv_parameter.DescriptorTable.pDescriptorRanges = &detail_srv_range;
                detail_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = detail_srv_parameter;

                detail_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                detail_sampler_range.NumDescriptors = 1U;
                detail_sampler_range.BaseShaderRegister = 9U;
                detail_sampler_range.RegisterSpace = 0U;
                detail_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                detail_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                detail_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                detail_sampler_parameter.DescriptorTable.pDescriptorRanges = &detail_sampler_range;
                detail_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = detail_sampler_parameter;
            }
            if (d3d12_pipeline_has_normal_detail_texture(program)) {
                normal_detail_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                normal_detail_srv_range.NumDescriptors = 1U;
                normal_detail_srv_range.BaseShaderRegister = 10U;
                normal_detail_srv_range.RegisterSpace = 0U;
                normal_detail_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                normal_detail_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                normal_detail_srv_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                normal_detail_srv_parameter.DescriptorTable.pDescriptorRanges = &normal_detail_srv_range;
                normal_detail_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = normal_detail_srv_parameter;

                normal_detail_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                normal_detail_sampler_range.NumDescriptors = 1U;
                normal_detail_sampler_range.BaseShaderRegister = 11U;
                normal_detail_sampler_range.RegisterSpace = 0U;
                normal_detail_sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                normal_detail_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                normal_detail_sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
                normal_detail_sampler_parameter.DescriptorTable.pDescriptorRanges = &normal_detail_sampler_range;
                normal_detail_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = normal_detail_sampler_parameter;
            }
            if (d3d12_pipeline_has_damage_texture(program)) {
                damage_srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1U, 12U, 0U,
                                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                damage_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                damage_srv_parameter.DescriptorTable = {1U, &damage_srv_range};
                damage_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = damage_srv_parameter;
                damage_sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U, 13U, 0U,
                                        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                damage_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                damage_sampler_parameter.DescriptorTable = {1U, &damage_sampler_range};
                damage_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = damage_sampler_parameter;
            }
            if (d3d12_pipeline_has_damage_mask_texture(program)) {
                damage_mask_srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1U, 14U, 0U,
                                         D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                damage_mask_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                damage_mask_srv_parameter.DescriptorTable = {1U, &damage_mask_srv_range};
                damage_mask_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = damage_mask_srv_parameter;
                damage_mask_sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U, 15U, 0U,
                                             D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                damage_mask_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                damage_mask_sampler_parameter.DescriptorTable = {1U, &damage_mask_sampler_range};
                damage_mask_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                root_parameters[root_parameter_count++] = damage_mask_sampler_parameter;
            }
            if (d3d12_pipeline_has_directional_shadow_receiver(program)) {
                shadow_srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3U, 16U, 0U,
                                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                shadow_srv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                shadow_srv_parameter.DescriptorTable = {1U, &shadow_srv_range};
                shadow_srv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                shadow_srv_root_indices[index] = root_parameter_count;
                root_parameters[root_parameter_count++] = shadow_srv_parameter;
                shadow_sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U, 19U, 0U,
                                        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                shadow_sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                shadow_sampler_parameter.DescriptorTable = {1U, &shadow_sampler_range};
                shadow_sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                shadow_sampler_root_indices[index] = root_parameter_count;
                root_parameters[root_parameter_count++] = shadow_sampler_parameter;
                shadow_cbv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1U, 20U, 0U,
                                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                shadow_cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                shadow_cbv_parameter.DescriptorTable = {1U, &shadow_cbv_range};
                shadow_cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                shadow_cbv_root_indices[index] = root_parameter_count;
                root_parameters[root_parameter_count++] = shadow_cbv_parameter;
            }
            if (d3d12_pipeline_has_multimap_reflection(program)) {
                multimap_cube_srv_range = {
                    D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1U,
                    portable_multimap_cube_texture_binding, 0U,
                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                multimap_cube_srv_parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                multimap_cube_srv_parameter.DescriptorTable = {
                    1U, &multimap_cube_srv_range};
                multimap_cube_srv_parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
                multimap_cube_srv_root_indices[index] = root_parameter_count;
                root_parameters[root_parameter_count++] =
                    multimap_cube_srv_parameter;
                multimap_cube_sampler_range = {
                    D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1U,
                    portable_multimap_cube_sampler_binding, 0U,
                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                multimap_cube_sampler_parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                multimap_cube_sampler_parameter.DescriptorTable = {
                    1U, &multimap_cube_sampler_range};
                multimap_cube_sampler_parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
                multimap_cube_sampler_root_indices[index] =
                    root_parameter_count;
                root_parameters[root_parameter_count++] =
                    multimap_cube_sampler_parameter;
                multimap_reflection_cbv_range = {
                    D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1U,
                    portable_multimap_reflection_constants_binding, 0U,
                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
                multimap_reflection_cbv_parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                multimap_reflection_cbv_parameter.DescriptorTable = {
                    1U, &multimap_reflection_cbv_range};
                multimap_reflection_cbv_parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
                multimap_reflection_cbv_root_indices[index] =
                    root_parameter_count;
                root_parameters[root_parameter_count++] =
                    multimap_reflection_cbv_parameter;
            }
        }
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        root_description.NumParameters = root_parameter_count;
        root_description.pParameters = root_parameters.data();
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_error;
        result = D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &root_blob, &root_error);
        if (FAILED(result)) {
            diagnostic = hresult_error("D3D12SerializeRootSignature(indexed batch)", result);
            diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                       IID_PPV_ARGS(&root_signatures[index]));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateRootSignature(indexed batch)", result);
            diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
            return false;
        }
        std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
        input_elements.reserve(program.vertex_layout.attributes.size());
        for (const PipelineVertexAttribute& attribute : program.vertex_layout.attributes) {
            D3D12_INPUT_ELEMENT_DESC element{};
            element.SemanticName = d3d12_pipeline_semantic(attribute.semantic);
            element.SemanticIndex = attribute.semantic == PipelineVertexSemantic::texcoord1 ? 1U : 0U;
            element.Format = d3d12_pipeline_vertex_format(attribute.format);
            element.InputSlot = 0U;
            element.AlignedByteOffset = attribute.offset;
            element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            input_elements.push_back(element);
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root_signatures[index].Get();
        pipeline_description.VS = {vertex_shader->bytes.data(), vertex_shader->bytes.size()};
        pipeline_description.PS = {fragment_shader->bytes.data(), fragment_shader->bytes.size()};
        pipeline_description.InputLayout = {input_elements.data(), static_cast<UINT>(input_elements.size())};
        pipeline_description.PrimitiveTopologyType =
            d3d12_pipeline_topology_type(program.raster.fill);
        pipeline_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline_description.RasterizerState.CullMode = program.raster.cull == PipelineCullMode::none
                                                             ? D3D12_CULL_MODE_NONE
                                                             : program.raster.cull == PipelineCullMode::front
                                                                   ? D3D12_CULL_MODE_FRONT
                                                                   : D3D12_CULL_MODE_BACK;
        pipeline_description.RasterizerState.FrontCounterClockwise =
            program.raster.front_face == PipelineFrontFace::counter_clockwise;
        pipeline_description.RasterizerState.DepthClipEnable = TRUE;
        configure_d3d12_blend_state(program.blend, pipeline_description.BlendState);
        pipeline_description.DepthStencilState.DepthEnable = use_depth && program.depth.test_enabled ? TRUE : FALSE;
        pipeline_description.DepthStencilState.DepthWriteMask =
            use_depth && program.depth.write_enabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline_description.DepthStencilState.DepthFunc =
            use_depth ? d3d12_pipeline_compare(program.depth.compare) : D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline_description.DepthStencilState.StencilEnable = FALSE;
        pipeline_description.NumRenderTargets = 1U;
        pipeline_description.RTVFormats[0] = dxgi_texture_format(description.format);
        pipeline_description.DSVFormat = use_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        pipeline_description.SampleMask = std::numeric_limits<UINT>::max();
        pipeline_description.SampleDesc.Count = description.samples;
        pipeline_description.SampleDesc.Quality = target_sample_quality;
        result = context->device->CreateGraphicsPipelineState(&pipeline_description, IID_PPV_ARGS(&pipelines[index]));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateGraphicsPipelineState(indexed batch)", result);
            diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.NumDescriptors = 1U;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    result = context->device->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateDescriptorHeap(indexed batch RTV)", result);
        diagnostic.code = "indexed_static_mesh_batch_pipeline_failed";
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = dxgi_texture_format(description.format);
    if (description.shape == TextureShape::texture_cube) {
        rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv_description.Texture2DArray.MipSlice = batch.target_subresource.mip_level;
        rtv_description.Texture2DArray.FirstArraySlice = target_physical_layer;
        rtv_description.Texture2DArray.ArraySize = 1U;
    } else {
        rtv_description.ViewDimension = description.samples > 1U ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                                                   : D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv_description.Texture2D.MipSlice = batch.target_subresource.mip_level;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context->device->CreateRenderTargetView(destination, &rtv_description, rtv);

    D3D12_RESOURCE_DESC resource_description = destination->GetDesc();
    ComPtr<ID3D12Resource> resolve_resource;
    ID3D12Resource* readback_source = destination;
    if (description.samples > 1U) {
        D3D12_RESOURCE_DESC resolve_description = resource_description;
        resolve_description.SampleDesc.Count = 1U;
        resolve_description.SampleDesc.Quality = 0U;
        if (retained_resolve_resource == nullptr) {
            D3D12_HEAP_PROPERTIES resolve_heap{};
            resolve_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            result = context->device->CreateCommittedResource(
                &resolve_heap, D3D12_HEAP_FLAG_NONE, &resolve_description,
                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolve_resource));
            if (FAILED(result)) {
                diagnostic = hresult_error("CreateCommittedResource(MSAA resolve batch)", result);
                diagnostic.code = "indexed_static_mesh_batch_execution_failed";
                return false;
            }
            readback_source = resolve_resource.Get();
        } else {
            readback_source = retained_resolve_resource;
        }
        resource_description = resolve_description;
    }
    ID3D12Resource* resolve_destination = retained_resolve_resource != nullptr
                                              ? retained_resolve_resource
                                              : resolve_resource.Get();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0U;
    UINT64 row_size = 0U;
    UINT64 readback_size = 0U;
    if (batch.capture_rgba8)
        context->device->GetCopyableFootprints(&resource_description, target_subresource, 1U, 0U,
                                               &footprint, &row_count, &row_size,
                                               &readback_size);
    constexpr UINT64 max_size_t = static_cast<UINT64>(std::numeric_limits<std::size_t>::max());
    const UINT64 row_bytes = static_cast<UINT64>(description.width) * 4U;
    if (batch.capture_rgba8 &&
        (description.height == 0U || row_bytes > max_size_t / description.height)) {
        diagnostic = {"indexed_static_mesh_batch_readback_size_invalid",
                      "D3D12 indexed static-mesh batch output exceeds platform limits"};
        return false;
    }
    const std::size_t output_size = static_cast<std::size_t>(row_bytes * static_cast<UINT64>(description.height));
    if (batch.capture_rgba8 &&
        (row_count < description.height || row_size < row_bytes ||
         footprint.Footprint.RowPitch < row_bytes || footprint.Offset > max_size_t ||
         readback_size > max_texture_readback_bytes || readback_size > max_size_t)) {
        diagnostic = {"indexed_static_mesh_batch_readback_footprint_invalid",
                      "D3D12 indexed static-mesh batch readback footprint is out of bounds"};
        return false;
    }
    const UINT64 last_row = batch.capture_rgba8
                                ? footprint.Offset +
                                      static_cast<UINT64>(description.height - 1U) *
                                          footprint.Footprint.RowPitch
                                : 0U;
    if (batch.capture_rgba8 &&
        (last_row < footprint.Offset || last_row > readback_size ||
         row_bytes > readback_size - last_row)) {
        diagnostic = {"indexed_static_mesh_batch_readback_footprint_invalid",
                      "D3D12 indexed static-mesh batch rows exceed the bounded footprint"};
        return false;
    }
    D3D12_RESOURCE_DESC readback_description{};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = readback_size;
    readback_description.Height = 1U;
    readback_description.DepthOrArraySize = 1U;
    readback_description.MipLevels = 1U;
    readback_description.SampleDesc.Count = 1U;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (batch.capture_rgba8)
        result = context->device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (batch.capture_rgba8 && FAILED(result)) {
        diagnostic = hresult_error("CreateCommittedResource(indexed batch readback)", result);
        diagnostic.code = "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    PortableSkyShaderConstants sky_constants{};
    ID3D12PipelineState* sky_pipeline = nullptr;
    if (batch.sky.has_value()) {
        if (!build_portable_sky_shader_constants(*batch.sky, sky_constants,
                                                 diagnostic))
            return false;
        if (!prepare_d3d12_portable_sky_pipeline(
                context, dxgi_texture_format(description.format),
                description.samples, target_sample_quality, sky_pipeline,
                diagnostic))
            return false;
    }
    PortableCloudShaderConstants cloud_constants{};
    ID3D12PipelineState* cloud_pipeline = nullptr;
    ComPtr<ID3D12DescriptorHeap> cloud_srv_heap;
    ComPtr<ID3D12Resource> cloud_constant_buffer;
    std::array<std::optional<D3D12PortableCloudTextureResource>,
               portable_cloud_texture_count>
        cloud_textures;
    std::vector<D3D12PortableCloudTextureResource*> cloud_texture_states;
    std::vector<const PortableCloudTextureRun*> cloud_runs;
    ID3D12Resource* cloud_vertex_buffer = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE cloud_sampler{};
    if (batch.clouds.has_value()) {
        const PortableCloudParameters& clouds = *batch.clouds;
        if (!build_portable_cloud_shader_constants(
                clouds, cloud_constants, diagnostic))
            return false;
        for (std::size_t slot = 0U; slot < clouds.textures.size(); ++slot) {
            if (clouds.textures[slot] == nullptr)
                continue;
            cloud_textures[slot].emplace();
            if (!inspect_d3d12_portable_cloud_texture(
                    clouds.textures[slot], context.get(),
                    *cloud_textures[slot], diagnostic))
                return false;
        }
        for (const PortableCloudTextureRun& run : clouds.texture_runs) {
            auto& texture = cloud_textures[run.texture];
            if (!texture.has_value())
                continue;
            const TextureDescription& texture_description = texture->description;
            if (!texture->initialized ||
                texture_description.shape != TextureShape::texture_2d ||
                texture_description.samples != 1U ||
                texture_description.width == 0U ||
                texture_description.height == 0U ||
                texture_description.mip_levels == 0U ||
                texture->format == DXGI_FORMAT_UNKNOWN) {
                diagnostic = {"portable_cloud_texture_invalid",
                              "Portable cloud textures must be initialized single-sample 2D images"};
                return false;
            }
            cloud_runs.push_back(&run);
            bool known = false;
            for (const auto* state : cloud_texture_states) {
                if (state->resource == texture->resource) {
                    known = true;
                    break;
                }
            }
            if (!known)
                cloud_texture_states.push_back(&*texture);
        }
        if (!cloud_runs.empty()) {
            const auto* vertex_buffer = dynamic_cast<const D3D12Buffer*>(
                clouds.vertex_buffer);
            if (vertex_buffer == nullptr ||
                vertex_buffer->context() != context.get() ||
                !inspect_d3d12_portable_cloud_sampler(
                    clouds.sampler, context.get(), cloud_sampler,
                    diagnostic)) {
                if (!diagnostic.code.empty())
                    return false;
                diagnostic = {"portable_cloud_context_mismatch",
                              "Portable cloud buffers and samplers must belong to the D3D12 device"};
                return false;
            }
            cloud_vertex_buffer = vertex_buffer->resource();
            if ((static_cast<UINT>(vertex_buffer->state()) &
                 static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)) == 0U) {
                diagnostic = {"portable_cloud_vertex_state_invalid",
                              "Portable cloud vertices must be in D3D12 vertex-buffer state"};
                return false;
            }
            const D3D12_RESOURCE_DESC vertex_description = vertex_buffer->resource()->GetDesc();
            for (const PortableCloudTextureRun* run : cloud_runs) {
                const UINT64 offset = static_cast<UINT64>(run->first_vertex) *
                                      portable_cloud_vertex_stride_bytes;
                const UINT64 bytes = static_cast<UINT64>(run->vertex_count) *
                                     portable_cloud_vertex_stride_bytes;
                if (offset > vertex_description.Width ||
                    bytes > vertex_description.Width - offset ||
                    bytes > std::numeric_limits<UINT>::max()) {
                    diagnostic = {"portable_cloud_vertex_range_invalid",
                                  "Portable cloud vertex runs exceed the D3D12 vertex buffer"};
                    return false;
                }
            }
            if (!prepare_d3d12_portable_cloud_pipeline(
                    context, dxgi_texture_format(description.format),
                    description.samples, target_sample_quality, cloud_pipeline,
                    diagnostic))
                return false;

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC constant_description{};
            constant_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            constant_description.Width = 256U;
            constant_description.Height = 1U;
            constant_description.DepthOrArraySize = 1U;
            constant_description.MipLevels = 1U;
            constant_description.SampleDesc.Count = 1U;
            constant_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            result = context->device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &constant_description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cloud_constant_buffer));
            if (FAILED(result)) {
                diagnostic = hresult_error(
                    "CreateCommittedResource(portable clouds constants)", result);
                diagnostic.code = "portable_cloud_execution_failed";
                return false;
            }
            void* mapped = nullptr;
            result = cloud_constant_buffer->Map(0U, nullptr, &mapped);
            if (FAILED(result)) {
                diagnostic = hresult_error("Map(portable clouds constants)", result);
                diagnostic.code = "portable_cloud_execution_failed";
                return false;
            }
            std::memcpy(mapped, &cloud_constants, sizeof(cloud_constants));
            cloud_constant_buffer->Unmap(0U, nullptr);

            D3D12_DESCRIPTOR_HEAP_DESC srv_description{};
            srv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srv_description.NumDescriptors = portable_cloud_texture_count;
            srv_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            result = context->device->CreateDescriptorHeap(
                &srv_description, IID_PPV_ARGS(&cloud_srv_heap));
            if (FAILED(result)) {
                diagnostic = hresult_error(
                    "CreateDescriptorHeap(portable clouds SRV)", result);
                diagnostic.code = "portable_cloud_execution_failed";
                return false;
            }
            const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE srv_start =
                cloud_srv_heap->GetCPUDescriptorHandleForHeapStart();
            for (std::uint32_t slot = 0U; slot < portable_cloud_texture_count; ++slot) {
                const auto& texture = cloud_textures[slot];
                if (!texture.has_value())
                    continue;
                D3D12_CPU_DESCRIPTOR_HANDLE target = srv_start;
                target.ptr += static_cast<SIZE_T>(slot) * srv_stride;
                if (!create_d3d12_batch_srv(
                        context->device.Get(), texture->resource,
                        texture->format, target)) {
                    diagnostic = {"portable_cloud_texture_view_invalid",
                                  "Portable cloud texture SRV creation requires a 2D texture"};
                    return false;
                }
            }
        }
    }
    PortableGrassShaderConstants grass_constants{};
    ID3D12PipelineState* grass_pipeline = nullptr;
    ComPtr<ID3D12DescriptorHeap> grass_srv_heap;
    ComPtr<ID3D12Resource> grass_constant_buffer;
    D3D12PortableGrassTextureResource grass_texture;
    ID3D12Resource* grass_vertex_buffer = nullptr;
    UINT grass_vertex_count = 0U;
    D3D12_GPU_DESCRIPTOR_HANDLE grass_sampler{};
    if (batch.grass.has_value()) {
        const PortableGrassParameters& grass = *batch.grass;
        if (!build_portable_grass_shader_constants(grass, grass_constants,
                                                   diagnostic))
            return false;
        grass_vertex_count = grass.vertex_count;
        if (grass_vertex_count != 0U) {
            if (!use_depth) {
                diagnostic = {"portable_grass_depth_attachment_missing",
                              "D3D12 portable grass requires a depth attachment"};
                return false;
            }
            const auto* vertex_buffer = dynamic_cast<const D3D12Buffer*>(
                grass.vertex_buffer);
            if (vertex_buffer == nullptr ||
                vertex_buffer->context() != context.get() ||
                vertex_buffer->resource() == nullptr ||
                !inspect_d3d12_portable_grass_texture(
                    grass.atlas, context.get(), grass_texture, diagnostic) ||
                !inspect_d3d12_portable_grass_sampler(
                    grass.sampler, context.get(), grass_sampler, diagnostic)) {
                if (!diagnostic.code.empty()) return false;
                diagnostic = {"portable_grass_context_mismatch",
                              "Portable grass resources must belong to the D3D12 device"};
                return false;
            }
            grass_vertex_buffer = vertex_buffer->resource();
            if ((static_cast<UINT>(vertex_buffer->state()) &
                 static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)) == 0U) {
                diagnostic = {"portable_grass_vertex_state_invalid",
                              "Portable grass vertices must be in D3D12 vertex-buffer state"};
                return false;
            }
            const D3D12_RESOURCE_DESC vertex_description =
                grass_vertex_buffer->GetDesc();
            const UINT64 bytes = static_cast<UINT64>(grass_vertex_count) *
                                 portable_grass_vertex_stride_bytes;
            if (bytes == 0U || bytes > vertex_description.Width ||
                bytes > std::numeric_limits<UINT>::max() ||
                grass_vertex_buffer->GetGPUVirtualAddress() == 0U) {
                diagnostic = {"portable_grass_vertex_range_invalid",
                              "Portable grass vertices exceed the D3D12 vertex buffer"};
                return false;
            }
            if (!grass_texture.initialized || grass_texture.resource == nullptr ||
                grass_texture.format == DXGI_FORMAT_UNKNOWN ||
                grass_texture.description.shape != TextureShape::texture_2d ||
                grass_texture.description.samples != 1U ||
                grass_texture.description.array_layers != 1U ||
                grass_texture.description.width == 0U ||
                grass_texture.description.height == 0U ||
                grass_texture.description.mip_levels == 0U) {
                diagnostic = {"portable_grass_texture_invalid",
                              "Portable grass atlas must be an initialized single-sample 2D image"};
                return false;
            }
            if (grass_texture.resource == destination ||
                (use_depth && grass_texture.resource == depth_attachment->resource())) {
                diagnostic = {"portable_grass_texture_alias_invalid",
                              "Portable grass atlas cannot alias a batch attachment"};
                return false;
            }
            if (!prepare_d3d12_portable_grass_pipeline(
                    context, dxgi_texture_format(description.format),
                    description.samples, target_sample_quality, grass_pipeline,
                    diagnostic))
                return false;

            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC constant_description{};
            constant_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            constant_description.Width = 256U;
            constant_description.Height = 1U;
            constant_description.DepthOrArraySize = 1U;
            constant_description.MipLevels = 1U;
            constant_description.SampleDesc.Count = 1U;
            constant_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            result = context->device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &constant_description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&grass_constant_buffer));
            if (FAILED(result)) {
                diagnostic = hresult_error(
                    "CreateCommittedResource(portable grass constants)", result);
                diagnostic.code = "portable_grass_execution_failed";
                return false;
            }
            void* mapped = nullptr;
            result = grass_constant_buffer->Map(0U, nullptr, &mapped);
            if (FAILED(result)) {
                diagnostic = hresult_error("Map(portable grass constants)", result);
                diagnostic.code = "portable_grass_execution_failed";
                return false;
            }
            std::memcpy(mapped, &grass_constants, sizeof(grass_constants));
            grass_constant_buffer->Unmap(0U, nullptr);

            D3D12_DESCRIPTOR_HEAP_DESC srv_description{};
            srv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srv_description.NumDescriptors = 1U;
            srv_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            result = context->device->CreateDescriptorHeap(
                &srv_description, IID_PPV_ARGS(&grass_srv_heap));
            if (FAILED(result)) {
                diagnostic = hresult_error(
                    "CreateDescriptorHeap(portable grass SRV)", result);
                diagnostic.code = "portable_grass_execution_failed";
                return false;
            }
            if (!create_d3d12_batch_srv(
                    context->device.Get(), grass_texture.resource,
                    grass_texture.format,
                    grass_srv_heap->GetCPUDescriptorHandleForHeapStart())) {
                diagnostic = {"portable_grass_texture_view_invalid",
                              "Portable grass atlas SRV creation requires a 2D texture"};
                return false;
            }
        }
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandAllocator(indexed batch)", result);
        diagnostic.code = "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandList(indexed batch)", result);
        diagnostic.code = "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    std::vector<D3D12DepthAttachment*> shadow_attachments;
    std::vector<D3D12_RESOURCE_STATES> shadow_states;
    for (const D3D12MaterialDescriptorBinding& binding : material_bindings) {
        if (!binding.shadow_enabled) continue;
        for (D3D12DepthAttachment* attachment : binding.shadow_attachments) {
            if (std::find(shadow_attachments.begin(), shadow_attachments.end(), attachment) ==
                shadow_attachments.end()) {
                shadow_attachments.push_back(attachment);
                shadow_states.push_back(attachment->state());
            }
        }
    }
    struct NativeSampledResource {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES* state = nullptr;
        D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
    };
    std::vector<NativeSampledResource> native_sampled_resources;
    const auto add_native_sampled_resource = [&](ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES* state) {
        if (resource == nullptr || state == nullptr) return false;
        for (std::size_t shadow_index = 0U;
             shadow_index < shadow_attachments.size(); ++shadow_index) {
            if (shadow_attachments[shadow_index]->resource() == resource) {
                if (*state != shadow_states[shadow_index]) return false;
                return true;
            }
        }
        for (const NativeSampledResource& existing : native_sampled_resources) {
            if (existing.resource == resource) {
                return existing.before == *state;
            }
        }
        native_sampled_resources.push_back({resource, state, *state});
        return true;
    };
    for (const D3D12IndexedBatchDraw& draw : draws) {
        if (!draw.native_stock_ks_per_pixel) continue;
        const auto& native = draw.native_stock_draw;
        if (!add_native_sampled_resource(native.diffuse_texture, native.diffuse_state)) {
            diagnostic = {"indexed_stock_native_batch_resource_alias_invalid",
                          "Hybrid native sampled resources have inconsistent state tracking"};
            return false;
        }
        for (std::size_t shadow_index = 0U; shadow_index < native.shadow_maps.size(); ++shadow_index) {
            if (!add_native_sampled_resource(native.shadow_maps[shadow_index],
                                             native.shadow_states[shadow_index])) {
                diagnostic = {"indexed_stock_native_batch_resource_alias_invalid",
                              "Hybrid native sampled resources have inconsistent state tracking"};
                return false;
            }
        }
    }
    bool grass_atlas_already_sampled = false;
    if (grass_pipeline != nullptr) {
        for (std::size_t shadow_index = 0U;
             shadow_index < shadow_attachments.size(); ++shadow_index) {
            if (shadow_attachments[shadow_index]->resource() != grass_texture.resource)
                continue;
            if (shadow_states[shadow_index] != grass_texture.original_state) {
                diagnostic = {"portable_grass_texture_alias_invalid",
                              "Portable grass atlas state tracking conflicts with a shadow attachment"};
                return false;
            }
            grass_atlas_already_sampled = true;
        }
        for (const NativeSampledResource& sampled : native_sampled_resources) {
            if (sampled.resource != grass_texture.resource) continue;
            if (sampled.before != grass_texture.original_state) {
                diagnostic = {"portable_grass_texture_alias_invalid",
                              "Portable grass atlas state tracking conflicts with a native sampled resource"};
                return false;
            }
            grass_atlas_already_sampled = true;
        }
        for (const auto* cloud_state : cloud_texture_states) {
            if (cloud_state->resource != grass_texture.resource) continue;
            if (cloud_state->original_state != grass_texture.original_state) {
                diagnostic = {"portable_grass_texture_alias_invalid",
                              "Portable grass atlas state tracking conflicts with a cloud texture"};
                return false;
            }
            grass_atlas_already_sampled = true;
        }
    }
    for (std::size_t shadow_index = 0U; shadow_index < shadow_attachments.size(); ++shadow_index) {
        if (shadow_states[shadow_index] == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shadow_attachments[shadow_index]->resource();
        barrier.Transition.StateBefore = shadow_states[shadow_index];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    for (const NativeSampledResource& sampled : native_sampled_resources) {
        if (sampled.before == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = sampled.resource;
        barrier.Transition.StateBefore = sampled.before;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    for (const auto* cloud_state : cloud_texture_states) {
        const bool already_sampled = std::any_of(
            native_sampled_resources.begin(), native_sampled_resources.end(),
            [&](const NativeSampledResource& sampled) {
                return sampled.resource == cloud_state->resource;
            });
        if (already_sampled) continue;
        if (cloud_state->original_state ==
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = cloud_state->resource;
        barrier.Transition.StateBefore = cloud_state->original_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    if (grass_pipeline != nullptr && !grass_atlas_already_sampled &&
        grass_texture.original_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = grass_texture.resource;
        barrier.Transition.StateBefore = grass_texture.original_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    if (destination_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = destination_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    D3D12_RESOURCE_STATES depth_state = use_depth ? depth_attachment->state() : D3D12_RESOURCE_STATE_COMMON;
    if (use_depth && batch.clear_depth && depth_state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = depth_attachment->resource();
        barrier.Transition.StateBefore = depth_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
        depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (use_depth && batch.clear_depth)
        list->ClearDepthStencilView(depth_attachment->dsv(true), D3D12_CLEAR_FLAG_DEPTH,
                                    batch.depth_clear_value, 0U, 0U, nullptr);
    if (!batch.load_color) list->ClearRenderTargetView(rtv, batch.clear_color.data(), 0U, nullptr);
    if (sky_pipeline != nullptr) {
        // This is the WebGL-aligned portable sky formula, not recovered
        // native SkyBox parity. It is intentionally drawn after the load/
        // clear and before every scene, selection, and overlay draw.
        list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
        list->SetGraphicsRootSignature(context->portable_sky_root_signature.Get());
        list->SetGraphicsRoot32BitConstants(
            0U,
            static_cast<UINT>(sizeof(PortableSkyShaderConstants) /
                              sizeof(std::uint32_t)),
            &sky_constants, 0U);
        list->SetPipelineState(sky_pipeline);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT sky_viewport{
            0.0F, 0.0F, static_cast<float>(description.width),
            static_cast<float>(description.height), 0.0F, 1.0F};
        D3D12_RECT sky_scissor{
            0, 0, static_cast<LONG>(description.width),
            static_cast<LONG>(description.height)};
        list->RSSetViewports(1U, &sky_viewport);
        list->RSSetScissorRects(1U, &sky_scissor);
        list->DrawInstanced(3U, 1U, 0U, 0U);
    }
    if (cloud_pipeline != nullptr && cloud_vertex_buffer != nullptr &&
        cloud_sampler.ptr != 0U) {
        // This is the WebGL-aligned portable cloud billboard pass, not
        // recovered native cloud shader parity. It is alpha blended after
        // sky and before retained scene geometry.
        ID3D12DescriptorHeap* descriptor_heaps[] = {
            cloud_srv_heap.Get(), context->sampler_heap.Get()};
        list->SetDescriptorHeaps(2U, descriptor_heaps);
        list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
        list->SetGraphicsRootSignature(context->portable_cloud_root_signature.Get());
        list->SetGraphicsRootConstantBufferView(
            0U, cloud_constant_buffer->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE srv_start =
            cloud_srv_heap->GetGPUDescriptorHandleForHeapStart();
        const UINT srv_stride = context->device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetGraphicsRootDescriptorTable(2U, cloud_sampler);
        D3D12_VIEWPORT cloud_viewport{
            0.0F, 0.0F, static_cast<float>(description.width),
            static_cast<float>(description.height), 0.0F, 1.0F};
        D3D12_RECT cloud_scissor{
            0, 0, static_cast<LONG>(description.width),
            static_cast<LONG>(description.height)};
        list->RSSetViewports(1U, &cloud_viewport);
        list->RSSetScissorRects(1U, &cloud_scissor);
        list->SetPipelineState(cloud_pipeline);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const PortableCloudTextureRun* run : cloud_runs) {
            if (batch.clouds->textures[run->texture] == nullptr) continue;
            D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu = srv_start;
            texture_gpu.ptr += static_cast<UINT64>(run->texture) * srv_stride;
            list->SetGraphicsRootDescriptorTable(1U, texture_gpu);
            const UINT64 vertex_offset = static_cast<UINT64>(run->first_vertex) *
                                         portable_cloud_vertex_stride_bytes;
            D3D12_VERTEX_BUFFER_VIEW vertex_view{};
            vertex_view.BufferLocation = cloud_vertex_buffer->GetGPUVirtualAddress() +
                                         vertex_offset;
            vertex_view.SizeInBytes = run->vertex_count *
                                      portable_cloud_vertex_stride_bytes;
            vertex_view.StrideInBytes = portable_cloud_vertex_stride_bytes;
            list->IASetVertexBuffers(0U, 1U, &vertex_view);
            list->DrawInstanced(run->vertex_count, 1U, 0U, 0U);
        }
    }
    bool grass_drawn = false;
    const auto draw_grass = [&] {
        if (grass_pipeline == nullptr || grass_drawn) return;
        if (use_depth && depth_state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depth_attachment->resource();
            barrier.Transition.StateBefore = depth_state;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &barrier);
            depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        ID3D12DescriptorHeap* descriptor_heaps[] = {
            grass_srv_heap.Get(), context->sampler_heap.Get()};
        list->SetDescriptorHeaps(2U, descriptor_heaps);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_attachment->dsv(true);
        list->OMSetRenderTargets(1U, &rtv, FALSE, &dsv);
        list->SetGraphicsRootSignature(context->portable_grass_root_signature.Get());
        list->SetGraphicsRootConstantBufferView(
            0U, grass_constant_buffer->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(
            1U, grass_srv_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootDescriptorTable(2U, grass_sampler);
        list->SetPipelineState(grass_pipeline);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        vertex_view.BufferLocation = grass_vertex_buffer->GetGPUVirtualAddress();
        vertex_view.SizeInBytes = grass_vertex_count * portable_grass_vertex_stride_bytes;
        vertex_view.StrideInBytes = portable_grass_vertex_stride_bytes;
        list->IASetVertexBuffers(0U, 1U, &vertex_view);
        D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(description.width),
                                static_cast<float>(description.height), 0.0F, 1.0F};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(description.width),
                           static_cast<LONG>(description.height)};
        list->RSSetViewports(1U, &viewport);
        list->RSSetScissorRects(1U, &scissor);
        list->DrawInstanced(grass_vertex_count, 1U, 0U, 0U);
        grass_drawn = true;
    };
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const auto& draw = draws[index];
        if (!grass_drawn && grass_pipeline != nullptr && draw.transparent)
            draw_grass();
        const bool depth_write = use_depth && draw.pipeline->depth.write_enabled;
        const D3D12_RESOURCE_STATES desired_depth_state = depth_write ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                       : D3D12_RESOURCE_STATE_DEPTH_READ;
        if (use_depth && depth_state != desired_depth_state) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depth_attachment->resource();
            barrier.Transition.StateBefore = depth_state;
            barrier.Transition.StateAfter = desired_depth_state;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &barrier);
            depth_state = desired_depth_state;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (use_depth) dsv = depth_attachment->dsv(depth_write);
        list->OMSetRenderTargets(1U, &rtv, FALSE, use_depth ? &dsv : nullptr);
        list->SetGraphicsRootSignature(root_signatures[index].Get());
        if (draw.native_stock_ks_per_pixel) {
            ID3D12DescriptorHeap* descriptor_heaps[] = {
                native_srv_heap.Get(), native_sampler_heap.Get()};
            list->SetDescriptorHeaps(2U, descriptor_heaps);
            const UINT cbv_base = native_descriptor_bases[index];
            const UINT sampler_base = native_sampler_bases[index];
            const UINT cbv_stride = context->device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const UINT sampler_stride = context->device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            D3D12_GPU_DESCRIPTOR_HANDLE cbv_start =
                native_srv_heap->GetGPUDescriptorHandleForHeapStart();
            cbv_start.ptr += static_cast<UINT64>(cbv_base) * cbv_stride;
            D3D12_GPU_DESCRIPTOR_HANDLE sampler_start =
                native_sampler_heap->GetGPUDescriptorHandleForHeapStart();
            sampler_start.ptr += static_cast<UINT64>(sampler_base) * sampler_stride;
            D3D12_GPU_DESCRIPTOR_HANDLE pixel_cbv = cbv_start;
            pixel_cbv.ptr += static_cast<UINT64>(4U) * cbv_stride;
            D3D12_GPU_DESCRIPTOR_HANDLE diffuse = cbv_start;
            diffuse.ptr += static_cast<UINT64>(7U) * cbv_stride;
            D3D12_GPU_DESCRIPTOR_HANDLE shadows = cbv_start;
            shadows.ptr += static_cast<UINT64>(8U) * cbv_stride;
            list->SetGraphicsRootDescriptorTable(0U, cbv_start);
            list->SetGraphicsRootDescriptorTable(1U, pixel_cbv);
            list->SetGraphicsRootDescriptorTable(2U, diffuse);
            list->SetGraphicsRootDescriptorTable(3U, shadows);
            list->SetGraphicsRootDescriptorTable(4U, sampler_start);
        } else {
            if (has_material_resources) {
                ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap.Get(), context->sampler_heap.Get()};
                list->SetDescriptorHeaps(2U, descriptor_heaps);
            }
        list->SetGraphicsRoot32BitConstants(0U, static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t)),
                                             &draw.matrices, 0U);
        if (draw.selected_color != nullptr &&
            selected_color_root_indices[index] !=
                std::numeric_limits<UINT>::max()) {
            list->SetGraphicsRootConstantBufferView(
                selected_color_root_indices[index],
                draw.selected_color->resource()->GetGPUVirtualAddress() +
                    draw.selected_color_offset);
        }
        if (!material_bindings.empty() && material_bindings[index].enabled) {
            list->SetGraphicsRootDescriptorTable(1U, material_bindings[index].srv_gpu);
            list->SetGraphicsRootDescriptorTable(2U, material_bindings[index].sampler_gpu);
            UINT constant_root_index = 3U;
            if (material_bindings[index].constant_enabled)
                list->SetGraphicsRootDescriptorTable(constant_root_index++, material_bindings[index].cbv_gpu);
            if (material_bindings[index].frame_enabled)
                list->SetGraphicsRootDescriptorTable(constant_root_index, material_bindings[index].frame_cbv_gpu);
            if (material_bindings[index].normal_enabled) {
                const UINT normal_root_index = constant_root_index +
                                               static_cast<UINT>(material_bindings[index].frame_enabled);
                list->SetGraphicsRootDescriptorTable(normal_root_index, material_bindings[index].normal_srv_gpu);
                list->SetGraphicsRootDescriptorTable(normal_root_index + 1U,
                                                      material_bindings[index].normal_sampler_gpu);
            }
            if (material_bindings[index].maps_enabled) {
                const UINT maps_root_index = constant_root_index +
                                             static_cast<UINT>(material_bindings[index].frame_enabled) +
                                             2U * static_cast<UINT>(material_bindings[index].normal_enabled);
                list->SetGraphicsRootDescriptorTable(maps_root_index, material_bindings[index].maps_srv_gpu);
                list->SetGraphicsRootDescriptorTable(maps_root_index + 1U,
                                                      material_bindings[index].maps_sampler_gpu);
            }
            if (material_bindings[index].detail_enabled) {
                const UINT detail_root_index = constant_root_index +
                                               static_cast<UINT>(material_bindings[index].frame_enabled) +
                                               2U * static_cast<UINT>(material_bindings[index].normal_enabled) +
                                               2U * static_cast<UINT>(material_bindings[index].maps_enabled);
                list->SetGraphicsRootDescriptorTable(detail_root_index,
                                                      material_bindings[index].detail_srv_gpu);
                list->SetGraphicsRootDescriptorTable(detail_root_index + 1U,
                                                      material_bindings[index].detail_sampler_gpu);
            }
            if (material_bindings[index].normal_detail_enabled) {
                const UINT normal_detail_root_index = constant_root_index +
                                                      static_cast<UINT>(material_bindings[index].frame_enabled) +
                                                      2U * static_cast<UINT>(material_bindings[index].normal_enabled) +
                                                      2U * static_cast<UINT>(material_bindings[index].maps_enabled) +
                                                      2U * static_cast<UINT>(material_bindings[index].detail_enabled);
                list->SetGraphicsRootDescriptorTable(normal_detail_root_index,
                                                      material_bindings[index].normal_detail_srv_gpu);
                list->SetGraphicsRootDescriptorTable(normal_detail_root_index + 1U,
                                                      material_bindings[index].normal_detail_sampler_gpu);
            }
            if (material_bindings[index].damage_enabled) {
                const UINT damage_root_index = constant_root_index +
                                               static_cast<UINT>(material_bindings[index].frame_enabled) +
                                               2U * static_cast<UINT>(material_bindings[index].normal_enabled) +
                                               2U * static_cast<UINT>(material_bindings[index].maps_enabled) +
                                               2U * static_cast<UINT>(material_bindings[index].detail_enabled) +
                                               2U * static_cast<UINT>(material_bindings[index].normal_detail_enabled);
                list->SetGraphicsRootDescriptorTable(damage_root_index,
                                                      material_bindings[index].damage_srv_gpu);
                list->SetGraphicsRootDescriptorTable(damage_root_index + 1U,
                                                      material_bindings[index].damage_sampler_gpu);
            }
            if (material_bindings[index].damage_mask_enabled) {
                const UINT damage_mask_root_index = constant_root_index +
                                                    static_cast<UINT>(material_bindings[index].frame_enabled) +
                                                    2U * static_cast<UINT>(material_bindings[index].normal_enabled) +
                                                    2U * static_cast<UINT>(material_bindings[index].maps_enabled) +
                                                    2U * static_cast<UINT>(material_bindings[index].detail_enabled) +
                                                    2U * static_cast<UINT>(material_bindings[index].normal_detail_enabled) +
                                                    2U * static_cast<UINT>(material_bindings[index].damage_enabled);
                list->SetGraphicsRootDescriptorTable(damage_mask_root_index,
                                                      material_bindings[index].damage_mask_srv_gpu);
                list->SetGraphicsRootDescriptorTable(damage_mask_root_index + 1U,
                                                      material_bindings[index].damage_mask_sampler_gpu);
            }
            if (material_bindings[index].shadow_enabled &&
                shadow_srv_root_indices[index] != std::numeric_limits<UINT>::max()) {
                list->SetGraphicsRootDescriptorTable(shadow_srv_root_indices[index],
                                                      material_bindings[index].shadow_srv_gpu);
                list->SetGraphicsRootDescriptorTable(shadow_sampler_root_indices[index],
                                                      material_bindings[index].shadow_sampler_gpu);
                list->SetGraphicsRootDescriptorTable(shadow_cbv_root_indices[index],
                                                      material_bindings[index].shadow_cbv_gpu);
            }
            if (material_bindings[index].multimap_reflection_enabled &&
                multimap_cube_srv_root_indices[index] !=
                    std::numeric_limits<UINT>::max()) {
                list->SetGraphicsRootDescriptorTable(
                    multimap_cube_srv_root_indices[index],
                    material_bindings[index].multimap_cube_srv_gpu);
                list->SetGraphicsRootDescriptorTable(
                    multimap_cube_sampler_root_indices[index],
                    material_bindings[index].multimap_cube_sampler_gpu);
                list->SetGraphicsRootDescriptorTable(
                    multimap_reflection_cbv_root_indices[index],
                    material_bindings[index].multimap_reflection_cbv_gpu);
            }
        }
        }
        list->SetPipelineState(pipelines[index].Get());
        list->IASetPrimitiveTopology(draw.native_stock_ks_per_pixel
                                          ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                          : d3d12_pipeline_topology(draw.pipeline->raster.fill));
        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        vertex_view.BufferLocation = draw.geometry.vertex_resource->GetGPUVirtualAddress() + draw.geometry.vertex_offset;
        vertex_view.SizeInBytes = draw.geometry.vertex_size;
        vertex_view.StrideInBytes = draw.geometry.vertex_stride;
        list->IASetVertexBuffers(0U, 1U, &vertex_view);
        D3D12_INDEX_BUFFER_VIEW index_view{};
        if (draw.geometry.indexed) {
            index_view.BufferLocation =
                draw.geometry.index_resource->GetGPUVirtualAddress() +
                draw.geometry.index_offset;
            index_view.SizeInBytes = draw.geometry.index_size;
            index_view.Format = DXGI_FORMAT_R16_UINT;
            list->IASetIndexBuffer(&index_view);
        } else {
            list->IASetIndexBuffer(nullptr);
        }
        D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(description.width),
                                static_cast<float>(description.height), 0.0F, 1.0F};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(description.width), static_cast<LONG>(description.height)};
        list->RSSetViewports(1U, &viewport);
        list->RSSetScissorRects(1U, &scissor);
        if (draw.geometry.indexed)
            list->DrawIndexedInstanced(draw.geometry.index_count, 1U, 0U, 0, 0U);
        else
            list->DrawInstanced(draw.geometry.vertex_count, 1U, 0U, 0U);
    }
    if (!grass_drawn && grass_pipeline != nullptr) draw_grass();
    for (std::size_t shadow_index = 0U; shadow_index < shadow_attachments.size(); ++shadow_index) {
        if (shadow_states[shadow_index] == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shadow_attachments[shadow_index]->resource();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = shadow_states[shadow_index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    for (const NativeSampledResource& sampled : native_sampled_resources) {
        if (sampled.before == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = sampled.resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = sampled.before;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    for (const auto* cloud_state : cloud_texture_states) {
        const bool already_sampled = std::any_of(
            native_sampled_resources.begin(), native_sampled_resources.end(),
            [&](const NativeSampledResource& sampled) {
                return sampled.resource == cloud_state->resource;
            });
        if (already_sampled) continue;
        if (cloud_state->original_state ==
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = cloud_state->resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = cloud_state->original_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    if (grass_pipeline != nullptr && !grass_atlas_already_sampled &&
        grass_texture.original_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = grass_texture.resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = grass_texture.original_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    const D3D12_RESOURCE_STATES final_state =
        texture_state(description.usage, description.access_policy);
    if (description.samples > 1U) {
        D3D12_RESOURCE_BARRIER resolve_source{};
        resolve_source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resolve_source.Transition.pResource = destination;
        resolve_source.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        resolve_source.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        resolve_source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &resolve_source);
        if (retained_resolve_state != nullptr &&
            *retained_resolve_state != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
            D3D12_RESOURCE_BARRIER resolve_target{};
            resolve_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            resolve_target.Transition.pResource = resolve_destination;
            resolve_target.Transition.StateBefore = *retained_resolve_state;
            resolve_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            resolve_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &resolve_target);
        }
        list->ResolveSubresource(resolve_destination, 0U, destination, 0U,
                                 dxgi_texture_format(description.format));
        D3D12_RESOURCE_BARRIER resolve_after{};
        resolve_after.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resolve_after.Transition.pResource = resolve_destination;
        resolve_after.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        resolve_after.Transition.StateAfter = batch.capture_rgba8
                                                   ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                                   : retained_resolve_final_state;
        resolve_after.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &resolve_after);
    } else if (batch.capture_rgba8) {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = destination;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &to_copy);
    }
    if (batch.capture_rgba8) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = readback_source;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = target_subresource;
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = readback.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = footprint;
        list->CopyTextureRegion(&target, 0U, 0U, 0U, &source, nullptr);
    }
    if (description.samples > 1U) {
        D3D12_RESOURCE_BARRIER destination_final{};
        destination_final.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        destination_final.Transition.pResource = destination;
        destination_final.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        destination_final.Transition.StateAfter = final_state;
        destination_final.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &destination_final);
        if (batch.capture_rgba8 && retained_resolve_resource != nullptr) {
            D3D12_RESOURCE_BARRIER resolve_final{};
            resolve_final.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            resolve_final.Transition.pResource = retained_resolve_resource;
            resolve_final.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            resolve_final.Transition.StateAfter = retained_resolve_final_state;
            resolve_final.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1U, &resolve_final);
        }
    } else if (batch.capture_rgba8 &&
               final_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("Close(indexed batch)", result);
        diagnostic.code = "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateFence(indexed batch)", result);
        diagnostic.code = "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        diagnostic = {"indexed_static_mesh_batch_execution_failed", "CreateEventW failed for indexed batch"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    result = context->queue->Signal(fence.Get(), 1U);
    if (SUCCEEDED(result)) result = fence->SetEventOnCompletion(1U, event);
    if (SUCCEEDED(result) && WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        destination_state = drained ? final_state : D3D12_RESOURCE_STATE_COMMON;
        if (retained_resolve_state != nullptr)
            *retained_resolve_state = drained ? retained_resolve_final_state
                                              : D3D12_RESOURCE_STATE_COMMON;
        if (retained_resolve_initialized != nullptr)
            *retained_resolve_initialized = drained;
        if (use_depth)
            depth_attachment->set_state(drained ? depth_state : D3D12_RESOURCE_STATE_COMMON);
        for (std::size_t shadow_index = 0U; shadow_index < shadow_attachments.size(); ++shadow_index)
            shadow_attachments[shadow_index]->set_state(
                drained ? shadow_states[shadow_index] : D3D12_RESOURCE_STATE_COMMON);
        for (const NativeSampledResource& sampled : native_sampled_resources)
            if (sampled.state != nullptr)
                *sampled.state = drained ? sampled.before : D3D12_RESOURCE_STATE_COMMON;
        for (const auto* cloud_state : cloud_texture_states)
            if (cloud_state->tracked_state != nullptr)
                *cloud_state->tracked_state = drained
                                                  ? cloud_state->original_state
                                                  : D3D12_RESOURCE_STATE_COMMON;
        if (grass_pipeline != nullptr && !grass_atlas_already_sampled &&
            grass_texture.tracked_state != nullptr)
            *grass_texture.tracked_state = drained
                                               ? grass_texture.original_state
                                               : D3D12_RESOURCE_STATE_COMMON;
        diagnostic = hresult_error("D3D12 indexed batch fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed" : "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    destination_state = final_state;
    if (retained_resolve_state != nullptr)
        *retained_resolve_state = retained_resolve_final_state;
    if (retained_resolve_initialized != nullptr)
        *retained_resolve_initialized = true;
    if (use_depth) {
        depth_attachment->set_state(depth_state);
        if (batch.clear_depth) depth_attachment->set_cleared();
    }
    for (std::size_t shadow_index = 0U; shadow_index < shadow_attachments.size(); ++shadow_index)
        shadow_attachments[shadow_index]->set_state(shadow_states[shadow_index]);
    for (const NativeSampledResource& sampled : native_sampled_resources)
        if (sampled.state != nullptr) *sampled.state = sampled.before;
    for (const auto* cloud_state : cloud_texture_states)
        if (cloud_state->tracked_state != nullptr)
            *cloud_state->tracked_state = cloud_state->original_state;
    if (grass_pipeline != nullptr && !grass_atlas_already_sampled &&
        grass_texture.tracked_state != nullptr)
        *grass_texture.tracked_state = grass_texture.original_state;
    if (!batch.capture_rgba8) {
        output.clear();
        diagnostic = {};
        return true;
    }
    void* mapped = nullptr;
    D3D12_RANGE read_range{0U, static_cast<SIZE_T>(readback_size)};
    result = readback->Map(0U, &read_range, &mapped);
    if (SUCCEEDED(result)) {
        try {
            output.resize(output_size);
            const auto* source_bytes = static_cast<const std::byte*>(mapped) + footprint.Offset;
            for (std::uint32_t row = 0U; row < description.height; ++row)
                std::memcpy(output.data() + static_cast<std::size_t>(row) * description.width * 4U,
                            source_bytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                            static_cast<std::size_t>(row_bytes));
        } catch (const std::bad_alloc&) {
            result = E_OUTOFMEMORY;
        }
        readback->Unmap(0U, nullptr);
    }
    if (FAILED(result)) {
        diagnostic = hresult_error("Map(indexed batch readback)", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed" : "indexed_static_mesh_batch_execution_failed";
        output.clear();
        return false;
    }
    if (description.format == TextureFormat::bgra8_unorm || description.format == TextureFormat::bgra8_srgb)
        for (std::size_t index = 0; index < output.size(); index += 4U)
            std::swap(output[index], output[index + 2U]);
    return true;
}

bool draw_indexed_static_mesh_and_readback(
    const std::shared_ptr<D3D12Context>& context,
    ID3D12Resource* destination,
    const TextureDescription& description,
    const IndexedStaticMeshDrawRequest& request,
    ID3D12Resource* vertex_resource,
    ID3D12Resource* index_resource,
    D3D12_RESOURCE_STATES& destination_state,
    D3D12_RESOURCE_STATES vertex_state,
    D3D12_RESOURCE_STATES index_state,
    D3D12DepthAttachment* depth_attachment,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    const auto& pipeline = *request.pipeline;
    const auto& packet = *request.packet;
    if (vertex_resource == nullptr || index_resource == nullptr) {
        diagnostic = {"indexed_static_mesh_buffer_missing", "Indexed static-mesh buffers have no D3D12 resource"};
        return false;
    }
    if (description.width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        description.height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        pipeline.vertex_layout.attributes.size() > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
        diagnostic = {"indexed_static_mesh_uint_limit",
                      "Indexed static-mesh dimensions or input layout exceed D3D12 UINT/LONG limits"};
        return false;
    }
    const UINT required_vertex_state = static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    const UINT required_index_state = static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER);
    if ((static_cast<UINT>(vertex_state) & required_vertex_state) == 0U ||
        (static_cast<UINT>(index_state) & required_index_state) == 0U) {
        diagnostic = {"indexed_static_mesh_resource_state_invalid",
                      "Indexed static-mesh buffers are not in vertex/index state"};
        return false;
    }

    const UINT64 stride = static_cast<UINT64>(pipeline.vertex_layout.stride);
    const UINT64 vertex_element_offset = static_cast<UINT64>(packet.vertex_offset);
    const UINT64 index_element_offset = static_cast<UINT64>(packet.index_offset);
    constexpr UINT64 max_uint = static_cast<UINT64>(std::numeric_limits<UINT>::max());
    if (stride == 0U || vertex_element_offset > std::numeric_limits<UINT64>::max() / stride ||
        index_element_offset > std::numeric_limits<UINT64>::max() / sizeof(std::uint16_t) ||
        static_cast<UINT64>(packet.vertex_count) > std::numeric_limits<UINT64>::max() / stride ||
        static_cast<UINT64>(packet.index_count) > std::numeric_limits<UINT64>::max() / sizeof(std::uint16_t)) {
        diagnostic = {"indexed_static_mesh_offset_overflow",
                      "Indexed static-mesh element offsets overflow byte arithmetic"};
        return false;
    }
    const UINT64 vertex_byte_offset = vertex_element_offset * stride;
    const UINT64 index_byte_offset = index_element_offset * sizeof(std::uint16_t);
    const UINT64 vertex_bytes = static_cast<UINT64>(packet.vertex_count) * stride;
    const UINT64 index_bytes = static_cast<UINT64>(packet.index_count) * sizeof(std::uint16_t);
    const D3D12_RESOURCE_DESC vertex_description = vertex_resource->GetDesc();
    const D3D12_RESOURCE_DESC index_description = index_resource->GetDesc();
    if (vertex_bytes > max_uint || index_bytes > max_uint ||
        vertex_byte_offset > vertex_description.Width ||
        vertex_bytes > vertex_description.Width - vertex_byte_offset ||
        index_byte_offset > index_description.Width ||
        index_bytes > index_description.Width - index_byte_offset) {
        diagnostic = {"indexed_static_mesh_buffer_range_invalid",
                      "Indexed static-mesh draw range exceeds D3D12 buffer limits"};
        return false;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS vertex_address = vertex_resource->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS index_address = index_resource->GetGPUVirtualAddress();
    if (vertex_address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - vertex_byte_offset ||
        index_address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - index_byte_offset) {
        diagnostic = {"indexed_static_mesh_gpu_address_overflow",
                      "Indexed static-mesh GPU buffer address overflows"};
        return false;
    }

    D3D12GeometryDescriptor geometry;
    geometry.vertex_resource = vertex_resource;
    geometry.vertex_offset = vertex_byte_offset;
    geometry.vertex_size = static_cast<UINT>(vertex_bytes);
    geometry.vertex_stride = static_cast<UINT>(stride);
    geometry.index_resource = index_resource;
    geometry.index_offset = index_byte_offset;
    geometry.index_size = static_cast<UINT>(index_bytes);
    geometry.index_count = packet.index_count;
    geometry.indexed = true;

    TriangleDrawRequest render_request;
    render_request.pipeline = request.pipeline;
    render_request.vertex_count = packet.vertex_count;
    render_request.mip_level = request.mip_level;
    render_request.array_layer = request.array_layer;
    render_request.clear_color = request.clear_color;
    const DrawMatrices draw_matrices{packet.world_matrix, request.camera_frame->view_projection};
    return draw_graphics_and_readback(context, destination, description, render_request, destination_state, output,
                                      diagnostic, &geometry, &draw_matrices, depth_attachment,
                                      request.clear_depth, request.depth_clear_value, request.load_color, &request);
}

bool execute_d3d12_depth_only_indexed_static_mesh_batch(
    const std::shared_ptr<D3D12Context>& context,
    D3D12DepthAttachment& depth_attachment,
    const DepthOnlyIndexedStaticMeshBatchDescription& batch,
    std::span<D3D12IndexedBatchDraw> draws,
    Diagnostic& diagnostic) {
    const DepthAttachmentDescription& description = depth_attachment.info().description;
    if (description.width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        description.height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())) {
        diagnostic = {"depth_only_indexed_static_mesh_batch_uint_limit",
                      "D3D12 depth-only dimensions exceed LONG limits"};
        return false;
    }
    if (!batch.clear_depth && !depth_attachment.cleared()) {
        diagnostic = {"depth_only_indexed_depth_load_before_clear",
                      "A D3D12 depth attachment must be initialized before it is loaded"};
        return false;
    }
    if (!validate_d3d12_sample_count(context, DXGI_FORMAT_D32_FLOAT,
                                     description.samples, diagnostic))
        return false;

    HRESULT result = S_OK;
    std::vector<ComPtr<ID3D12RootSignature>> root_signatures(draws.size());
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(draws.size());
    bool has_alpha_tested_draw = false;
    for (const D3D12IndexedBatchDraw& draw : draws)
        has_alpha_tested_draw = has_alpha_tested_draw || draw.alpha_tested;
    ComPtr<ID3D12DescriptorHeap> alpha_srv_heap;
    const UINT descriptor_size = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (has_alpha_tested_draw) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.NumDescriptors = 0U;
        for (const D3D12IndexedBatchDraw& draw : draws)
            heap_description.NumDescriptors += draw.alpha_tested ? 1U : 0U;
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = context->device->CreateDescriptorHeap(&heap_description,
                                                       IID_PPV_ARGS(&alpha_srv_heap));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateDescriptorHeap(depth-only alpha)", result);
            diagnostic.code = "depth_only_indexed_descriptor_heap_failed";
            return false;
        }
        UINT descriptor_index = 0U;
        for (D3D12IndexedBatchDraw& draw : draws) {
            if (!draw.alpha_tested) continue;
            if (draw.alpha_texture == nullptr || draw.alpha_sampler == nullptr ||
                draw.alpha_material == nullptr) {
                diagnostic = {"depth_only_indexed_alpha_binding_missing",
                              "D3D12 alpha-tested depth draws require diffuse, sampler, and material bindings"};
                return false;
            }
            const TextureDescription& texture_description =
                d3d12_texture_description(*draw.alpha_texture);
            const DXGI_FORMAT format = dxgi_texture_format(texture_description.format);
            if (format == DXGI_FORMAT_UNKNOWN || texture_description.width == 0U ||
                texture_description.height == 0U || texture_description.mip_levels == 0U ||
                texture_description.array_layers != 1U ||
                (static_cast<std::uint32_t>(texture_description.usage) &
                 static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                diagnostic = {"depth_only_indexed_alpha_texture_unsupported",
                              "D3D12 alpha-tested diffuse requires a nonempty one-layer sampled texture"};
                return false;
            }
            const D3D12_RESOURCE_DESC material_description =
                draw.alpha_material->resource()->GetDesc();
            const UINT64 material_offset = draw.alpha_material_offset;
            const D3D12_GPU_VIRTUAL_ADDRESS material_address =
                draw.alpha_material->resource()->GetGPUVirtualAddress();
            if ((material_offset % stock_shadow_caster_buffer_alignment) != 0U ||
                (material_address % stock_shadow_caster_buffer_alignment) != 0U ||
                material_offset > material_description.Width ||
                stock_shadow_caster_material_bytes >
                    material_description.Width - material_offset ||
                (static_cast<UINT>(draw.alpha_material->state()) &
                 static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)) == 0U) {
                diagnostic = {"depth_only_indexed_alpha_material_invalid",
                              "D3D12 alpha-tested material CBV is outside its aligned constant buffer"};
                return false;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                alpha_srv_heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(descriptor_index) * descriptor_size;
            const D3D12_SHADER_RESOURCE_VIEW_DESC srv =
                d3d12_texture_srv_description(texture_description, format);
            context->device->CreateShaderResourceView(
                d3d12_texture_resource(*draw.alpha_texture), &srv, cpu);
            D3D12_GPU_DESCRIPTOR_HANDLE gpu =
                alpha_srv_heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(descriptor_index) * descriptor_size;
            draw.alpha_srv_gpu = gpu;
            draw.alpha_sampler_gpu = d3d12_sampler_gpu(*draw.alpha_sampler);
            ++descriptor_index;
        }
    }
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const PipelineProgram& program = *draws[index].pipeline;
        const PipelineShaderModule* vertex_shader = nullptr;
        const PipelineShaderModule* fragment_shader = nullptr;
        for (const PipelineShaderModule& shader : program.shaders) {
            if (shader.format != PipelineShaderFormat::dxbc &&
                shader.format != PipelineShaderFormat::dxil) {
                diagnostic = {"depth_only_indexed_shader_format_unsupported",
                              "D3D12 depth-only execution requires DXIL/DXBC bytecode"};
                return false;
            }
            if (shader.stage == PipelineShaderStage::vertex) vertex_shader = &shader;
            if (shader.stage == PipelineShaderStage::fragment) fragment_shader = &shader;
        }
        if (vertex_shader == nullptr ||
            (!draws[index].alpha_tested && program.shaders.size() != 1U) ||
            (draws[index].alpha_tested &&
             (program.shaders.size() != 2U || fragment_shader == nullptr))) {
            diagnostic = {"depth_only_indexed_shader_pair_invalid",
                          draws[index].alpha_tested
                              ? "D3D12 alpha-tested depth execution requires one vertex and one pixel shader"
                              : "D3D12 depth-only execution requires one vertex shader and no pixel shader"};
            return false;
        }

        std::array<D3D12_ROOT_PARAMETER, 4U> root_parameters{};
        D3D12_ROOT_PARAMETER& transform_parameter = root_parameters[0];
        transform_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transform_parameter.Constants.ShaderRegister = 0U;
        transform_parameter.Constants.RegisterSpace = 0U;
        transform_parameter.Constants.Num32BitValues =
            static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t));
        transform_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_DESCRIPTOR_RANGE alpha_srv_range{};
        D3D12_DESCRIPTOR_RANGE alpha_sampler_range{};
        if (draws[index].alpha_tested) {
            alpha_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            alpha_srv_range.NumDescriptors = 1U;
            alpha_srv_range.BaseShaderRegister = 0U;
            alpha_srv_range.RegisterSpace = 0U;
            alpha_srv_range.OffsetInDescriptorsFromTableStart = 0U;
            root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            root_parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
            root_parameters[1].DescriptorTable.pDescriptorRanges = &alpha_srv_range;
            root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            alpha_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            alpha_sampler_range.NumDescriptors = 1U;
            alpha_sampler_range.BaseShaderRegister = 3U;
            alpha_sampler_range.RegisterSpace = 0U;
            alpha_sampler_range.OffsetInDescriptorsFromTableStart = 0U;
            root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            root_parameters[2].DescriptorTable.NumDescriptorRanges = 1U;
            root_parameters[2].DescriptorTable.pDescriptorRanges = &alpha_sampler_range;
            root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            root_parameters[3].Descriptor.ShaderRegister = 4U;
            root_parameters[3].Descriptor.RegisterSpace = 0U;
            root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        root_description.NumParameters = draws[index].alpha_tested ? 4U : 1U;
        root_description.pParameters = root_parameters.data();
        ComPtr<ID3DBlob> root_blob;
        ComPtr<ID3DBlob> root_error;
        result = D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &root_blob, &root_error);
        if (FAILED(result)) {
            diagnostic = hresult_error("D3D12SerializeRootSignature(depth-only batch)", result);
            diagnostic.code = "depth_only_indexed_pipeline_failed";
            return false;
        }
        result = context->device->CreateRootSignature(
            0U, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&root_signatures[index]));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateRootSignature(depth-only batch)", result);
            diagnostic.code = "depth_only_indexed_pipeline_failed";
            return false;
        }

        std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
        input_elements.reserve(program.vertex_layout.attributes.size());
        for (const PipelineVertexAttribute& attribute : program.vertex_layout.attributes) {
            D3D12_INPUT_ELEMENT_DESC element{};
            element.SemanticName = d3d12_pipeline_semantic(attribute.semantic);
            element.SemanticIndex =
                attribute.semantic == PipelineVertexSemantic::texcoord1 ? 1U : 0U;
            element.Format = d3d12_pipeline_vertex_format(attribute.format);
            element.InputSlot = 0U;
            element.AlignedByteOffset = attribute.offset;
            element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            input_elements.push_back(element);
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root_signatures[index].Get();
        pipeline_description.VS = {vertex_shader->bytes.data(), vertex_shader->bytes.size()};
        pipeline_description.PS = draws[index].alpha_tested
                                      ? D3D12_SHADER_BYTECODE{fragment_shader->bytes.data(),
                                                              fragment_shader->bytes.size()}
                                      : D3D12_SHADER_BYTECODE{nullptr, 0U};
        pipeline_description.InputLayout = {input_elements.data(),
                                            static_cast<UINT>(input_elements.size())};
        pipeline_description.PrimitiveTopologyType =
            d3d12_pipeline_topology_type(program.raster.fill);
        pipeline_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline_description.RasterizerState.CullMode =
            program.raster.cull == PipelineCullMode::none
                ? D3D12_CULL_MODE_NONE
                : program.raster.cull == PipelineCullMode::front ? D3D12_CULL_MODE_FRONT
                                                                  : D3D12_CULL_MODE_BACK;
        pipeline_description.RasterizerState.FrontCounterClockwise =
            program.raster.front_face == PipelineFrontFace::counter_clockwise;
        pipeline_description.RasterizerState.DepthClipEnable = TRUE;
        pipeline_description.DepthStencilState.DepthEnable = TRUE;
        pipeline_description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pipeline_description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pipeline_description.DepthStencilState.StencilEnable = FALSE;
        pipeline_description.NumRenderTargets = 0U;
        pipeline_description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pipeline_description.SampleMask = std::numeric_limits<UINT>::max();
        pipeline_description.SampleDesc.Count = description.samples;
        pipeline_description.SampleDesc.Quality = 0U;
        result = context->device->CreateGraphicsPipelineState(
            &pipeline_description, IID_PPV_ARGS(&pipelines[index]));
        if (FAILED(result)) {
            diagnostic = hresult_error("CreateGraphicsPipelineState(depth-only batch)", result);
            diagnostic.code = "depth_only_indexed_pipeline_failed";
            return false;
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandAllocator(depth-only batch)", result);
        diagnostic.code = "depth_only_indexed_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    result = context->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandList(depth-only batch)", result);
        diagnostic.code = "depth_only_indexed_execution_failed";
        return false;
    }
    struct AlphaTextureState {
        D3D12Texture* texture = nullptr;
        D3D12_RESOURCE_STATES original = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES current = D3D12_RESOURCE_STATE_COMMON;
    };
    std::vector<AlphaTextureState> alpha_texture_states;
    for (const D3D12IndexedBatchDraw& draw : draws) {
        if (!draw.alpha_tested || draw.alpha_texture == nullptr) continue;
        bool known = false;
        for (const AlphaTextureState& state : alpha_texture_states) {
            if (state.texture == draw.alpha_texture) {
                known = true;
                break;
            }
        }
        if (!known) {
            const D3D12_RESOURCE_STATES state =
                d3d12_texture_state(*draw.alpha_texture);
            alpha_texture_states.push_back({draw.alpha_texture, state, state});
        }
    }
    const auto restore_alpha_texture_states = [&](bool drained) noexcept {
        for (const AlphaTextureState& state : alpha_texture_states)
            d3d12_texture_set_state(
                *state.texture,
                drained ? state.original : D3D12_RESOURCE_STATE_COMMON);
    };
    D3D12_RESOURCE_STATES depth_state = depth_attachment.state();
    if (depth_state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = depth_attachment.resource();
        barrier.Transition.StateBefore = depth_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
        depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_attachment.dsv(true);
    if (batch.clear_depth)
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, batch.depth_clear_value,
                                    0U, 0U, nullptr);
    list->OMSetRenderTargets(0U, nullptr, FALSE, &dsv);
    if (has_alpha_tested_draw) {
        ID3D12DescriptorHeap* descriptor_heaps[] = {
            alpha_srv_heap.Get(), context->sampler_heap.Get()};
        list->SetDescriptorHeaps(2U, descriptor_heaps);
    }
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const D3D12IndexedBatchDraw& draw = draws[index];
        if (draw.alpha_tested && draw.alpha_texture != nullptr) {
            for (AlphaTextureState& state : alpha_texture_states) {
                if (state.texture != draw.alpha_texture ||
                    state.current == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                    continue;
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource =
                    d3d12_texture_resource(*draw.alpha_texture);
                barrier.Transition.StateBefore = state.current;
                barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1U, &barrier);
                state.current = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
        }
        list->SetGraphicsRootSignature(root_signatures[index].Get());
        list->SetGraphicsRoot32BitConstants(
            0U, static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t)),
            &draw.matrices, 0U);
        if (draw.alpha_tested) {
            list->SetGraphicsRootDescriptorTable(1U, draw.alpha_srv_gpu);
            list->SetGraphicsRootDescriptorTable(2U, draw.alpha_sampler_gpu);
            list->SetGraphicsRootConstantBufferView(
                3U, draw.alpha_material->resource()->GetGPUVirtualAddress() +
                        draw.alpha_material_offset);
        }
        list->SetPipelineState(pipelines[index].Get());
        list->IASetPrimitiveTopology(d3d12_pipeline_topology(draw.pipeline->raster.fill));
        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        vertex_view.BufferLocation = draw.geometry.vertex_resource->GetGPUVirtualAddress() +
                                     draw.geometry.vertex_offset;
        vertex_view.SizeInBytes = draw.geometry.vertex_size;
        vertex_view.StrideInBytes = draw.geometry.vertex_stride;
        list->IASetVertexBuffers(0U, 1U, &vertex_view);
        D3D12_INDEX_BUFFER_VIEW index_view{};
        index_view.BufferLocation = draw.geometry.index_resource->GetGPUVirtualAddress() +
                                    draw.geometry.index_offset;
        index_view.SizeInBytes = draw.geometry.index_size;
        index_view.Format = DXGI_FORMAT_R16_UINT;
        list->IASetIndexBuffer(&index_view);
        D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(description.width),
                                static_cast<float>(description.height), 0.0F, 1.0F};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(description.width),
                           static_cast<LONG>(description.height)};
        list->RSSetViewports(1U, &viewport);
        list->RSSetScissorRects(1U, &scissor);
        list->DrawIndexedInstanced(draw.geometry.index_count, 1U, 0U, 0, 0U);
    }
    for (const AlphaTextureState& state : alpha_texture_states) {
        if (state.current == state.original) continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = d3d12_texture_resource(*state.texture);
        barrier.Transition.StateBefore = state.current;
        barrier.Transition.StateAfter = state.original;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }
    result = list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("Close(depth-only batch)", result);
        diagnostic.code = "depth_only_indexed_execution_failed";
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    result = context->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateFence(depth-only batch)", result);
        diagnostic.code = "depth_only_indexed_execution_failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        diagnostic = {"depth_only_indexed_execution_failed",
                      "CreateEventW failed for the D3D12 depth-only batch"};
        return false;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    context->queue->ExecuteCommandLists(1U, command_lists);
    constexpr UINT64 fence_value = 1U;
    result = context->queue->Signal(fence.Get(), fence_value);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value)
        result = fence->SetEventOnCompletion(fence_value, event);
    if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value &&
        WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    if (FAILED(result)) {
        const bool drained = context->wait_idle();
        depth_attachment.set_state(drained ? depth_state : D3D12_RESOURCE_STATE_COMMON);
        restore_alpha_texture_states(drained);
        diagnostic = hresult_error("D3D12 depth-only batch fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed"
                              : "depth_only_indexed_execution_failed";
        return false;
    }
    depth_attachment.set_state(depth_state);
    restore_alpha_texture_states(true);
    if (batch.clear_depth) depth_attachment.set_cleared();
    diagnostic = {};
    return true;
}

class D3D12Texture final : public Texture {
public:
    D3D12Texture(std::shared_ptr<D3D12Context> context,
                 ComPtr<ID3D12Resource> resource,
                 TextureDescription description,
                 D3D12_RESOURCE_STATES state,
                 bool initialized)
        : context_(std::move(context)), resource_(std::move(resource)), info_({description}),
          state_(state), initialized_(initialized),
          base_mip_initialized_(
              static_cast<std::size_t>(
                  texture_physical_array_layers(description)),
              0U) {}

    ~D3D12Texture() override {
        if (context_) context_->wait_idle();
    }
    Backend backend() const noexcept override { return Backend::D3D12; }
    const TextureInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const D3D12Context* context() const noexcept { return context_.get(); }
    [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
    [[nodiscard]] D3D12_RESOURCE_STATES state() const noexcept { return state_; }
    [[nodiscard]] D3D12_RESOURCE_STATES* state_pointer() noexcept {
        return &state_;
    }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool base_mip_initialized() const noexcept {
        return !base_mip_initialized_.empty() &&
               std::all_of(base_mip_initialized_.begin(),
                           base_mip_initialized_.end(),
                           [](std::uint8_t value) { return value != 0U; });
    }
    void mark_initialized() noexcept {
        initialized_ = true;
        std::fill(base_mip_initialized_.begin(), base_mip_initialized_.end(),
                  1U);
    }
    void set_state(D3D12_RESOURCE_STATES state) noexcept { state_ = state; }

    bool generate_mips(Diagnostic& diagnostic) {
        return execute_texture_mip_generation(
            context_, resource_.Get(), info_.description, state_, diagnostic);
    }

    bool upload(const TextureUploadPlan& uploads, Diagnostic& diagnostic) {
        const bool uploaded = execute_texture_upload(
            context_, resource_.Get(), info_.description, uploads, state_,
            texture_state(info_.description.usage,
                          info_.description.access_policy),
            diagnostic);
        initialized_ = initialized_ || uploaded;
        if (uploaded) {
            for (const TextureUpload& upload : uploads.subresources) {
                if (upload.mip_level != 0U) continue;
                const std::size_t layer = static_cast<std::size_t>(
                    texture_upload_physical_array_layer(
                        info_.description, upload));
                if (layer < base_mip_initialized_.size())
                    base_mip_initialized_[layer] = 1U;
            }
        }
        return uploaded;
    }

    bool clear_readback(const TextureClearReadbackRequest& request,
                        std::vector<std::byte>& output,
                        Diagnostic& diagnostic) {
        const bool cleared = clear_texture_readback(context_, resource_.Get(), info_.description, request,
                                                    state_, output, diagnostic);
        initialized_ = initialized_ || cleared;
        if (cleared && !base_mip_initialized_.empty())
            base_mip_initialized_[0U] = 1U;
        return cleared;
    }

    bool draw_triangle(const TriangleDrawRequest& request,
                       std::vector<std::byte>& output,
                       Diagnostic& diagnostic) {
        const bool drawn = draw_graphics_and_readback(context_, resource_.Get(), info_.description, request,
                                                      state_, output, diagnostic);
        initialized_ = initialized_ || drawn;
        if (drawn && !base_mip_initialized_.empty())
            base_mip_initialized_[0U] = 1U;
        return drawn;
    }

    bool draw_indexed_static_mesh(const IndexedStaticMeshDrawRequest& request,
                                  const D3D12Buffer& vertex_buffer,
                                  const D3D12Buffer& index_buffer,
                                  D3D12DepthAttachment* depth_attachment,
                                  std::vector<std::byte>& output,
                                  Diagnostic& diagnostic) {
        if (request.load_color && !initialized_) {
            diagnostic = {"indexed_color_load_before_clear",
                          "A color load was requested before the color attachment was initialized"};
            return false;
        }
        const bool drawn = draw_indexed_static_mesh_and_readback(
            context_, resource_.Get(), info_.description, request, vertex_buffer.resource(),
            index_buffer.resource(), state_, vertex_buffer.state(), index_buffer.state(), depth_attachment,
            output, diagnostic);
        initialized_ = initialized_ || drawn;
        if (drawn && !base_mip_initialized_.empty())
            base_mip_initialized_[0U] = 1U;
        return drawn;
    }

    bool draw_stock_ks_per_pixel_native(
        const IndexedStaticMeshDrawRequest& request,
        const D3D12Buffer& vertex_buffer,
        const D3D12Buffer& index_buffer,
        D3D12DepthAttachment* depth_attachment,
        std::vector<std::byte>& output,
        Diagnostic& diagnostic);

    bool draw_stock_ks_per_pixel_native_batch(
        const IndexedStaticMeshBatchDescription& batch,
        std::span<const D3D12StockKsPerPixelNativeBatchDraw> draws,
        D3D12DepthAttachment* depth_attachment,
        D3D12Texture* resolve_target,
        std::vector<std::byte>& output,
        Diagnostic& diagnostic);

    bool draw_indexed_static_mesh_batch(const IndexedStaticMeshBatchDescription& batch,
                                        std::span<const D3D12IndexedBatchDraw> draws,
                                        D3D12DepthAttachment* depth_attachment,
                                        D3D12Texture* resolve_target,
                                        std::vector<std::byte>& output,
                                        Diagnostic& diagnostic) {
        if (batch.load_color && !initialized_) {
            diagnostic = {"indexed_color_load_before_clear",
                          "A color load was requested before the color attachment was initialized"};
            return false;
        }
        const bool drawn = draw_indexed_static_mesh_batch_and_readback(
            context_, resource_.Get(), info_.description, batch, draws, depth_attachment,
            initialized_, state_,
            resolve_target != nullptr ? resolve_target->resource_.Get() : nullptr,
            resolve_target != nullptr ? &resolve_target->state_ : nullptr,
            resolve_target != nullptr ? &resolve_target->initialized_ : nullptr,
            resolve_target != nullptr
                ? texture_state(
                      resolve_target->info().description.usage,
                      resolve_target->info().description.access_policy)
                : D3D12_RESOURCE_STATE_RENDER_TARGET,
            output, diagnostic);
        initialized_ = initialized_ || drawn;
        if (drawn) {
            const std::size_t layer = static_cast<std::size_t>(
                texture_target_physical_array_layer(
                    info_.description, batch.target_subresource));
            if (layer < base_mip_initialized_.size())
                base_mip_initialized_[layer] = 1U;
            if (resolve_target != nullptr &&
                !resolve_target->base_mip_initialized_.empty())
                resolve_target->base_mip_initialized_[0U] = 1U;
        }
        return drawn;
    }

private:
    std::shared_ptr<D3D12Context> context_;
    ComPtr<ID3D12Resource> resource_;
    TextureInfo info_;
    D3D12_RESOURCE_STATES state_;
    bool initialized_ = false;
    std::vector<std::uint8_t> base_mip_initialized_;
};

D3D12_RESOURCE_STATES d3d12_texture_state(
    const D3D12Texture& texture) noexcept {
    return texture.state();
}

ID3D12Resource* d3d12_texture_resource(
    const D3D12Texture& texture) noexcept {
    return texture.resource();
}

const TextureDescription& d3d12_texture_description(
    const D3D12Texture& texture) noexcept {
    return texture.info().description;
}

void d3d12_texture_set_state(D3D12Texture& texture,
                             D3D12_RESOURCE_STATES state) noexcept {
    texture.set_state(state);
}

class D3D12PresentationTarget final : public PresentationTarget {
public:
    D3D12PresentationTarget(std::shared_ptr<D3D12Context> context,
                            const PresentationTargetDescription& description,
                            Diagnostic& diagnostic)
        : context_(std::move(context)), info_({description}) {
        valid_ = create(diagnostic);
        if (!valid_) reset();
    }

    ~D3D12PresentationTarget() override { reset(); }

    Backend backend() const noexcept override { return Backend::D3D12; }
    const PresentationTargetInfo& info() const noexcept override { return info_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<D3D12Context>& context() const noexcept { return context_; }

    PresentationFrameResult clear_and_present(const std::array<float, 4>& clear_color) {
        for (const float value : clear_color) {
            if (!std::isfinite(value)) {
                return {PresentationFrameStatus::invalid_request,
                        {"presentation_clear_color_non_finite",
                         "Presentation clear colors must contain finite values"}};
            }
        }
        if (!begin_frame()) return frame_failure("d3d12_presentation_target_invalid",
                                                 "The D3D12 presentation target is not initialized");
        ID3D12Resource* back_buffer = back_buffers_[frame_index_].Get();
        transition(back_buffer, D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(frame_index_);
        list_->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
        const float color[4] = {clear_color[0], clear_color[1], clear_color[2], clear_color[3]};
        list_->ClearRenderTargetView(rtv, color, 0U, nullptr);
        transition(back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
        return finish_frame();
    }

    PresentationFrameResult present_texture(D3D12Texture& source) {
        if (!source.initialized()) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_uninitialized",
                     "The presentation source must be rendered or initialized before use"}};
        }
        const TextureDescription& source_description = source.info().description;
        if (source_description.width != info_.description.width ||
            source_description.height != info_.description.height) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_dimensions_mismatch",
                     "The presentation source dimensions must match the target exactly"}};
        }
        if (source_description.format != info_.description.format) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_format_mismatch",
                     "The presentation source format must match the target exactly"}};
        }
        if (source_description.samples != 1U || source_description.mip_levels != 1U ||
            source_description.array_layers != 1U) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_shape_unsupported",
                     "The presentation source must have one mip, one layer, and one sample"}};
        }
        const auto usage = static_cast<std::uint32_t>(source_description.usage);
        if ((usage & static_cast<std::uint32_t>(TextureUsage::transfer_source)) == 0U ||
            (usage & static_cast<std::uint32_t>(TextureUsage::color_attachment)) == 0U) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_usage_invalid",
                     "The presentation source must support color attachment and transfer source usage"}};
        }
        if (!begin_frame()) return frame_failure("d3d12_presentation_target_invalid",
                                                 "The D3D12 presentation target is not initialized");
        ID3D12Resource* back_buffer = back_buffers_[frame_index_].Get();
        const D3D12_RESOURCE_STATES source_state = source.state();
        if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(source.resource(), source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyResource(back_buffer, source.resource());
        transition(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(source.resource(), D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        return finish_frame();
    }

private:
    static void record_transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after,
                                  ID3D12GraphicsCommandList* list) noexcept {
        if (before == after || resource == nullptr || list == nullptr) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1U, &barrier);
    }

    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) noexcept {
        record_transition(resource, before, after, list_.Get());
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(std::size_t index) const noexcept {
        auto result = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        result.ptr += static_cast<SIZE_T>(index) * rtv_stride_;
        return result;
    }

    [[nodiscard]] bool wait_fence() noexcept {
        if (fence_ == nullptr || fence_value_ == 0U ||
            fence_->GetCompletedValue() >= fence_value_)
            return true;
        if (FAILED(fence_->SetEventOnCompletion(fence_value_, fence_event_))) return false;
        return WaitForSingleObject(fence_event_, INFINITE) == WAIT_OBJECT_0;
    }

    [[nodiscard]] bool begin_frame() noexcept {
        if (!valid_ || context_ == nullptr || swapchain_ == nullptr || allocator_ == nullptr ||
            list_ == nullptr || !wait_fence())
            return false;
        frame_index_ = swapchain_->GetCurrentBackBufferIndex();
        if (frame_index_ >= back_buffers_.size()) return false;
        if (FAILED(allocator_->Reset())) return false;
        return SUCCEEDED(list_->Reset(allocator_.Get(), nullptr));
    }

    PresentationFrameResult finish_frame() {
        if (fence_value_ == std::numeric_limits<UINT64>::max())
            return frame_failure("d3d12_presentation_execution_failed",
                                 "D3D12 presentation fence value exhausted");
        HRESULT result = list_->Close();
        if (FAILED(result)) return failure("ID3D12GraphicsCommandList::Close", result);
        ID3D12CommandList* command_lists[] = {list_.Get()};
        context_->queue->ExecuteCommandLists(1U, command_lists);

        const UINT sync_interval = info_.description.vsync ? 1U : 0U;
        const HRESULT present_result = swapchain_->Present(sync_interval, 0U);
        const UINT64 submitted_value = fence_value_ + 1U;
        result = context_->queue->Signal(fence_.Get(), submitted_value);
        if (FAILED(result)) {
            (void)context_->wait_idle();
            if (present_result == DXGI_ERROR_DEVICE_REMOVED ||
                present_result == DXGI_ERROR_DEVICE_RESET)
                return frame_failure("d3d12_device_removed",
                                     "The D3D12 device was removed or reset");
            return failure("ID3D12CommandQueue::Signal(presentation)", result);
        }
        fence_value_ = submitted_value;
        const bool waited = wait_fence();
        if (!waited) return frame_failure("d3d12_presentation_execution_failed",
                                          "D3D12 presentation fence wait failed");
        if (present_result == DXGI_ERROR_INVALID_CALL ||
            present_result == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS)
            return frame_failure("d3d12_presentation_target_out_of_date",
                                 "The D3D12 presentation target requires recreation");
        if (present_result == DXGI_ERROR_DEVICE_REMOVED ||
            present_result == DXGI_ERROR_DEVICE_RESET)
            return frame_failure("d3d12_device_removed", "The D3D12 device was removed or reset");
        if (FAILED(present_result)) return failure("IDXGISwapChain::Present", present_result);
        return {PresentationFrameStatus::ready, {}};
    }

    static PresentationFrameResult frame_failure(const char* code, const char* message) {
        return {PresentationFrameStatus::execution_failed, {code, message}};
    }

    static PresentationFrameResult failure(const char* operation, HRESULT result) {
        Diagnostic diagnostic = hresult_error(operation, result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed"
                              : "d3d12_presentation_execution_failed";
        return {PresentationFrameStatus::execution_failed, std::move(diagnostic)};
    }

    bool create(Diagnostic& diagnostic) {
        if (context_ == nullptr || context_->factory == nullptr || context_->device == nullptr ||
            context_->queue == nullptr || context_->native_window == nullptr) {
            diagnostic = {"d3d12_native_window_required",
                          "D3D12 presentation requires an initialized Win32 window"};
            return false;
        }
        const HWND window = static_cast<HWND>(context_->native_window);
        if (!IsWindow(window)) {
            diagnostic = {"d3d12_native_window_invalid",
                          "D3D12 native presentation requires a live Win32 window handle"};
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = info_.description.width;
        description.Height = info_.description.height;
        description.Format = dxgi_texture_format(info_.description.format);
        description.Stereo = FALSE;
        description.SampleDesc.Count = 1U;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = info_.description.image_count;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> swapchain;
        HRESULT result = context_->factory->CreateSwapChainForHwnd(
            context_->queue.Get(), window, &description, nullptr, nullptr, &swapchain);
        if (FAILED(result)) {
            diagnostic = hresult_error("IDXGIFactory2::CreateSwapChainForHwnd", result);
            diagnostic.code = "d3d12_presentation_target_allocation_failed";
            return false;
        }
        (void)context_->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        result = swapchain.As(&swapchain_);
        if (FAILED(result)) {
            diagnostic = hresult_error("IDXGISwapChain1::QueryInterface", result);
            diagnostic.code = "d3d12_presentation_target_allocation_failed";
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 actual{};
        result = swapchain_->GetDesc1(&actual);
        if (FAILED(result) || actual.BufferCount == 0U || actual.BufferCount > max_presentation_image_count) {
            diagnostic = {"d3d12_presentation_images_unavailable",
                          "The D3D12 swapchain returned an invalid bounded image count"};
            return false;
        }
        info_.description.width = actual.Width;
        info_.description.height = actual.Height;
        info_.description.image_count = actual.BufferCount;
        rtv_stride_ = context_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
        rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_description.NumDescriptors = actual.BufferCount;
        result = context_->device->CreateDescriptorHeap(&rtv_description, IID_PPV_ARGS(&rtv_heap_));
        if (FAILED(result)) {
            diagnostic = hresult_error("ID3D12Device::CreateDescriptorHeap(presentation RTV)", result);
            diagnostic.code = "d3d12_presentation_target_allocation_failed";
            return false;
        }
        back_buffers_.resize(actual.BufferCount);
        for (UINT index = 0U; index < actual.BufferCount; ++index) {
            result = swapchain_->GetBuffer(index, IID_PPV_ARGS(&back_buffers_[index]));
            if (FAILED(result)) {
                diagnostic = hresult_error("IDXGISwapChain::GetBuffer(presentation)", result);
                diagnostic.code = "d3d12_presentation_target_allocation_failed";
                return false;
            }
            context_->device->CreateRenderTargetView(back_buffers_[index].Get(), nullptr,
                                                      rtv_handle(index));
        }
        result = context_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           IID_PPV_ARGS(&allocator_));
        if (SUCCEEDED(result))
            result = context_->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         allocator_.Get(), nullptr,
                                                         IID_PPV_ARGS(&list_));
        if (SUCCEEDED(result)) result = list_->Close();
        if (FAILED(result)) {
            diagnostic = hresult_error("D3D12 presentation command setup", result);
            diagnostic.code = "d3d12_presentation_target_allocation_failed";
            return false;
        }
        result = context_->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(result) || fence_event_ == nullptr) {
            diagnostic = {"d3d12_presentation_target_allocation_failed",
                          "D3D12 presentation synchronization setup failed"};
            return false;
        }
        return true;
    }

    void reset() noexcept {
        std::unique_lock<std::mutex> command_guard;
        if (context_ != nullptr) {
            command_guard = std::unique_lock<std::mutex>(context_->command_mutex);
            (void)wait_fence();
        }
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        fence_.Reset();
        list_.Reset();
        allocator_.Reset();
        back_buffers_.clear();
        rtv_heap_.Reset();
        swapchain_.Reset();
        if (context_ != nullptr) context_->presentation_target_active = false;
        valid_ = false;
    }

    std::shared_ptr<D3D12Context> context_;
    PresentationTargetInfo info_;
    ComPtr<IDXGISwapChain3> swapchain_;
    std::vector<ComPtr<ID3D12Resource>> back_buffers_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT rtv_stride_ = 0U;
    UINT frame_index_ = 0U;
    UINT64 fence_value_ = 0U;
    bool valid_ = false;
};

class D3D12Sampler final : public Sampler {
public:
    D3D12Sampler(std::shared_ptr<D3D12Context> context,
                 D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
                 D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
                 SamplerDescription description)
        : context_(std::move(context)), descriptor_(descriptor), gpu_descriptor_(gpu_descriptor), info_({description}) {}
    Backend backend() const noexcept override { return Backend::D3D12; }
    const SamplerInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const D3D12Context* context() const noexcept { return context_.get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor() const noexcept { return gpu_descriptor_; }

private:
    std::shared_ptr<D3D12Context> context_;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_{};
    SamplerInfo info_;
};

D3D12_GPU_DESCRIPTOR_HANDLE d3d12_sampler_gpu(
    const D3D12Sampler& sampler) noexcept {
    return sampler.gpu_descriptor();
}

bool inspect_d3d12_portable_cloud_texture(
    const Texture* requested, const D3D12Context* context,
    D3D12PortableCloudTextureResource& output,
    Diagnostic& diagnostic) {
    output = {};
    const auto* texture = dynamic_cast<const D3D12Texture*>(requested);
    if (texture == nullptr) {
        diagnostic = {"portable_cloud_texture_type_unsupported",
                      "Portable cloud textures must be D3D12 texture handles"};
        return false;
    }
    if (texture->context() != context) {
        diagnostic = {"portable_cloud_context_mismatch",
                      "Portable cloud textures must belong to the D3D12 device"};
        return false;
    }
    output.resource = texture->resource();
    output.format = dxgi_texture_format(texture->info().description.format);
    output.tracked_state =
        const_cast<D3D12Texture*>(texture)->state_pointer();
    output.original_state = texture->state();
    output.description = texture->info().description;
    output.initialized = texture->initialized();
    diagnostic = {};
    return true;
}

bool inspect_d3d12_portable_cloud_sampler(
    const Sampler* requested, const D3D12Context* context,
    D3D12_GPU_DESCRIPTOR_HANDLE& output,
    Diagnostic& diagnostic) {
    output = {};
    const auto* sampler = dynamic_cast<const D3D12Sampler*>(requested);
    if (sampler == nullptr) {
        diagnostic = {"portable_cloud_sampler_type_unsupported",
                      "Portable cloud samplers must be D3D12 sampler handles"};
        return false;
    }
    if (sampler->context() != context || sampler->gpu_descriptor().ptr == 0U) {
        diagnostic = {"portable_cloud_context_mismatch",
                      "Portable cloud samplers must belong to the D3D12 device"};
        return false;
    }
    output = sampler->gpu_descriptor();
    diagnostic = {};
    return true;
}

bool inspect_d3d12_portable_grass_texture(
    const Texture* requested, const D3D12Context* context,
    D3D12PortableGrassTextureResource& output,
    Diagnostic& diagnostic) {
    output = {};
    const auto* texture = dynamic_cast<const D3D12Texture*>(requested);
    if (texture == nullptr) {
        diagnostic = {"portable_grass_texture_type_unsupported",
                      "Portable grass atlases must be D3D12 texture handles"};
        return false;
    }
    if (texture->context() != context) {
        diagnostic = {"portable_grass_context_mismatch",
                      "Portable grass atlases must belong to the D3D12 device"};
        return false;
    }
    output.resource = texture->resource();
    output.format = dxgi_texture_format(texture->info().description.format);
    output.tracked_state = const_cast<D3D12Texture*>(texture)->state_pointer();
    output.original_state = texture->state();
    output.description = texture->info().description;
    output.initialized = texture->initialized();
    diagnostic = {};
    return true;
}

bool inspect_d3d12_portable_grass_sampler(
    const Sampler* requested, const D3D12Context* context,
    D3D12_GPU_DESCRIPTOR_HANDLE& output,
    Diagnostic& diagnostic) {
    output = {};
    const auto* sampler = dynamic_cast<const D3D12Sampler*>(requested);
    if (sampler == nullptr) {
        diagnostic = {"portable_grass_sampler_type_unsupported",
                      "Portable grass samplers must be D3D12 sampler handles"};
        return false;
    }
    if (sampler->context() != context || sampler->gpu_descriptor().ptr == 0U) {
        diagnostic = {"portable_grass_context_mismatch",
                      "Portable grass samplers must belong to the D3D12 device"};
        return false;
    }
    output = sampler->gpu_descriptor();
    diagnostic = {};
    return true;
}

bool prepare_d3d12_material_binding(
    const std::shared_ptr<D3D12Context>& context,
    const IndexedStaticMeshDrawRequest& request,
    ID3D12DescriptorHeap* srv_heap,
    UINT srv_index,
    UINT cbv_index,
    UINT frame_cbv_index,
    UINT normal_srv_index,
    UINT maps_srv_index,
    UINT detail_srv_index,
    UINT normal_detail_srv_index,
    UINT damage_srv_index,
    UINT damage_mask_srv_index,
    UINT shadow_srv_index,
    UINT shadow_cbv_index,
    UINT multimap_cube_srv_index,
    UINT multimap_reflection_cbv_index,
    D3D12MaterialDescriptorBinding& output,
    Diagnostic& diagnostic) {
    output = {};
    if (request.pipeline == nullptr) {
        diagnostic = {"indexed_resource_pipeline_missing",
                      "D3D12 material binding preparation requires a pipeline"};
        return false;
    }
    if (request.pipeline->resources.empty()) {
        if (request.sampled_binding.texture != nullptr || request.sampled_binding.sampler != nullptr ||
            request.normal_binding.texture != nullptr || request.normal_binding.sampler != nullptr ||
            request.maps_binding.texture != nullptr || request.maps_binding.sampler != nullptr ||
            request.detail_binding.texture != nullptr || request.detail_binding.sampler != nullptr ||
            request.normal_detail_binding.texture != nullptr || request.normal_detail_binding.sampler != nullptr ||
            request.damage_binding.texture != nullptr || request.damage_binding.sampler != nullptr ||
            request.damage_mask_binding.texture != nullptr ||
            request.damage_mask_binding.sampler != nullptr ||
            std::any_of(request.directional_shadow_binding.maps.begin(),
                        request.directional_shadow_binding.maps.end(),
                        [](const DepthAttachment* map) { return map != nullptr; }) ||
            request.directional_shadow_binding.sampler != nullptr ||
            request.directional_shadow_binding.constants != nullptr ||
            request.multimap_reflection_binding.cube.texture != nullptr ||
            request.multimap_reflection_binding.cube.sampler != nullptr ||
            request.multimap_reflection_binding.constants.buffer != nullptr ||
            request.multimap_reflection_binding.constants.offset_bytes != 0U ||
            request.multimap_reflection_binding.constants.range_bytes != 0U ||
            request.material_binding.buffer != nullptr || request.material_binding.offset_bytes != 0U ||
            request.material_binding.range_bytes != 0U || request.frame_binding.buffer != nullptr ||
            request.frame_binding.offset_bytes != 0U || request.frame_binding.range_bytes != 0U) {
            diagnostic = {"indexed_resource_binding_unexpected",
                          "A resource-free D3D12 pipeline cannot receive material bindings"};
            return false;
        }
        return true;
    }
    if (request.sampled_binding.texture == nullptr || request.sampled_binding.sampler == nullptr) {
        diagnostic = {"indexed_resource_binding_missing",
                      "D3D12 material binding requires both a sampled texture and sampler"};
        return false;
    }
    const auto* texture = dynamic_cast<const D3D12Texture*>(request.sampled_binding.texture);
    const auto* sampler = dynamic_cast<const D3D12Sampler*>(request.sampled_binding.sampler);
    if (texture == nullptr || sampler == nullptr) {
        diagnostic = {"indexed_resource_type_unsupported",
                      "The D3D12 device received an unknown material resource handle"};
        return false;
    }
    const bool has_material_constants = d3d12_pipeline_has_material_constants(*request.pipeline);
    const bool has_frame_constants = d3d12_pipeline_has_frame_constants(*request.pipeline);
    const bool has_normal_texture = d3d12_pipeline_has_normal_texture(*request.pipeline);
    const bool has_maps_texture = d3d12_pipeline_has_maps_texture(*request.pipeline);
    const bool has_detail_texture = d3d12_pipeline_has_detail_texture(*request.pipeline);
    const bool has_normal_detail_texture = d3d12_pipeline_has_normal_detail_texture(*request.pipeline);
    const bool has_damage_texture = d3d12_pipeline_has_damage_texture(*request.pipeline);
    const bool has_damage_mask_texture = d3d12_pipeline_has_damage_mask_texture(*request.pipeline);
    const bool has_shadow_receiver = d3d12_pipeline_has_directional_shadow_receiver(*request.pipeline);
    const bool has_multimap_reflection =
        d3d12_pipeline_has_multimap_reflection(*request.pipeline);
    const D3D12Texture* normal_texture = nullptr;
    const D3D12Sampler* normal_sampler = nullptr;
    const D3D12Texture* maps_texture = nullptr;
    const D3D12Sampler* maps_sampler = nullptr;
    const D3D12Texture* detail_texture = nullptr;
    const D3D12Sampler* detail_sampler = nullptr;
    const D3D12Texture* normal_detail_texture = nullptr;
    const D3D12Sampler* normal_detail_sampler = nullptr;
    const D3D12Texture* damage_texture = nullptr;
    const D3D12Sampler* damage_sampler = nullptr;
    const D3D12Texture* damage_mask_texture = nullptr;
    const D3D12Sampler* damage_mask_sampler = nullptr;
    std::array<const D3D12DepthAttachment*, indexed_directional_shadow_cascade_count> shadow_attachments{};
    const D3D12Sampler* shadow_sampler = nullptr;
    const D3D12Buffer* shadow_constants = nullptr;
    const D3D12Texture* multimap_cube = nullptr;
    const D3D12Sampler* multimap_cube_sampler = nullptr;
    const D3D12Buffer* multimap_reflection_constants = nullptr;
    if (has_normal_texture) {
        if (request.normal_binding.texture == nullptr || request.normal_binding.sampler == nullptr) {
            diagnostic = {"indexed_normal_binding_missing",
                          "D3D12 normal binding requires both a texture and sampler"};
            return false;
        }
        normal_texture = dynamic_cast<const D3D12Texture*>(request.normal_binding.texture);
        normal_sampler = dynamic_cast<const D3D12Sampler*>(request.normal_binding.sampler);
        if (normal_texture == nullptr || normal_sampler == nullptr) {
            diagnostic = {"indexed_normal_type_unsupported",
                          "The D3D12 device received an unknown normal resource handle"};
            return false;
        }
    } else if (request.normal_binding.texture != nullptr || request.normal_binding.sampler != nullptr) {
        diagnostic = {"indexed_normal_binding_unexpected",
                      "The D3D12 diffuse ABI cannot receive a normal texture or sampler"};
        return false;
    }
    if (has_maps_texture) {
        if (request.maps_binding.texture == nullptr || request.maps_binding.sampler == nullptr) {
            diagnostic = {"indexed_maps_binding_missing",
                          "D3D12 maps binding requires both a texture and sampler"};
            return false;
        }
        maps_texture = dynamic_cast<const D3D12Texture*>(request.maps_binding.texture);
        maps_sampler = dynamic_cast<const D3D12Sampler*>(request.maps_binding.sampler);
        if (maps_texture == nullptr || maps_sampler == nullptr) {
            diagnostic = {"indexed_maps_type_unsupported",
                          "The D3D12 device received an unknown maps resource handle"};
            return false;
        }
    } else if (request.maps_binding.texture != nullptr || request.maps_binding.sampler != nullptr) {
        diagnostic = {"indexed_maps_binding_unexpected",
                      "The D3D12 diffuse ABI cannot receive a maps texture or sampler"};
        return false;
    }
    if (has_detail_texture) {
        if (request.detail_binding.texture == nullptr || request.detail_binding.sampler == nullptr) {
            diagnostic = {"indexed_detail_binding_missing",
                          "D3D12 detail binding requires both a texture and sampler"};
            return false;
        }
        detail_texture = dynamic_cast<const D3D12Texture*>(request.detail_binding.texture);
        detail_sampler = dynamic_cast<const D3D12Sampler*>(request.detail_binding.sampler);
        if (detail_texture == nullptr || detail_sampler == nullptr) {
            diagnostic = {"indexed_detail_type_unsupported",
                          "The D3D12 device received an unknown detail resource handle"};
            return false;
        }
    } else if (request.detail_binding.texture != nullptr || request.detail_binding.sampler != nullptr) {
        diagnostic = {"indexed_detail_binding_unexpected",
                      "The D3D12 material ABI cannot receive a detail texture or sampler"};
        return false;
    }
    if (has_normal_detail_texture) {
        if (request.normal_detail_binding.texture == nullptr || request.normal_detail_binding.sampler == nullptr) {
            diagnostic = {"indexed_normal_detail_binding_missing",
                          "D3D12 normal-detail binding requires both a texture and sampler"};
            return false;
        }
        normal_detail_texture = dynamic_cast<const D3D12Texture*>(request.normal_detail_binding.texture);
        normal_detail_sampler = dynamic_cast<const D3D12Sampler*>(request.normal_detail_binding.sampler);
        if (normal_detail_texture == nullptr || normal_detail_sampler == nullptr) {
            diagnostic = {"indexed_normal_detail_type_unsupported",
                          "The D3D12 device received an unknown normal-detail resource handle"};
            return false;
        }
    } else if (request.normal_detail_binding.texture != nullptr || request.normal_detail_binding.sampler != nullptr) {
        diagnostic = {"indexed_normal_detail_binding_unexpected",
                      "The D3D12 material ABI cannot receive a normal-detail texture or sampler"};
        return false;
    }
    const auto resolve_damage_binding =
        [&](bool declared, const IndexedSampledTextureBinding& binding,
            const char* role, const D3D12Texture*& output_texture,
            const D3D12Sampler*& output_sampler) {
            if (!declared) {
                if (binding.texture != nullptr || binding.sampler != nullptr) {
                    diagnostic = {std::string("indexed_") + role + "_binding_unexpected",
                                  "The D3D12 material ABI cannot receive an undeclared damage binding"};
                    return false;
                }
                return true;
            }
            if (binding.texture == nullptr || binding.sampler == nullptr) {
                diagnostic = {std::string("indexed_") + role + "_binding_missing",
                              "A D3D12 damage binding requires both a texture and sampler"};
                return false;
            }
            output_texture = dynamic_cast<const D3D12Texture*>(binding.texture);
            output_sampler = dynamic_cast<const D3D12Sampler*>(binding.sampler);
            if (output_texture == nullptr || output_sampler == nullptr) {
                diagnostic = {std::string("indexed_") + role + "_type_unsupported",
                              "The D3D12 device received an unknown damage resource handle"};
                return false;
            }
            return true;
        };
    if (!resolve_damage_binding(has_damage_texture, request.damage_binding, "damage",
                                damage_texture, damage_sampler) ||
        !resolve_damage_binding(has_damage_mask_texture, request.damage_mask_binding,
                                "damage_mask", damage_mask_texture,
                                damage_mask_sampler))
        return false;
    if (has_damage_texture != has_damage_mask_texture) {
        diagnostic = {"indexed_damage_layout_unsupported",
                      "The D3D12 damage ABI requires both damage and damage-mask bindings"};
        return false;
    }
    if (has_multimap_reflection) {
        multimap_cube = dynamic_cast<const D3D12Texture*>(
            request.multimap_reflection_binding.cube.texture);
        multimap_cube_sampler = dynamic_cast<const D3D12Sampler*>(
            request.multimap_reflection_binding.cube.sampler);
        multimap_reflection_constants = dynamic_cast<const D3D12Buffer*>(
            request.multimap_reflection_binding.constants.buffer);
        if (multimap_cube == nullptr || multimap_cube_sampler == nullptr ||
            multimap_reflection_constants == nullptr) {
            diagnostic = {
                "indexed_multimap_reflection_resource_type_unsupported",
                "D3D12 MultiMap reflection requires cube, sampler, and uniform-buffer handles"};
            return false;
        }
        if (multimap_cube->context() != context.get() ||
            multimap_cube_sampler->context() != context.get() ||
            multimap_reflection_constants->context() != context.get()) {
            diagnostic = {
                "indexed_multimap_reflection_resource_context_mismatch",
                "D3D12 MultiMap reflection resources belong to another device"};
            return false;
        }
        const TextureDescription& cube_description =
            multimap_cube->info().description;
        const auto cube_usage =
            static_cast<std::uint32_t>(cube_description.usage);
        const DXGI_FORMAT cube_format =
            dxgi_texture_format(cube_description.format);
        if (multimap_cube->resource() == nullptr ||
            !multimap_cube->initialized() ||
            cube_description.shape != TextureShape::texture_cube ||
            cube_description.array_layers != 1U ||
            cube_description.width == 0U ||
            cube_description.width != cube_description.height ||
            cube_description.mip_levels == 0U ||
            cube_description.samples != 1U ||
            cube_format == DXGI_FORMAT_UNKNOWN ||
            (cube_usage &
             static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (cube_usage &
             static_cast<std::uint32_t>(TextureUsage::storage)) != 0U ||
            (static_cast<UINT>(multimap_cube->state()) &
             static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) ==
                0U ||
            multimap_cube_sampler->gpu_descriptor().ptr == 0U) {
            diagnostic = {
                "indexed_multimap_reflection_cube_invalid",
                "D3D12 MultiMap reflection requires one initialized sampled square cube"};
            return false;
        }
        if (multimap_cube_sampler->info().description.compare !=
            SamplerCompare::disabled) {
            diagnostic = {
                "indexed_multimap_reflection_sampler_invalid",
                "D3D12 MultiMap reflection requires comparison disabled"};
            return false;
        }
        const BufferDescription& constants_description =
            multimap_reflection_constants->info().description;
        const IndexedMaterialBufferBinding& constants =
            request.multimap_reflection_binding.constants;
        constexpr std::uint64_t reflection_range =
            portable_multimap_reflection_buffer_view_bytes;
        if (multimap_reflection_constants->resource() == nullptr ||
            constants_description.usage != BufferUsage::uniform ||
            (static_cast<UINT>(multimap_reflection_constants->state()) &
             static_cast<UINT>(
                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)) == 0U ||
            constants.offset_bytes %
                    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT !=
                0U ||
            constants.range_bytes != reflection_range ||
            constants.offset_bytes > constants_description.size_bytes ||
            reflection_range >
                constants_description.size_bytes - constants.offset_bytes) {
            diagnostic = {
                "indexed_multimap_reflection_constants_invalid",
                "D3D12 MultiMap reflection constants violate the 256-byte CBV contract"};
            return false;
        }
        const D3D12_RESOURCE_DESC native_constants =
            multimap_reflection_constants->resource()->GetDesc();
        if (constants.offset_bytes > native_constants.Width ||
            reflection_range > native_constants.Width - constants.offset_bytes) {
            diagnostic = {
                "indexed_multimap_reflection_constants_invalid",
                "D3D12 MultiMap reflection constants exceed the native buffer"};
            return false;
        }
        const UINT64 gpu_base =
            multimap_reflection_constants->resource()->GetGPUVirtualAddress();
        if (gpu_base == 0U ||
            gpu_base > std::numeric_limits<UINT64>::max() -
                           constants.offset_bytes ||
            (gpu_base + constants.offset_bytes) %
                    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT !=
                0U) {
            diagnostic = {
                "indexed_multimap_reflection_constants_gpu_address_invalid",
                "D3D12 MultiMap reflection constants GPU address is unavailable or unaligned"};
            return false;
        }
    } else if (request.multimap_reflection_binding.cube.texture != nullptr ||
               request.multimap_reflection_binding.cube.sampler != nullptr ||
               request.multimap_reflection_binding.constants.buffer != nullptr ||
               request.multimap_reflection_binding.constants.offset_bytes != 0U ||
               request.multimap_reflection_binding.constants.range_bytes != 0U) {
        diagnostic = {
            "indexed_multimap_reflection_binding_unexpected",
            "A D3D12 pipeline without the reflection extension cannot receive reflection resources"};
        return false;
    }
    if (has_shadow_receiver) {
        if (request.directional_shadow_binding.sampler == nullptr ||
            request.directional_shadow_binding.constants == nullptr) {
            diagnostic = {"indexed_directional_shadow_binding_missing",
                          "D3D12 directional-shadow receiver requires maps, sampler, and constants"};
            return false;
        }
        shadow_sampler = dynamic_cast<const D3D12Sampler*>(request.directional_shadow_binding.sampler);
        shadow_constants = dynamic_cast<const D3D12Buffer*>(request.directional_shadow_binding.constants);
        if (shadow_sampler == nullptr || shadow_constants == nullptr) {
            diagnostic = {"indexed_directional_shadow_resource_type_unsupported",
                          "The D3D12 directional-shadow receiver received an unknown resource handle"};
            return false;
        }
        if (shadow_sampler->context() != context.get() || shadow_constants->context() != context.get()) {
            diagnostic = {"indexed_directional_shadow_resource_context_mismatch",
                          "D3D12 directional-shadow resources belong to another device"};
            return false;
        }
        if (shadow_sampler->gpu_descriptor().ptr == 0U) {
            diagnostic = {"indexed_directional_shadow_sampler_invalid",
                          "D3D12 directional-shadow sampler has no shader-visible descriptor"};
            return false;
        }
        for (std::size_t cascade = 0U; cascade < indexed_directional_shadow_cascade_count; ++cascade) {
            shadow_attachments[cascade] =
                dynamic_cast<const D3D12DepthAttachment*>(request.directional_shadow_binding.maps[cascade]);
            if (shadow_attachments[cascade] == nullptr ||
                shadow_attachments[cascade]->context() != context.get() ||
                shadow_attachments[cascade]->resource() == nullptr ||
                !shadow_attachments[cascade]->cleared() ||
                shadow_attachments[cascade]->srv().ptr == 0U) {
                diagnostic = {"indexed_directional_shadow_map_invalid",
                              "D3D12 directional-shadow maps must be initialized shader-readable depth attachments"};
                return false;
            }
        }
        if (shadow_constants->resource() == nullptr ||
            shadow_constants->info().description.usage != BufferUsage::uniform) {
            diagnostic = {"indexed_directional_shadow_constants_usage_invalid",
                          "D3D12 directional-shadow constants require an initialized uniform buffer"};
            return false;
        }
        constexpr UINT required_buffer_state =
            static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        if ((static_cast<UINT>(shadow_constants->state()) & required_buffer_state) == 0U) {
            diagnostic = {"indexed_directional_shadow_constants_state_invalid",
                          "D3D12 directional-shadow constants are not in constant-buffer state"};
            return false;
        }
        const std::uint64_t offset = request.directional_shadow_binding.constants_offset_bytes;
        const std::uint64_t logical_range = request.directional_shadow_binding.constants_range_bytes;
        constexpr std::uint64_t cbv_range = portable_directional_shadow_buffer_view_bytes;
        const std::uint64_t buffer_size = shadow_constants->info().description.size_bytes;
        if (offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0U ||
            (logical_range != portable_directional_shadow_buffer_view_bytes &&
             logical_range != stock_directional_shadow_buffer_view_bytes) ||
            offset > buffer_size || cbv_range > buffer_size - offset) {
            diagnostic = {"indexed_directional_shadow_constants_backing_range_invalid",
                          "D3D12 directional-shadow CBVs require a bounded 256-byte backing range for the logical stock or portable view"};
            return false;
        }
        const D3D12_RESOURCE_DESC resource_description = shadow_constants->resource()->GetDesc();
        if (offset > resource_description.Width || cbv_range > resource_description.Width - offset) {
            diagnostic = {"indexed_directional_shadow_constants_backing_range_invalid",
                          "D3D12 directional-shadow CBV exceeds the native buffer resource"};
            return false;
        }
        const UINT64 gpu_base = shadow_constants->resource()->GetGPUVirtualAddress();
        if (gpu_base == 0U || gpu_base > std::numeric_limits<UINT64>::max() - offset) {
            diagnostic = {"indexed_directional_shadow_constants_gpu_address_invalid",
                          "D3D12 directional-shadow constants GPU address is unavailable or overflows"};
            return false;
        }
        const UINT64 gpu_address = gpu_base + offset;
        if (gpu_address % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0U ||
            gpu_address > std::numeric_limits<UINT64>::max() - cbv_range) {
            diagnostic = {"indexed_directional_shadow_constants_alignment_invalid",
                          "D3D12 directional-shadow CBV address is not 256-byte aligned or overflows"};
            return false;
        }
    }
    if (texture->context() != context.get() || sampler->context() != context.get()) {
        diagnostic = {"indexed_resource_context_mismatch",
                      "D3D12 material resources belong to another device"};
        return false;
    }
    if (has_normal_texture &&
        (normal_texture->context() != context.get() || normal_sampler->context() != context.get())) {
        diagnostic = {"indexed_normal_context_mismatch",
                      "D3D12 normal resources belong to another device"};
        return false;
    }
    if (has_maps_texture &&
        (maps_texture->context() != context.get() || maps_sampler->context() != context.get())) {
        diagnostic = {"indexed_maps_context_mismatch",
                      "D3D12 maps resources belong to another device"};
        return false;
    }
    if (has_detail_texture &&
        (detail_texture->context() != context.get() || detail_sampler->context() != context.get())) {
        diagnostic = {"indexed_detail_context_mismatch",
                      "D3D12 detail resources belong to another device"};
        return false;
    }
    if (has_normal_detail_texture &&
        (normal_detail_texture->context() != context.get() ||
         normal_detail_sampler->context() != context.get())) {
        diagnostic = {"indexed_normal_detail_context_mismatch",
                      "D3D12 normal-detail resources belong to another device"};
        return false;
    }
    if (has_damage_texture &&
        (damage_texture->context() != context.get() || damage_sampler->context() != context.get() ||
         damage_mask_texture->context() != context.get() ||
         damage_mask_sampler->context() != context.get())) {
        diagnostic = {"indexed_damage_context_mismatch",
                      "D3D12 damage resources belong to another device"};
        return false;
    }
    if (texture->resource() == nullptr || !texture->initialized()) {
        diagnostic = {"indexed_resource_texture_uninitialized",
                      "A D3D12 sampled texture must contain uploaded data before drawing"};
        return false;
    }
    if (has_normal_texture && (normal_texture->resource() == nullptr || !normal_texture->initialized())) {
        diagnostic = {"indexed_normal_texture_uninitialized",
                      "A D3D12 normal texture must contain uploaded data before drawing"};
        return false;
    }
    if (has_maps_texture && (maps_texture->resource() == nullptr || !maps_texture->initialized())) {
        diagnostic = {"indexed_maps_texture_uninitialized",
                      "A D3D12 maps texture must contain uploaded data before drawing"};
        return false;
    }
    if (has_detail_texture && (detail_texture->resource() == nullptr || !detail_texture->initialized())) {
        diagnostic = {"indexed_detail_texture_uninitialized",
                      "A D3D12 detail texture must contain uploaded data before drawing"};
        return false;
    }
    if (has_normal_detail_texture &&
        (normal_detail_texture->resource() == nullptr || !normal_detail_texture->initialized())) {
        diagnostic = {"indexed_normal_detail_texture_uninitialized",
                      "A D3D12 normal-detail texture must contain uploaded data before drawing"};
        return false;
    }
    if (has_damage_texture &&
        (damage_texture->resource() == nullptr || !damage_texture->initialized() ||
         damage_mask_texture->resource() == nullptr || !damage_mask_texture->initialized())) {
        diagnostic = {"indexed_damage_texture_uninitialized",
                      "D3D12 damage textures must contain uploaded data before drawing"};
        return false;
    }
    constexpr UINT required_state = static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if ((static_cast<UINT>(texture->state()) & required_state) == 0U) {
        diagnostic = {"indexed_resource_texture_state_invalid",
                      "A D3D12 sampled texture is not in pixel-shader-resource state"};
        return false;
    }
    if (has_normal_texture &&
        (static_cast<UINT>(normal_texture->state()) & required_state) == 0U) {
        diagnostic = {"indexed_normal_texture_state_invalid",
                      "A D3D12 normal texture is not in pixel-shader-resource state"};
        return false;
    }
    if (has_maps_texture &&
        (static_cast<UINT>(maps_texture->state()) & required_state) == 0U) {
        diagnostic = {"indexed_maps_texture_state_invalid",
                      "A D3D12 maps texture is not in pixel-shader-resource state"};
        return false;
    }
    if (has_detail_texture &&
        (static_cast<UINT>(detail_texture->state()) & required_state) == 0U) {
        diagnostic = {"indexed_detail_texture_state_invalid",
                      "A D3D12 detail texture is not in pixel-shader-resource state"};
        return false;
    }
    if (has_normal_detail_texture &&
        (static_cast<UINT>(normal_detail_texture->state()) & required_state) == 0U) {
        diagnostic = {"indexed_normal_detail_texture_state_invalid",
                      "A D3D12 normal-detail texture is not in pixel-shader-resource state"};
        return false;
    }
    if (has_damage_texture &&
        (((static_cast<UINT>(damage_texture->state()) & required_state) == 0U) ||
         ((static_cast<UINT>(damage_mask_texture->state()) & required_state) == 0U))) {
        diagnostic = {"indexed_damage_texture_state_invalid",
                      "D3D12 damage textures are not in pixel-shader-resource state"};
        return false;
    }
    if (srv_heap == nullptr) {
        diagnostic = {"indexed_resource_descriptor_heap_missing",
                      "D3D12 material binding requires a shader-visible SRV heap"};
        return false;
    }
    if (context == nullptr || context->device == nullptr || context->sampler_heap == nullptr) {
        diagnostic = {"indexed_resource_context_unavailable",
                      "D3D12 material binding requires an initialized device and sampler heap"};
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC heap_description = srv_heap->GetDesc();
    if (heap_description.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        (heap_description.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0U) {
        diagnostic = {"indexed_resource_descriptor_slot_invalid",
                      "D3D12 material SRV descriptor slot is outside its bounded shader-visible heap"};
        return false;
    }
    if (srv_index >= heap_description.NumDescriptors ||
        (has_material_constants && cbv_index >= heap_description.NumDescriptors) ||
        (has_frame_constants && frame_cbv_index >= heap_description.NumDescriptors) ||
        (has_normal_texture && normal_srv_index >= heap_description.NumDescriptors) ||
        (has_maps_texture && maps_srv_index >= heap_description.NumDescriptors) ||
        (has_detail_texture && detail_srv_index >= heap_description.NumDescriptors) ||
        (has_normal_detail_texture && normal_detail_srv_index >= heap_description.NumDescriptors) ||
        (has_damage_texture && damage_srv_index >= heap_description.NumDescriptors) ||
        (has_damage_mask_texture && damage_mask_srv_index >= heap_description.NumDescriptors) ||
        (has_shadow_receiver &&
         (heap_description.NumDescriptors < 3U ||
          shadow_srv_index > heap_description.NumDescriptors - 3U ||
          shadow_cbv_index >= heap_description.NumDescriptors)) ||
        (has_multimap_reflection &&
         (multimap_cube_srv_index >= heap_description.NumDescriptors ||
          multimap_reflection_cbv_index >=
              heap_description.NumDescriptors))) {
        diagnostic = {"indexed_resource_descriptor_slot_invalid",
                      "D3D12 material descriptor slot is outside its bounded shader-visible heap"};
        return false;
    }
    const TextureDescription& texture_description = texture->info().description;
    const DXGI_FORMAT format = dxgi_texture_format(texture_description.format);
    if (format == DXGI_FORMAT_UNKNOWN || texture_description.mip_levels == 0U) {
        diagnostic = {"indexed_resource_texture_format_unsupported",
                      "D3D12 material texture format or mip count is unsupported"};
        return false;
    }
    DXGI_FORMAT normal_format = DXGI_FORMAT_UNKNOWN;
    if (has_normal_texture) {
        const TextureDescription& normal_description = normal_texture->info().description;
        normal_format = dxgi_texture_format(normal_description.format);
        const std::uint32_t normal_usage = static_cast<std::uint32_t>(normal_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((normal_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (normal_usage & forbidden_usage) != 0U) {
            diagnostic = {"indexed_normal_texture_usage_invalid",
                          "D3D12 normal textures require sampled-only usage"};
            return false;
        }
        const bool rgba8 = normal_description.format == TextureFormat::rgba8_unorm ||
                           normal_description.format == TextureFormat::rgba8_srgb ||
                           normal_description.format == TextureFormat::bgra8_unorm ||
                           normal_description.format == TextureFormat::bgra8_srgb;
        if (!rgba8 || normal_format == DXGI_FORMAT_UNKNOWN || normal_description.width == 0U ||
            normal_description.height == 0U || normal_description.mip_levels == 0U ||
            normal_description.array_layers != 1U) {
            diagnostic = {"indexed_normal_texture_format_unsupported",
                          "D3D12 normal textures require nonempty one-layer RGBA8 or BGRA8 data"};
            return false;
        }
    }
    DXGI_FORMAT maps_format = DXGI_FORMAT_UNKNOWN;
    if (has_maps_texture) {
        const TextureDescription& maps_description = maps_texture->info().description;
        maps_format = dxgi_texture_format(maps_description.format);
        const std::uint32_t maps_usage = static_cast<std::uint32_t>(maps_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((maps_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (maps_usage & forbidden_usage) != 0U) {
            diagnostic = {"indexed_maps_texture_usage_invalid",
                          "D3D12 maps textures require sampled-only usage"};
            return false;
        }
        const bool linear_color = maps_description.format == TextureFormat::rgba8_unorm ||
                                  maps_description.format == TextureFormat::bgra8_unorm ||
                                  maps_description.format == TextureFormat::bc1_unorm ||
                                  maps_description.format == TextureFormat::bc3_unorm ||
                                  maps_description.format == TextureFormat::bc7_unorm;
        if (!linear_color || maps_format == DXGI_FORMAT_UNKNOWN || maps_description.width == 0U ||
            maps_description.height == 0U || maps_description.mip_levels == 0U ||
            maps_description.array_layers != 1U) {
            diagnostic = {"indexed_maps_texture_format_unsupported",
                          "D3D12 maps textures require nonempty one-layer linear RGBA8, BGRA8, BC1, BC3, or BC7 UNORM data"};
            return false;
        }
    }
    DXGI_FORMAT detail_format = DXGI_FORMAT_UNKNOWN;
    if (has_detail_texture) {
        const TextureDescription& detail_description = detail_texture->info().description;
        detail_format = dxgi_texture_format(detail_description.format);
        const std::uint32_t detail_usage = static_cast<std::uint32_t>(detail_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((detail_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (detail_usage & forbidden_usage) != 0U) {
            diagnostic = {"indexed_detail_texture_usage_invalid",
                          "D3D12 detail textures require sampled-only usage"};
            return false;
        }
        const bool color = detail_description.format == TextureFormat::rgba8_unorm ||
                           detail_description.format == TextureFormat::rgba8_srgb ||
                           detail_description.format == TextureFormat::bgra8_unorm ||
                           detail_description.format == TextureFormat::bgra8_srgb ||
                           detail_description.format == TextureFormat::bc1_unorm ||
                           detail_description.format == TextureFormat::bc1_srgb ||
                           detail_description.format == TextureFormat::bc3_unorm ||
                           detail_description.format == TextureFormat::bc3_srgb ||
                           detail_description.format == TextureFormat::bc7_unorm ||
                           detail_description.format == TextureFormat::bc7_srgb;
        if (!color || detail_format == DXGI_FORMAT_UNKNOWN || detail_description.width == 0U ||
            detail_description.height == 0U || detail_description.mip_levels == 0U ||
            detail_description.array_layers != 1U) {
            diagnostic = {"indexed_detail_texture_format_unsupported",
                          "D3D12 detail textures require nonempty one-layer RGBA8, BGRA8, BC1, BC3, or BC7 data"};
            return false;
        }
    }
    DXGI_FORMAT normal_detail_format = DXGI_FORMAT_UNKNOWN;
    if (has_normal_detail_texture) {
        const TextureDescription& normal_detail_description = normal_detail_texture->info().description;
        normal_detail_format = dxgi_texture_format(normal_detail_description.format);
        const std::uint32_t normal_detail_usage = static_cast<std::uint32_t>(normal_detail_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((normal_detail_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (normal_detail_usage & forbidden_usage) != 0U) {
            diagnostic = {"indexed_normal_detail_texture_usage_invalid",
                          "D3D12 normal-detail textures require sampled-only usage"};
            return false;
        }
        const bool linear_rgba8 = normal_detail_description.format == TextureFormat::rgba8_unorm ||
                                  normal_detail_description.format == TextureFormat::bgra8_unorm;
        if (!linear_rgba8 || normal_detail_format == DXGI_FORMAT_UNKNOWN ||
            normal_detail_description.width == 0U || normal_detail_description.height == 0U ||
            normal_detail_description.mip_levels == 0U || normal_detail_description.array_layers != 1U) {
            diagnostic = {"indexed_normal_detail_texture_format_unsupported",
                          "D3D12 normal-detail textures require nonempty one-layer linear RGBA8 or BGRA8 data"};
            return false;
        }
    }
    DXGI_FORMAT damage_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT damage_mask_format = DXGI_FORMAT_UNKNOWN;
    if (has_damage_texture) {
        const auto validate_damage_format =
            [&](const D3D12Texture& source_texture, bool allow_srgb,
                const char* role, DXGI_FORMAT& output_format) {
                const TextureDescription& source = source_texture.info().description;
                output_format = dxgi_texture_format(source.format);
                const std::uint32_t source_usage =
                    static_cast<std::uint32_t>(source.usage);
                const std::uint32_t forbidden_usage =
                    static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                    static_cast<std::uint32_t>(TextureUsage::storage) |
                    static_cast<std::uint32_t>(TextureUsage::transfer_destination);
                const bool supported =
                    source.format == TextureFormat::rgba8_unorm ||
                    source.format == TextureFormat::bgra8_unorm ||
                    source.format == TextureFormat::bc1_unorm ||
                    source.format == TextureFormat::bc3_unorm ||
                    source.format == TextureFormat::bc7_unorm ||
                    (allow_srgb &&
                     (source.format == TextureFormat::rgba8_srgb ||
                      source.format == TextureFormat::bgra8_srgb ||
                      source.format == TextureFormat::bc1_srgb ||
                      source.format == TextureFormat::bc3_srgb ||
                      source.format == TextureFormat::bc7_srgb));
                if ((source_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
                    (source_usage & forbidden_usage) != 0U || !supported ||
                    output_format == DXGI_FORMAT_UNKNOWN ||
                    source.width == 0U || source.height == 0U ||
                    source.mip_levels == 0U || source.array_layers != 1U) {
                    diagnostic = {std::string("indexed_") + role +
                                      "_texture_format_unsupported",
                                  "D3D12 damage textures require nonempty one-layer RGBA8, BGRA8, BC1, BC3, or BC7 data"};
                    return false;
                }
                return true;
            };
        if (!validate_damage_format(*damage_texture, true, "damage", damage_format) ||
            !validate_damage_format(*damage_mask_texture, false, "damage_mask",
                                    damage_mask_format))
            return false;
    }
    const UINT descriptor_size = context->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (descriptor_size == 0U ||
        static_cast<UINT64>(srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(cbv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(frame_cbv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(normal_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(maps_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(detail_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(normal_detail_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(damage_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(damage_mask_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(shadow_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(shadow_cbv_index) > std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(multimap_cube_srv_index) >
            std::numeric_limits<UINT64>::max() / descriptor_size ||
        static_cast<UINT64>(multimap_reflection_cbv_index) >
            std::numeric_limits<UINT64>::max() / descriptor_size) {
        diagnostic = {"indexed_resource_descriptor_offset_overflow",
                      "D3D12 material descriptor offset exceeds handle addressability"};
        return false;
    }
    const UINT64 srv_offset = static_cast<UINT64>(srv_index) * descriptor_size;
    const UINT64 cbv_offset = static_cast<UINT64>(cbv_index) * descriptor_size;
    const UINT64 frame_cbv_offset = static_cast<UINT64>(frame_cbv_index) * descriptor_size;
    const UINT64 normal_srv_offset = static_cast<UINT64>(normal_srv_index) * descriptor_size;
    const UINT64 maps_srv_offset = static_cast<UINT64>(maps_srv_index) * descriptor_size;
    const UINT64 detail_srv_offset = static_cast<UINT64>(detail_srv_index) * descriptor_size;
    const UINT64 normal_detail_srv_offset = static_cast<UINT64>(normal_detail_srv_index) * descriptor_size;
    const UINT64 damage_srv_offset = static_cast<UINT64>(damage_srv_index) * descriptor_size;
    const UINT64 damage_mask_srv_offset = static_cast<UINT64>(damage_mask_srv_index) * descriptor_size;
    const UINT64 shadow_srv_offset = static_cast<UINT64>(shadow_srv_index) * descriptor_size;
    const UINT64 shadow_cbv_offset = static_cast<UINT64>(shadow_cbv_index) * descriptor_size;
    const UINT64 multimap_cube_srv_offset =
        static_cast<UINT64>(multimap_cube_srv_index) * descriptor_size;
    const UINT64 multimap_reflection_cbv_offset =
        static_cast<UINT64>(multimap_reflection_cbv_index) * descriptor_size;
    if (srv_offset > std::numeric_limits<SIZE_T>::max() ||
        cbv_offset > std::numeric_limits<SIZE_T>::max() ||
        frame_cbv_offset > std::numeric_limits<SIZE_T>::max() ||
        normal_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        maps_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        detail_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        normal_detail_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        damage_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        damage_mask_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        shadow_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        shadow_cbv_offset > std::numeric_limits<SIZE_T>::max() ||
        multimap_cube_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        multimap_reflection_cbv_offset > std::numeric_limits<SIZE_T>::max()) {
        diagnostic = {"indexed_resource_descriptor_offset_overflow",
                      "D3D12 material descriptor offset exceeds handle addressability"};
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(srv_offset)) {
        diagnostic = {"indexed_resource_descriptor_offset_overflow",
                      "D3D12 material SRV descriptor CPU handle overflows"};
        return false;
    }
    cpu.ptr += static_cast<SIZE_T>(srv_offset);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
    if (gpu.ptr > std::numeric_limits<UINT64>::max() - srv_offset) {
        diagnostic = {"indexed_resource_descriptor_offset_overflow",
                      "D3D12 material SRV descriptor GPU handle overflows"};
        return false;
    }
    gpu.ptr += srv_offset;
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv =
        d3d12_texture_srv_description(texture_description, format);
    context->device->CreateShaderResourceView(texture->resource(), &srv, cpu);
    output.srv_heap = srv_heap;
    output.srv_gpu = gpu;
    output.sampler_gpu = sampler->gpu_descriptor();
    if (has_normal_texture) {
        D3D12_CPU_DESCRIPTOR_HANDLE normal_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (normal_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(normal_srv_offset)) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 normal SRV descriptor CPU handle overflows"};
            return false;
        }
        normal_cpu.ptr += static_cast<SIZE_T>(normal_srv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE normal_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (normal_gpu.ptr > std::numeric_limits<UINT64>::max() - normal_srv_offset) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 normal SRV descriptor GPU handle overflows"};
            return false;
        }
        normal_gpu.ptr += normal_srv_offset;
        const TextureDescription& normal_description = normal_texture->info().description;
        const D3D12_SHADER_RESOURCE_VIEW_DESC normal_srv =
            d3d12_texture_srv_description(normal_description, normal_format);
        context->device->CreateShaderResourceView(normal_texture->resource(), &normal_srv, normal_cpu);
        output.normal_srv_gpu = normal_gpu;
        output.normal_sampler_gpu = normal_sampler->gpu_descriptor();
        output.normal_enabled = true;
    }
    if (has_maps_texture) {
        D3D12_CPU_DESCRIPTOR_HANDLE maps_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (maps_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(maps_srv_offset)) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 maps SRV descriptor CPU handle overflows"};
            return false;
        }
        maps_cpu.ptr += static_cast<SIZE_T>(maps_srv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE maps_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (maps_gpu.ptr > std::numeric_limits<UINT64>::max() - maps_srv_offset) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 maps SRV descriptor GPU handle overflows"};
            return false;
        }
        maps_gpu.ptr += maps_srv_offset;
        const TextureDescription& maps_description = maps_texture->info().description;
        const D3D12_SHADER_RESOURCE_VIEW_DESC maps_srv =
            d3d12_texture_srv_description(maps_description, maps_format);
        context->device->CreateShaderResourceView(maps_texture->resource(), &maps_srv, maps_cpu);
        output.maps_srv_gpu = maps_gpu;
        output.maps_sampler_gpu = maps_sampler->gpu_descriptor();
        output.maps_enabled = true;
    }
    if (has_detail_texture) {
        D3D12_CPU_DESCRIPTOR_HANDLE detail_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (detail_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(detail_srv_offset)) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 detail SRV descriptor CPU handle overflows"};
            return false;
        }
        detail_cpu.ptr += static_cast<SIZE_T>(detail_srv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE detail_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (detail_gpu.ptr > std::numeric_limits<UINT64>::max() - detail_srv_offset) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 detail SRV descriptor GPU handle overflows"};
            return false;
        }
        detail_gpu.ptr += detail_srv_offset;
        const TextureDescription& detail_description = detail_texture->info().description;
        const D3D12_SHADER_RESOURCE_VIEW_DESC detail_srv =
            d3d12_texture_srv_description(detail_description, detail_format);
        context->device->CreateShaderResourceView(detail_texture->resource(), &detail_srv, detail_cpu);
        output.detail_srv_gpu = detail_gpu;
        output.detail_sampler_gpu = detail_sampler->gpu_descriptor();
        output.detail_enabled = true;
    }
    if (has_normal_detail_texture) {
        D3D12_CPU_DESCRIPTOR_HANDLE normal_detail_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (normal_detail_cpu.ptr >
            std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(normal_detail_srv_offset)) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 normal-detail SRV descriptor CPU handle overflows"};
            return false;
        }
        normal_detail_cpu.ptr += static_cast<SIZE_T>(normal_detail_srv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE normal_detail_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (normal_detail_gpu.ptr > std::numeric_limits<UINT64>::max() - normal_detail_srv_offset) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 normal-detail SRV descriptor GPU handle overflows"};
            return false;
        }
        normal_detail_gpu.ptr += normal_detail_srv_offset;
        const TextureDescription& normal_detail_description = normal_detail_texture->info().description;
        const D3D12_SHADER_RESOURCE_VIEW_DESC normal_detail_srv =
            d3d12_texture_srv_description(normal_detail_description,
                                          normal_detail_format);
        context->device->CreateShaderResourceView(normal_detail_texture->resource(), &normal_detail_srv,
                                                  normal_detail_cpu);
        output.normal_detail_srv_gpu = normal_detail_gpu;
        output.normal_detail_sampler_gpu = normal_detail_sampler->gpu_descriptor();
        output.normal_detail_enabled = true;
    }
    const auto create_damage_srv =
        [&](const D3D12Texture& source_texture, const D3D12Sampler& source_sampler,
            DXGI_FORMAT source_format, UINT64 offset,
            D3D12_GPU_DESCRIPTOR_HANDLE& output_gpu,
            D3D12_GPU_DESCRIPTOR_HANDLE& output_sampler) {
            D3D12_CPU_DESCRIPTOR_HANDLE source_cpu =
                srv_heap->GetCPUDescriptorHandleForHeapStart();
            if (source_cpu.ptr > std::numeric_limits<SIZE_T>::max() -
                                     static_cast<SIZE_T>(offset)) {
                diagnostic = {"indexed_resource_descriptor_offset_overflow",
                              "D3D12 damage SRV descriptor CPU handle overflows"};
                return false;
            }
            source_cpu.ptr += static_cast<SIZE_T>(offset);
            D3D12_GPU_DESCRIPTOR_HANDLE source_gpu =
                srv_heap->GetGPUDescriptorHandleForHeapStart();
            if (source_gpu.ptr > std::numeric_limits<UINT64>::max() - offset) {
                diagnostic = {"indexed_resource_descriptor_offset_overflow",
                              "D3D12 damage SRV descriptor GPU handle overflows"};
                return false;
            }
            source_gpu.ptr += offset;
            const D3D12_SHADER_RESOURCE_VIEW_DESC source_srv =
                d3d12_texture_srv_description(source_texture.info().description,
                                              source_format);
            context->device->CreateShaderResourceView(source_texture.resource(), &source_srv,
                                                      source_cpu);
            output_gpu = source_gpu;
            output_sampler = source_sampler.gpu_descriptor();
            return true;
        };
    if (has_damage_texture) {
        if (!create_damage_srv(*damage_texture, *damage_sampler, damage_format,
                               damage_srv_offset, output.damage_srv_gpu,
                               output.damage_sampler_gpu) ||
            !create_damage_srv(*damage_mask_texture, *damage_mask_sampler,
                               damage_mask_format, damage_mask_srv_offset,
                               output.damage_mask_srv_gpu,
                               output.damage_mask_sampler_gpu))
            return false;
        output.damage_enabled = true;
        output.damage_mask_enabled = true;
    }

    if (has_shadow_receiver) {
        D3D12_CPU_DESCRIPTOR_HANDLE shadow_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (shadow_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(shadow_srv_offset)) {
            diagnostic = {"indexed_directional_shadow_descriptor_offset_overflow",
                          "D3D12 directional-shadow SRV descriptor CPU handle overflows"};
            return false;
        }
        shadow_cpu.ptr += static_cast<SIZE_T>(shadow_srv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE shadow_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (shadow_gpu.ptr > std::numeric_limits<UINT64>::max() - shadow_srv_offset) {
            diagnostic = {"indexed_directional_shadow_descriptor_offset_overflow",
                          "D3D12 directional-shadow SRV descriptor GPU handle overflows"};
            return false;
        }
        shadow_gpu.ptr += shadow_srv_offset;
        for (std::size_t cascade = 0U; cascade < indexed_directional_shadow_cascade_count; ++cascade) {
            D3D12_CPU_DESCRIPTOR_HANDLE destination = shadow_cpu;
            const UINT64 cascade_offset = static_cast<UINT64>(cascade) * descriptor_size;
            if (destination.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(cascade_offset)) {
                diagnostic = {"indexed_directional_shadow_descriptor_offset_overflow",
                              "D3D12 directional-shadow cascade descriptor CPU handle overflows"};
                return false;
            }
            destination.ptr += static_cast<SIZE_T>(cascade_offset);
            context->device->CopyDescriptorsSimple(1U, destination,
                                                    shadow_attachments[cascade]->srv(),
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            output.shadow_attachments[cascade] =
                const_cast<D3D12DepthAttachment*>(shadow_attachments[cascade]);
        }
        output.shadow_srv_gpu = shadow_gpu;
        output.shadow_sampler_gpu = shadow_sampler->gpu_descriptor();
        output.shadow_enabled = true;
        D3D12_CPU_DESCRIPTOR_HANDLE shadow_cbv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (shadow_cbv_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(shadow_cbv_offset)) {
            diagnostic = {"indexed_directional_shadow_descriptor_offset_overflow",
                          "D3D12 directional-shadow CBV descriptor CPU handle overflows"};
            return false;
        }
        shadow_cbv_cpu.ptr += static_cast<SIZE_T>(shadow_cbv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE shadow_cbv_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (shadow_cbv_gpu.ptr > std::numeric_limits<UINT64>::max() - shadow_cbv_offset) {
            diagnostic = {"indexed_directional_shadow_descriptor_offset_overflow",
                          "D3D12 directional-shadow CBV descriptor GPU handle overflows"};
            return false;
        }
        shadow_cbv_gpu.ptr += shadow_cbv_offset;
        const UINT64 shadow_gpu_address =
            shadow_constants->resource()->GetGPUVirtualAddress() +
            request.directional_shadow_binding.constants_offset_bytes;
        D3D12_CONSTANT_BUFFER_VIEW_DESC shadow_cbv{};
        shadow_cbv.BufferLocation = shadow_gpu_address;
        shadow_cbv.SizeInBytes = portable_directional_shadow_buffer_view_bytes;
        context->device->CreateConstantBufferView(&shadow_cbv, shadow_cbv_cpu);
        output.shadow_cbv_gpu = shadow_cbv_gpu;
    }

    if (has_multimap_reflection) {
        D3D12_CPU_DESCRIPTOR_HANDLE cube_cpu =
            srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE cube_gpu =
            srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (cube_cpu.ptr > std::numeric_limits<SIZE_T>::max() -
                               static_cast<SIZE_T>(multimap_cube_srv_offset) ||
            cube_gpu.ptr > std::numeric_limits<UINT64>::max() -
                               multimap_cube_srv_offset) {
            diagnostic = {
                "indexed_multimap_reflection_descriptor_offset_overflow",
                "D3D12 reflection cube descriptor handle overflows"};
            return false;
        }
        cube_cpu.ptr += static_cast<SIZE_T>(multimap_cube_srv_offset);
        cube_gpu.ptr += multimap_cube_srv_offset;
        const TextureDescription& cube_description =
            multimap_cube->info().description;
        const D3D12_SHADER_RESOURCE_VIEW_DESC cube_srv =
            d3d12_texture_srv_description(
                cube_description,
                dxgi_texture_format(cube_description.format));
        context->device->CreateShaderResourceView(multimap_cube->resource(),
                                                  &cube_srv, cube_cpu);

        D3D12_CPU_DESCRIPTOR_HANDLE constants_cpu =
            srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE constants_gpu =
            srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (constants_cpu.ptr >
                std::numeric_limits<SIZE_T>::max() -
                    static_cast<SIZE_T>(multimap_reflection_cbv_offset) ||
            constants_gpu.ptr > std::numeric_limits<UINT64>::max() -
                                    multimap_reflection_cbv_offset) {
            diagnostic = {
                "indexed_multimap_reflection_descriptor_offset_overflow",
                "D3D12 reflection constants descriptor handle overflows"};
            return false;
        }
        constants_cpu.ptr +=
            static_cast<SIZE_T>(multimap_reflection_cbv_offset);
        constants_gpu.ptr += multimap_reflection_cbv_offset;
        D3D12_CONSTANT_BUFFER_VIEW_DESC reflection_cbv{};
        reflection_cbv.BufferLocation =
            multimap_reflection_constants->resource()->GetGPUVirtualAddress() +
            request.multimap_reflection_binding.constants.offset_bytes;
        reflection_cbv.SizeInBytes =
            portable_multimap_reflection_buffer_view_bytes;
        context->device->CreateConstantBufferView(&reflection_cbv,
                                                  constants_cpu);
        output.multimap_cube_srv_gpu = cube_gpu;
        output.multimap_cube_sampler_gpu =
            multimap_cube_sampler->gpu_descriptor();
        output.multimap_reflection_cbv_gpu = constants_gpu;
        output.multimap_reflection_enabled = true;
    }

    if (has_material_constants) {
        const auto* material_buffer = dynamic_cast<const D3D12Buffer*>(request.material_binding.buffer);
        if (material_buffer == nullptr) {
            diagnostic = {"indexed_material_buffer_type_unsupported",
                          "The D3D12 material constants binding did not reference a D3D12 buffer"};
            return false;
        }
        if (material_buffer->context() != context.get()) {
            diagnostic = {"indexed_material_buffer_context_mismatch",
                          "D3D12 material constants belong to another device"};
            return false;
        }
        if (material_buffer->resource() == nullptr || material_buffer->info().description.usage != BufferUsage::uniform) {
            diagnostic = {"indexed_material_buffer_usage_invalid",
                          "D3D12 material constants require an initialized uniform buffer"};
            return false;
        }
        constexpr UINT required_buffer_state =
            static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        if ((static_cast<UINT>(material_buffer->state()) & required_buffer_state) == 0U) {
            diagnostic = {"indexed_material_buffer_state_invalid",
                          "D3D12 material constants are not in constant-buffer state"};
            return false;
        }
        const std::uint64_t offset = request.material_binding.offset_bytes;
        constexpr std::uint64_t range = portable_material_buffer_view_bytes;
        const std::uint64_t buffer_size = material_buffer->info().description.size_bytes;
        if (offset % portable_material_buffer_view_bytes != 0U ||
            request.material_binding.range_bytes != portable_material_buffer_view_bytes ||
            offset > buffer_size || range > buffer_size - offset) {
            diagnostic = {"indexed_material_buffer_range_invalid",
                          "D3D12 material constants require one bounded 256-byte buffer view"};
            return false;
        }
        const D3D12_RESOURCE_DESC resource_description = material_buffer->resource()->GetDesc();
        if (offset > resource_description.Width || range > resource_description.Width - offset) {
            diagnostic = {"indexed_material_buffer_range_invalid",
                          "D3D12 material constants exceed the native resource size"};
            return false;
        }
        const UINT64 gpu_base = material_buffer->resource()->GetGPUVirtualAddress();
        if (gpu_base == 0U || gpu_base > std::numeric_limits<UINT64>::max() - offset) {
            diagnostic = {"indexed_material_buffer_gpu_address_invalid",
                          "D3D12 material constants GPU address is unavailable or overflows"};
            return false;
        }
        const UINT64 gpu_address = gpu_base + offset;
        if (gpu_address > std::numeric_limits<UINT64>::max() - range) {
            diagnostic = {"indexed_material_buffer_gpu_address_invalid",
                          "D3D12 material constants GPU view range overflows"};
            return false;
        }
        if ((gpu_address % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) != 0U) {
            diagnostic = {"indexed_material_buffer_alignment_invalid",
                          "D3D12 material constants require a 256-byte aligned GPU address"};
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cbv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (cbv_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(cbv_offset)) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 material CBV descriptor CPU handle overflows"};
            return false;
        }
        cbv_cpu.ptr += static_cast<SIZE_T>(cbv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE cbv_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (cbv_gpu.ptr > std::numeric_limits<UINT64>::max() - cbv_offset) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 material CBV descriptor GPU handle overflows"};
            return false;
        }
        cbv_gpu.ptr += cbv_offset;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = gpu_address;
        cbv.SizeInBytes = static_cast<UINT>(range);
        context->device->CreateConstantBufferView(&cbv, cbv_cpu);
        output.cbv_gpu = cbv_gpu;
        output.constant_enabled = true;
    }
    if (has_frame_constants) {
        const auto* frame_buffer = dynamic_cast<const D3D12Buffer*>(request.frame_binding.buffer);
        if (frame_buffer == nullptr) {
            diagnostic = {"indexed_frame_buffer_type_unsupported",
                          "The D3D12 frame constants binding did not reference a D3D12 buffer"};
            return false;
        }
        if (frame_buffer->context() != context.get()) {
            diagnostic = {"indexed_frame_buffer_context_mismatch",
                          "D3D12 frame constants belong to another device"};
            return false;
        }
        if (frame_buffer->resource() == nullptr || frame_buffer->info().description.usage != BufferUsage::uniform) {
            diagnostic = {"indexed_frame_buffer_usage_invalid",
                          "D3D12 frame constants require an initialized uniform buffer"};
            return false;
        }
        constexpr UINT required_buffer_state =
            static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        if ((static_cast<UINT>(frame_buffer->state()) & required_buffer_state) == 0U) {
            diagnostic = {"indexed_frame_buffer_state_invalid",
                          "D3D12 frame constants are not in constant-buffer state"};
            return false;
        }
        const std::uint64_t offset = request.frame_binding.offset_bytes;
        constexpr std::uint64_t range = portable_frame_buffer_view_bytes;
        const std::uint64_t buffer_size = frame_buffer->info().description.size_bytes;
        if (offset % portable_frame_buffer_view_bytes != 0U ||
            request.frame_binding.range_bytes != portable_frame_buffer_view_bytes ||
            offset > buffer_size || range > buffer_size - offset) {
            diagnostic = {"indexed_frame_buffer_range_invalid",
                          "D3D12 frame constants require one bounded 256-byte buffer view"};
            return false;
        }
        const D3D12_RESOURCE_DESC resource_description = frame_buffer->resource()->GetDesc();
        if (offset > resource_description.Width || range > resource_description.Width - offset) {
            diagnostic = {"indexed_frame_buffer_range_invalid",
                          "D3D12 frame constants exceed the native resource size"};
            return false;
        }
        const UINT64 gpu_base = frame_buffer->resource()->GetGPUVirtualAddress();
        if (gpu_base == 0U || gpu_base > std::numeric_limits<UINT64>::max() - offset) {
            diagnostic = {"indexed_frame_buffer_gpu_address_invalid",
                          "D3D12 frame constants GPU address is unavailable or overflows"};
            return false;
        }
        const UINT64 gpu_address = gpu_base + offset;
        if (gpu_address > std::numeric_limits<UINT64>::max() - range) {
            diagnostic = {"indexed_frame_buffer_gpu_address_invalid",
                          "D3D12 frame constants GPU view range overflows"};
            return false;
        }
        if ((gpu_address % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) != 0U) {
            diagnostic = {"indexed_frame_buffer_alignment_invalid",
                          "D3D12 frame constants require a 256-byte aligned GPU address"};
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE frame_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        if (frame_cpu.ptr > std::numeric_limits<SIZE_T>::max() - static_cast<SIZE_T>(frame_cbv_offset)) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 frame CBV descriptor CPU handle overflows"};
            return false;
        }
        frame_cpu.ptr += static_cast<SIZE_T>(frame_cbv_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE frame_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (frame_gpu.ptr > std::numeric_limits<UINT64>::max() - frame_cbv_offset) {
            diagnostic = {"indexed_resource_descriptor_offset_overflow",
                          "D3D12 frame CBV descriptor GPU handle overflows"};
            return false;
        }
        frame_gpu.ptr += frame_cbv_offset;
        D3D12_CONSTANT_BUFFER_VIEW_DESC frame_cbv{};
        frame_cbv.BufferLocation = gpu_address;
        frame_cbv.SizeInBytes = static_cast<UINT>(range);
        context->device->CreateConstantBufferView(&frame_cbv, frame_cpu);
        output.frame_cbv_gpu = frame_gpu;
        output.frame_enabled = true;
    }
    output.enabled = true;
    return true;
}

class D3D12ShaderModule final : public ShaderModule {
public:
    D3D12ShaderModule(std::shared_ptr<D3D12Context> context,
                      std::shared_ptr<const std::vector<std::byte>> bytecode,
                      ShaderModuleInfo info)
        : context_(std::move(context)), bytecode_(std::move(bytecode)), info_(info) {}
    Backend backend() const noexcept override { return Backend::D3D12; }
    const ShaderModuleInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const D3D12Context* context() const noexcept {
        return context_.get();
    }

private:
    std::shared_ptr<D3D12Context> context_;
    std::shared_ptr<const std::vector<std::byte>> bytecode_;
    ShaderModuleInfo info_;
};

bool D3D12Texture::draw_stock_ks_per_pixel_native(
    const IndexedStaticMeshDrawRequest& request,
    const D3D12Buffer& vertex_buffer,
    const D3D12Buffer& index_buffer,
    D3D12DepthAttachment* depth_attachment,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    if (request.load_color && !initialized_) {
        diagnostic = {"indexed_color_load_before_clear",
                      "A color load was requested before the color attachment was initialized"};
        return false;
    }
    const StockKsPerPixelNativeDrawBinding* binding =
        request.stock_ks_per_pixel_native;
    if (binding == nullptr || binding->resources == nullptr ||
        !binding->resources->ready() ||
        binding->diffuse_texture == nullptr) {
        diagnostic = {"indexed_stock_native_binding_missing",
                      "The native D3D12 draw binding is incomplete"};
        return false;
    }

    auto* diffuse = const_cast<D3D12Texture*>(
        dynamic_cast<const D3D12Texture*>(binding->diffuse_texture));
    std::array<D3D12DepthAttachment*,
               stock_ks_per_pixel_shadow_cascade_count>
        shadows{};
    for (std::size_t index = 0U; index < shadows.size(); ++index) {
        shadows[index] = const_cast<D3D12DepthAttachment*>(
            dynamic_cast<const D3D12DepthAttachment*>(
                binding->shadow_maps[index]));
    }
    std::array<const D3D12Buffer*,
               static_cast<std::size_t>(
                   StockKsPerPixelNativeConstantSlot::count)>
        constants{};
    for (std::size_t index = 0U; index < constants.size(); ++index) {
        constants[index] = dynamic_cast<const D3D12Buffer*>(
            binding->resources->constant_buffers().buffer(
                static_cast<StockKsPerPixelNativeConstantSlot>(index)));
    }
    const auto* linear_sampler = dynamic_cast<const D3D12Sampler*>(
        binding->resources->samplers().sampler(
            StockKsPerPixelNativeSamplerSlot::linear));
    const auto* shadow_sampler = dynamic_cast<const D3D12Sampler*>(
        binding->resources->samplers().sampler(
            StockKsPerPixelNativeSamplerSlot::shadow));
    const auto* vertex_shader = dynamic_cast<const D3D12ShaderModule*>(
        &binding->resources->shader_program().vertex_shader());
    const auto* pixel_shader = dynamic_cast<const D3D12ShaderModule*>(
        &binding->resources->shader_program().pixel_shader());
    if (diffuse == nullptr || linear_sampler == nullptr ||
        shadow_sampler == nullptr || vertex_shader == nullptr ||
        pixel_shader == nullptr ||
        std::any_of(shadows.begin(), shadows.end(),
                    [](const D3D12DepthAttachment* value) {
                        return value == nullptr;
                    }) ||
        std::any_of(constants.begin(), constants.end(),
                    [](const D3D12Buffer* value) {
                        return value == nullptr;
                    })) {
        diagnostic = {"indexed_stock_native_type_unsupported",
                      "The native binding contains an unknown D3D12 resource handle"};
        return false;
    }

    const D3D12Context* expected_context = context_.get();
    const bool foreign_context =
        vertex_buffer.context() != expected_context ||
        index_buffer.context() != expected_context ||
        diffuse->context() != expected_context ||
        linear_sampler->context() != expected_context ||
        shadow_sampler->context() != expected_context ||
        vertex_shader->context() != expected_context ||
        pixel_shader->context() != expected_context ||
        (depth_attachment != nullptr &&
         depth_attachment->context() != expected_context) ||
        std::any_of(shadows.begin(), shadows.end(),
                    [&](const D3D12DepthAttachment* value) {
                        return value->context() != expected_context;
                    }) ||
        std::any_of(constants.begin(), constants.end(),
                    [&](const D3D12Buffer* value) {
                        return value->context() != expected_context;
                    });
    if (foreign_context) {
        diagnostic = {"indexed_stock_native_context_mismatch",
                      "Native ksPerPixel resources belong to different D3D12 devices"};
        return false;
    }
    if (!diffuse->initialized()) {
        diagnostic = {"indexed_stock_native_diffuse_uninitialized",
                      "The native diffuse texture has no uploaded data"};
        return false;
    }
    if (std::any_of(shadows.begin(), shadows.end(),
                    [](const D3D12DepthAttachment* value) {
                        return !value->cleared();
                    })) {
        diagnostic = {"indexed_stock_native_shadow_uninitialized",
                      "Every native shadow map must be cleared before sampling"};
        return false;
    }

    std::lock_guard command_guard(context_->command_mutex);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_description.NumDescriptors = 1U;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    HRESULT result = context_->device->CreateDescriptorHeap(
        &rtv_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "CreateDescriptorHeap(stock native RTV)", result);
        diagnostic.code = "indexed_stock_native_descriptor_failed";
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context_->device->CreateRenderTargetView(resource_.Get(), nullptr, rtv);

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context_->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "CreateCommandAllocator(stock native)", result);
        diagnostic.code = "indexed_stock_native_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> command_list;
    result = context_->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&command_list));
    if (SUCCEEDED(result)) result = command_list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommandList(stock native)", result);
        diagnostic.code = "indexed_stock_native_execution_failed";
        return false;
    }

    D3D12_RESOURCE_STATES target_state = state_;
    D3D12_RESOURCE_STATES diffuse_state = diffuse->state();
    D3D12_RESOURCE_STATES depth_state =
        depth_attachment != nullptr ? depth_attachment->state()
                                    : D3D12_RESOURCE_STATE_COMMON;
    bool depth_cleared =
        depth_attachment != nullptr && depth_attachment->cleared();
    std::array<D3D12_RESOURCE_STATES,
               stock_ks_per_pixel_shadow_cascade_count>
        shadow_states{};
    std::array<ID3D12Resource*, stock_ks_per_pixel_shadow_cascade_count>
        shadow_resources{};
    for (std::size_t index = 0U; index < shadows.size(); ++index) {
        shadow_states[index] = shadows[index]->state();
        shadow_resources[index] = shadows[index]->resource();
    }
    std::array<ID3D12Resource*,
               static_cast<std::size_t>(
                   StockKsPerPixelNativeConstantSlot::count)>
        constant_resources{};
    for (std::size_t index = 0U; index < constants.size(); ++index)
        constant_resources[index] = constants[index]->resource();

    D3D12StockKsPerPixelNativeDrawContext native_context;
    native_context.device = context_->device.Get();
    native_context.queue = context_->queue.Get();
    native_context.allocator = allocator.Get();
    native_context.command_list = command_list.Get();
    native_context.target = resource_.Get();
    native_context.target_rtv = rtv;
    native_context.target_state = &target_state;
    native_context.target_format = dxgi_texture_format(info_.description.format);
    native_context.target_width = info_.description.width;
    native_context.target_height = info_.description.height;
    native_context.depth =
        depth_attachment != nullptr ? depth_attachment->resource() : nullptr;
    native_context.depth_write_dsv =
        depth_attachment != nullptr ? depth_attachment->dsv(true)
                                    : D3D12_CPU_DESCRIPTOR_HANDLE{};
    native_context.depth_read_dsv =
        depth_attachment != nullptr ? depth_attachment->dsv(false)
                                    : D3D12_CPU_DESCRIPTOR_HANDLE{};
    native_context.depth_state =
        depth_attachment != nullptr ? &depth_state : nullptr;
    native_context.depth_cleared =
        depth_attachment != nullptr ? &depth_cleared : nullptr;
    native_context.vertex_buffer = vertex_buffer.resource();
    native_context.vertex_state = vertex_buffer.state();
    native_context.index_buffer = index_buffer.resource();
    native_context.index_state = index_buffer.state();
    native_context.diffuse_texture = diffuse->resource();
    native_context.diffuse_format =
        dxgi_texture_format(diffuse->info().description.format);
    native_context.diffuse_state = &diffuse_state;
    native_context.shadow_maps = shadow_resources;
    for (std::size_t index = 0U; index < shadow_states.size(); ++index)
        native_context.shadow_states[index] = &shadow_states[index];
    native_context.shader_program = &binding->resources->shader_program();
    native_context.constant_buffers = &binding->resources->constant_buffers();
    native_context.samplers = &binding->resources->samplers();
    native_context.constant_buffer_resources = constant_resources;
    native_context.request = &request;
    native_context.readback = &output;

    output.clear();
    const bool drawn = record_d3d12_stock_ks_per_pixel_native_draw(
        native_context, diagnostic);
    if (!drawn &&
        diagnostic.code == "d3d12_stock_native_submit_undrained") {
        state_ = D3D12_RESOURCE_STATE_COMMON;
        diffuse->set_state(D3D12_RESOURCE_STATE_COMMON);
        for (D3D12DepthAttachment* shadow_attachment : shadows)
            shadow_attachment->set_state(D3D12_RESOURCE_STATE_COMMON);
        if (depth_attachment != nullptr)
            depth_attachment->set_state(D3D12_RESOURCE_STATE_COMMON);
        return false;
    }
    if (!drawn) return false;

    state_ = target_state;
    diffuse->set_state(diffuse_state);
    for (std::size_t index = 0U; index < shadows.size(); ++index)
        shadows[index]->set_state(shadow_states[index]);
    if (depth_attachment != nullptr) {
        depth_attachment->set_state(depth_state);
        if (depth_cleared) depth_attachment->set_cleared();
    }
    initialized_ = true;
    if (!base_mip_initialized_.empty())
        base_mip_initialized_[0U] = 1U;
    return true;
}

bool D3D12Texture::draw_stock_ks_per_pixel_native_batch(
    const IndexedStaticMeshBatchDescription& batch,
    std::span<const D3D12StockKsPerPixelNativeBatchDraw> draws,
    D3D12DepthAttachment* depth_attachment,
    D3D12Texture* resolve_target,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    if (batch.load_color && !initialized_) {
        diagnostic = {
            "indexed_color_load_before_clear",
            "A color load was requested before the color attachment was initialized"};
        return false;
    }

    std::lock_guard command_guard(context_->command_mutex);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_description.NumDescriptors = 1U;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    HRESULT result = context_->device->CreateDescriptorHeap(
        &rtv_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "CreateDescriptorHeap(stock native batch RTV)", result);
        diagnostic.code = "d3d12_stock_native_batch_descriptor_failed";
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    context_->device->CreateRenderTargetView(resource_.Get(), nullptr, rtv);

    ComPtr<ID3D12CommandAllocator> allocator;
    result = context_->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "CreateCommandAllocator(stock native batch)", result);
        diagnostic.code = "d3d12_stock_native_batch_execution_failed";
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> command_list;
    result = context_->device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&command_list));
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "CreateCommandList(stock native batch)", result);
        diagnostic.code = "d3d12_stock_native_batch_execution_failed";
        return false;
    }
    result = command_list->Close();
    if (FAILED(result)) {
        diagnostic = hresult_error(
            "Close(initial stock native batch command list)", result);
        diagnostic.code = "d3d12_stock_native_batch_execution_failed";
        return false;
    }

    D3D12StockKsPerPixelNativeBatchContext native_context;
    native_context.device = context_->device.Get();
    native_context.queue = context_->queue.Get();
    native_context.allocator = allocator.Get();
    native_context.command_list = command_list.Get();
    native_context.target = resource_.Get();
    native_context.target_rtv = rtv;
    native_context.target_state = &state_;
    native_context.target_format = dxgi_texture_format(info_.description.format);
    native_context.target_width = info_.description.width;
    native_context.target_height = info_.description.height;
    native_context.load_color = batch.load_color;
    native_context.clear_color = batch.clear_color;
    native_context.target_cleared = &initialized_;
    native_context.resolve_target =
        resolve_target != nullptr ? resolve_target->resource_.Get() : nullptr;
    native_context.resolve_target_state =
        resolve_target != nullptr ? &resolve_target->state_ : nullptr;
    native_context.resolve_target_cleared =
        resolve_target != nullptr ? &resolve_target->initialized_ : nullptr;
    native_context.depth =
        depth_attachment != nullptr ? depth_attachment->resource() : nullptr;
    native_context.depth_write_dsv =
        depth_attachment != nullptr ? depth_attachment->dsv(true)
                                    : D3D12_CPU_DESCRIPTOR_HANDLE{};
    native_context.depth_read_dsv =
        depth_attachment != nullptr ? depth_attachment->dsv(false)
                                    : D3D12_CPU_DESCRIPTOR_HANDLE{};
    native_context.depth_state =
        depth_attachment != nullptr ? depth_attachment->state_pointer() : nullptr;
    native_context.depth_cleared =
        depth_attachment != nullptr ? depth_attachment->cleared_pointer() : nullptr;
    native_context.clear_depth = batch.clear_depth;
    native_context.depth_clear_value = batch.depth_clear_value;
    native_context.draws = draws;
    native_context.capture_rgba8 = batch.capture_rgba8;
    native_context.readback = &output;

    output.clear();
    const bool drawn = record_d3d12_stock_ks_per_pixel_native_batch(
        native_context, diagnostic);
    if (drawn) {
        initialized_ = true;
        const std::size_t layer = static_cast<std::size_t>(
            texture_target_physical_array_layer(
                info_.description, batch.target_subresource));
        if (layer < base_mip_initialized_.size())
            base_mip_initialized_[layer] = 1U;
        if (resolve_target != nullptr) {
            resolve_target->initialized_ = true;
            if (!resolve_target->base_mip_initialized_.empty())
                resolve_target->base_mip_initialized_[0U] = 1U;
        }
    }
    return drawn;
}

[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE d3d12_sampler_address(SamplerAddressMode mode) noexcept {
    switch (mode) {
    case SamplerAddressMode::repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case SamplerAddressMode::mirrored_repeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case SamplerAddressMode::clamp_to_edge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case SamplerAddressMode::clamp_to_border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

[[nodiscard]] D3D12_COMPARISON_FUNC d3d12_sampler_compare(SamplerCompare compare) noexcept {
    switch (compare) {
    case SamplerCompare::less: return D3D12_COMPARISON_FUNC_LESS;
    case SamplerCompare::less_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case SamplerCompare::greater: return D3D12_COMPARISON_FUNC_GREATER;
    case SamplerCompare::greater_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case SamplerCompare::equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case SamplerCompare::not_equal: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case SamplerCompare::always: return D3D12_COMPARISON_FUNC_ALWAYS;
    case SamplerCompare::never: return D3D12_COMPARISON_FUNC_NEVER;
    case SamplerCompare::disabled: break;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

[[nodiscard]] D3D12_FILTER d3d12_sampler_filter(const SamplerDescription& description) noexcept {
    const bool anisotropic = description.min_filter == SamplerFilter::anisotropic ||
                             description.mag_filter == SamplerFilter::anisotropic ||
                             description.mip_filter == SamplerFilter::anisotropic;
    const bool comparison = description.compare != SamplerCompare::disabled;
    if (anisotropic) return comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    const bool min_linear = description.min_filter == SamplerFilter::linear;
    const bool mag_linear = description.mag_filter == SamplerFilter::linear;
    const bool mip_linear = description.mip_filter == SamplerFilter::linear;
    const unsigned key = (static_cast<unsigned>(min_linear) << 2U) |
                         (static_cast<unsigned>(mag_linear) << 1U) | static_cast<unsigned>(mip_linear);
    constexpr D3D12_FILTER normal[] = {
        D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
        D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR,
        D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR};
    constexpr D3D12_FILTER comparison_filters[] = {
        D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR,
        D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT,
        D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR,
        D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT,
        D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR};
    return comparison ? comparison_filters[key] : normal[key];
}

class D3D12Device final : public Device {
public:
    explicit D3D12Device(std::shared_ptr<D3D12Context> context) : context_(std::move(context)) {}

    const DeviceInfo& info() const noexcept override { return context_->info; }
    bool owns_resource(const Texture& texture) const noexcept override {
        const auto* native = dynamic_cast<const D3D12Texture*>(&texture);
        return native != nullptr && native->context() == context_.get();
    }
    bool owns_resource(
        const DepthAttachment& attachment) const noexcept override {
        const auto* native =
            dynamic_cast<const D3D12DepthAttachment*>(&attachment);
        return native != nullptr && native->context() == context_.get();
    }
    bool owns_resource(const Sampler& sampler) const noexcept override {
        const auto* native = dynamic_cast<const D3D12Sampler*>(&sampler);
        return native != nullptr && native->context() == context_.get();
    }

    PresentationCapabilities presentation_capabilities() const noexcept override {
        const bool context_ready = context_ != nullptr && context_->factory.Get() != nullptr &&
                                   context_->device.Get() != nullptr && context_->queue.Get() != nullptr;
        PresentationCapabilities result;
        result.offscreen = true;
        result.swapchain_api_available = context_ready;
        result.native_surface_api_available = context_ready;
        result.headless_surface_api_available = false;
        return result;
    }

    PresentationTargetResult create_presentation_target(
        const PresentationTargetDescription& description) override {
        Diagnostic diagnostic;
        const PresentationTargetStatus validation =
            validate_presentation_target_description(description, diagnostic);
        if (validation != PresentationTargetStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        if (context_ == nullptr || context_->native_window == nullptr)
            return {PresentationTargetStatus::unsupported,
                    {"d3d12_native_window_required",
                     "D3D12 presentation requires an initialized Win32 window"}, nullptr};
        if (!IsWindow(static_cast<HWND>(context_->native_window)))
            return {PresentationTargetStatus::unsupported,
                    {"d3d12_native_window_invalid",
                     "D3D12 native presentation requires a live Win32 window handle"}, nullptr};
        if (context_->presentation_target_active)
            return {PresentationTargetStatus::unsupported,
                    {"d3d12_presentation_target_active",
                     "This D3D12 device already owns a presentation target"}, nullptr};
        try {
            auto target = std::make_unique<D3D12PresentationTarget>(
                context_, description, diagnostic);
            if (!target->valid())
                return {PresentationTargetStatus::allocation_failed,
                        std::move(diagnostic), nullptr};
            context_->presentation_target_active = true;
            return {PresentationTargetStatus::ready, {}, std::move(target)};
        } catch (const std::bad_alloc&) {
            return {PresentationTargetStatus::allocation_failed,
                    {"d3d12_presentation_target_allocation_failed",
                     "D3D12 presentation target allocation exhausted host memory"},
                    nullptr};
        }
    }

    PresentationFrameResult clear_and_present(
        PresentationTarget& target, const std::array<float, 4>& clear_color) override {
        auto* d3d_target = dynamic_cast<D3D12PresentationTarget*>(&target);
        if (d3d_target == nullptr || target.backend() != Backend::D3D12)
            return {PresentationFrameStatus::unsupported,
                    {"presentation_target_backend_mismatch",
                     "The D3D12 device received a presentation target from another backend"}};
        if (d3d_target->context().get() != context_.get())
            return {PresentationFrameStatus::unsupported,
                    {"presentation_target_device_mismatch",
                     "The presentation target belongs to another D3D12 device"}};
        std::lock_guard command_guard(context_->command_mutex);
        return d3d_target->clear_and_present(clear_color);
    }

    PresentationFrameResult present_texture(
        PresentationTarget& target, Texture& source) override {
        auto* d3d_target = dynamic_cast<D3D12PresentationTarget*>(&target);
        auto* d3d_source = dynamic_cast<D3D12Texture*>(&source);
        if (d3d_target == nullptr || d3d_source == nullptr || target.backend() != Backend::D3D12 ||
            source.backend() != Backend::D3D12)
            return {PresentationFrameStatus::unsupported,
                    {"presentation_texture_backend_mismatch",
                     "The D3D12 presentation target and source must use D3D12"}};
        if (d3d_target->context().get() != context_.get() || d3d_source->context() != context_.get())
            return {PresentationFrameStatus::unsupported,
                    {"presentation_texture_device_mismatch",
                     "The presentation target and source must belong to this D3D12 device"}};
        std::lock_guard command_guard(context_->command_mutex);
        return d3d_target->present_texture(*d3d_source);
    }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        Diagnostic diagnostic;
        if (!valid_buffer_description(description, initial_data.size(), diagnostic))
            return {BufferStatus::invalid_description, std::move(diagnostic), nullptr};
        if (!valid_d3d12_usage(description.usage, diagnostic))
            return {BufferStatus::unsupported, std::move(diagnostic), nullptr};
        const auto usage_bits = static_cast<std::uint32_t>(description.usage);
        if (description.memory == BufferMemory::host_visible &&
            (usage_bits & (static_cast<std::uint32_t>(BufferUsage::storage) |
                           static_cast<std::uint32_t>(BufferUsage::transfer_destination))) != 0U) {
            return {BufferStatus::unsupported,
                    {"d3d12_host_visible_usage_unsupported",
                     "D3D12 upload heaps do not support host-visible storage or transfer-destination buffers"},
                    nullptr};
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = description.memory == BufferMemory::host_visible ? D3D12_HEAP_TYPE_UPLOAD
                                                                      : D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resource_description{};
        resource_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource_description.Width = static_cast<UINT64>(description.size_bytes);
        resource_description.Height = 1;
        resource_description.DepthOrArraySize = 1;
        resource_description.MipLevels = 1;
        resource_description.SampleDesc.Count = 1;
        resource_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_RESOURCE_STATES final_state = resource_state(description.usage);
        const bool device_local = description.memory == BufferMemory::device_local;
        const bool needs_upload = device_local && !initial_data.empty();
        const D3D12_RESOURCE_STATES initial_state = !device_local
                                                       ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                       : needs_upload ? D3D12_RESOURCE_STATE_COPY_DEST : final_state;
        ComPtr<ID3D12Resource> resource;
        const HRESULT result = context_->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource_description, initial_state, nullptr,
            IID_PPV_ARGS(&resource));
        if (FAILED(result)) {
            Diagnostic failure = hresult_error("ID3D12Device::CreateCommittedResource", result);
            failure.code = "buffer_allocation_failed";
            return {BufferStatus::allocation_failed, std::move(failure), nullptr};
        }
        auto buffer = std::make_unique<D3D12Buffer>(context_, std::move(resource), description, initial_state);
        bool uploaded = true;
        if (!initial_data.empty()) {
            uploaded = device_local ? buffer->write_device(0, initial_data, diagnostic)
                                    : buffer->write_host(0, initial_data, diagnostic);
        }
        if (!uploaded) return {BufferStatus::upload_failed, std::move(diagnostic), nullptr};
        return {BufferStatus::ready, {}, std::move(buffer)};
    }

    BufferUpdateResult update_buffer(Buffer& buffer,
                                      std::uint64_t offset,
                                      std::span<const std::byte> data) override {
        Diagnostic diagnostic;
        if (buffer.backend() != Backend::D3D12)
            return {BufferStatus::unsupported, {"buffer_backend_mismatch", "The buffer belongs to another graphics backend"}};
        auto* d3d_buffer = dynamic_cast<D3D12Buffer*>(&buffer);
        if (d3d_buffer == nullptr)
            return {BufferStatus::unsupported, {"buffer_type_unsupported", "The D3D12 device received an unknown buffer handle"}};
        if (d3d_buffer->context() != context_.get())
            return {BufferStatus::unsupported,
                    {"buffer_context_mismatch", "The buffer belongs to another D3D12 device"}};
        if (!valid_buffer_update(buffer, offset, data.size(), diagnostic))
            return {BufferStatus::invalid_description, std::move(diagnostic)};
        const bool updated = d3d_buffer->info().description.memory == BufferMemory::host_visible
                                 ? d3d_buffer->write_host(offset, data, diagnostic)
                                 : d3d_buffer->write_device(offset, data, diagnostic);
        return updated ? BufferUpdateResult{BufferStatus::ready, {}}
                       : BufferUpdateResult{BufferStatus::upload_failed, std::move(diagnostic)};
    }

    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription& description) override {
        Diagnostic diagnostic;
        const DepthAttachmentStatus validation =
            validate_depth_attachment_description(description, diagnostic);
        if (validation != DepthAttachmentStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        if (description.format != DepthAttachmentFormat::d32_float ||
            (description.samples != 1U && description.samples != 4U)) {
            return {DepthAttachmentStatus::unsupported,
                    {"d3d12_depth_attachment_format_unsupported",
                     "D3D12 persistent depth attachments support only single- or four-sample D32_FLOAT"},
                    nullptr};
        }
        if (!validate_d3d12_sample_count(context_, DXGI_FORMAT_D32_FLOAT, description.samples, diagnostic))
            return {DepthAttachmentStatus::unsupported, std::move(diagnostic), nullptr};
        D3D12_RESOURCE_DESC resource_description{};
        resource_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_description.Width = description.width;
        resource_description.Height = description.height;
        resource_description.DepthOrArraySize = 1U;
        resource_description.MipLevels = 1U;
        resource_description.Format = description.shader_readable
                                          ? DXGI_FORMAT_R32_TYPELESS
                                          : DXGI_FORMAT_D32_FLOAT;
        resource_description.SampleDesc.Count = description.samples;
        resource_description.SampleDesc.Quality = 0U;
        resource_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear_value{};
        clear_value.Format = DXGI_FORMAT_D32_FLOAT;
        clear_value.DepthStencil.Depth = 1.0F;
        clear_value.DepthStencil.Stencil = 0U;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> resource;
        HRESULT result = context_->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource_description, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear_value, IID_PPV_ARGS(&resource));
        if (FAILED(result)) {
            Diagnostic failure = hresult_error("ID3D12Device::CreateCommittedResource(depth)", result);
            failure.code = "depth_attachment_allocation_failed";
            return {DepthAttachmentStatus::allocation_failed, std::move(failure), nullptr};
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap_description.NumDescriptors = 2U;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> dsv_heap;
        result = context_->device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&dsv_heap));
        if (FAILED(result)) {
            Diagnostic failure = hresult_error("ID3D12Device::CreateDescriptorHeap(depth DSV)", result);
            failure.code = "depth_attachment_allocation_failed";
            return {DepthAttachmentStatus::allocation_failed, std::move(failure), nullptr};
        }
        const UINT dsv_stride = context_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const D3D12_CPU_DESCRIPTOR_HANDLE write_dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE read_dsv = write_dsv;
        read_dsv.ptr += static_cast<SIZE_T>(dsv_stride);
        D3D12_DEPTH_STENCIL_VIEW_DESC view_description{};
        view_description.Format = DXGI_FORMAT_D32_FLOAT;
        view_description.ViewDimension = description.samples > 1U ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                                     : D3D12_DSV_DIMENSION_TEXTURE2D;
        view_description.Texture2D.MipSlice = 0U;
        context_->device->CreateDepthStencilView(resource.Get(), &view_description, write_dsv);
        view_description.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        context_->device->CreateDepthStencilView(resource.Get(), &view_description, read_dsv);
        ComPtr<ID3D12DescriptorHeap> srv_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        if (description.shader_readable) {
            D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
            srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srv_heap_description.NumDescriptors = 1U;
            srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            result = context_->device->CreateDescriptorHeap(
                &srv_heap_description, IID_PPV_ARGS(&srv_heap));
            if (FAILED(result)) {
                Diagnostic failure = hresult_error(
                    "ID3D12Device::CreateDescriptorHeap(depth SRV)", result);
                failure.code = "depth_attachment_allocation_failed";
                return {DepthAttachmentStatus::allocation_failed,
                        std::move(failure), nullptr};
            }
            srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_description{};
            srv_description.Format = DXGI_FORMAT_R32_FLOAT;
            srv_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_description.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_description.Texture2D.MipLevels = 1U;
            context_->device->CreateShaderResourceView(
                resource.Get(), &srv_description, srv);
        }
        return {DepthAttachmentStatus::ready,
                {},
                std::make_unique<D3D12DepthAttachment>(context_, std::move(resource),
                                                        std::move(dsv_heap), std::move(srv_heap),
                                                        description, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                        write_dsv, read_dsv, srv)};
    }

    DepthAttachmentReadbackResult read_depth_attachment(
        DepthAttachment& attachment,
        const DepthAttachmentReadbackRequest& request) override {
        Diagnostic diagnostic;
        const DepthAttachmentReadbackStatus validation =
            validate_depth_attachment_readback(attachment, request, diagnostic);
        if (validation != DepthAttachmentReadbackStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (attachment.backend() != Backend::D3D12)
            return {DepthAttachmentReadbackStatus::unsupported,
                    {"depth_attachment_backend_mismatch",
                     "The D3D12 device received a depth attachment from another backend"},
                    {}};
        auto* d3d_depth = dynamic_cast<D3D12DepthAttachment*>(&attachment);
        if (d3d_depth == nullptr)
            return {DepthAttachmentReadbackStatus::unsupported,
                    {"depth_attachment_type_unsupported",
                     "The D3D12 device received an unknown depth attachment handle"},
                    {}};
        if (d3d_depth->context() != context_.get())
            return {DepthAttachmentReadbackStatus::unsupported,
                    {"depth_attachment_context_mismatch",
                     "The depth attachment belongs to another D3D12 device"},
                    {}};
        if (!d3d_depth->cleared())
            return {DepthAttachmentReadbackStatus::invalid_request,
                    {"depth_attachment_uninitialized",
                     "D32 depth readback requires an initialized depth attachment"},
                    {}};
        std::vector<float> output;
        if (!d3d_depth->readback(request, output, diagnostic))
            return {DepthAttachmentReadbackStatus::execution_failed, std::move(diagnostic), {}};
        return {DepthAttachmentReadbackStatus::ready, {}, std::move(output)};
    }

    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan& initial_uploads) override {
        Diagnostic diagnostic;
        const TextureStatus validation = validate_texture_description(description, initial_uploads, diagnostic);
        if (validation != TextureStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        if (!valid_d3d12_texture_usage(description, diagnostic))
            return {TextureStatus::unsupported, std::move(diagnostic), nullptr};
        if (description.memory != TextureMemory::device_local) {
            return {TextureStatus::unsupported,
                    {"d3d12_host_visible_texture_unsupported",
                     "D3D12 texture uploads use default-heap resources; host-visible texture memory is unsupported"},
                    nullptr};
        }
        if (texture_format_is_compressed(description.format)) {
            if (!is_d3d12_supported_block_format(description.format)) {
                return {TextureStatus::unsupported,
                        {"d3d12_compressed_format_unsupported",
                         "D3D12 supports only BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 compressed sampled textures"},
                        nullptr};
            }
            const bool supported_layers = description.shape == TextureShape::texture_cube ||
                                          description.array_layers == 1U;
            if (description.usage != TextureUsage::sampled ||
                description.mutability != TextureMutability::immutable ||
                description.samples != 1U || !supported_layers) {
                return {TextureStatus::unsupported,
                        {"d3d12_compressed_texture_unsupported",
                         "D3D12 BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 textures require immutable sampled one-layer 2D or explicit cube, single-sample resources"},
                        nullptr};
            }
            if (!validate_d3d12_texture_format_support(
                    context_, dxgi_texture_format(description.format), description.mip_levels, diagnostic))
                return {TextureStatus::unsupported, std::move(diagnostic), nullptr};
        }
        if ((description.format == TextureFormat::r32_sfloat ||
             description.format == TextureFormat::rgba16_sfloat) &&
            !validate_d3d12_texture_format_support(
                context_, dxgi_texture_format(description.format), description.mip_levels,
                diagnostic, description.usage))
            return {TextureStatus::unsupported, std::move(diagnostic), nullptr};
        if (description.samples != 1U) {
            const std::uint32_t usage = static_cast<std::uint32_t>(description.usage);
            const std::uint32_t required_usage =
                static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                static_cast<std::uint32_t>(TextureUsage::transfer_source);
            if (description.samples != 4U || usage != required_usage ||
                description.mip_levels != 1U || description.array_layers != 1U ||
                !initial_uploads.subresources.empty()) {
                return {TextureStatus::unsupported,
                        {"d3d12_multisample_texture_unsupported",
                         "D3D12 multisample textures require one-layer, one-mip color targets with transfer-source usage and no upload"},
                        nullptr};
            }
            if (!validate_d3d12_sample_count(context_, dxgi_texture_format(description.format),
                                             description.samples, diagnostic))
                return {TextureStatus::unsupported, std::move(diagnostic), nullptr};
        }
        const D3D12_RESOURCE_STATES final_state =
            texture_state(description.usage, description.access_policy);
        const bool needs_upload = !initial_uploads.subresources.empty();
        const D3D12_RESOURCE_STATES initial_state = needs_upload ? D3D12_RESOURCE_STATE_COPY_DEST : final_state;
        D3D12_RESOURCE_DESC resource_description{};
        resource_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_description.Width = description.width;
        resource_description.Height = description.height;
        resource_description.DepthOrArraySize =
            static_cast<UINT16>(texture_physical_array_layers(description));
        resource_description.MipLevels = static_cast<UINT16>(description.mip_levels);
        resource_description.Format = dxgi_texture_format(description.format);
        resource_description.SampleDesc.Count = description.samples;
        resource_description.SampleDesc.Quality = 0U;
        resource_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_description.Flags = texture_flags(description.usage);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> resource;
        const HRESULT result = context_->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource_description, initial_state, nullptr,
            IID_PPV_ARGS(&resource));
        if (FAILED(result)) {
            Diagnostic failure = hresult_error("ID3D12Device::CreateCommittedResource(texture)", result);
            failure.code = "texture_allocation_failed";
            return {TextureStatus::allocation_failed, std::move(failure), nullptr};
        }
        auto texture = std::make_unique<D3D12Texture>(context_, std::move(resource), description,
                                                      initial_state, false);
        if (!initial_uploads.subresources.empty() && !texture->upload(initial_uploads, diagnostic))
            return {TextureStatus::upload_failed, std::move(diagnostic), nullptr};
        return {TextureStatus::ready, {}, std::move(texture)};
    }

    TextureUpdateResult update_texture(Texture& texture,
                                       const TextureUploadPlan& uploads) override {
        Diagnostic diagnostic;
        if (texture.backend() != Backend::D3D12)
            return {TextureStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}};
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        if (d3d_texture == nullptr)
            return {TextureStatus::unsupported,
                    {"texture_type_unsupported", "The D3D12 device received an unknown texture handle"}};
        if (d3d_texture->context() != context_.get())
            return {TextureStatus::unsupported,
                    {"texture_context_mismatch", "The texture belongs to another D3D12 device"}};
        if (!valid_texture_update(texture, uploads, diagnostic))
            return {TextureStatus::invalid_description, std::move(diagnostic)};
        return d3d_texture->upload(uploads, diagnostic)
                   ? TextureUpdateResult{TextureStatus::ready, {}}
                   : TextureUpdateResult{TextureStatus::upload_failed, std::move(diagnostic)};
    }

    TextureUpdateResult generate_texture_mips(Texture& texture) override {
        Diagnostic diagnostic;
        const TextureStatus validation =
            validate_texture_mip_generation_description(
                texture.info().description, diagnostic);
        if (validation != TextureStatus::ready)
            return {validation, std::move(diagnostic)};
        if (texture.backend() != Backend::D3D12)
            return {TextureStatus::unsupported,
                    {"texture_backend_mismatch",
                     "The texture belongs to another graphics backend"}};
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        if (d3d_texture == nullptr)
            return {TextureStatus::unsupported,
                    {"texture_type_unsupported",
                     "The D3D12 device received an unknown texture handle"}};
        if (d3d_texture->context() != context_.get())
            return {TextureStatus::unsupported,
                    {"texture_context_mismatch",
                     "The texture belongs to another D3D12 device"}};
        if (!d3d_texture->base_mip_initialized())
            return {TextureStatus::invalid_description,
                    {"texture_mip_generation_base_uninitialized",
                     "Texture mip generation requires initialized mip-zero data for every physical layer"}};
        if (!validate_d3d12_texture_format_support(
                context_, dxgi_texture_format(texture.info().description.format),
                texture.info().description.mip_levels, diagnostic,
                texture.info().description.usage))
            return {TextureStatus::unsupported, std::move(diagnostic)};
        return d3d_texture->generate_mips(diagnostic)
                   ? TextureUpdateResult{TextureStatus::ready, {}}
                   : TextureUpdateResult{TextureStatus::upload_failed,
                                         std::move(diagnostic)};
    }

    HdrLuminanceResult measure_hdr_luminance(Texture& texture) override {
        HdrLuminanceResult output;
        Diagnostic diagnostic;
        const HdrLuminanceStatus validation =
            validate_hdr_luminance_request(texture, diagnostic);
        if (validation != HdrLuminanceStatus::ready) {
            output.status = validation;
            output.diagnostic = std::move(diagnostic);
            return output;
        }
        if (texture.backend() != Backend::D3D12) {
            output.status = HdrLuminanceStatus::unsupported;
            output.diagnostic = {
                "hdr_luminance_backend_mismatch",
                "D3D12 HDR luminance measurement requires a D3D12 texture"};
            return output;
        }
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        if (d3d_texture == nullptr) {
            output.status = HdrLuminanceStatus::unsupported;
            output.diagnostic = {
                "hdr_luminance_texture_type_unsupported",
                "The D3D12 device received an unknown HDR luminance texture handle"};
            return output;
        }
        if (d3d_texture->context() != context_.get()) {
            output.status = HdrLuminanceStatus::unsupported;
            output.diagnostic = {
                "hdr_luminance_context_mismatch",
                "HDR luminance texture belongs to another D3D12 device"};
            return output;
        }
        if (!d3d_texture->initialized() ||
            !d3d_texture->base_mip_initialized()) {
            output.status = HdrLuminanceStatus::invalid_request;
            output.diagnostic = {
                "hdr_luminance_source_uninitialized",
                "HDR luminance measurement requires an initialized source texture"};
            return output;
        }
        if (!validate_d3d12_texture_format_support(
                context_, dxgi_texture_format(texture.info().description.format),
                texture.info().description.mip_levels, diagnostic,
                texture.info().description.usage)) {
            output.status = HdrLuminanceStatus::unsupported;
            output.diagnostic = std::move(diagnostic);
            return output;
        }
        // The reduction pass is deliberately shared with reflection capture:
        // it provides the same linear RGBA16F filtering and restores the
        // texture's tracked shader-readable state before the copy below.
        if (texture.info().description.mip_levels > 1U &&
            !d3d_texture->generate_mips(diagnostic)) {
            output.status = HdrLuminanceStatus::execution_failed;
            output.diagnostic = std::move(diagnostic);
            return output;
        }
        if (!read_d3d12_hdr_luminance(
                context_, d3d_texture->resource(), texture.info().description,
                *d3d_texture->state_pointer(), output.luminance, diagnostic)) {
            output.status = HdrLuminanceStatus::execution_failed;
            output.diagnostic = std::move(diagnostic);
            return output;
        }
        output.status = HdrLuminanceStatus::ready;
        output.diagnostic = {};
        return output;
    }

    HdrToneMapResult tone_map_hdr_texture(
        Texture& source, Texture& destination,
        const HdrToneMapParameters& parameters = {}) override {
        Diagnostic diagnostic;
        const HdrToneMapStatus validation = validate_hdr_tone_map_request(
            source, destination, parameters, diagnostic);
        if (validation != HdrToneMapStatus::ready)
            return {validation, std::move(diagnostic)};
        if (source.backend() != Backend::D3D12 ||
            destination.backend() != Backend::D3D12)
            return {HdrToneMapStatus::unsupported,
                    {"hdr_tone_map_backend_mismatch",
                     "D3D12 HDR tone mapping requires D3D12 textures"}};
        auto* d3d_source = dynamic_cast<D3D12Texture*>(&source);
        auto* d3d_destination = dynamic_cast<D3D12Texture*>(&destination);
        if (d3d_source == nullptr || d3d_destination == nullptr)
            return {HdrToneMapStatus::unsupported,
                    {"hdr_tone_map_texture_type_unsupported",
                     "The D3D12 device received an unknown tone-map texture handle"}};
        if (d3d_source->context() != context_.get() ||
            d3d_destination->context() != context_.get())
            return {HdrToneMapStatus::unsupported,
                    {"hdr_tone_map_context_mismatch",
                     "HDR tone-map textures belong to another D3D12 device"}};
        if (!d3d_source->initialized() || !d3d_source->base_mip_initialized())
            return {HdrToneMapStatus::invalid_request,
                    {"hdr_tone_map_source_uninitialized",
                     "HDR tone mapping requires an initialized source texture"}};
        const DXGI_FORMAT source_format = dxgi_texture_format(
            source.info().description.format);
        const DXGI_FORMAT destination_format = dxgi_texture_format(
            destination.info().description.format);
        if (!validate_d3d12_texture_format_support(
                context_, source_format, 1U, diagnostic,
                source.info().description.usage))
            return {HdrToneMapStatus::unsupported, std::move(diagnostic)};
        if (!validate_d3d12_texture_format_support(
                context_, destination_format, 1U, diagnostic,
                destination.info().description.usage))
            return {HdrToneMapStatus::unsupported, std::move(diagnostic)};
        if (parameters.bloom.enabled) {
            ComPtr<ID3D12Resource> bloom_composite;
            if (!execute_d3d12_hdr_bloom(
                    context_, d3d_source->resource(),
                    *d3d_source->state_pointer(), source.info().description,
                    parameters.exposure, parameters.bloom, bloom_composite,
                    diagnostic) ||
                !execute_d3d12_hdr_bloom_tone_map(
                    context_, d3d_source->resource(),
                    *d3d_source->state_pointer(), bloom_composite.Get(),
                    d3d_destination->resource(),
                    *d3d_destination->state_pointer(),
                    destination.info().description, parameters, diagnostic))
                return {HdrToneMapStatus::execution_failed,
                        std::move(diagnostic)};
        } else if (!execute_d3d12_hdr_tone_map(
                       context_, d3d_source->resource(),
                       *d3d_source->state_pointer(), d3d_destination->resource(),
                       *d3d_destination->state_pointer(),
                       destination.info().description, parameters, diagnostic))
            return {HdrToneMapStatus::execution_failed, std::move(diagnostic)};
        d3d_destination->mark_initialized();
        return {HdrToneMapStatus::ready, {}};
    }

    FxaaResult apply_fxaa(Texture& source, Texture& destination) override {
        Diagnostic diagnostic;
        const FxaaStatus validation =
            validate_fxaa_request(source, destination, diagnostic);
        if (validation != FxaaStatus::ready)
            return {validation, std::move(diagnostic)};
        if (source.backend() != Backend::D3D12 ||
            destination.backend() != Backend::D3D12)
            return {FxaaStatus::unsupported,
                    {"fxaa_backend_mismatch",
                     "D3D12 FXAA requires D3D12 textures"}};
        auto* d3d_source = dynamic_cast<D3D12Texture*>(&source);
        auto* d3d_destination = dynamic_cast<D3D12Texture*>(&destination);
        if (d3d_source == nullptr || d3d_destination == nullptr)
            return {FxaaStatus::unsupported,
                    {"fxaa_texture_type_unsupported",
                     "The D3D12 device received an unknown FXAA texture handle"}};
        if (d3d_source->context() != context_.get() ||
            d3d_destination->context() != context_.get())
            return {FxaaStatus::unsupported,
                    {"fxaa_context_mismatch",
                     "FXAA textures belong to another D3D12 device"}};
        if (!d3d_source->initialized())
            return {FxaaStatus::invalid_request,
                    {"fxaa_source_uninitialized",
                     "FXAA requires an initialized source texture"}};
        if (!validate_d3d12_texture_format_support(
                context_, DXGI_FORMAT_R8G8B8A8_UNORM, 1U, diagnostic,
                source.info().description.usage))
            return {FxaaStatus::unsupported, std::move(diagnostic)};
        if (!validate_d3d12_texture_format_support(
                context_, dxgi_texture_format(destination.info().description.format),
                1U, diagnostic, destination.info().description.usage))
            return {FxaaStatus::unsupported, std::move(diagnostic)};
        if (!execute_d3d12_fxaa(
                context_, d3d_source->resource(), *d3d_source->state_pointer(),
                d3d_destination->resource(), *d3d_destination->state_pointer(),
                destination.info().description, diagnostic))
            return {FxaaStatus::execution_failed, std::move(diagnostic)};
        d3d_destination->mark_initialized();
        return {FxaaStatus::ready, {}};
    }

    TextureClearReadbackResult clear_texture_and_readback(
        Texture& texture, const TextureClearReadbackRequest& request) override {
        Diagnostic diagnostic;
        const TextureReadbackStatus validation = validate_texture_clear_readback(texture, request, diagnostic);
        if (validation != TextureReadbackStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (texture.backend() != Backend::D3D12)
            return {TextureReadbackStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}, {}};
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        if (d3d_texture == nullptr)
            return {TextureReadbackStatus::unsupported,
                    {"texture_type_unsupported", "The D3D12 device received an unknown texture handle"}, {}};
        if (d3d_texture->context() != context_.get())
            return {TextureReadbackStatus::unsupported,
                    {"texture_context_mismatch", "The texture belongs to another D3D12 device"}, {}};
        std::vector<std::byte> output;
        if (!d3d_texture->clear_readback(request, output, diagnostic))
            return {TextureReadbackStatus::execution_failed, std::move(diagnostic), {}};
        return {TextureReadbackStatus::ready, {}, std::move(output)};
    }

    TriangleDrawResult draw_triangle_and_readback(
        Texture& texture, const TriangleDrawRequest& request) override {
        Diagnostic diagnostic;
        const TriangleDrawStatus validation = validate_triangle_draw_request(texture, request, diagnostic);
        if (validation != TriangleDrawStatus::ready) return {validation, std::move(diagnostic), {}};
        if (texture.backend() != Backend::D3D12)
            return {TriangleDrawStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}, {}};
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        if (d3d_texture == nullptr)
            return {TriangleDrawStatus::unsupported,
                    {"texture_type_unsupported", "The D3D12 device received an unknown texture handle"}, {}};
        if (d3d_texture->context() != context_.get())
            return {TriangleDrawStatus::unsupported,
                    {"texture_context_mismatch", "The texture belongs to another D3D12 device"}, {}};
        std::vector<std::byte> output;
        if (!d3d_texture->draw_triangle(request, output, diagnostic))
            return {TriangleDrawStatus::execution_failed, std::move(diagnostic), {}};
        return {TriangleDrawStatus::ready, {}, std::move(output)};
    }

    IndexedStaticMeshDrawResult draw_indexed_static_mesh_and_readback(
        Texture& texture, const IndexedStaticMeshDrawRequest& request) override {
        Diagnostic diagnostic;
        const IndexedStaticMeshDrawStatus validation =
            validate_indexed_static_mesh_draw_request(texture, request, diagnostic);
        if (validation != IndexedStaticMeshDrawStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (texture.backend() != Backend::D3D12)
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}, {}};
        if (request.vertex_buffer->backend() != Backend::D3D12 ||
            request.index_buffer->backend() != Backend::D3D12) {
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_backend_mismatch",
                     "Indexed static-mesh resources must belong to the D3D12 backend"}, {}};
        }
        D3D12DepthAttachment* d3d_depth = nullptr;
        if (request.depth_attachment != nullptr) {
            if (request.depth_attachment->backend() != Backend::D3D12) {
                return {IndexedStaticMeshDrawStatus::unsupported,
                        {"indexed_static_mesh_backend_mismatch",
                         "Indexed static-mesh depth attachment must belong to the D3D12 backend"}, {}};
            }
            d3d_depth = dynamic_cast<D3D12DepthAttachment*>(request.depth_attachment);
            if (d3d_depth == nullptr) {
                return {IndexedStaticMeshDrawStatus::unsupported,
                        {"indexed_static_mesh_type_unsupported",
                         "The D3D12 device received an unknown depth attachment handle"}, {}};
            }
        }
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        const auto* d3d_vertex = dynamic_cast<const D3D12Buffer*>(request.vertex_buffer);
        const auto* d3d_index = dynamic_cast<const D3D12Buffer*>(request.index_buffer);
        if (d3d_texture == nullptr || d3d_vertex == nullptr || d3d_index == nullptr)
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_type_unsupported", "The D3D12 device received an unknown indexed static-mesh handle"}, {}};
        const D3D12Context* context = context_.get();
        if (d3d_texture->context() != context || d3d_vertex->context() != context || d3d_index->context() != context)
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_context_mismatch", "Indexed static-mesh resources belong to another D3D12 device"}, {}};
        if (d3d_depth != nullptr && d3d_depth->context() != context)
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_context_mismatch",
                     "Indexed static-mesh depth attachment belongs to another D3D12 device"}, {}};
        std::vector<std::byte> output;
        if (request.shader_authority ==
            IndexedShaderAuthority::explicit_stock_ks_per_pixel_native) {
            if (!d3d_texture->draw_stock_ks_per_pixel_native(
                    request, *d3d_vertex, *d3d_index, d3d_depth, output,
                    diagnostic))
                return {indexed_static_mesh_draw_status(
                            classify_d3d12_stock_ks_per_pixel_failure(
                                diagnostic.code)),
                        std::move(diagnostic), {}};
            return {IndexedStaticMeshDrawStatus::ready, {},
                    std::move(output)};
        }
        if (!d3d_texture->draw_indexed_static_mesh(request, *d3d_vertex, *d3d_index, d3d_depth, output, diagnostic))
            return {IndexedStaticMeshDrawStatus::execution_failed, std::move(diagnostic), {}};
        return {IndexedStaticMeshDrawStatus::ready, {}, std::move(output)};
    }

    IndexedStaticMeshBatchResult draw_indexed_static_mesh_batch_and_readback(
        Texture& texture, const IndexedStaticMeshBatchDescription& batch) override {
        Diagnostic diagnostic;
        const IndexedStaticMeshBatchStatus validation =
            validate_indexed_static_mesh_batch_description(texture, batch, diagnostic);
        if (validation != IndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (texture.backend() != Backend::D3D12)
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}, {}};
        auto* d3d_texture = dynamic_cast<D3D12Texture*>(&texture);
        if (d3d_texture == nullptr)
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"texture_type_unsupported", "The D3D12 device received an unknown batch texture handle"}, {}};
        D3D12DepthAttachment* d3d_depth = nullptr;
        if (batch.depth_attachment != nullptr) {
            if (batch.depth_attachment->backend() != Backend::D3D12)
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"indexed_static_mesh_batch_depth_backend_mismatch",
                         "The batch depth attachment belongs to another graphics backend"}, {}};
            d3d_depth = dynamic_cast<D3D12DepthAttachment*>(batch.depth_attachment);
            if (d3d_depth == nullptr)
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"indexed_static_mesh_batch_depth_type_unsupported",
                         "The D3D12 device received an unknown batch depth attachment handle"}, {}};
        }
        const D3D12Context* context = context_.get();
        D3D12Texture* resolve_target = nullptr;
        if (batch.resolve_target != nullptr) {
            resolve_target = dynamic_cast<D3D12Texture*>(batch.resolve_target);
            if (resolve_target == nullptr || resolve_target->context() != context) {
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"indexed_static_mesh_batch_resolve_context_mismatch",
                         "The resolve target belongs to another D3D12 device"}, {}};
            }
        }
        if (d3d_texture->context() != context ||
            (d3d_depth != nullptr && d3d_depth->context() != context)) {
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"indexed_static_mesh_batch_context_mismatch",
                     "Batch resources belong to another D3D12 device"}, {}};
        }
        const bool native_only = batch.selected_mesh_draws.empty() &&
                                  batch.overlay_draws.empty() &&
                                  !batch.viewport_frame_border.has_value() &&
                                  !batch.sky.has_value() &&
                                  !batch.clouds.has_value() &&
                                  !batch.grass.has_value() &&
                                  !batch.draws.empty() && std::all_of(
            batch.draws.begin(), batch.draws.end(),
            [](const IndexedStaticMeshDrawRequest& request) {
                return request.shader_authority ==
                       IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
            });
        if (native_only) {
            std::vector<D3D12StockKsPerPixelNativeBatchDraw> native_draws;
            try {
                native_draws.reserve(batch.draws.size());
            } catch (const std::bad_alloc&) {
                return {IndexedStaticMeshBatchStatus::execution_failed,
                        {"d3d12_stock_native_batch_allocation_failed",
                         "Native batch bridge allocation failed"},
                        {}};
            }
            for (const IndexedStaticMeshDrawRequest& request : batch.draws) {
                const auto* binding = request.stock_ks_per_pixel_native;
                const auto* resources =
                    binding != nullptr ? binding->resources : nullptr;
                if (resources == nullptr || resources->device() != this) {
                    return {IndexedStaticMeshBatchStatus::unsupported,
                            {"indexed_stock_native_batch_device_mismatch",
                             "Native batch owners belong to another D3D12 device"},
                            {}};
                }
                const auto* vertex =
                    dynamic_cast<const D3D12Buffer*>(request.vertex_buffer);
                const auto* index =
                    dynamic_cast<const D3D12Buffer*>(request.index_buffer);
                auto* diffuse = const_cast<D3D12Texture*>(
                    dynamic_cast<const D3D12Texture*>(
                        binding->diffuse_texture));
                const auto* vertex_shader =
                    dynamic_cast<const D3D12ShaderModule*>(
                        &resources->shader_program().vertex_shader());
                const auto* pixel_shader =
                    dynamic_cast<const D3D12ShaderModule*>(
                        &resources->shader_program().pixel_shader());
                const auto* linear_sampler =
                    dynamic_cast<const D3D12Sampler*>(
                        resources->samplers().sampler(
                            StockKsPerPixelNativeSamplerSlot::linear));
                const auto* shadow_sampler =
                    dynamic_cast<const D3D12Sampler*>(
                        resources->samplers().sampler(
                            StockKsPerPixelNativeSamplerSlot::shadow));
                std::array<const D3D12Buffer*, 5U> constants{};
                for (std::size_t index_value = 0U;
                     index_value < constants.size(); ++index_value) {
                    constants[index_value] =
                        dynamic_cast<const D3D12Buffer*>(
                            resources->constant_buffers().buffer(
                                static_cast<
                                    StockKsPerPixelNativeConstantSlot>(
                                    index_value)));
                }
                std::array<D3D12DepthAttachment*, 3U> shadows{};
                for (std::size_t index_value = 0U;
                     index_value < shadows.size(); ++index_value) {
                    shadows[index_value] =
                        const_cast<D3D12DepthAttachment*>(
                            dynamic_cast<const D3D12DepthAttachment*>(
                                binding->shadow_maps[index_value]));
                }
                const bool missing_type =
                    vertex == nullptr || index == nullptr || diffuse == nullptr ||
                    vertex_shader == nullptr || pixel_shader == nullptr ||
                    linear_sampler == nullptr || shadow_sampler == nullptr ||
                    std::any_of(constants.begin(), constants.end(),
                                [](const D3D12Buffer* value) {
                                    return value == nullptr;
                                }) ||
                    std::any_of(shadows.begin(), shadows.end(),
                                [](const D3D12DepthAttachment* value) {
                                    return value == nullptr;
                                });
                if (missing_type) {
                    return {IndexedStaticMeshBatchStatus::unsupported,
                            {"indexed_stock_native_batch_type_unsupported",
                             "Native batch contains an unknown D3D12 resource handle"},
                            {}};
                }
                const bool foreign_context =
                    vertex->context() != context || index->context() != context ||
                    diffuse->context() != context ||
                    vertex_shader->context() != context ||
                    pixel_shader->context() != context ||
                    linear_sampler->context() != context ||
                    shadow_sampler->context() != context ||
                    std::any_of(constants.begin(), constants.end(),
                                [&](const D3D12Buffer* value) {
                                    return value->context() != context;
                                }) ||
                    std::any_of(shadows.begin(), shadows.end(),
                                [&](const D3D12DepthAttachment* value) {
                                    return value->context() != context;
                                });
                if (foreign_context) {
                    return {IndexedStaticMeshBatchStatus::unsupported,
                            {"indexed_stock_native_batch_context_mismatch",
                             "Native batch resources belong to different D3D12 devices"},
                            {}};
                }
                if (!diffuse->initialized() ||
                    std::any_of(shadows.begin(), shadows.end(),
                                [](const D3D12DepthAttachment* value) {
                                    return !value->cleared();
                                })) {
                    return {IndexedStaticMeshBatchStatus::invalid_request,
                            {"indexed_stock_native_batch_resource_uninitialized",
                             "Native batch textures and shadow maps must be initialized"},
                            {}};
                }

                D3D12StockKsPerPixelNativeBatchDraw native_draw;
                native_draw.packet = request.packet;
                native_draw.pipeline = request.pipeline;
                native_draw.index_type = request.index_type;
                native_draw.vertex_buffer = vertex->resource();
                native_draw.vertex_state = vertex->state();
                native_draw.index_buffer = index->resource();
                native_draw.index_state = index->state();
                native_draw.shader_program = &resources->shader_program();
                native_draw.constant_buffers = &resources->constant_buffers();
                native_draw.samplers = &resources->samplers();
                for (std::size_t index_value = 0U;
                     index_value < constants.size(); ++index_value) {
                    native_draw.constant_buffer_resources[index_value] =
                        constants[index_value]->resource();
                }
                native_draw.diffuse_texture = diffuse->resource();
                native_draw.diffuse_format =
                    dxgi_texture_format(diffuse->info().description.format);
                native_draw.diffuse_state = diffuse->state_pointer();
                for (std::size_t index_value = 0U;
                     index_value < shadows.size(); ++index_value) {
                    native_draw.shadow_maps[index_value] =
                        shadows[index_value]->resource();
                    native_draw.shadow_states[index_value] =
                        shadows[index_value]->state_pointer();
                }
                native_draws.push_back(native_draw);
            }
            std::vector<std::byte> output;
            if (!d3d_texture->draw_stock_ks_per_pixel_native_batch(
                    batch, native_draws, d3d_depth, resolve_target, output,
                    diagnostic)) {
                return {indexed_static_mesh_batch_status(
                            classify_d3d12_stock_ks_per_pixel_failure(
                                diagnostic.code)),
                        std::move(diagnostic), {}};
            }
            return {IndexedStaticMeshBatchStatus::ready, {}, std::move(output)};
        }
        std::vector<D3D12IndexedBatchDraw> draws;
        draws.reserve(batch.draws.size() + batch.selected_mesh_draws.size() +
                      batch.overlay_draws.size() +
                      (batch.viewport_frame_border.has_value() ? 1U : 0U));
        IndexedStaticMeshBatchStatus assembly_status =
            IndexedStaticMeshBatchStatus::ready;
        const auto append_indexed_draw =
            [&](const Buffer* vertex_buffer, const Buffer* index_buffer,
                const DrawPacket& packet, const PipelineProgram& pipeline,
                const DrawMatrices& matrices,
                const IndexedStaticMeshDrawRequest* scene_request,
                const SelectedMeshDrawRequest* selected_request) {
            const auto* vertex = dynamic_cast<const D3D12Buffer*>(vertex_buffer);
            const auto* index = dynamic_cast<const D3D12Buffer*>(index_buffer);
            if (vertex == nullptr || index == nullptr || vertex->context() != context || index->context() != context) {
                assembly_status = IndexedStaticMeshBatchStatus::unsupported;
                diagnostic = {"indexed_static_mesh_batch_context_mismatch",
                              "Batch geometry buffers belong to another D3D12 device"};
                return false;
            }
            const UINT required_vertex_state =
                static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            const UINT required_index_state = static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER);
            if ((static_cast<UINT>(vertex->state()) & required_vertex_state) == 0U ||
                (static_cast<UINT>(index->state()) & required_index_state) == 0U) {
                assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                diagnostic = {"indexed_static_mesh_batch_resource_state_invalid",
                              "Batch geometry buffers are not in D3D12 vertex/index state"};
                return false;
            }
            const std::uint64_t stride = pipeline.vertex_layout.stride;
            const std::uint64_t vertex_offset = static_cast<std::uint64_t>(packet.vertex_offset);
            const std::uint64_t index_offset = static_cast<std::uint64_t>(packet.index_offset);
            if (stride == 0U || vertex_offset > std::numeric_limits<std::uint64_t>::max() / stride ||
                index_offset > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
                assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                diagnostic = {"indexed_static_mesh_batch_offset_overflow",
                              "Batch geometry offsets overflow D3D12 byte arithmetic"};
                return false;
            }
            const std::uint64_t vertex_byte_offset = vertex_offset * stride;
            const std::uint64_t index_byte_offset = index_offset * sizeof(std::uint16_t);
            const std::uint64_t vertex_bytes = static_cast<std::uint64_t>(packet.vertex_count) * stride;
            const std::uint64_t index_bytes =
                static_cast<std::uint64_t>(packet.index_count) * sizeof(std::uint16_t);
            constexpr std::uint64_t max_uint = static_cast<std::uint64_t>(std::numeric_limits<UINT>::max());
            const D3D12_RESOURCE_DESC vertex_description = vertex->resource()->GetDesc();
            const D3D12_RESOURCE_DESC index_description = index->resource()->GetDesc();
            const D3D12_GPU_VIRTUAL_ADDRESS vertex_address =
                vertex->resource()->GetGPUVirtualAddress();
            const D3D12_GPU_VIRTUAL_ADDRESS index_address =
                index->resource()->GetGPUVirtualAddress();
            if (vertex_bytes > max_uint || index_bytes > max_uint ||
                vertex_byte_offset > vertex_description.Width ||
                vertex_bytes > vertex_description.Width - vertex_byte_offset ||
                index_byte_offset > index_description.Width ||
                index_bytes > index_description.Width - index_byte_offset ||
                vertex_address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - vertex_byte_offset ||
                index_address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - index_byte_offset) {
                assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                diagnostic = {"indexed_static_mesh_batch_buffer_range_invalid",
                              "Batch geometry range exceeds D3D12 buffer limits"};
                return false;
            }
            D3D12IndexedBatchDraw draw;
            draw.pipeline = &pipeline;
            draw.scene_request = scene_request;
            draw.transparent = packet.flags.transparent ||
                               packet.flags.blend_enabled ||
                               pipeline.blend.enabled;
            draw.matrices = matrices;
            draw.geometry.vertex_resource = vertex->resource();
            draw.geometry.vertex_offset = vertex_byte_offset;
            draw.geometry.vertex_size = static_cast<UINT>(vertex_bytes);
            draw.geometry.vertex_stride = static_cast<UINT>(stride);
            draw.geometry.index_resource = index->resource();
            draw.geometry.index_offset = index_byte_offset;
            draw.geometry.index_size = static_cast<UINT>(index_bytes);
            draw.geometry.index_count = packet.index_count;
            draw.geometry.indexed = true;
            if (selected_request != nullptr) {
                const auto* color = dynamic_cast<const D3D12Buffer*>(
                    selected_request->color_buffer);
                if (color == nullptr || color->context() != context) {
                    assembly_status = IndexedStaticMeshBatchStatus::unsupported;
                    diagnostic = {"selected_mesh_context_mismatch",
                                  "The selected-mesh color buffer belongs to another D3D12 device"};
                    return false;
                }
                if ((static_cast<UINT>(color->state()) & required_vertex_state) == 0U) {
                    assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                    diagnostic = {"selected_mesh_resource_state_invalid",
                                  "The selected-mesh color buffer is not in D3D12 constant-buffer state"};
                    return false;
                }
                const D3D12_RESOURCE_DESC color_description =
                    color->resource()->GetDesc();
                const D3D12_GPU_VIRTUAL_ADDRESS color_address =
                    color->resource()->GetGPUVirtualAddress();
                if (selected_request->color_offset_bytes > color_description.Width ||
                    selected_request->color_range_bytes >
                        color_description.Width - selected_request->color_offset_bytes ||
                    color_address >
                        std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() -
                            selected_request->color_offset_bytes) {
                    assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                    diagnostic = {"selected_mesh_buffer_range_invalid",
                                  "The selected-mesh color view exceeds D3D12 buffer limits"};
                    return false;
                }
                draw.selected_color = color;
                draw.selected_color_offset =
                    selected_request->color_offset_bytes;
            }
            if (scene_request != nullptr &&
                scene_request->shader_authority ==
                    IndexedShaderAuthority::explicit_stock_ks_per_pixel_native) {
                const StockKsPerPixelNativeDrawBinding* binding =
                    scene_request->stock_ks_per_pixel_native;
                const StockKsPerPixelNativeDrawResources* resources =
                    binding != nullptr ? binding->resources : nullptr;
                if (resources == nullptr || resources->device() != this ||
                    !resources->ready() || binding->diffuse_texture == nullptr) {
                    assembly_status = IndexedStaticMeshBatchStatus::unsupported;
                    diagnostic = {"indexed_stock_native_batch_binding_missing",
                                  "Hybrid native draw binding is incomplete"};
                    return false;
                }
                auto* diffuse = const_cast<D3D12Texture*>(dynamic_cast<const D3D12Texture*>(
                    binding->diffuse_texture));
                const auto* vertex_shader = dynamic_cast<const D3D12ShaderModule*>(
                    &resources->shader_program().vertex_shader());
                const auto* pixel_shader = dynamic_cast<const D3D12ShaderModule*>(
                    &resources->shader_program().pixel_shader());
                const auto* linear_sampler = dynamic_cast<const D3D12Sampler*>(
                    resources->samplers().sampler(StockKsPerPixelNativeSamplerSlot::linear));
                const auto* shadow_sampler = dynamic_cast<const D3D12Sampler*>(
                    resources->samplers().sampler(StockKsPerPixelNativeSamplerSlot::shadow));
                std::array<const D3D12Buffer*, 5U> constants{};
                for (std::size_t constant_index = 0U;
                     constant_index < constants.size(); ++constant_index) {
                    constants[constant_index] = dynamic_cast<const D3D12Buffer*>(
                        resources->constant_buffers().buffer(
                            static_cast<StockKsPerPixelNativeConstantSlot>(constant_index)));
                }
                std::array<D3D12DepthAttachment*, 3U> shadows{};
                for (std::size_t shadow_index = 0U;
                     shadow_index < shadows.size(); ++shadow_index) {
                    shadows[shadow_index] = const_cast<D3D12DepthAttachment*>(
                        dynamic_cast<const D3D12DepthAttachment*>(
                            binding->shadow_maps[shadow_index]));
                }
                const bool missing_type =
                    diffuse == nullptr || vertex_shader == nullptr || pixel_shader == nullptr ||
                    linear_sampler == nullptr || shadow_sampler == nullptr ||
                    std::any_of(constants.begin(), constants.end(),
                                [](const D3D12Buffer* value) { return value == nullptr; }) ||
                    std::any_of(shadows.begin(), shadows.end(),
                                [](const D3D12DepthAttachment* value) { return value == nullptr; });
                const bool foreign_context =
                    !missing_type &&
                    (diffuse->context() != context || vertex_shader->context() != context ||
                     pixel_shader->context() != context || linear_sampler->context() != context ||
                     shadow_sampler->context() != context ||
                     std::any_of(constants.begin(), constants.end(),
                                 [&](const D3D12Buffer* value) { return value->context() != context; }) ||
                     std::any_of(shadows.begin(), shadows.end(),
                                 [&](const D3D12DepthAttachment* value) { return value->context() != context; }));
                if (missing_type || foreign_context) {
                    assembly_status = IndexedStaticMeshBatchStatus::unsupported;
                    diagnostic = {foreign_context ? "indexed_stock_native_batch_context_mismatch"
                                                   : "indexed_stock_native_batch_type_unsupported",
                                  foreign_context
                                      ? "Hybrid native resources belong to a different D3D12 device"
                                      : "Hybrid native binding contains an unknown D3D12 resource handle"};
                    return false;
                }
                if (!diffuse->initialized() ||
                    std::any_of(shadows.begin(), shadows.end(),
                                [](const D3D12DepthAttachment* value) { return !value->cleared(); })) {
                    assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                    diagnostic = {"indexed_stock_native_batch_resource_uninitialized",
                                  "Hybrid native textures and shadow maps must be initialized"};
                    return false;
                }
                D3D12StockKsPerPixelNativeBatchDraw native_draw;
                native_draw.packet = scene_request->packet;
                native_draw.pipeline = scene_request->pipeline;
                native_draw.index_type = scene_request->index_type;
                native_draw.vertex_buffer = vertex->resource();
                native_draw.vertex_state = vertex->state();
                native_draw.index_buffer = index->resource();
                native_draw.index_state = index->state();
                native_draw.shader_program = &resources->shader_program();
                native_draw.constant_buffers = &resources->constant_buffers();
                native_draw.samplers = &resources->samplers();
                for (std::size_t constant_index = 0U;
                     constant_index < constants.size(); ++constant_index)
                    native_draw.constant_buffer_resources[constant_index] =
                        constants[constant_index]->resource();
                native_draw.diffuse_texture = diffuse->resource();
                native_draw.diffuse_format =
                    dxgi_texture_format(diffuse->info().description.format);
                native_draw.diffuse_state = diffuse->state_pointer();
                for (std::size_t shadow_index = 0U;
                     shadow_index < shadows.size(); ++shadow_index) {
                    native_draw.shadow_maps[shadow_index] = shadows[shadow_index]->resource();
                    native_draw.shadow_states[shadow_index] = shadows[shadow_index]->state_pointer();
                }
                draw.native_stock_ks_per_pixel = true;
                draw.native_stock_draw = native_draw;
            }
            draws.push_back(draw);
            return true;
        };
        const auto append_scene_draw = [&](const IndexedStaticMeshDrawRequest& request) {
            return append_indexed_draw(
                request.vertex_buffer, request.index_buffer, *request.packet,
                *request.pipeline,
                {request.packet->world_matrix,
                 request.camera_frame->view_projection},
                &request, nullptr);
        };
        const auto append_selected_draw = [&](const SelectedMeshDrawRequest& request) {
            return append_indexed_draw(
                request.vertex_buffer, request.index_buffer, *request.packet,
                *request.pipeline, request.matrices, nullptr, &request);
        };
        const auto append_overlay_draw = [&](const OverlayLineDrawRequest& request) {
            const auto* vertex =
                dynamic_cast<const D3D12Buffer*>(request.vertex_buffer);
            if (vertex == nullptr || vertex->context() != context) {
                assembly_status = IndexedStaticMeshBatchStatus::unsupported;
                diagnostic = {"overlay_line_context_mismatch",
                              "Overlay line buffers must belong to this D3D12 device"};
                return false;
            }
            const UINT required_vertex_state = static_cast<UINT>(
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            if ((static_cast<UINT>(vertex->state()) & required_vertex_state) == 0U) {
                assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                diagnostic = {"overlay_line_resource_state_invalid",
                              "Overlay line buffer is not in D3D12 vertex state"};
                return false;
            }
            const std::uint64_t vertex_bytes =
                static_cast<std::uint64_t>(request.vertex_count) *
                sizeof(OverlayLineVertex);
            const auto resource_description = vertex->resource()->GetDesc();
            constexpr std::uint64_t max_uint =
                static_cast<std::uint64_t>(std::numeric_limits<UINT>::max());
            if (vertex_bytes > max_uint ||
                request.vertex_offset_bytes > resource_description.Width ||
                vertex_bytes >
                    resource_description.Width - request.vertex_offset_bytes) {
                assembly_status = IndexedStaticMeshBatchStatus::invalid_request;
                diagnostic = {"overlay_line_vertex_range_invalid",
                              "Overlay line vertex range exceeds D3D12 buffer limits"};
                return false;
            }
            D3D12IndexedBatchDraw draw;
            draw.pipeline = request.pipeline;
            draw.matrices = request.matrices;
            draw.geometry.vertex_resource = vertex->resource();
            draw.geometry.vertex_offset = request.vertex_offset_bytes;
            draw.geometry.vertex_size = static_cast<UINT>(vertex_bytes);
            draw.geometry.vertex_stride = sizeof(OverlayLineVertex);
            draw.geometry.vertex_count = request.vertex_count;
            draw.geometry.indexed = false;
            draws.push_back(draw);
            return true;
        };
        if (!visit_indexed_static_mesh_batch_draws(
                batch, append_scene_draw, append_selected_draw,
                append_overlay_draw))
            return {assembly_status, std::move(diagnostic), {}};
        if (batch.viewport_frame_border.has_value()) {
            const ViewportFrameBorderDrawRequest& request =
                *batch.viewport_frame_border;
            DrawPacket packet;
            packet.vertex_count = static_cast<std::uint32_t>(
                viewport_frame_border_vertex_count);
            packet.index_count = static_cast<std::uint32_t>(
                viewport_frame_border_index_count);
            // Preserve border-last order when grass has no transparent scene
            // packet to establish its insertion point. The pipeline is opaque.
            packet.flags.transparent = true;
            if (!append_indexed_draw(request.vertex_buffer, request.index_buffer,
                                     packet, *request.pipeline,
                                     request.matrices, nullptr, nullptr))
                return {assembly_status, std::move(diagnostic), {}};
        }
        std::vector<std::byte> output;
        if (!d3d_texture->draw_indexed_static_mesh_batch(
                batch, draws, d3d_depth, resolve_target, output, diagnostic))
            return {IndexedStaticMeshBatchStatus::execution_failed, std::move(diagnostic), {}};
        return {IndexedStaticMeshBatchStatus::ready, {}, std::move(output)};
    }

    DepthOnlyIndexedStaticMeshDrawResult draw_depth_only_indexed_static_mesh(
        DepthAttachment& depth,
        const DepthOnlyIndexedStaticMeshDrawRequest& request) override {
        Diagnostic diagnostic;
        const auto validation = validate_depth_only_indexed_static_mesh_draw_request(
            depth, request, diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshDrawStatus::ready)
            return {validation, std::move(diagnostic)};
        DepthOnlyIndexedStaticMeshDrawRequest draw = request;
        draw.clear_depth = false;
        draw.depth_clear_value = 1.0F;
        const std::array<DepthOnlyIndexedStaticMeshDrawRequest, 1U> draws = {draw};
        const DepthOnlyIndexedStaticMeshBatchDescription batch{
            draws, &depth, request.clear_depth, request.depth_clear_value};
        const auto result = draw_depth_only_indexed_static_mesh_batch(batch);
        if (result.status == DepthOnlyIndexedStaticMeshBatchStatus::ready)
            return {DepthOnlyIndexedStaticMeshDrawStatus::ready, {}};
        const auto status = result.status == DepthOnlyIndexedStaticMeshBatchStatus::unsupported
                                ? DepthOnlyIndexedStaticMeshDrawStatus::unsupported
                            : result.status == DepthOnlyIndexedStaticMeshBatchStatus::invalid_request
                                ? DepthOnlyIndexedStaticMeshDrawStatus::invalid_request
                                : DepthOnlyIndexedStaticMeshDrawStatus::execution_failed;
        return {status, result.diagnostic};
    }

    DepthOnlyIndexedStaticMeshBatchResult draw_depth_only_indexed_static_mesh_batch(
        const DepthOnlyIndexedStaticMeshBatchDescription& batch) override {
        Diagnostic diagnostic;
        const auto validation =
            validate_depth_only_indexed_static_mesh_batch_description(batch, diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic)};
        auto* depth = dynamic_cast<D3D12DepthAttachment*>(batch.depth_attachment);
        if (depth == nullptr || depth->context() != context_.get()) {
            return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                    {"depth_only_indexed_depth_context_mismatch",
                     "The depth attachment does not belong to this D3D12 device"}};
        }
        const D3D12Context* context = context_.get();
        std::vector<D3D12IndexedBatchDraw> draws;
        draws.reserve(batch.draws.size());
        for (const DepthOnlyIndexedStaticMeshDrawRequest& request : batch.draws) {
            const auto* vertex = dynamic_cast<const D3D12Buffer*>(request.vertex_buffer);
            const auto* index = dynamic_cast<const D3D12Buffer*>(request.index_buffer);
            if (vertex == nullptr || index == nullptr || vertex->context() != context ||
                index->context() != context) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                        {"depth_only_indexed_static_mesh_context_mismatch",
                         "Depth-only geometry buffers belong to another D3D12 device"}};
            }
            const UINT required_vertex_state =
                static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            const UINT required_index_state = static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER);
            if ((static_cast<UINT>(vertex->state()) & required_vertex_state) == 0U ||
                (static_cast<UINT>(index->state()) & required_index_state) == 0U) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                        {"depth_only_indexed_static_mesh_resource_state_invalid",
                         "Depth-only geometry buffers are not in D3D12 vertex/index state"}};
            }
            const std::uint64_t stride = request.pipeline->vertex_layout.stride;
            const std::uint64_t vertex_offset = request.packet->vertex_offset;
            const std::uint64_t index_offset = request.packet->index_offset;
            if (stride == 0U || vertex_offset > std::numeric_limits<std::uint64_t>::max() / stride ||
                index_offset > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                        {"depth_only_indexed_static_mesh_offset_overflow",
                         "Depth-only geometry offsets overflow D3D12 byte arithmetic"}};
            }
            const std::uint64_t vertex_byte_offset = vertex_offset * stride;
            const std::uint64_t index_byte_offset = index_offset * sizeof(std::uint16_t);
            const std::uint64_t vertex_bytes =
                static_cast<std::uint64_t>(request.packet->vertex_count) * stride;
            const std::uint64_t index_bytes =
                static_cast<std::uint64_t>(request.packet->index_count) * sizeof(std::uint16_t);
            constexpr std::uint64_t max_uint =
                static_cast<std::uint64_t>(std::numeric_limits<UINT>::max());
            const D3D12_RESOURCE_DESC vertex_description = vertex->resource()->GetDesc();
            const D3D12_RESOURCE_DESC index_description = index->resource()->GetDesc();
            const D3D12_GPU_VIRTUAL_ADDRESS vertex_address = vertex->resource()->GetGPUVirtualAddress();
            const D3D12_GPU_VIRTUAL_ADDRESS index_address = index->resource()->GetGPUVirtualAddress();
            if (vertex_bytes > max_uint || index_bytes > max_uint ||
                vertex_byte_offset > vertex_description.Width ||
                vertex_bytes > vertex_description.Width - vertex_byte_offset ||
                index_byte_offset > index_description.Width ||
                index_bytes > index_description.Width - index_byte_offset ||
                vertex_address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - vertex_byte_offset ||
                index_address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - index_byte_offset) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                        {"depth_only_indexed_static_mesh_buffer_range_invalid",
                         "Depth-only geometry range exceeds D3D12 buffer limits"}};
            }
            D3D12IndexedBatchDraw draw;
            draw.pipeline = request.pipeline;
            draw.matrices = {request.packet->world_matrix,
                             request.camera_frame->view_projection};
            draw.geometry.vertex_resource = vertex->resource();
            draw.geometry.vertex_offset = vertex_byte_offset;
            draw.geometry.vertex_size = static_cast<UINT>(vertex_bytes);
            draw.geometry.vertex_stride = static_cast<UINT>(stride);
            draw.geometry.index_resource = index->resource();
            draw.geometry.index_offset = index_byte_offset;
            draw.geometry.index_size = static_cast<UINT>(index_bytes);
            draw.geometry.index_count = request.packet->index_count;
            draw.geometry.indexed = true;
            draw.alpha_tested = request.material_mode ==
                                DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::stock_alpha_tested;
            if (draw.alpha_tested) {
                const auto* alpha_texture = dynamic_cast<const D3D12Texture*>(
                    request.alpha_tested_diffuse_binding.texture);
                const auto* alpha_sampler = dynamic_cast<const D3D12Sampler*>(
                    request.alpha_tested_diffuse_binding.sampler);
                const auto* alpha_material = dynamic_cast<const D3D12Buffer*>(
                    request.alpha_tested_material_binding.buffer);
                if (alpha_texture == nullptr || alpha_sampler == nullptr ||
                    alpha_material == nullptr || alpha_texture->context() != context ||
                    alpha_sampler->context() != context ||
                    alpha_material->context() != context) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                            {"depth_only_indexed_alpha_context_mismatch",
                             "D3D12 alpha-tested resources belong to another device"}};
                }
                if (!alpha_texture->initialized()) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                            {"depth_only_indexed_alpha_texture_uninitialized",
                             "The D3D12 alpha-tested diffuse texture has no uploaded data"}};
                }
                draw.alpha_texture = const_cast<D3D12Texture*>(alpha_texture);
                draw.alpha_sampler = const_cast<D3D12Sampler*>(alpha_sampler);
                draw.alpha_material = const_cast<D3D12Buffer*>(alpha_material);
                draw.alpha_material_offset =
                    request.alpha_tested_material_binding.offset_bytes;
            }
            draws.push_back(draw);
        }
        if (!execute_d3d12_depth_only_indexed_static_mesh_batch(
                context_, *depth, batch, draws, diagnostic)) {
            return {DepthOnlyIndexedStaticMeshBatchStatus::execution_failed,
                    std::move(diagnostic)};
        }
        return {DepthOnlyIndexedStaticMeshBatchStatus::ready, {}};
    }

    SamplerResult create_sampler(const SamplerDescription& description) override {
        Diagnostic diagnostic;
        if (!valid_sampler_description(description, diagnostic))
            return {SamplerStatus::invalid_description, std::move(diagnostic), nullptr};
        if (description.mip_lod_bias < -16.0F ||
            description.mip_lod_bias > 15.99F)
            return {SamplerStatus::unsupported,
                    {"d3d12_sampler_lod_bias_limit",
                     "The requested sampler LOD bias exceeds the D3D12 range"},
                    nullptr};
        if (!context_->sampler_heap)
            return {SamplerStatus::unsupported,
                    {"d3d12_sampler_heap_unavailable", "The D3D12 sampler descriptor heap is unavailable"}, nullptr};
        std::lock_guard lock(context_->sampler_mutex);
        if (context_->next_sampler >= kD3d12SamplerDescriptorCapacity)
            return {SamplerStatus::allocation_failed,
                    {"d3d12_sampler_limit", "The D3D12 sampler descriptor heap is full"}, nullptr};
        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = d3d12_sampler_filter(description);
        sampler.AddressU = d3d12_sampler_address(description.address_u);
        sampler.AddressV = d3d12_sampler_address(description.address_v);
        sampler.AddressW = d3d12_sampler_address(description.address_w);
        sampler.MipLODBias = description.mip_lod_bias;
        sampler.MaxAnisotropy = static_cast<UINT>(description.max_anisotropy);
        sampler.ComparisonFunc = d3d12_sampler_compare(description.compare);
        sampler.MinLOD = description.min_lod;
        sampler.MaxLOD = description.max_lod;
        sampler.BorderColor[0] = 0.0F;
        sampler.BorderColor[1] = 0.0F;
        sampler.BorderColor[2] = 0.0F;
        sampler.BorderColor[3] = 0.0F;
        auto descriptor = context_->sampler_heap->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += static_cast<SIZE_T>(context_->next_sampler) * context_->sampler_descriptor_size;
        auto gpu_descriptor = context_->sampler_heap->GetGPUDescriptorHandleForHeapStart();
        gpu_descriptor.ptr += static_cast<UINT64>(context_->next_sampler) * context_->sampler_descriptor_size;
        ++context_->next_sampler;
        context_->device->CreateSampler(&sampler, descriptor);
        return {SamplerStatus::ready, {},
                std::make_unique<D3D12Sampler>(context_, descriptor, gpu_descriptor, description)};
    }

    ShaderModuleResult create_shader_module(const ShaderModuleDescription& description) override {
        Diagnostic diagnostic;
        const auto validation = validate_shader_module_description(description, diagnostic);
        if (validation != ShaderModuleStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        ShaderBytecodeFormat format{};
        if (!shader_bytecode_format(description.bytecode, format) ||
            (format != ShaderBytecodeFormat::dxbc &&
             format != ShaderBytecodeFormat::dxil))
            return {ShaderModuleStatus::unsupported,
                    {"d3d12_shader_format_unsupported", "D3D12 shader modules require DXBC or DXIL bytecode"}, nullptr};
        try {
            auto bytecode = std::make_shared<std::vector<std::byte>>(description.bytecode.begin(), description.bytecode.end());
            return {ShaderModuleStatus::ready, {},
                    std::make_unique<D3D12ShaderModule>(context_, std::move(bytecode),
                                                         ShaderModuleInfo{description.stage, format, description.bytecode.size()})};
        } catch (const std::bad_alloc&) {
            return {ShaderModuleStatus::allocation_failed,
                    {"shader_module_allocation_failed", "D3D12 shader bytecode allocation failed"}, nullptr};
        }
    }

    void wait_idle() noexcept override {
        if (context_) context_->wait_idle();
    }

private:
    std::shared_ptr<D3D12Context> context_;
};

} // namespace

AdapterResult enumerate_d3d12_adapters(const DeviceOptions& options) {
    Diagnostic validation_diagnostic;
    if (!prepare_validation(options, validation_diagnostic)) {
        AdapterResult result;
        result.status = DeviceStatus::unavailable;
        result.diagnostic = std::move(validation_diagnostic);
        return result;
    }
    ComPtr<IDXGIFactory6> factory;
    const auto values = adapters(options, factory);
    AdapterResult result;
    if (!factory) {
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"d3d12_factory_unavailable", "DXGI factory creation failed"};
        return result;
    }
    result.status = DeviceStatus::ready;
    for (const auto& value : values) {
        result.adapters.push_back(info_from(value.description));
    }
    if (result.adapters.empty()) {
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"d3d12_no_usable_device", "DXGI found no usable D3D12 adapter"};
    }
    return result;
}

DeviceResult create_d3d12_device(const DeviceOptions& options) {
    Diagnostic validation_diagnostic;
    if (!prepare_validation(options, validation_diagnostic)) {
        DeviceResult result;
        result.status = DeviceStatus::unavailable;
        result.diagnostic = std::move(validation_diagnostic);
        return result;
    }
    ComPtr<IDXGIFactory6> factory;
    const auto values = adapters(options, factory);
    if (!factory || values.empty()) {
        DeviceResult result;
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"d3d12_no_usable_device", "DXGI found no usable D3D12 adapter"};
        return result;
    }
    if (options.adapter_index >= values.size()) {
        DeviceResult result;
        result.status = DeviceStatus::invalid_options;
        result.diagnostic = {"adapter_index_out_of_range", "D3D12 adapter index is outside the enumerated device list"};
        return result;
    }

    const Adapter& selected = values[options.adapter_index];
    ComPtr<ID3D12Device> device;
    HRESULT result_code = D3D12CreateDevice(selected.handle.Get(), D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device));
    if (FAILED(result_code)) {
        DeviceResult result;
        result.status = DeviceStatus::initialization_failed;
        result.diagnostic = hresult_error("D3D12CreateDevice", result_code);
        return result;
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_description.NodeMask = 0;
    ComPtr<ID3D12CommandQueue> queue;
    result_code = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
    if (FAILED(result_code)) {
        DeviceResult result;
        result.status = DeviceStatus::initialization_failed;
        result.diagnostic = hresult_error("ID3D12Device::CreateCommandQueue", result_code);
        return result;
    }

    DeviceResult result;
    result.status = DeviceStatus::ready;
    auto context = std::make_shared<D3D12Context>();
    context->factory = std::move(factory);
    context->adapter = selected.handle;
    context->device = std::move(device);
    context->queue = std::move(queue);
    if (!options.headless && options.native_surface.has_value())
        context->native_window = options.native_surface->win32Window;
    context->info = info_from(selected.description);
    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_description{};
    sampler_heap_description.NumDescriptors = kD3d12SamplerDescriptorCapacity;
    sampler_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result_code = context->device->CreateDescriptorHeap(&sampler_heap_description, IID_PPV_ARGS(&context->sampler_heap));
    if (FAILED(result_code)) {
        result.status = DeviceStatus::initialization_failed;
        result.diagnostic = hresult_error("ID3D12Device::CreateDescriptorHeap(sampler)", result_code);
        return result;
    }
    context->sampler_descriptor_size = context->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    result.device = std::make_unique<D3D12Device>(std::move(context));
    return result;
}

#else

AdapterResult enumerate_d3d12_adapters(const DeviceOptions&) {
    AdapterResult result;
    result.status = DeviceStatus::unavailable;
#    if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12
    result.diagnostic = {"d3d12_platform_unavailable", "D3D12 support is only available on Windows"};
#    else
    result.diagnostic = {"d3d12_not_compiled", "D3D12 support is not compiled into this build"};
#    endif
    return result;
}

DeviceResult create_d3d12_device(const DeviceOptions&) {
    DeviceResult result;
    result.status = DeviceStatus::unavailable;
#    if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12
    result.diagnostic = {"d3d12_platform_unavailable", "D3D12 support is only available on Windows"};
#    else
    result.diagnostic = {"d3d12_not_compiled", "D3D12 support is not compiled into this build"};
#    endif
    return result;
}

#endif

} // namespace apex::render
