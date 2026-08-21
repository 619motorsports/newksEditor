#include "backend_internal.hpp"

#include <cstring>
#include <cstdio>
#include <memory>
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

#if defined(APEX_HAS_D3D12) && APEX_HAS_D3D12 && defined(_WIN32)

namespace {

using Microsoft::WRL::ComPtr;

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
    DeviceInfo info;

    ~D3D12Context() { wait_idle(); }

    void wait_idle() noexcept {
        if (!queue || !device) return;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return;
        constexpr UINT64 value = 1;
        if (SUCCEEDED(queue->Signal(fence.Get(), value)) && fence->GetCompletedValue() < value &&
            SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
            const DWORD wait_result = WaitForSingleObject(event, INFINITE);
            if (wait_result != WAIT_OBJECT_0) {
                CloseHandle(event);
                return;
            }
        }
        CloseHandle(event);
    }
};

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

class D3D12Device final : public Device {
public:
    explicit D3D12Device(std::shared_ptr<D3D12Context> context) : context_(std::move(context)) {}

    const DeviceInfo& info() const noexcept override { return context_->info; }

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
        if (!valid_buffer_update(buffer, offset, data.size(), diagnostic))
            return {BufferStatus::invalid_description, std::move(diagnostic)};
        auto* d3d_buffer = dynamic_cast<D3D12Buffer*>(&buffer);
        if (d3d_buffer == nullptr)
            return {BufferStatus::unsupported, {"buffer_type_unsupported", "The D3D12 device received an unknown buffer handle"}};
        const bool updated = d3d_buffer->info().description.memory == BufferMemory::host_visible
                                 ? d3d_buffer->write_host(offset, data, diagnostic)
                                 : d3d_buffer->write_device(offset, data, diagnostic);
        return updated ? BufferUpdateResult{BufferStatus::ready, {}}
                       : BufferUpdateResult{BufferStatus::upload_failed, std::move(diagnostic)};
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
    context->info = info_from(selected.description);
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
