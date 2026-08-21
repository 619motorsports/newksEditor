#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apex::render {

/** Graphics APIs supported by the native renderer. */
enum class Backend {
    Vulkan,
    D3D12,
};

/** Stable status values returned by backend discovery and initialization. */
enum class DeviceStatus {
    ready,
    unavailable,
    invalid_options,
    initialization_failed,
};

struct Diagnostic {
    std::string code;
    std::string message;
};

/** Options shared by all backend implementations. */
struct DeviceOptions {
    // A native renderer device is intentionally headless. No window-system
    // extensions or swapchain are requested by either backend.
    bool headless = true;
    bool enable_validation = false;
    bool allow_software = true;
    // Prefer a software adapter when one is available. D3D12 resolves WARP
    // explicitly; Vulkan prioritizes CPU physical devices.
    bool prefer_software = false;
    std::uint32_t adapter_index = 0;
};

struct DeviceInfo {
    Backend backend = Backend::Vulkan;
    std::string name;
    std::string driver;
    std::uint32_t api_major = 0;
    std::uint32_t api_minor = 0;
    std::uint32_t api_patch = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    bool software = false;
};

struct AdapterResult {
    DeviceStatus status = DeviceStatus::unavailable;
    Diagnostic diagnostic;
    std::vector<DeviceInfo> adapters;

    [[nodiscard]] bool ok() const noexcept { return status == DeviceStatus::ready; }
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual const DeviceInfo& info() const noexcept = 0;

    // This is deliberately the only synchronization operation in the first
    // slice. Command encoding and resource objects will be added without
    // exposing Vk*/D3D12* types through this interface.
    virtual void wait_idle() noexcept = 0;
};

struct DeviceResult {
    DeviceStatus status = DeviceStatus::unavailable;
    Diagnostic diagnostic;
    std::unique_ptr<Device> device;

    [[nodiscard]] bool ok() const noexcept {
        return status == DeviceStatus::ready && device != nullptr;
    }
};

[[nodiscard]] const char* backend_name(Backend backend) noexcept;
[[nodiscard]] const char* device_status_name(DeviceStatus status) noexcept;

[[nodiscard]] AdapterResult enumerate_adapters(Backend backend,
                                                const DeviceOptions& options = {});

[[nodiscard]] DeviceResult create_device(Backend backend,
                                         const DeviceOptions& options = {});

} // namespace apex::render
