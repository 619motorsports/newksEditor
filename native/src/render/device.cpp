#include "apex/render/device.hpp"
#include "backend_internal.hpp"

#include <limits>
#include <utility>

namespace apex::render {

namespace {

AdapterResult unavailable(const char* code, std::string message) {
    AdapterResult result;
    result.status = DeviceStatus::unavailable;
    result.diagnostic = {code, std::move(message)};
    return result;
}

AdapterResult invalid_options(const char* code, std::string message) {
    AdapterResult result;
    result.status = DeviceStatus::invalid_options;
    result.diagnostic = {code, std::move(message)};
    return result;
}

DeviceResult unavailable_device(const char* code, std::string message) {
    DeviceResult result;
    result.status = DeviceStatus::unavailable;
    result.diagnostic = {code, std::move(message)};
    return result;
}

DeviceResult invalid_device_options(const char* code, std::string message) {
    DeviceResult result;
    result.status = DeviceStatus::invalid_options;
    result.diagnostic = {code, std::move(message)};
    return result;
}

} // namespace

bool valid_buffer_usage(BufferUsage usage) noexcept {
    constexpr auto known = BufferUsage::transfer_source | BufferUsage::transfer_destination |
                           BufferUsage::vertex | BufferUsage::index | BufferUsage::uniform |
                           BufferUsage::storage;
    const auto raw = static_cast<std::uint32_t>(usage);
    return raw != 0U && (raw & ~static_cast<std::uint32_t>(known)) == 0U;
}

BufferStatus validate_buffer_description(const BufferDescription& description,
                                         std::size_t initial_data_size,
                                         Diagnostic& diagnostic) {
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (description.size_bytes == 0U) {
        diagnostic = {"buffer_size_zero", "A buffer must contain at least one byte"};
        return BufferStatus::invalid_description;
    }
    if (description.size_bytes > max_size_t) {
        diagnostic = {"buffer_size_platform_limit", "The buffer size cannot be represented by this platform"};
        return BufferStatus::invalid_description;
    }
    if (description.size_bytes > max_buffer_bytes) {
        diagnostic = {"buffer_size_limit", "The buffer size exceeds the backend-neutral safety limit"};
        return BufferStatus::invalid_description;
    }
    if (!valid_buffer_usage(description.usage)) {
        diagnostic = {"buffer_usage_invalid", "A buffer must specify only known non-empty usage bits"};
        return BufferStatus::invalid_description;
    }
    if (static_cast<std::uint64_t>(initial_data_size) > description.size_bytes) {
        diagnostic = {"buffer_initial_data_too_large", "Initial buffer data exceeds the declared buffer size"};
        return BufferStatus::invalid_description;
    }
    return BufferStatus::ready;
}

BufferStatus validate_buffer_update(const Buffer& buffer,
                                    std::uint64_t offset,
                                    std::size_t data_size,
                                    Diagnostic& diagnostic) {
    const BufferDescription& description = buffer.info().description;
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (description.size_bytes > max_size_t || offset > max_size_t) {
        diagnostic = {"buffer_update_platform_limit", "The buffer update range cannot be represented by this platform"};
        return BufferStatus::invalid_description;
    }
    if (description.mutability != BufferMutability::mutable_data) {
        diagnostic = {"buffer_immutable", "Immutable buffers cannot be updated"};
        return BufferStatus::invalid_description;
    }
    if (offset > description.size_bytes || static_cast<std::uint64_t>(data_size) > description.size_bytes - offset) {
        diagnostic = {"buffer_update_out_of_range", "The buffer update range exceeds the declared buffer size"};
        return BufferStatus::invalid_description;
    }
    if (data_size == 0U) {
        diagnostic = {"buffer_update_empty", "A buffer update must contain at least one byte"};
        return BufferStatus::invalid_description;
    }
    return BufferStatus::ready;
}

bool valid_buffer_description(const BufferDescription& description,
                              std::size_t initial_data_size,
                              Diagnostic& diagnostic) {
    return validate_buffer_description(description, initial_data_size, diagnostic) == BufferStatus::ready;
}

bool valid_buffer_update(const Buffer& buffer,
                         std::uint64_t offset,
                         std::size_t data_size,
                         Diagnostic& diagnostic) {
    return validate_buffer_update(buffer, offset, data_size, diagnostic) == BufferStatus::ready;
}

const char* backend_name(Backend backend) noexcept {
    switch (backend) {
    case Backend::Vulkan:
        return "Vulkan";
    case Backend::D3D12:
        return "Direct3D 12";
    }
    return "Unknown";
}

const char* device_status_name(DeviceStatus status) noexcept {
    switch (status) {
    case DeviceStatus::ready:
        return "ready";
    case DeviceStatus::unavailable:
        return "unavailable";
    case DeviceStatus::invalid_options:
        return "invalid_options";
    case DeviceStatus::initialization_failed:
        return "initialization_failed";
    }
    return "unknown";
}

const char* buffer_status_name(BufferStatus status) noexcept {
    switch (status) {
    case BufferStatus::ready:
        return "ready";
    case BufferStatus::invalid_description:
        return "invalid_description";
    case BufferStatus::unsupported:
        return "unsupported";
    case BufferStatus::allocation_failed:
        return "allocation_failed";
    case BufferStatus::upload_failed:
        return "upload_failed";
    }
    return "unknown";
}

AdapterResult enumerate_adapters(Backend backend, const DeviceOptions& options) {
    if (!options.headless) {
        return invalid_options("headless_required",
                               "The native renderer device contract only supports headless initialization");
    }
    if (options.prefer_software && !options.allow_software) {
        return invalid_options("software_adapter_disallowed",
                               "A software adapter cannot be preferred when software adapters are disabled");
    }

    switch (backend) {
    case Backend::Vulkan:
        return enumerate_vulkan_adapters(options);
    case Backend::D3D12:
        return enumerate_d3d12_adapters(options);
    }
    return unavailable("unknown_backend", "Unknown renderer backend");
}

DeviceResult create_device(Backend backend, const DeviceOptions& options) {
    if (!options.headless) {
        return invalid_device_options("headless_required",
                                      "The native renderer device contract only supports headless initialization");
    }
    if (options.prefer_software && !options.allow_software) {
        return invalid_device_options("software_adapter_disallowed",
                                      "A software adapter cannot be preferred when software adapters are disabled");
    }

    switch (backend) {
    case Backend::Vulkan:
        return create_vulkan_device(options);
    case Backend::D3D12:
        return create_d3d12_device(options);
    }
    return unavailable_device("unknown_backend", "Unknown renderer backend");
}

} // namespace apex::render
