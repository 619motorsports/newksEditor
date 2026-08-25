#include "backend_internal.hpp"
#include "apex/render/draw_packet.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <limits>
#include <mutex>
#include <span>
#include <utility>

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#    include <d3d12.h>
#    include <dxgi1_6.h>
#    include <wrl/client.h>
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
    bool constant_enabled = false;
    bool frame_enabled = false;
    bool normal_enabled = false;
    bool maps_enabled = false;
    bool detail_enabled = false;
    bool normal_detail_enabled = false;
    bool damage_enabled = false;
    bool damage_mask_enabled = false;
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
    [[nodiscard]] const D3D12Context* context() const noexcept { return context_.get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv(bool writable) const noexcept {
        return writable ? write_dsv_ : read_dsv_;
    }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE srv() const noexcept { return srv_; }
    [[nodiscard]] bool cleared() const noexcept { return cleared_; }
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
    case TextureFormat::r5g6b5_unorm:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case TextureFormat::bc1_unorm:
        return DXGI_FORMAT_BC1_UNORM;
    case TextureFormat::bc1_srgb:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case TextureFormat::bc3_unorm:
        return DXGI_FORMAT_BC3_UNORM;
    case TextureFormat::bc3_srgb:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case TextureFormat::bc5_unorm:
        return DXGI_FORMAT_BC5_UNORM;
    case TextureFormat::bc7_unorm:
        return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::bc7_srgb:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] bool is_d3d12_supported_block_format(TextureFormat format) noexcept {
    return format == TextureFormat::bc1_unorm || format == TextureFormat::bc1_srgb ||
           format == TextureFormat::bc3_unorm || format == TextureFormat::bc3_srgb ||
           format == TextureFormat::bc5_unorm ||
           format == TextureFormat::bc7_unorm || format == TextureFormat::bc7_srgb;
}

[[nodiscard]] bool validate_d3d12_texture_format_support(
    const std::shared_ptr<D3D12Context>& context, DXGI_FORMAT format,
    std::uint32_t mip_levels, Diagnostic& diagnostic) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    const HRESULT result = context->device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(result)) {
        diagnostic = hresult_error("ID3D12Device::CheckFeatureSupport(format)", result);
        diagnostic.code = "d3d12_texture_format_query_failed";
        return false;
    }
    constexpr UINT required = static_cast<UINT>(D3D12_FORMAT_SUPPORT1_TEXTURE2D) |
                              static_cast<UINT>(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
    const UINT support1 = static_cast<UINT>(support.Support1);
    if ((support1 & required) != required ||
        (mip_levels > 1U && (support1 & static_cast<UINT>(D3D12_FORMAT_SUPPORT1_MIP)) == 0U)) {
        diagnostic = {"d3d12_texture_format_unsupported",
                      "The D3D12 adapter does not support the requested BC1, BC3, BC5, or BC7 sampled texture format"};
        return false;
    }
    return true;
}

