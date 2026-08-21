#pragma once

#include "apex/render/texture_format.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

/** Resource memory classes exposed by the backend-neutral buffer contract. */
enum class BufferMemory : std::uint8_t {
    device_local,
    host_visible,
};

/** Whether a buffer may be updated after creation. */
enum class BufferMutability : std::uint8_t {
    immutable,
    mutable_data,
};

/** Usage bits are intentionally API-neutral and may be combined. */
enum class BufferUsage : std::uint32_t {
    none = 0,
    transfer_source = 1U << 0U,
    transfer_destination = 1U << 1U,
    vertex = 1U << 2U,
    index = 1U << 3U,
    uniform = 1U << 4U,
    storage = 1U << 5U,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) |
                                    static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr BufferUsage operator&(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) &
                                    static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool any(BufferUsage usage) noexcept {
    return static_cast<std::uint32_t>(usage) != 0U;
}

inline constexpr std::uint64_t max_buffer_bytes = 1ULL << 34U;
inline constexpr std::uint64_t max_texture_bytes = 1ULL << 34U;
inline constexpr std::uint32_t max_texture_dimension = 16384U;

struct BufferDescription {
    std::uint64_t size_bytes = 0;
    BufferUsage usage = BufferUsage::none;
    BufferMemory memory = BufferMemory::device_local;
    BufferMutability mutability = BufferMutability::immutable;
};

enum class BufferStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct BufferInfo {
    BufferDescription description{};
};

class Buffer {
public:
    virtual ~Buffer() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const BufferInfo& info() const noexcept = 0;
};

struct BufferResult {
    BufferStatus status = BufferStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<Buffer> buffer;

    [[nodiscard]] bool ok() const noexcept {
        return status == BufferStatus::ready && buffer != nullptr;
    }
};

struct BufferUpdateResult {
    BufferStatus status = BufferStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return status == BufferStatus::ready; }
};

[[nodiscard]] constexpr std::size_t texture_format_bytes_per_pixel(TextureFormat format) noexcept {
    return texture_format_info(format).bytes_per_pixel;
}

enum class TextureUsage : std::uint32_t {
    none = 0,
    sampled = 1U << 0U,
    transfer_source = 1U << 1U,
    transfer_destination = 1U << 2U,
    color_attachment = 1U << 3U,
    storage = 1U << 4U,
};

[[nodiscard]] constexpr TextureUsage operator|(TextureUsage left, TextureUsage right) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) |
                                     static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr TextureUsage operator&(TextureUsage left, TextureUsage right) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) &
                                     static_cast<std::uint32_t>(right));
}

enum class TextureMemory : std::uint8_t {
    device_local,
    host_visible,
};

enum class TextureMutability : std::uint8_t {
    immutable,
    mutable_data,
};

struct TextureDescription {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    TextureFormat format = TextureFormat::rgba8_unorm;
    TextureUsage usage = TextureUsage::none;
    TextureMemory memory = TextureMemory::device_local;
    TextureMutability mutability = TextureMutability::immutable;
};

struct TextureUpload {
    std::uint32_t mip_level = 0;
    std::uint32_t array_layer = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::span<const std::byte> data{};
};

struct TextureUploadPlan {
    std::vector<TextureUpload> subresources;
};

enum class TextureStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct TextureInfo {
    TextureDescription description{};
};

class Texture {
public:
    virtual ~Texture() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const TextureInfo& info() const noexcept = 0;
};

struct TextureResult {
    TextureStatus status = TextureStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<Texture> texture;

    [[nodiscard]] bool ok() const noexcept {
        return status == TextureStatus::ready && texture != nullptr;
    }
};

struct TextureUpdateResult {
    TextureStatus status = TextureStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return status == TextureStatus::ready; }
};

[[nodiscard]] BufferStatus validate_buffer_description(
    const BufferDescription& description,
    std::size_t initial_data_size,
    Diagnostic& diagnostic);

[[nodiscard]] BufferStatus validate_buffer_update(
    const Buffer& buffer,
    std::uint64_t offset,
    std::size_t data_size,
    Diagnostic& diagnostic);

[[nodiscard]] TextureStatus validate_texture_description(
    const TextureDescription& description,
    const TextureUploadPlan& initial_uploads,
    Diagnostic& diagnostic);

[[nodiscard]] TextureStatus validate_texture_upload_plan(
    const TextureDescription& description,
    const TextureUploadPlan& uploads,
    Diagnostic& diagnostic);

[[nodiscard]] TextureStatus validate_texture_update(
    const Texture& texture,
    const TextureUploadPlan& uploads,
    Diagnostic& diagnostic);

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

    // Buffer lifetimes may extend beyond this Device object: backend handles
    // retain their private shared context. Public callers only see this
    // backend-neutral RAII contract, never Vk* or ID3D12* types.
    [[nodiscard]] virtual BufferResult create_buffer(
        const BufferDescription& description,
        std::span<const std::byte> initial_data = {}) = 0;

    [[nodiscard]] virtual BufferUpdateResult update_buffer(
        Buffer& buffer,
        std::uint64_t offset,
        std::span<const std::byte> data) = 0;

    [[nodiscard]] virtual TextureResult create_texture(
        const TextureDescription& description,
        const TextureUploadPlan& initial_uploads = {}) = 0;

    [[nodiscard]] virtual TextureUpdateResult update_texture(
        Texture& texture,
        const TextureUploadPlan& uploads) = 0;

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
[[nodiscard]] const char* buffer_status_name(BufferStatus status) noexcept;
[[nodiscard]] const char* texture_status_name(TextureStatus status) noexcept;

[[nodiscard]] AdapterResult enumerate_adapters(Backend backend,
                                                const DeviceOptions& options = {});

[[nodiscard]] DeviceResult create_device(Backend backend,
                                         const DeviceOptions& options = {});

} // namespace apex::render