[[nodiscard]] D3D12_RESOURCE_STATES texture_state(TextureUsage usage) noexcept {
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

[[nodiscard]] bool valid_d3d12_texture_usage(TextureUsage usage, Diagnostic& diagnostic) noexcept {
    const auto raw = static_cast<std::uint32_t>(usage);
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
                      "D3D12 texture uploads support only BC1, BC3, BC5, and BC7 block-compressed formats"};
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
    const UINT subresource_count = description.mip_levels * description.array_layers;
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
            const UINT subresource = upload.array_layer * description.mip_levels + upload.mip_level;
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
            const UINT subresource = upload.array_layer * description.mip_levels + upload.mip_level;
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
    if (texture_state(description.usage) != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = texture_state(description.usage);
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
    const D3D12_RESOURCE_STATES final_state = texture_state(description.usage);
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

struct D3D12GeometryDescriptor {
    ID3D12Resource* vertex_resource = nullptr;
    UINT64 vertex_offset = 0U;
    UINT vertex_size = 0U;
    UINT vertex_stride = 0U;
    ID3D12Resource* index_resource = nullptr;
    UINT64 index_offset = 0U;
    UINT index_size = 0U;
    UINT index_count = 0U;
    bool indexed = false;
};

struct D3D12IndexedBatchDraw {
    D3D12GeometryDescriptor geometry{};
    const PipelineProgram* pipeline = nullptr;
    DrawMatrices matrices{};
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
        if (shader.format != PipelineShaderFormat::dxil) {
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
        srv_heap_description.NumDescriptors = 1U + static_cast<UINT>(has_material_constants) +
                                              static_cast<UINT>(has_frame_constants) +
                                              static_cast<UINT>(has_normal_texture) +
                                              static_cast<UINT>(has_maps_texture) +
                                              static_cast<UINT>(has_detail_texture) +
                                              static_cast<UINT>(has_normal_detail_texture) +
                                              static_cast<UINT>(has_damage_texture) +
                                              static_cast<UINT>(has_damage_mask_texture);
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
    std::array<D3D12_ROOT_PARAMETER, 17> root_parameters{};
    UINT root_parameter_count = 0U;
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
    const D3D12_RESOURCE_STATES final_state = texture_state(description.usage);
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

bool draw_indexed_static_mesh_batch_and_readback(
    const std::shared_ptr<D3D12Context>& context,
    ID3D12Resource* destination,
    const TextureDescription& description,
    const IndexedStaticMeshBatchDescription& batch,
    std::span<const D3D12IndexedBatchDraw> draws,
    D3D12DepthAttachment* depth_attachment,
    bool color_initialized,
    D3D12_RESOURCE_STATES& destination_state,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    if (draws.empty() || destination == nullptr) {
        diagnostic = {"indexed_static_mesh_batch_empty", "D3D12 indexed static-mesh batch has no executable draws"};
        return false;
    }
    if (batch.load_color && !color_initialized) {
        diagnostic = {"indexed_color_load_before_clear",
                      "A color load was requested before the D3D12 color attachment was cleared"};
        return false;
    }
    const bool use_depth = depth_attachment != nullptr;
    if (!validate_d3d12_sample_count(context, dxgi_texture_format(description.format),
                                     description.samples, diagnostic))
        return false;
    for (const IndexedStaticMeshDrawRequest& request : batch.draws) {
        if (request.pipeline == nullptr || request.pipeline->targets.colors.empty() ||
            request.pipeline->targets.colors.front().samples != description.samples) {
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
    for (const IndexedStaticMeshDrawRequest& request : batch.draws) {
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
        static_cast<std::size_t>(has_damage_mask_texture);
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
            const std::size_t descriptor_index = index * material_descriptor_stride;
            if (!prepare_d3d12_material_binding(context, batch.draws[index], srv_heap.Get(),
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
                                                 material_bindings[index], diagnostic))
                return false;
        }
    }

    std::vector<ComPtr<ID3D12RootSignature>> root_signatures;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    root_signatures.resize(draws.size());
    pipelines.resize(draws.size());
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const PipelineProgram& program = *draws[index].pipeline;
        const PipelineShaderModule* vertex_shader = nullptr;
        const PipelineShaderModule* fragment_shader = nullptr;
        for (const PipelineShaderModule& shader : program.shaders) {
            if (shader.format != PipelineShaderFormat::dxil) {
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
        std::array<D3D12_ROOT_PARAMETER, 17> root_parameters{};
        UINT root_parameter_count = 1U;
        root_parameters[0] = transform_parameter;
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
        pipeline_description.SampleDesc.Quality = 0U;
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
            diagnostic = hresult_error("CreateCommittedResource(MSAA resolve batch)", result);
            diagnostic.code = "indexed_static_mesh_batch_execution_failed";
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
        diagnostic = {"indexed_static_mesh_batch_readback_size_invalid",
                      "D3D12 indexed static-mesh batch output exceeds platform limits"};
        return false;
    }
    const std::size_t output_size = static_cast<std::size_t>(row_bytes * static_cast<UINT64>(description.height));
    if (row_count < description.height || row_size < row_bytes || footprint.Footprint.RowPitch < row_bytes ||
        footprint.Offset > max_size_t || readback_size > max_texture_readback_bytes || readback_size > max_size_t) {
        diagnostic = {"indexed_static_mesh_batch_readback_footprint_invalid",
                      "D3D12 indexed static-mesh batch readback footprint is out of bounds"};
        return false;
    }
    const UINT64 last_row = footprint.Offset + static_cast<UINT64>(description.height - 1U) * footprint.Footprint.RowPitch;
    if (last_row < footprint.Offset || last_row > readback_size || row_bytes > readback_size - last_row) {
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
    result = context->device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                       &readback_description, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) {
        diagnostic = hresult_error("CreateCommittedResource(indexed batch readback)", result);
        diagnostic.code = "indexed_static_mesh_batch_execution_failed";
        return false;
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
    if (has_material_resources) {
        ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap.Get(), context->sampler_heap.Get()};
        list->SetDescriptorHeaps(2U, descriptor_heaps);
    }

    for (std::size_t index = 0; index < draws.size(); ++index) {
        const auto& draw = draws[index];
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
        list->SetGraphicsRoot32BitConstants(0U, static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t)),
                                             &draw.matrices, 0U);
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
        }
        list->SetPipelineState(pipelines[index].Get());
        list->IASetPrimitiveTopology(d3d12_pipeline_topology(draw.pipeline->raster.fill));
        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        vertex_view.BufferLocation = draw.geometry.vertex_resource->GetGPUVirtualAddress() + draw.geometry.vertex_offset;
        vertex_view.SizeInBytes = draw.geometry.vertex_size;
        vertex_view.StrideInBytes = draw.geometry.vertex_stride;
        list->IASetVertexBuffers(0U, 1U, &vertex_view);
        D3D12_INDEX_BUFFER_VIEW index_view{};
        index_view.BufferLocation = draw.geometry.index_resource->GetGPUVirtualAddress() + draw.geometry.index_offset;
        index_view.SizeInBytes = draw.geometry.index_size;
        index_view.Format = DXGI_FORMAT_R16_UINT;
        list->IASetIndexBuffer(&index_view);
        D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(description.width),
                                static_cast<float>(description.height), 0.0F, 1.0F};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(description.width), static_cast<LONG>(description.height)};
        list->RSSetViewports(1U, &viewport);
        list->RSSetScissorRects(1U, &scissor);
        list->DrawIndexedInstanced(draw.geometry.index_count, 1U, 0U, 0, 0U);
    }
    const D3D12_RESOURCE_STATES final_state = texture_state(description.usage);
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
        if (use_depth)
            depth_attachment->set_state(drained ? depth_state : D3D12_RESOURCE_STATE_COMMON);
        diagnostic = hresult_error("D3D12 indexed batch fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed" : "indexed_static_mesh_batch_execution_failed";
        return false;
    }
    destination_state = final_state;
    if (use_depth) {
        depth_attachment->set_state(depth_state);
        if (batch.clear_depth) depth_attachment->set_cleared();
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
    std::span<const D3D12IndexedBatchDraw> draws,
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
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const PipelineProgram& program = *draws[index].pipeline;
        const PipelineShaderModule* vertex_shader = nullptr;
        for (const PipelineShaderModule& shader : program.shaders) {
            if (shader.format != PipelineShaderFormat::dxil) {
                diagnostic = {"depth_only_indexed_shader_format_unsupported",
                              "D3D12 depth-only execution requires DXIL/DXBC bytecode"};
                return false;
            }
            if (shader.stage == PipelineShaderStage::vertex) vertex_shader = &shader;
        }
        if (vertex_shader == nullptr || program.shaders.size() != 1U) {
            diagnostic = {"depth_only_indexed_shader_pair_invalid",
                          "D3D12 depth-only execution requires one vertex shader and no pixel shader"};
            return false;
        }

        D3D12_ROOT_PARAMETER transform_parameter{};
        transform_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transform_parameter.Constants.ShaderRegister = 0U;
        transform_parameter.Constants.RegisterSpace = 0U;
        transform_parameter.Constants.Num32BitValues =
            static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t));
        transform_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        root_description.NumParameters = 1U;
        root_description.pParameters = &transform_parameter;
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
        pipeline_description.PS = {nullptr, 0U};
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
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const D3D12IndexedBatchDraw& draw = draws[index];
        list->SetGraphicsRootSignature(root_signatures[index].Get());
        list->SetGraphicsRoot32BitConstants(
            0U, static_cast<UINT>(sizeof(DrawMatrices) / sizeof(std::uint32_t)),
            &draw.matrices, 0U);
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
        diagnostic = hresult_error("D3D12 depth-only batch fence", result);
        diagnostic.code = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                              ? "d3d12_device_removed"
                              : "depth_only_indexed_execution_failed";
        return false;
    }
    depth_attachment.set_state(depth_state);
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
          state_(state), initialized_(initialized) {}

    ~D3D12Texture() override {
        if (context_) context_->wait_idle();
    }
    Backend backend() const noexcept override { return Backend::D3D12; }
    const TextureInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const D3D12Context* context() const noexcept { return context_.get(); }
    [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
    [[nodiscard]] D3D12_RESOURCE_STATES state() const noexcept { return state_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    bool upload(const TextureUploadPlan& uploads, Diagnostic& diagnostic) {
        const bool uploaded = execute_texture_upload(context_, resource_.Get(), info_.description, uploads,
                                                     state_, texture_state(info_.description.usage), diagnostic);
        initialized_ = initialized_ || uploaded;
        return uploaded;
    }

    bool clear_readback(const TextureClearReadbackRequest& request,
                        std::vector<std::byte>& output,
                        Diagnostic& diagnostic) {
        const bool cleared = clear_texture_readback(context_, resource_.Get(), info_.description, request,
                                                    state_, output, diagnostic);
        initialized_ = initialized_ || cleared;
        return cleared;
    }

    bool draw_triangle(const TriangleDrawRequest& request,
                       std::vector<std::byte>& output,
                       Diagnostic& diagnostic) {
        const bool drawn = draw_graphics_and_readback(context_, resource_.Get(), info_.description, request,
                                                      state_, output, diagnostic);
        initialized_ = initialized_ || drawn;
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
        return drawn;
    }

    bool draw_indexed_static_mesh_batch(const IndexedStaticMeshBatchDescription& batch,
                                        std::span<const D3D12IndexedBatchDraw> draws,
                                        D3D12DepthAttachment* depth_attachment,
                                        std::vector<std::byte>& output,
                                        Diagnostic& diagnostic) {
        if (batch.load_color && !initialized_) {
            diagnostic = {"indexed_color_load_before_clear",
                          "A color load was requested before the color attachment was initialized"};
            return false;
        }
        const bool drawn = draw_indexed_static_mesh_batch_and_readback(
            context_, resource_.Get(), info_.description, batch, draws, depth_attachment,
            initialized_, state_, output, diagnostic);
        initialized_ = initialized_ || drawn;
        return drawn;
    }

private:
    std::shared_ptr<D3D12Context> context_;
    ComPtr<ID3D12Resource> resource_;
    TextureInfo info_;
    D3D12_RESOURCE_STATES state_;
    bool initialized_ = false;
};

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
        (has_damage_mask_texture && damage_mask_srv_index >= heap_description.NumDescriptors)) {
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
        static_cast<UINT64>(damage_mask_srv_index) > std::numeric_limits<UINT64>::max() / descriptor_size) {
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
    if (srv_offset > std::numeric_limits<SIZE_T>::max() ||
        cbv_offset > std::numeric_limits<SIZE_T>::max() ||
        frame_cbv_offset > std::numeric_limits<SIZE_T>::max() ||
        normal_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        maps_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        detail_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        normal_detail_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        damage_srv_offset > std::numeric_limits<SIZE_T>::max() ||
        damage_mask_srv_offset > std::numeric_limits<SIZE_T>::max()) {
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
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 0U;
    srv.Texture2D.MipLevels = texture_description.mip_levels;
    srv.Texture2D.PlaneSlice = 0U;
    srv.Texture2D.ResourceMinLODClamp = 0.0F;
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
        D3D12_SHADER_RESOURCE_VIEW_DESC normal_srv{};
        const TextureDescription& normal_description = normal_texture->info().description;
        normal_srv.Format = normal_format;
        normal_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        normal_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        normal_srv.Texture2D.MostDetailedMip = 0U;
        normal_srv.Texture2D.MipLevels = normal_description.mip_levels;
        normal_srv.Texture2D.PlaneSlice = 0U;
        normal_srv.Texture2D.ResourceMinLODClamp = 0.0F;
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
        D3D12_SHADER_RESOURCE_VIEW_DESC maps_srv{};
        const TextureDescription& maps_description = maps_texture->info().description;
        maps_srv.Format = maps_format;
        maps_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        maps_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        maps_srv.Texture2D.MostDetailedMip = 0U;
        maps_srv.Texture2D.MipLevels = maps_description.mip_levels;
        maps_srv.Texture2D.PlaneSlice = 0U;
        maps_srv.Texture2D.ResourceMinLODClamp = 0.0F;
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
        D3D12_SHADER_RESOURCE_VIEW_DESC detail_srv{};
        const TextureDescription& detail_description = detail_texture->info().description;
        detail_srv.Format = detail_format;
        detail_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        detail_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        detail_srv.Texture2D.MostDetailedMip = 0U;
        detail_srv.Texture2D.MipLevels = detail_description.mip_levels;
        detail_srv.Texture2D.PlaneSlice = 0U;
        detail_srv.Texture2D.ResourceMinLODClamp = 0.0F;
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
        D3D12_SHADER_RESOURCE_VIEW_DESC normal_detail_srv{};
        const TextureDescription& normal_detail_description = normal_detail_texture->info().description;
        normal_detail_srv.Format = normal_detail_format;
        normal_detail_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        normal_detail_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        normal_detail_srv.Texture2D.MostDetailedMip = 0U;
        normal_detail_srv.Texture2D.MipLevels = normal_detail_description.mip_levels;
        normal_detail_srv.Texture2D.PlaneSlice = 0U;
        normal_detail_srv.Texture2D.ResourceMinLODClamp = 0.0F;
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
            D3D12_SHADER_RESOURCE_VIEW_DESC source_srv{};
            source_srv.Format = source_format;
            source_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            source_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            source_srv.Texture2D.MostDetailedMip = 0U;
            source_srv.Texture2D.MipLevels = source_texture.info().description.mip_levels;
            source_srv.Texture2D.PlaneSlice = 0U;
            source_srv.Texture2D.ResourceMinLODClamp = 0.0F;
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

private:
    std::shared_ptr<D3D12Context> context_;
    std::shared_ptr<const std::vector<std::byte>> bytecode_;
    ShaderModuleInfo info_;
};

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
        if (!valid_d3d12_texture_usage(description.usage, diagnostic))
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
                         "D3D12 supports only BC1, BC3, BC5, and BC7 compressed sampled textures"},
                        nullptr};
            }
            if (description.usage != TextureUsage::sampled ||
                description.mutability != TextureMutability::immutable ||
                description.samples != 1U || description.array_layers != 1U) {
                return {TextureStatus::unsupported,
                        {"d3d12_compressed_texture_unsupported",
                         "D3D12 BC1, BC3, BC5, and BC7 textures require immutable sampled one-layer, single-sample resources"},
                        nullptr};
            }
            if (!validate_d3d12_texture_format_support(
                    context_, dxgi_texture_format(description.format), description.mip_levels, diagnostic))
                return {TextureStatus::unsupported, std::move(diagnostic), nullptr};
        }
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
        const D3D12_RESOURCE_STATES final_state = texture_state(description.usage);
        const bool needs_upload = !initial_uploads.subresources.empty();
        const D3D12_RESOURCE_STATES initial_state = needs_upload ? D3D12_RESOURCE_STATE_COPY_DEST : final_state;
        D3D12_RESOURCE_DESC resource_description{};
        resource_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_description.Width = description.width;
        resource_description.Height = description.height;
        resource_description.DepthOrArraySize = static_cast<UINT16>(description.array_layers);
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
                                                      initial_state, needs_upload);
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
        if (request.pipeline != nullptr &&
            pipeline_declares_directional_shadow_receiver(*request.pipeline)) {
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"d3d12_directional_shadow_receiver_staged",
                     "D3D12 sampled D32 allocation is ready, but receiver descriptor execution requires a Windows/WARP verification build"},
                    {}};
        }
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
        if (std::any_of(batch.draws.begin(), batch.draws.end(),
                        [](const IndexedStaticMeshDrawRequest& request) {
                            return request.pipeline != nullptr &&
                                   pipeline_declares_directional_shadow_receiver(
                                       *request.pipeline);
                        })) {
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"d3d12_directional_shadow_receiver_staged",
                     "D3D12 sampled D32 allocation is ready, but receiver descriptor execution requires a Windows/WARP verification build"},
                    {}};
        }
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
        if (d3d_texture->context() != context ||
            (d3d_depth != nullptr && d3d_depth->context() != context)) {
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"indexed_static_mesh_batch_context_mismatch",
                     "Batch resources belong to another D3D12 device"}, {}};
        }
        std::vector<D3D12IndexedBatchDraw> draws;
        draws.reserve(batch.draws.size());
        for (const IndexedStaticMeshDrawRequest& request : batch.draws) {
            const auto* vertex = dynamic_cast<const D3D12Buffer*>(request.vertex_buffer);
            const auto* index = dynamic_cast<const D3D12Buffer*>(request.index_buffer);
            if (vertex == nullptr || index == nullptr || vertex->context() != context || index->context() != context) {
                return {IndexedStaticMeshBatchStatus::unsupported,
                        {"indexed_static_mesh_batch_context_mismatch",
                         "Batch geometry buffers belong to another D3D12 device"}, {}};
            }
            const UINT required_vertex_state =
                static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            const UINT required_index_state = static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER);
            if ((static_cast<UINT>(vertex->state()) & required_vertex_state) == 0U ||
                (static_cast<UINT>(index->state()) & required_index_state) == 0U) {
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"indexed_static_mesh_batch_resource_state_invalid",
                         "Batch geometry buffers are not in D3D12 vertex/index state"}, {}};
            }
            const std::uint64_t stride = request.pipeline->vertex_layout.stride;
            const std::uint64_t vertex_offset = static_cast<std::uint64_t>(request.packet->vertex_offset);
            const std::uint64_t index_offset = static_cast<std::uint64_t>(request.packet->index_offset);
            if (stride == 0U || vertex_offset > std::numeric_limits<std::uint64_t>::max() / stride ||
                index_offset > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"indexed_static_mesh_batch_offset_overflow",
                         "Batch geometry offsets overflow D3D12 byte arithmetic"}, {}};
            }
            const std::uint64_t vertex_byte_offset = vertex_offset * stride;
            const std::uint64_t index_byte_offset = index_offset * sizeof(std::uint16_t);
            const std::uint64_t vertex_bytes = static_cast<std::uint64_t>(request.packet->vertex_count) * stride;
            const std::uint64_t index_bytes =
                static_cast<std::uint64_t>(request.packet->index_count) * sizeof(std::uint16_t);
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
                return {IndexedStaticMeshBatchStatus::invalid_request,
                        {"indexed_static_mesh_batch_buffer_range_invalid",
                         "Batch geometry range exceeds D3D12 buffer limits"}, {}};
            }
            D3D12IndexedBatchDraw draw;
            draw.pipeline = request.pipeline;
            draw.matrices = {request.packet->world_matrix, request.camera_frame->view_projection};
            draw.geometry.vertex_resource = vertex->resource();
            draw.geometry.vertex_offset = vertex_byte_offset;
            draw.geometry.vertex_size = static_cast<UINT>(vertex_bytes);
            draw.geometry.vertex_stride = static_cast<UINT>(stride);
            draw.geometry.index_resource = index->resource();
            draw.geometry.index_offset = index_byte_offset;
            draw.geometry.index_size = static_cast<UINT>(index_bytes);
            draw.geometry.index_count = request.packet->index_count;
            draw.geometry.indexed = true;
            draws.push_back(draw);
        }
        std::vector<std::byte> output;
        if (!d3d_texture->draw_indexed_static_mesh_batch(batch, draws, d3d_depth, output, diagnostic))
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
        sampler.MipLODBias = 0.0F;
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
        if (!shader_bytecode_format(description.bytecode, format) || format != ShaderBytecodeFormat::dxil)
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
