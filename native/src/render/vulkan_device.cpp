#include "backend_internal.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/lighting.hpp"
#include "half_float.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#if defined(APEX_HAS_VULKAN) && APEX_HAS_VULKAN
#    include <vulkan/vulkan.h>
#    include "generated/fxaa_spirv.hpp"
#    include "generated/hdr_tone_map_spirv.hpp"
#    include "generated/hdr_bloom_spirv.hpp"
#    include "generated/portable_sky_spirv.hpp"
#    include "generated/portable_clouds_spirv.hpp"
#    include "generated/portable_grass_spirv.hpp"
#endif

namespace apex::render {

#if defined(APEX_HAS_VULKAN) && APEX_HAS_VULKAN

namespace {

const char* result_name(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    default:
        return "VkResult error";
    }
}

Diagnostic vk_error(const char* operation, VkResult result) {
    return {"vulkan_initialization_failed",
            std::string(operation) + " failed with " + result_name(result) +
                " (" + std::to_string(static_cast<int>(result)) + ")"};
}

bool has_validation_layer() {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (count != 0 && vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    for (const auto& layer : layers) {
        if (std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation") {
            return true;
        }
    }
    return false;
}

// WSI is intentionally not enabled by the headless device contract. Keep
// this inventory separate from instance creation so callers can inspect
// prerequisites without constructing a VkSurfaceKHR. In particular, the
// presence of VK_KHR_swapchain and a platform surface extension does not
// prove that a surface or a present-capable queue is available.
inline constexpr std::uint32_t max_extension_properties = 4096U;

template <typename EnumerateFunction>
std::vector<VkExtensionProperties> enumerate_extension_properties(
    EnumerateFunction&& enumerate) {
    std::uint32_t count = 0U;
    VkResult result = enumerate(&count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return {};
    if (count == 0U || count > max_extension_properties) return {};

    std::vector<VkExtensionProperties> properties(count);
    result = enumerate(&count, properties.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return {};
    if (count < properties.size()) properties.resize(count);
    return properties;
}

bool has_extension(std::span<const VkExtensionProperties> properties,
                   std::string_view name) noexcept {
    for (const VkExtensionProperties& property : properties) {
        if (name == std::string_view(property.extensionName)) return true;
    }
    return false;
}

bool has_native_surface_extension(std::span<const VkExtensionProperties> properties) noexcept {
#if defined(_WIN32)
    return has_extension(properties, "VK_KHR_win32_surface");
#elif defined(__linux__)
    return has_extension(properties, "VK_KHR_xcb_surface") ||
           has_extension(properties, "VK_KHR_xlib_surface") ||
           has_extension(properties, "VK_KHR_wayland_surface");
#elif defined(__APPLE__)
    return has_extension(properties, "VK_EXT_metal_surface");
#else
    return false;
#endif
}

PresentationCapabilities query_vulkan_presentation_capabilities(
    VkPhysicalDevice physical_device) {
    PresentationCapabilities capabilities;
    const auto instance_properties = enumerate_extension_properties(
        [](std::uint32_t* count, VkExtensionProperties* properties) {
            return vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
        });
    const auto device_properties = enumerate_extension_properties(
        [physical_device](std::uint32_t* count, VkExtensionProperties* properties) {
            return vkEnumerateDeviceExtensionProperties(physical_device, nullptr, count, properties);
        });

    const bool surface_api = has_extension(instance_properties, "VK_KHR_surface");
    capabilities.swapchain_api_available =
        surface_api && has_extension(device_properties, "VK_KHR_swapchain");
    capabilities.native_surface_api_available =
        surface_api && has_native_surface_extension(instance_properties);
    capabilities.headless_surface_api_available =
        surface_api && has_extension(instance_properties, "VK_EXT_headless_surface");
    return capabilities;
}

struct Instance {
    VkInstance value = VK_NULL_HANDLE;

    ~Instance() {
        if (value != VK_NULL_HANDLE) {
            vkDestroyInstance(value, nullptr);
        }
    }

    Instance() = default;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&& other) noexcept : value(std::exchange(other.value, VK_NULL_HANDLE)) {}
};

struct RawVulkanSurface {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    void* callback_context = nullptr;
    platform::NativeSurfaceSource::DestroyVulkanSurface destroy_callback = nullptr;

    RawVulkanSurface() = default;
    RawVulkanSurface(const RawVulkanSurface&) = delete;
    RawVulkanSurface& operator=(const RawVulkanSurface&) = delete;
    RawVulkanSurface(RawVulkanSurface&& other) noexcept
        : instance(std::exchange(other.instance, VK_NULL_HANDLE)),
          surface(std::exchange(other.surface, VK_NULL_HANDLE)),
          callback_context(std::exchange(other.callback_context, nullptr)),
          destroy_callback(std::exchange(other.destroy_callback, nullptr)) {}
    RawVulkanSurface& operator=(RawVulkanSurface&& other) noexcept {
        if (this == &other) return *this;
        reset();
        instance = std::exchange(other.instance, VK_NULL_HANDLE);
        surface = std::exchange(other.surface, VK_NULL_HANDLE);
        callback_context = std::exchange(other.callback_context, nullptr);
        destroy_callback = std::exchange(other.destroy_callback, nullptr);
        return *this;
    }
    ~RawVulkanSurface() { reset(); }

    void reset() noexcept {
        if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
            if (destroy_callback != nullptr) {
                destroy_callback(callback_context, &instance, &surface);
            } else {
                vkDestroySurfaceKHR(instance, surface, nullptr);
            }
        }
        instance = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
        callback_context = nullptr;
        destroy_callback = nullptr;
    }
};

bool create_headless_surface(VkInstance instance, RawVulkanSurface& output,
                             Diagnostic& diagnostic) {
    output.reset();
    auto create_function = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
    if (create_function == nullptr) {
        diagnostic = {"vulkan_headless_surface_function_unavailable",
                      "VK_EXT_headless_surface is advertised but vkCreateHeadlessSurfaceEXT is unavailable"};
        return false;
    }
    output.instance = instance;
    VkHeadlessSurfaceCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    create_info.flags = 0U;
    const VkResult result = create_function(instance, &create_info, nullptr, &output.surface);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateHeadlessSurfaceEXT", result);
        diagnostic.code = "vulkan_headless_surface_creation_failed";
        output.reset();
        return false;
    }
    return true;
}

bool create_native_surface(VkInstance instance,
                           const platform::NativeSurfaceSource& source,
                           RawVulkanSurface& output,
                           Diagnostic& diagnostic) {
    output.reset();
    output.instance = instance;
    output.callback_context = source.context;
    output.destroy_callback = source.destroyVulkanSurface;
    std::string callback_diagnostic;
    if (!source.createVulkanSurface(source.context, &output.instance, &output.surface,
                                    callback_diagnostic)) {
        diagnostic = {"vulkan_native_surface_creation_failed",
                      callback_diagnostic.empty()
                          ? "The platform failed to create the Vulkan native surface"
                          : callback_diagnostic};
        output.reset();
        return false;
    }
    if (output.surface == VK_NULL_HANDLE) {
        diagnostic = {"vulkan_native_surface_invalid",
                      "The platform native-surface callback returned no Vulkan surface"};
        output.reset();
        return false;
    }
    return true;
}

struct PhysicalDevice {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::uint32_t graphics_queue_family = UINT32_MAX;
};

AdapterResult make_instance(const DeviceOptions& options, Instance& instance) {
    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    if (options.enable_validation) {
        if (!has_validation_layer()) {
            AdapterResult result;
            result.status = DeviceStatus::unavailable;
            result.diagnostic = {"vulkan_validation_unavailable",
                                 "VK_LAYER_KHRONOS_validation was requested but is not installed"};
            return result;
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    if (options.enable_headless_presentation) {
        const auto available_extensions = enumerate_extension_properties(
            [](std::uint32_t* count, VkExtensionProperties* properties) {
                return vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
            });
        if (!has_extension(available_extensions, "VK_KHR_surface") ||
            !has_extension(available_extensions, "VK_EXT_headless_surface")) {
            AdapterResult result;
            result.status = DeviceStatus::unavailable;
            result.diagnostic = {"vulkan_headless_surface_unavailable",
                                 "VK_KHR_surface and VK_EXT_headless_surface are required for headless presentation"};
            return result;
        }
        extensions.push_back("VK_KHR_surface");
        extensions.push_back("VK_EXT_headless_surface");
    } else if (!options.headless && options.native_surface.has_value()) {
        const auto available_extensions = enumerate_extension_properties(
            [](std::uint32_t* count, VkExtensionProperties* properties) {
                return vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
            });
        for (const std::string& requested : options.native_surface->vulkanInstanceExtensions) {
            if (!has_extension(available_extensions, requested)) {
                AdapterResult result;
                result.status = DeviceStatus::unavailable;
                result.diagnostic = {"vulkan_native_surface_extension_unavailable",
                                     "The Vulkan instance does not expose requested native-surface extension " +
                                         requested};
                return result;
            }
            extensions.push_back(requested.c_str());
        }
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Apex Editor native renderer";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "Apex Editor";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    const VkResult result = vkCreateInstance(&create_info, nullptr, &instance.value);
    if (result != VK_SUCCESS) {
        AdapterResult failure;
        failure.status = DeviceStatus::initialization_failed;
        failure.diagnostic = vk_error("vkCreateInstance", result);
        return failure;
    }
    AdapterResult success;
    success.status = DeviceStatus::ready;
    return success;
}

std::vector<PhysicalDevice> physical_devices(VkInstance instance,
                                             VkSurfaceKHR surface = VK_NULL_HANDLE) {
    std::uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        return {};
    }
    std::vector<VkPhysicalDevice> handles(count);
    if (vkEnumeratePhysicalDevices(instance, &count, handles.data()) != VK_SUCCESS) {
        return {};
    }
    std::vector<PhysicalDevice> devices;
    for (VkPhysicalDevice handle : handles) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(handle, &properties);
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &family_count, families.data());
        std::uint32_t graphics_family = UINT32_MAX;
        for (std::uint32_t index = 0; index < family_count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
                families[index].queueCount == 0)
                continue;
            if (surface != VK_NULL_HANDLE) {
                VkBool32 present_supported = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(handle, index, surface,
                                                         &present_supported) != VK_SUCCESS ||
                    present_supported == VK_FALSE)
                    continue;
            }
            graphics_family = index;
            break;
        }
        if (graphics_family != UINT32_MAX) {
            devices.push_back({handle, properties, graphics_family});
        }
    }
    return devices;
}

DeviceInfo info_from(const VkPhysicalDeviceProperties& properties) {
    DeviceInfo info;
    info.backend = Backend::Vulkan;
    info.name = properties.deviceName;
    info.driver = std::to_string(VK_VERSION_MAJOR(properties.driverVersion)) + "." +
                  std::to_string(VK_VERSION_MINOR(properties.driverVersion)) + "." +
                  std::to_string(VK_VERSION_PATCH(properties.driverVersion));
    info.api_major = VK_VERSION_MAJOR(properties.apiVersion);
    info.api_minor = VK_VERSION_MINOR(properties.apiVersion);
    info.api_patch = VK_VERSION_PATCH(properties.apiVersion);
    info.vendor_id = properties.vendorID;
    info.device_id = properties.deviceID;
    info.software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    return info;
}

struct VulkanContext {
    Instance instance;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = UINT32_MAX;
    VkSurfaceKHR presentation_surface = VK_NULL_HANDLE;
    void* native_surface_context = nullptr;
    platform::NativeSurfaceSource::DestroyVulkanSurface native_surface_destroy = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::mutex command_mutex;
    bool sampler_anisotropy = false;
    bool image_cube_array = false;
    float max_sampler_anisotropy = 1.0F;
    VkDeviceSize max_uniform_buffer_range = 0U;
    VkDeviceSize min_uniform_buffer_offset_alignment = 1U;
    PresentationCapabilities presentation_capabilities{};
    bool presentation_target_active = false;
    DeviceInfo info;

    VulkanContext(Instance instance_value,
                  VkPhysicalDevice physical,
                  VkDevice device_value,
                  VkQueue queue_value,
                  std::uint32_t family,
                  DeviceInfo info_value,
                  bool sampler_anisotropy_value,
                  bool image_cube_array_value,
                  float max_sampler_anisotropy_value,
                  VkDeviceSize max_uniform_buffer_range_value,
                  VkDeviceSize min_uniform_buffer_offset_alignment_value)
        : instance(std::move(instance_value)), physical_device(physical), device(device_value),
          queue(queue_value), queue_family(family), sampler_anisotropy(sampler_anisotropy_value),
          image_cube_array(image_cube_array_value),
          max_sampler_anisotropy(max_sampler_anisotropy_value),
          max_uniform_buffer_range(max_uniform_buffer_range_value),
          min_uniform_buffer_offset_alignment(min_uniform_buffer_offset_alignment_value == 0U
                                                  ? 1U : min_uniform_buffer_offset_alignment_value),
          info(std::move(info_value)) {}

    ~VulkanContext() {
        if (device != VK_NULL_HANDLE) {
            (void)wait_idle();
            if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (presentation_surface != VK_NULL_HANDLE && instance.value != VK_NULL_HANDLE) {
            if (native_surface_destroy != nullptr) {
                native_surface_destroy(native_surface_context, &instance.value,
                                       &presentation_surface);
            } else {
                vkDestroySurfaceKHR(instance.value, presentation_surface, nullptr);
            }
        }
    }

    bool wait_idle() noexcept {
        std::lock_guard lock(command_mutex);
        return device == VK_NULL_HANDLE || vkDeviceWaitIdle(device) == VK_SUCCESS;
    }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
};

struct RawVulkanBuffer {
    std::shared_ptr<VulkanContext> context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryPropertyFlags properties = 0;

    RawVulkanBuffer() = default;
    RawVulkanBuffer(const RawVulkanBuffer&) = delete;
    RawVulkanBuffer& operator=(const RawVulkanBuffer&) = delete;
    RawVulkanBuffer(RawVulkanBuffer&& other) noexcept
        : context(std::move(other.context)), buffer(std::exchange(other.buffer, VK_NULL_HANDLE)),
          memory(std::exchange(other.memory, VK_NULL_HANDLE)), properties(other.properties) {
        other.properties = 0;
    }
    RawVulkanBuffer& operator=(RawVulkanBuffer&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        properties = other.properties;
        other.properties = 0;
        return *this;
    }
    ~RawVulkanBuffer() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(context->device, buffer, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(context->device, memory, nullptr);
        }
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        properties = 0;
        context.reset();
    }
};

std::optional<std::uint32_t> memory_type(const VulkanContext& context,
                                         std::uint32_t type_bits,
                                         VkMemoryPropertyFlags required,
                                         VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &properties);
    std::optional<std::uint32_t> fallback;
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1U << index)) == 0U) continue;
        const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return index;
        if (!fallback.has_value()) fallback = index;
    }
    return fallback;
}

VkBufferUsageFlags vk_usage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    const auto raw = static_cast<std::uint32_t>(usage);
    if ((raw & static_cast<std::uint32_t>(BufferUsage::transfer_source)) != 0U)
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if ((raw & static_cast<std::uint32_t>(BufferUsage::transfer_destination)) != 0U)
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((raw & static_cast<std::uint32_t>(BufferUsage::vertex)) != 0U)
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if ((raw & static_cast<std::uint32_t>(BufferUsage::index)) != 0U)
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if ((raw & static_cast<std::uint32_t>(BufferUsage::uniform)) != 0U)
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if ((raw & static_cast<std::uint32_t>(BufferUsage::storage)) != 0U)
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    return flags;
}

bool create_raw_buffer(const std::shared_ptr<VulkanContext>& context,
                       const BufferDescription& description,
                       VkBufferUsageFlags usage,
                       BufferMemory memory,
                       RawVulkanBuffer& raw,
                       Diagnostic& diagnostic) {
    raw.reset();
    raw.context = context;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(description.size_bytes);
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(context->device, &buffer_info, nullptr, &raw.buffer);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateBuffer", result);
        diagnostic.code = "buffer_allocation_failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context->device, raw.buffer, &requirements);
    const VkMemoryPropertyFlags required = memory == BufferMemory::device_local
                                                ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                                : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags preferred = memory == BufferMemory::device_local
                                                 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                                 : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto type = memory_type(*context, requirements.memoryTypeBits, required, preferred);
    if (!type.has_value()) {
        diagnostic = {"buffer_memory_type_unavailable", "Vulkan has no compatible memory type for the buffer"};
        raw.reset();
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    result = vkAllocateMemory(context->device, &allocation, nullptr, &raw.memory);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateMemory", result);
        diagnostic.code = "buffer_allocation_failed";
        raw.reset();
        return false;
    }
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context->physical_device, &properties);
    raw.properties = properties.memoryTypes[*type].propertyFlags;
    result = vkBindBufferMemory(context->device, raw.buffer, raw.memory, 0);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkBindBufferMemory", result);
        diagnostic.code = "buffer_allocation_failed";
        raw.reset();
        return false;
    }
    return true;
}

bool write_host_buffer(const std::shared_ptr<VulkanContext>& context,
                       const RawVulkanBuffer& raw,
                       std::uint64_t offset,
                       std::span<const std::byte> data,
                       Diagnostic& diagnostic) {
    void* mapped = nullptr;
    // Map from zero so sub-range updates do not depend on the device's
    // minMemoryMapAlignment property.
    VkResult result = vkMapMemory(context->device, raw.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkMapMemory", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    std::memcpy(static_cast<std::byte*>(mapped) + static_cast<std::size_t>(offset), data.data(), data.size());
    if ((raw.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = raw.memory;
        // VK_WHOLE_SIZE avoids requiring callers to know the device's
        // non-coherent atom size when updating a sub-range.
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        result = vkFlushMappedMemoryRanges(context->device, 1, &range);
    }
    vkUnmapMemory(context->device, raw.memory);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkFlushMappedMemoryRanges", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    return true;
}

bool copy_buffer(const std::shared_ptr<VulkanContext>& context,
                 VkBuffer source,
                 VkBuffer destination,
                 VkDeviceSize destination_offset,
                 VkDeviceSize size,
                 BufferUsage destination_usage,
                 Diagnostic& diagnostic) {
    std::lock_guard command_guard(context->command_mutex);
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateCommandBuffers", result);
        diagnostic.code = "buffer_upload_failed";
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command, &begin);
    if (result == VK_SUCCESS) {
        VkBufferCopy region{};
        region.dstOffset = destination_offset;
        region.size = size;
        vkCmdCopyBuffer(command, source, destination, 1, &region);
        VkAccessFlags destination_access = 0;
        VkPipelineStageFlags destination_stage = 0;
        const auto usage = static_cast<std::uint32_t>(destination_usage);
        if ((usage & static_cast<std::uint32_t>(BufferUsage::transfer_source)) != 0U) {
            destination_access |= VK_ACCESS_TRANSFER_READ_BIT;
            destination_stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if ((usage & static_cast<std::uint32_t>(BufferUsage::transfer_destination)) != 0U) {
            destination_access |= VK_ACCESS_TRANSFER_WRITE_BIT;
            destination_stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if ((usage & static_cast<std::uint32_t>(BufferUsage::vertex)) != 0U) {
            destination_access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            destination_stage |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        }
        if ((usage & static_cast<std::uint32_t>(BufferUsage::index)) != 0U) {
            destination_access |= VK_ACCESS_INDEX_READ_BIT;
            destination_stage |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        }
        if ((usage & static_cast<std::uint32_t>(BufferUsage::uniform)) != 0U) {
            destination_access |= VK_ACCESS_UNIFORM_READ_BIT;
            destination_stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if ((usage & static_cast<std::uint32_t>(BufferUsage::storage)) != 0U) {
            destination_access |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            destination_stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if (destination_access != 0U) {
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = destination_access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = destination;
            barrier.offset = destination_offset;
            barrier.size = size;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, destination_stage, 0, 0,
                                 nullptr, 1, &barrier, 0, nullptr);
        }
        result = vkEndCommandBuffer(command);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain != VK_SUCCESS) result = drain;
    }
    vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan buffer copy", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "buffer_upload_failed";
        return false;
    }
    return true;
}

class VulkanBuffer final : public Buffer {
public:
    VulkanBuffer(std::shared_ptr<VulkanContext> context,
                 RawVulkanBuffer raw,
        BufferDescription description)
        : context_(std::move(context)), raw_(std::move(raw)), info_({description}) {}

    ~VulkanBuffer() override = default;

    Backend backend() const noexcept override { return Backend::Vulkan; }
    const BufferInfo& info() const noexcept override { return info_; }
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }
    const RawVulkanBuffer& raw() const noexcept { return raw_; }

    bool write_host(std::uint64_t offset, std::span<const std::byte> data, Diagnostic& diagnostic) {
        if ((raw_.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
            diagnostic = {"buffer_memory_not_host_visible", "The Vulkan buffer is not host-visible"};
            return false;
        }
        return write_host_buffer(context_, raw_, offset, data, diagnostic);
    }

    bool write_device(std::uint64_t offset,
                      std::span<const std::byte> data,
                      Diagnostic& diagnostic) {
        BufferDescription staging_description;
        staging_description.size_bytes = static_cast<std::uint64_t>(data.size());
        staging_description.usage = BufferUsage::transfer_source;
        staging_description.memory = BufferMemory::host_visible;
        staging_description.mutability = BufferMutability::immutable;
        RawVulkanBuffer staging;
        if (!create_raw_buffer(context_, staging_description, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               BufferMemory::host_visible, staging, diagnostic)) return false;
        const bool written = write_host_buffer(context_, staging, 0, data, diagnostic);
        const bool copied = written && copy_buffer(context_, staging.buffer, raw_.buffer,
                                                   static_cast<VkDeviceSize>(offset),
                                                   static_cast<VkDeviceSize>(data.size()),
                                                   info_.description.usage, diagnostic);
        return copied;
    }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanBuffer raw_;
    BufferInfo info_;
};

VkFormat vk_texture_format(TextureFormat format) {
    switch (format) {
    case TextureFormat::rgba8_unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::bgra8_unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::rgba8_srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::bgra8_srgb:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case TextureFormat::r8_unorm:
        return VK_FORMAT_R8_UNORM;
    case TextureFormat::r32_sfloat:
        return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::r5g6b5_unorm:
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case TextureFormat::rgba16_sfloat:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::bc1_unorm:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TextureFormat::bc1_srgb:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case TextureFormat::bc2_unorm:
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case TextureFormat::bc2_srgb:
        return VK_FORMAT_BC2_SRGB_BLOCK;
    case TextureFormat::bc3_unorm:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureFormat::bc3_srgb:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case TextureFormat::bc4_unorm:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case TextureFormat::bc5_unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureFormat::bc5_snorm:
        return VK_FORMAT_BC5_SNORM_BLOCK;
    case TextureFormat::bc6h_ufloat:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case TextureFormat::bc6h_sfloat:
        return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case TextureFormat::bc7_unorm:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::bc7_srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool vk_supported_block_upload_format(TextureFormat format) noexcept {
    return format == TextureFormat::bc1_unorm || format == TextureFormat::bc1_srgb ||
           format == TextureFormat::bc2_unorm || format == TextureFormat::bc2_srgb ||
           format == TextureFormat::bc3_unorm || format == TextureFormat::bc3_srgb ||
           format == TextureFormat::bc4_unorm ||
           format == TextureFormat::bc5_unorm || format == TextureFormat::bc5_snorm ||
           format == TextureFormat::bc6h_ufloat || format == TextureFormat::bc6h_sfloat ||
           format == TextureFormat::bc7_unorm || format == TextureFormat::bc7_srgb;
}

bool validate_vulkan_texture_format_capabilities(const std::shared_ptr<VulkanContext>& context,
                                                 const TextureDescription& description,
                                                 VkFormat format,
                                                 Diagnostic& diagnostic) {
    const bool compressed = vk_supported_block_upload_format(description.format);
    const bool scalar_float = description.format == TextureFormat::r32_sfloat;
    const bool rgba_float = description.format == TextureFormat::rgba16_sfloat;
    if (!compressed && !scalar_float && !rgba_float) return true;
    if (description.samples != 1U && !rgba_float) {
        diagnostic = {compressed ? "vulkan_compressed_samples_unsupported"
                                 : "vulkan_scalar_float_samples_unsupported",
                      compressed
                          ? "Vulkan BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 uploads require a single-sample texture"
                          : "Vulkan R32_SFLOAT uploads require a single-sample texture"};
        return false;
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context->physical_device, format, &properties);
    const auto usage = static_cast<std::uint32_t>(description.usage);
    VkFormatFeatureFlags required = 0U;
    if ((usage & static_cast<std::uint32_t>(TextureUsage::transfer_destination)) != 0U)
        required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((usage & static_cast<std::uint32_t>(TextureUsage::sampled)) != 0U)
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((usage & static_cast<std::uint32_t>(TextureUsage::transfer_source)) != 0U)
        required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((usage & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if ((usage & static_cast<std::uint32_t>(TextureUsage::storage)) != 0U)
        required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & required) != required) {
        diagnostic = {compressed ? "vulkan_compressed_format_unsupported"
                                 : rgba_float ? "vulkan_rgba_float_format_unsupported"
                                              : "vulkan_scalar_float_format_unsupported",
                      compressed
                          ? "The Vulkan device does not support the requested BC1, BC2, BC3, BC4, BC5, BC6H, or BC7 texture usage"
                          : rgba_float
                                ? "The Vulkan device does not support the requested RGBA16_SFLOAT texture usage"
                                : "The Vulkan device does not support the requested R32_SFLOAT texture usage"};
        return false;
    }
    return true;
}

VkSampleCountFlagBits vk_sample_count(std::uint32_t samples) noexcept {
    switch (samples) {
    case 1U: return VK_SAMPLE_COUNT_1_BIT;
    case 4U: return VK_SAMPLE_COUNT_4_BIT;
    default: return static_cast<VkSampleCountFlagBits>(0U);
    }
}

VkImageUsageFlags vk_texture_usage(TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    const auto raw = static_cast<std::uint32_t>(usage);
    if ((raw & static_cast<std::uint32_t>(TextureUsage::sampled)) != 0U)
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::transfer_source)) != 0U)
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::transfer_destination)) != 0U)
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::storage)) != 0U)
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    return flags;
}

VkImageLayout texture_final_layout(
    TextureUsage usage,
    TextureAccessPolicy access_policy = TextureAccessPolicy::fixed_usage) {
    if (access_policy == TextureAccessPolicy::render_then_sample)
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto raw = static_cast<std::uint32_t>(usage);
    if ((raw & static_cast<std::uint32_t>(TextureUsage::storage)) != 0U)
        return VK_IMAGE_LAYOUT_GENERAL;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::color_attachment)) != 0U)
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if ((raw & static_cast<std::uint32_t>(TextureUsage::sampled)) != 0U)
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

struct RawVulkanImage {
    std::shared_ptr<VulkanContext> context;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    RawVulkanImage() = default;
    RawVulkanImage(const RawVulkanImage&) = delete;
    RawVulkanImage& operator=(const RawVulkanImage&) = delete;
    RawVulkanImage(RawVulkanImage&& other) noexcept
        : context(std::move(other.context)), image(std::exchange(other.image, VK_NULL_HANDLE)),
          memory(std::exchange(other.memory, VK_NULL_HANDLE)), view(std::exchange(other.view, VK_NULL_HANDLE)) {}
    RawVulkanImage& operator=(RawVulkanImage&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        image = std::exchange(other.image, VK_NULL_HANDLE);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        view = std::exchange(other.view, VK_NULL_HANDLE);
        return *this;
    }
    ~RawVulkanImage() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device, view, nullptr);
            if (image != VK_NULL_HANDLE) vkDestroyImage(context->device, image, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(context->device, memory, nullptr);
        }
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        context.reset();
    }
};

struct ScopedVulkanImageView {
    VkDevice device = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    ScopedVulkanImageView() = default;
    ScopedVulkanImageView(const ScopedVulkanImageView&) = delete;
    ScopedVulkanImageView& operator=(const ScopedVulkanImageView&) = delete;
    ~ScopedVulkanImageView() {
        if (device != VK_NULL_HANDLE && view != VK_NULL_HANDLE)
            vkDestroyImageView(device, view, nullptr);
    }
};

// Depth attachments are persistent frame resources rather than color
// textures. Keep their layout and clear history with the image so a later
// indexed draw can safely choose LOAD instead of reading an undefined image.
struct RawVulkanDepthAttachment {
    std::shared_ptr<VulkanContext> context;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false;

    RawVulkanDepthAttachment() = default;
    RawVulkanDepthAttachment(const RawVulkanDepthAttachment&) = delete;
    RawVulkanDepthAttachment& operator=(const RawVulkanDepthAttachment&) = delete;
    RawVulkanDepthAttachment(RawVulkanDepthAttachment&& other) noexcept
        : context(std::move(other.context)), image(std::exchange(other.image, VK_NULL_HANDLE)),
          memory(std::exchange(other.memory, VK_NULL_HANDLE)), view(std::exchange(other.view, VK_NULL_HANDLE)),
          layout(other.layout), initialized(other.initialized) {
        other.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        other.initialized = false;
    }
    RawVulkanDepthAttachment& operator=(RawVulkanDepthAttachment&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        image = std::exchange(other.image, VK_NULL_HANDLE);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        view = std::exchange(other.view, VK_NULL_HANDLE);
        layout = other.layout;
        initialized = other.initialized;
        other.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        other.initialized = false;
        return *this;
    }
    ~RawVulkanDepthAttachment() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device, view, nullptr);
            if (image != VK_NULL_HANDLE) vkDestroyImage(context->device, image, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(context->device, memory, nullptr);
        }
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        layout = VK_IMAGE_LAYOUT_UNDEFINED;
        initialized = false;
        context.reset();
    }
};

bool create_raw_image(const std::shared_ptr<VulkanContext>& context,
                      const TextureDescription& description,
                      RawVulkanImage& raw,
                      Diagnostic& diagnostic) {
    raw.reset();
    raw.context = context;
    const VkSampleCountFlagBits sample_count = vk_sample_count(description.samples);
    if (sample_count == 0U) {
        diagnostic = {"vulkan_texture_samples_unsupported",
                      "Vulkan textures support only one or four samples in the bounded contract"};
        return false;
    }
    if (description.shape == TextureShape::texture_cube &&
        description.array_layers > 1U && !context->image_cube_array) {
        diagnostic = {
            "vulkan_cube_array_unsupported",
            "The Vulkan device does not support cube-array sampled views"};
        return false;
    }
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = vk_texture_format(description.format);
    image_info.extent = {description.width, description.height, 1};
    image_info.mipLevels = description.mip_levels;
    image_info.arrayLayers = static_cast<std::uint32_t>(texture_physical_array_layers(description));
    if (description.shape == TextureShape::texture_cube)
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image_info.samples = sample_count;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = vk_texture_usage(description.usage);
    // The first contract always permits synchronous uploads. A later
    // resource policy can remove this bit when immutable no-upload images are
    // introduced, but doing so now would make update validation backend-state
    // dependent.
    if (description.samples == 1U) image_info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!validate_vulkan_texture_format_capabilities(context, description, image_info.format, diagnostic))
        return false;
    VkImageFormatProperties format_properties{};
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        context->physical_device, image_info.format, image_info.imageType, image_info.tiling,
        image_info.usage, image_info.flags, &format_properties);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkGetPhysicalDeviceImageFormatProperties", result);
        diagnostic.code = "texture_format_unsupported";
        return false;
    }
    if ((format_properties.sampleCounts & static_cast<VkSampleCountFlags>(sample_count)) == 0U) {
        diagnostic = {"vulkan_texture_samples_unsupported",
                      "The Vulkan device does not support the requested texture sample count"};
        return false;
    }
    result = vkCreateImage(context->device, &image_info, nullptr, &raw.image);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateImage", result);
        diagnostic.code = "texture_allocation_failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context->device, raw.image, &requirements);
    const auto type = memory_type(*context, requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type.has_value()) {
        diagnostic = {"texture_memory_type_unavailable", "Vulkan has no device-local memory type for the texture"};
        raw.reset();
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    result = vkAllocateMemory(context->device, &allocation, nullptr, &raw.memory);
    if (result == VK_SUCCESS) result = vkBindImageMemory(context->device, raw.image, raw.memory, 0);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan image memory", result);
        diagnostic.code = "texture_allocation_failed";
        raw.reset();
        return false;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = raw.image;
    switch (texture_default_sample_view_kind(description)) {
    case TextureSampleViewKind::texture_2d: view_info.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
    case TextureSampleViewKind::texture_2d_array: view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case TextureSampleViewKind::texture_cube: view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case TextureSampleViewKind::texture_cube_array: view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
    }
    view_info.format = image_info.format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = description.mip_levels;
    view_info.subresourceRange.layerCount = image_info.arrayLayers;
    result = vkCreateImageView(context->device, &view_info, nullptr, &raw.view);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateImageView", result);
        diagnostic.code = "texture_allocation_failed";
        raw.reset();
        return false;
    }
    return true;
}

bool create_raw_depth_attachment(const std::shared_ptr<VulkanContext>& context,
                                 const DepthAttachmentDescription& description,
                                 RawVulkanDepthAttachment& raw,
                                 Diagnostic& diagnostic) {
    raw.reset();
    raw.context = context;

    const VkSampleCountFlagBits sample_count = vk_sample_count(description.samples);
    if (sample_count == 0U) {
        diagnostic = {"vulkan_depth_samples_unsupported",
                      "Vulkan depth attachments support only one or four samples in the bounded contract"};
        return false;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_D32_SFLOAT;
    image_info.extent = {description.width, description.height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = sample_count;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Keep the persistent attachment readable without creating a second
    // depth image. Readback transitions the image to TRANSFER_SRC and then
    // restores the tracked depth-attachment layout synchronously.
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (description.shader_readable)
        image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageFormatProperties format_properties{};
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        context->physical_device, image_info.format, image_info.imageType, image_info.tiling,
        image_info.usage, 0U, &format_properties);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkGetPhysicalDeviceImageFormatProperties(depth)", result);
        diagnostic.code = "depth_attachment_format_unsupported";
        raw.reset();
        return false;
    }
    if ((format_properties.sampleCounts & static_cast<VkSampleCountFlags>(sample_count)) == 0U) {
        diagnostic = {"vulkan_depth_samples_unsupported",
                      "The Vulkan device does not support the requested depth sample count"};
        raw.reset();
        return false;
    }
    result = vkCreateImage(context->device, &image_info, nullptr, &raw.image);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateImage(depth)", result);
        diagnostic.code = "depth_attachment_allocation_failed";
        raw.reset();
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context->device, raw.image, &requirements);
    const auto type = memory_type(*context, requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type.has_value()) {
        diagnostic = {"depth_attachment_memory_unavailable",
                      "Vulkan has no device-local memory type for the D32 depth attachment"};
        raw.reset();
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    result = vkAllocateMemory(context->device, &allocation, nullptr, &raw.memory);
    if (result == VK_SUCCESS) result = vkBindImageMemory(context->device, raw.image, raw.memory, 0U);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan depth image memory", result);
        diagnostic.code = "depth_attachment_allocation_failed";
        raw.reset();
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = raw.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_D32_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.levelCount = 1U;
    view_info.subresourceRange.layerCount = 1U;
    result = vkCreateImageView(context->device, &view_info, nullptr, &raw.view);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateImageView(depth)", result);
        diagnostic.code = "depth_attachment_allocation_failed";
        raw.reset();
        return false;
    }
    return true;
}

class VulkanDepthAttachment final : public DepthAttachment {
public:
    VulkanDepthAttachment(std::shared_ptr<VulkanContext> context,
                          RawVulkanDepthAttachment raw,
                          DepthAttachmentDescription description)
        : context_(std::move(context)), raw_(std::move(raw)), info_({description}) {}

    ~VulkanDepthAttachment() override = default;

    Backend backend() const noexcept override { return Backend::Vulkan; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }
    VkImage image() const noexcept { return raw_.image; }
    VkImageView view() const noexcept { return raw_.view; }
    VkImageLayout layout() const noexcept { return raw_.layout; }
    bool initialized() const noexcept { return raw_.initialized; }

    void mark_rendered(bool initialized) noexcept {
        raw_.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        raw_.initialized = raw_.initialized || initialized;
    }
    void mark_layout(VkImageLayout layout) noexcept {
        raw_.layout = layout;
    }
    void mark_invalid() noexcept {
        raw_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        raw_.initialized = false;
    }

    bool readback(const DepthAttachmentReadbackRequest& request,
                  std::vector<float>& output,
                  Diagnostic& diagnostic) {
        if (!raw_.initialized ||
            (raw_.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
             raw_.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) {
            diagnostic = {"depth_attachment_uninitialized",
                          "D32 depth readback requires an initialized depth attachment"};
            return false;
        }

        const std::size_t output_size = static_cast<std::size_t>(request.output_width) *
                                        static_cast<std::size_t>(request.output_height) * sizeof(float);
        BufferDescription staging_description;
        staging_description.size_bytes = output_size;
        staging_description.usage = BufferUsage::transfer_destination;
        staging_description.memory = BufferMemory::host_visible;
        RawVulkanBuffer staging;
        if (!create_raw_buffer(context_, staging_description, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               BufferMemory::host_visible, staging, diagnostic)) {
            diagnostic.code = "depth_attachment_readback_failed";
            return false;
        }

        const VkImageLayout restored_layout = raw_.layout;
        std::lock_guard command_guard(context_->command_mutex);
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = context_->command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1U;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(context_->device, &allocation, &command);
        bool submitted = false;
        bool completed = false;
        if (result == VK_SUCCESS) {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(command, &begin);
            if (result == VK_SUCCESS) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = raw_.layout;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcAccessMask =
                    restored_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                        ? VK_ACCESS_SHADER_READ_BIT
                        : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = raw_.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                barrier.subresourceRange.levelCount = 1U;
                barrier.subresourceRange.layerCount = 1U;
                vkCmdPipelineBarrier(command,
                                     restored_layout ==
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                         ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                         : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr,
                                     1U, &barrier);

                VkBufferImageCopy copy{};
                copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                copy.imageSubresource.mipLevel = 0U;
                copy.imageSubresource.baseArrayLayer = 0U;
                copy.imageSubresource.layerCount = 1U;
                copy.imageExtent = {request.output_width, request.output_height, 1U};
                vkCmdCopyImageToBuffer(command, raw_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       staging.buffer, 1U, &copy);

                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = restored_layout;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask =
                    restored_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                        ? VK_ACCESS_SHADER_READ_BIT
                        : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     restored_layout ==
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                         ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                         : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
                result = vkEndCommandBuffer(command);
            }
        }
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &command;
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            result = vkCreateFence(context_->device, &fence_info, nullptr, &fence);
            if (result == VK_SUCCESS) {
                result = vkQueueSubmit(context_->queue, 1U, &submit, fence);
                submitted = result == VK_SUCCESS;
                if (result == VK_SUCCESS) {
                    result = vkWaitForFences(context_->device, 1U, &fence, VK_TRUE, UINT64_MAX);
                    completed = result == VK_SUCCESS;
                }
                vkDestroyFence(context_->device, fence, nullptr);
            }
        }
        if (submitted && !completed) {
            const VkResult drain = vkDeviceWaitIdle(context_->device);
            if (drain != VK_SUCCESS) result = drain;
            else completed = true;
        }
        if (command != VK_NULL_HANDLE)
            vkFreeCommandBuffers(context_->device, context_->command_pool, 1U, &command);
        if (result != VK_SUCCESS || !completed) {
            diagnostic = vk_error("Vulkan depth attachment readback", result);
            diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                              : "depth_attachment_readback_failed";
            return false;
        }

        void* mapped = nullptr;
        result = vkMapMemory(context_->device, staging.memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkMapMemory(depth readback)", result);
            diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                              : "depth_attachment_readback_failed";
            return false;
        }
        if ((staging.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = staging.memory;
            range.offset = 0U;
            range.size = VK_WHOLE_SIZE;
            result = vkInvalidateMappedMemoryRanges(context_->device, 1U, &range);
        }
        if (result == VK_SUCCESS) {
            try {
                output.resize(static_cast<std::size_t>(request.output_width) *
                              static_cast<std::size_t>(request.output_height));
                std::memcpy(output.data(), mapped, output_size);
            } catch (const std::bad_alloc&) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        vkUnmapMemory(context_->device, staging.memory);
        if (result != VK_SUCCESS) {
            output.clear();
            diagnostic = vk_error("Vulkan depth readback map", result);
            diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                              : "depth_attachment_readback_failed";
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanDepthAttachment raw_;
    DepthAttachmentInfo info_;
};

bool copy_texture_uploads(const std::shared_ptr<VulkanContext>& context,
                          RawVulkanImage& image,
                          const TextureDescription& description,
                          const TextureUploadPlan& uploads,
                          VkImageLayout& current_layout,
                          Diagnostic& diagnostic) {
    if (uploads.subresources.empty()) {
        // No command transitioned this image. Keep the tracked layout
        // undefined so a later clear/draw emits the required first barrier;
        // sampled-resource validation separately rejects uninitialized data.
        return true;
    }
    if (description.samples != 1U) {
        diagnostic = {"vulkan_multisample_texture_upload_unsupported",
                      "Multisampled Vulkan textures cannot receive transfer uploads"};
        return false;
    }
    const bool block_compressed = vk_supported_block_upload_format(description.format);
    if (!texture_format_cpu_upload_supported(description.format) && !block_compressed) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "Block-compressed texture uploads require a dedicated GPU upload path"};
        return false;
    }
    const TextureFormatInfo format_info = texture_format_info(description.format);
    const auto bytes_per_pixel = texture_format_bytes_per_pixel(description.format);
    if (!block_compressed && bytes_per_pixel == 0U) {
        diagnostic = {"texture_format_unknown", "Texture upload format has no byte layout"};
        return false;
    }
    std::lock_guard command_guard(context->command_mutex);
    RawVulkanBuffer staging;
    std::vector<VkBufferImageCopy> copies;
    std::size_t staging_size = 0U;
    for (const TextureUpload& upload : uploads.subresources) {
        if (staging_size > std::numeric_limits<std::size_t>::max() - 3U) {
            diagnostic = {"texture_upload_size_overflow", "Texture upload staging alignment overflows the host size type"};
            return false;
        }
        staging_size = (staging_size + 3U) & ~static_cast<std::size_t>(3U);
        if (upload.data.size() > std::numeric_limits<std::size_t>::max() - staging_size) {
            diagnostic = {"texture_upload_size_overflow", "Texture upload staging size overflows the host size type"};
            return false;
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = staging_size;
        std::uint64_t row_bytes = 0U;
        std::uint64_t row_count = 0U;
        std::uint64_t buffer_row_length = 0U;
        std::uint64_t buffer_image_height = 0U;
        if (block_compressed) {
            const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(upload.width) + 3U) / 4U;
            const std::uint64_t blocks_high = (static_cast<std::uint64_t>(upload.height) + 3U) / 4U;
            row_bytes = blocks_wide * format_info.block_bytes;
            row_count = blocks_high;
            if (row_bytes == 0U || upload.row_pitch < row_bytes ||
                static_cast<std::uint64_t>(upload.row_pitch) % format_info.block_bytes != 0U ||
                row_count > std::numeric_limits<std::uint64_t>::max() / format_info.block_height) {
                diagnostic = {"texture_upload_block_layout_invalid",
                              "Compressed texture upload pitch does not match its block layout"};
                return false;
            }
            buffer_row_length = (static_cast<std::uint64_t>(upload.row_pitch) / format_info.block_bytes) *
                                format_info.block_width;
            buffer_image_height = row_count * format_info.block_height;
        } else {
            row_bytes = static_cast<std::uint64_t>(upload.width) * bytes_per_pixel;
            row_count = upload.height;
            buffer_row_length = upload.row_pitch / bytes_per_pixel;
            buffer_image_height = upload.height;
        }
        if (upload.row_pitch == 0U ||
            buffer_row_length == 0U || buffer_row_length > std::numeric_limits<std::uint32_t>::max() ||
            buffer_image_height == 0U || buffer_image_height > std::numeric_limits<std::uint32_t>::max() ||
            row_count > std::numeric_limits<std::size_t>::max() / upload.row_pitch ||
            row_count * upload.row_pitch > upload.data.size()) {
            diagnostic = {"texture_upload_block_layout_invalid",
                          "Texture upload rows exceed the declared source data"};
            return false;
        }
        copy.bufferRowLength = static_cast<std::uint32_t>(buffer_row_length);
        copy.bufferImageHeight = static_cast<std::uint32_t>(buffer_image_height);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = upload.mip_level;
        copy.imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(
            texture_upload_physical_array_layer(description, upload));
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {upload.width, upload.height, 1};
        copies.push_back(copy);
        staging_size += upload.data.size();
    }
    if (!uploads.subresources.empty()) {
        BufferDescription staging_description;
        staging_description.size_bytes = static_cast<std::uint64_t>(staging_size);
        staging_description.usage = BufferUsage::transfer_source;
        staging_description.memory = BufferMemory::host_visible;
        staging_description.mutability = BufferMutability::immutable;
        if (!create_raw_buffer(context, staging_description, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               BufferMemory::host_visible, staging, diagnostic)) return false;
        for (std::size_t index = 0; index < uploads.subresources.size(); ++index) {
            if (!write_host_buffer(context, staging, copies[index].bufferOffset,
                                   uploads.subresources[index].data, diagnostic)) {
                staging.reset();
                return false;
            }
        }
    }
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
        if (result == VK_SUCCESS) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = current_layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = description.mip_levels;
            barrier.subresourceRange.layerCount =
                static_cast<std::uint32_t>(texture_physical_array_layers(description));
            barrier.srcAccessMask = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                        ? 0U
                                        : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            const VkPipelineStageFlags source_stage = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                                           ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                           : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            vkCmdPipelineBarrier(command, source_stage,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            if (!copies.empty()) vkCmdCopyBufferToImage(command, staging.buffer, image.image,
                                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                        static_cast<std::uint32_t>(copies.size()), copies.data());
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = texture_final_layout(
                description.usage, description.access_policy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            result = vkEndCommandBuffer(command);
        }
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
                if (completed)
                    current_layout = texture_final_layout(
                        description.usage, description.access_policy);
            }
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain != VK_SUCCESS) {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            result = drain;
        } else {
            current_layout = texture_final_layout(
                description.usage, description.access_policy);
        }
    }
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    staging.reset();
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan texture upload", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "texture_upload_failed";
        return false;
    }
    if (!completed)
        current_layout = texture_final_layout(
            description.usage, description.access_policy);
    return true;
}

bool generate_texture_mips(const std::shared_ptr<VulkanContext>& context,
                           RawVulkanImage& image,
                           const TextureDescription& description,
                           VkImageLayout& current_layout,
                           Diagnostic& diagnostic) {
    const std::uint32_t physical_layers = static_cast<std::uint32_t>(
        texture_physical_array_layers(description));
    const VkImageLayout final_layout = texture_final_layout(
        description.usage, description.access_policy);
    std::lock_guard command_guard(context->command_mutex);
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1U;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(
        context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
    }
    if (result == VK_SUCCESS) {
        std::array<VkImageMemoryBarrier, 2U> initial{};
        for (VkImageMemoryBarrier& barrier : initial) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = current_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseArrayLayer = 0U;
            barrier.subresourceRange.layerCount = physical_layers;
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                    VK_ACCESS_MEMORY_WRITE_BIT;
        }
        initial[0U].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        initial[0U].subresourceRange.baseMipLevel = 0U;
        initial[0U].subresourceRange.levelCount = 1U;
        initial[0U].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        initial[1U].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        initial[1U].subresourceRange.baseMipLevel = 1U;
        initial[1U].subresourceRange.levelCount =
            description.mip_levels - 1U;
        initial[1U].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U,
                             nullptr, 0U, nullptr,
                             static_cast<std::uint32_t>(initial.size()),
                             initial.data());

        for (std::uint32_t mip = 1U; mip < description.mip_levels; ++mip) {
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1U;
            blit.srcSubresource.baseArrayLayer = 0U;
            blit.srcSubresource.layerCount = physical_layers;
            blit.srcOffsets[1] = {
                static_cast<std::int32_t>(
                    std::max(1U, description.width >> (mip - 1U))),
                static_cast<std::int32_t>(
                    std::max(1U, description.height >> (mip - 1U))),
                1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.baseArrayLayer = 0U;
            blit.dstSubresource.layerCount = physical_layers;
            blit.dstOffsets[1] = {
                static_cast<std::int32_t>(
                    std::max(1U, description.width >> mip)),
                static_cast<std::int32_t>(
                    std::max(1U, description.height >> mip)),
                1};
            vkCmdBlitImage(command, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1U, &blit, VK_FILTER_LINEAR);
            if (mip + 1U < description.mip_levels) {
                VkImageMemoryBarrier next{};
                next.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                next.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                next.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                next.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                next.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                next.image = image.image;
                next.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                next.subresourceRange.baseMipLevel = mip;
                next.subresourceRange.levelCount = 1U;
                next.subresourceRange.baseArrayLayer = 0U;
                next.subresourceRange.layerCount = physical_layers;
                next.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                next.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U,
                                     nullptr, 0U, nullptr, 1U, &next);
            }
        }

        std::array<VkImageMemoryBarrier, 2U> finish{};
        for (VkImageMemoryBarrier& barrier : finish) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.newLayout = final_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseArrayLayer = 0U;
            barrier.subresourceRange.layerCount = physical_layers;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        finish[0U].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        finish[0U].subresourceRange.baseMipLevel = 0U;
        finish[0U].subresourceRange.levelCount = description.mip_levels - 1U;
        finish[0U].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        finish[1U].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        finish[1U].subresourceRange.baseMipLevel = description.mip_levels - 1U;
        finish[1U].subresourceRange.levelCount = 1U;
        finish[1U].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U,
                             nullptr, 0U, nullptr,
                             static_cast<std::uint32_t>(finish.size()),
                             finish.data());
        result = vkEndCommandBuffer(command);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1U, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1U, &fence,
                                         VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain == VK_SUCCESS) {
            current_layout = final_layout;
            completed = true;
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            result = drain;
        }
    } else if (completed) {
        current_layout = final_layout;
    }
    if (command != VK_NULL_HANDLE)
        vkFreeCommandBuffers(context->device, context->command_pool, 1U,
                             &command);
    if (result != VK_SUCCESS || !completed) {
        diagnostic = vk_error("Vulkan texture mip generation", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "texture_mip_generation_failed";
        return false;
    }
    diagnostic = {};
    return true;
}

bool clear_texture_and_readback(const std::shared_ptr<VulkanContext>& context,
                                RawVulkanImage& image,
                                const TextureDescription& description,
                                const TextureClearReadbackRequest& request,
                                VkImageLayout& current_layout,
                                std::vector<std::byte>& output,
                                Diagnostic& diagnostic) {
    if (description.samples != 1U) {
        diagnostic = {"vulkan_multisample_clear_readback_unsupported",
                      "Vulkan clear/readback uses a single-sample color target"};
        return false;
    }
    const std::size_t output_size = static_cast<std::size_t>(request.output_width) *
                                    static_cast<std::size_t>(request.output_height) * 4U;
    BufferDescription staging_description;
    staging_description.size_bytes = output_size;
    staging_description.usage = BufferUsage::transfer_destination;
    staging_description.memory = BufferMemory::host_visible;
    RawVulkanBuffer staging;
    if (!create_raw_buffer(context, staging_description, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           BufferMemory::host_visible, staging, diagnostic)) {
        diagnostic.code = "texture_readback_failed";
        return false;
    }
    std::lock_guard command_guard(context->command_mutex);

    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
        if (result == VK_SUCCESS) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = current_layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = description.mip_levels;
            barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(
                texture_physical_array_layers(description));
            barrier.srcAccessMask = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                        ? 0U
                                        : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(command,
                                 current_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                              : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkClearColorValue clear{};
            for (std::size_t index = 0; index < 4U; ++index)
                clear.float32[index] = request.clear_color[index];
            VkImageSubresourceRange target_range{};
            target_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            target_range.baseMipLevel = request.mip_level;
            target_range.levelCount = 1;
            target_range.baseArrayLayer = request.array_layer;
            target_range.layerCount = 1;
            vkCmdClearColorImage(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                 &target_range);

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = request.mip_level;
            copy.imageSubresource.baseArrayLayer = request.array_layer;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {request.output_width, request.output_height, 1};
            vkCmdCopyImageToBuffer(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   staging.buffer, 1, &copy);

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = texture_final_layout(
                description.usage, description.access_policy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);
            result = vkEndCommandBuffer(command);
        }
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain == VK_SUCCESS) {
            current_layout = texture_final_layout(
                description.usage, description.access_policy);
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(
            description.usage, description.access_policy);
    }
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan texture clear/readback", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "texture_readback_failed";
        return false;
    }

    void* mapped = nullptr;
    result = vkMapMemory(context->device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkMapMemory(texture readback)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "texture_readback_failed";
        return false;
    }
    if ((staging.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = staging.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(context->device, 1, &range);
    }
    if (result == VK_SUCCESS) {
        try {
            output.resize(output_size);
            std::memcpy(output.data(), mapped, output_size);
        } catch (const std::bad_alloc&) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    vkUnmapMemory(context->device, staging.memory);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan texture readback map", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "texture_readback_failed";
        output.clear();
        return false;
    }
    if (description.format == TextureFormat::bgra8_unorm || description.format == TextureFormat::bgra8_srgb) {
        for (std::size_t index = 0; index < output.size(); index += 4U)
            std::swap(output[index], output[index + 2U]);
    }
    return true;
}

bool read_hdr_luminance(const std::shared_ptr<VulkanContext>& context,
                        VkImage image,
                        const TextureDescription& description,
                        VkImageLayout& current_layout,
                        float& luminance,
                        Diagnostic& diagnostic) {
    luminance = 0.0F;
    constexpr std::size_t readback_size = sizeof(std::uint16_t) * 4U;
    BufferDescription staging_description;
    staging_description.size_bytes = readback_size;
    staging_description.usage = BufferUsage::transfer_destination;
    staging_description.memory = BufferMemory::host_visible;
    RawVulkanBuffer staging;
    if (!create_raw_buffer(context, staging_description,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           BufferMemory::host_visible, staging, diagnostic)) {
        diagnostic.code = "vulkan_hdr_luminance_readback_failed";
        return false;
    }

    std::lock_guard command_guard(context->command_mutex);
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1U;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context->device, &allocation,
                                               &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
    }
    if (result == VK_SUCCESS) {
        const VkImageLayout final_layout = texture_final_layout(
            description.usage, description.access_policy);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = current_layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = description.mip_levels - 1U;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.baseArrayLayer = 0U;
        barrier.subresourceRange.layerCount = 1U;
        barrier.srcAccessMask = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                    ? 0U
                                    : VK_ACCESS_MEMORY_READ_BIT |
                                          VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            command,
            current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
            &barrier);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = description.mip_levels - 1U;
        copy.imageSubresource.baseArrayLayer = 0U;
        copy.imageSubresource.layerCount = 1U;
        copy.imageExtent = {1U, 1U, 1U};
        vkCmdCopyImageToBuffer(command, image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1U, &copy);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = final_layout;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : VK_ACCESS_MEMORY_READ_BIT |
                                          VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
        result = vkEndCommandBuffer(command);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1U, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1U, &fence,
                                         VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain == VK_SUCCESS) {
            current_layout = texture_final_layout(
                description.usage, description.access_policy);
            completed = true;
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(
            description.usage, description.access_policy);
    }
    if (fence != VK_NULL_HANDLE)
        vkDestroyFence(context->device, fence, nullptr);
    if (command != VK_NULL_HANDLE)
        vkFreeCommandBuffers(context->device, context->command_pool, 1U,
                             &command);
    if (result != VK_SUCCESS || !completed) {
        diagnostic = vk_error("Vulkan HDR luminance readback", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_hdr_luminance_readback_failed";
        return false;
    }

    void* mapped = nullptr;
    result = vkMapMemory(context->device, staging.memory, 0U, VK_WHOLE_SIZE,
                         0U, &mapped);
    if (result == VK_SUCCESS &&
        (staging.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = staging.memory;
        range.offset = 0U;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(context->device, 1U, &range);
    }
    std::array<std::uint16_t, 4U> channels{};
    if (result == VK_SUCCESS)
        std::memcpy(channels.data(), mapped, readback_size);
    if (mapped != nullptr)
        vkUnmapMemory(context->device, staging.memory);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkMapMemory(HDR luminance readback)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_hdr_luminance_readback_failed";
        return false;
    }
    const float red = detail::half_to_float(channels[0U]);
    const float green = detail::half_to_float(channels[1U]);
    const float blue = detail::half_to_float(channels[2U]);
    luminance = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
    // Keep this behavior aligned with ksEditorAutoExposure and the WebGL
    // path: invalid GPU values are treated as zero, which then selects the
    // configured maximum exposure instead of propagating NaN into tone map
    // constants.
    if (!std::isfinite(luminance)) luminance = 0.0F;
    diagnostic = {};
    return true;
}

VkFormat vk_pipeline_vertex_format(PipelineVertexAttributeFormat format) {
    switch (format) {
    case PipelineVertexAttributeFormat::float32: return VK_FORMAT_R32_SFLOAT;
    case PipelineVertexAttributeFormat::float32x2: return VK_FORMAT_R32G32_SFLOAT;
    case PipelineVertexAttributeFormat::float32x3: return VK_FORMAT_R32G32B32_SFLOAT;
    case PipelineVertexAttributeFormat::float32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case PipelineVertexAttributeFormat::uint32x4: return VK_FORMAT_R32G32B32A32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkBlendFactor vk_pipeline_blend_factor(PipelineBlendFactor factor) noexcept {
    switch (factor) {
    case PipelineBlendFactor::zero: return VK_BLEND_FACTOR_ZERO;
    case PipelineBlendFactor::one: return VK_BLEND_FACTOR_ONE;
    case PipelineBlendFactor::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case PipelineBlendFactor::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case PipelineBlendFactor::destination_color: return VK_BLEND_FACTOR_DST_COLOR;
    case PipelineBlendFactor::destination_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case PipelineBlendFactor::one_minus_destination_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ZERO;
}

VkBlendOp vk_pipeline_blend_operation(PipelineBlendOperation operation) noexcept {
    switch (operation) {
    case PipelineBlendOperation::add: return VK_BLEND_OP_ADD;
    case PipelineBlendOperation::subtract: return VK_BLEND_OP_SUBTRACT;
    case PipelineBlendOperation::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    }
    return VK_BLEND_OP_ADD;
}

[[nodiscard]] VkCompareOp vk_pipeline_compare(
    PipelineCompareOperation compare) noexcept {
    switch (compare) {
    case PipelineCompareOperation::never: return VK_COMPARE_OP_NEVER;
    case PipelineCompareOperation::less: return VK_COMPARE_OP_LESS;
    case PipelineCompareOperation::equal: return VK_COMPARE_OP_EQUAL;
    case PipelineCompareOperation::less_or_equal:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case PipelineCompareOperation::greater: return VK_COMPARE_OP_GREATER;
    case PipelineCompareOperation::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
    case PipelineCompareOperation::greater_or_equal:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case PipelineCompareOperation::always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkShaderStageFlagBits vk_pipeline_stage(PipelineShaderStage stage) {
    return stage == PipelineShaderStage::vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
}

VkPrimitiveTopology vk_pipeline_topology(PipelineFillMode fill) noexcept {
    return fill == PipelineFillMode::wireframe
               ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

struct VulkanDrawGeometry {
    const RawVulkanBuffer& vertices;
    VkDeviceSize vertex_offset = 0U;
    const RawVulkanBuffer* indices = nullptr;
    VkDeviceSize index_offset = 0U;
    std::uint32_t vertex_count = 0U;
    std::uint32_t index_count = 0U;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
};

class VulkanTexture;
class VulkanSampler;

bool extract_vulkan_sampler(const Sampler* sampler,
                            const std::shared_ptr<VulkanContext>& context,
                            VkSampler& raw_sampler,
                            Diagnostic& diagnostic);

struct VulkanIndexedBatchDraw {
    const PipelineProgram* program = nullptr;
    const VulkanBuffer* vertices = nullptr;
    const VulkanBuffer* indices = nullptr;
    VkDeviceSize vertex_offset = 0U;
    VkDeviceSize index_offset = 0U;
    std::uint32_t vertex_count = 0U;
    std::uint32_t index_count = 0U;
    bool indexed = true;
    // Derived from the packet/pipeline blend classification. Grass is placed
    // immediately before the first transparent scene draw, preserving the
    // ordered scene contract while leaving selected/overlay insertion intact.
    bool transparent = false;
    DrawMatrices matrices{};
    bool has_selected_color = false;
    VkBuffer selected_color_buffer = VK_NULL_HANDLE;
    VkDeviceSize selected_color_offset = 0U;
    VkDeviceSize selected_color_range = 0U;
    bool has_sampled_binding = false;
    VkImage sampled_image = VK_NULL_HANDLE;
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkSampler sampled_sampler = VK_NULL_HANDLE;
    bool has_normal_binding = false;
    VkImage normal_image = VK_NULL_HANDLE;
    VkImageView normal_view = VK_NULL_HANDLE;
    VkSampler normal_sampler = VK_NULL_HANDLE;
    bool has_maps_binding = false;
    VkImage maps_image = VK_NULL_HANDLE;
    VkImageView maps_view = VK_NULL_HANDLE;
    VkSampler maps_sampler = VK_NULL_HANDLE;
    bool has_detail_binding = false;
    VkImage detail_image = VK_NULL_HANDLE;
    VkImageView detail_view = VK_NULL_HANDLE;
    VkSampler detail_sampler = VK_NULL_HANDLE;
    bool has_normal_detail_binding = false;
    VkImage normal_detail_image = VK_NULL_HANDLE;
    VkImageView normal_detail_view = VK_NULL_HANDLE;
    VkSampler normal_detail_sampler = VK_NULL_HANDLE;
    bool has_damage_binding = false;
    VkImage damage_image = VK_NULL_HANDLE;
    VkImageView damage_view = VK_NULL_HANDLE;
    VkSampler damage_sampler = VK_NULL_HANDLE;
    bool has_damage_mask_binding = false;
    VkImage damage_mask_image = VK_NULL_HANDLE;
    VkImageView damage_mask_view = VK_NULL_HANDLE;
    VkSampler damage_mask_sampler = VK_NULL_HANDLE;
    bool has_material_binding = false;
    VkBuffer material_buffer = VK_NULL_HANDLE;
    VkDeviceSize material_offset = 0U;
    VkDeviceSize material_range = 0U;
    bool has_frame_binding = false;
    VkBuffer frame_buffer = VK_NULL_HANDLE;
    VkDeviceSize frame_offset = 0U;
    VkDeviceSize frame_range = 0U;
    bool has_directional_shadow_binding = false;
    std::array<VulkanDepthAttachment*, indexed_directional_shadow_cascade_count>
        shadow_attachments{};
    std::array<VkImageView, indexed_directional_shadow_cascade_count> shadow_views{};
    VkSampler shadow_sampler = VK_NULL_HANDLE;
    VkBuffer shadow_constants_buffer = VK_NULL_HANDLE;
    VkDeviceSize shadow_constants_offset = 0U;
    VkDeviceSize shadow_constants_range = 0U;
    bool has_multimap_reflection_binding = false;
    VkImage multimap_cube_image = VK_NULL_HANDLE;
    VkImageView multimap_cube_view = VK_NULL_HANDLE;
    VkSampler multimap_cube_sampler = VK_NULL_HANDLE;
    VkBuffer multimap_reflection_buffer = VK_NULL_HANDLE;
    VkDeviceSize multimap_reflection_offset = 0U;
    VkDeviceSize multimap_reflection_range = 0U;
    bool has_alpha_tested_binding = false;
    VkImage alpha_image = VK_NULL_HANDLE;
    VkImageView alpha_view = VK_NULL_HANDLE;
    VkSampler alpha_sampler = VK_NULL_HANDLE;
    VkBuffer alpha_material_buffer = VK_NULL_HANDLE;
    VkDeviceSize alpha_material_offset = 0U;
    VkDeviceSize alpha_material_range = 0U;
    std::uint32_t alpha_mip_levels = 0U;
    const VkImageLayout* alpha_layout = nullptr;
    bool has_stock_ks_per_pixel_vulkan_native_abi = false;
    std::array<VkBuffer, 5U> stock_native_abi_uniform_buffers{};
    std::array<VkDeviceSize, 5U> stock_native_abi_uniform_offsets{};
    std::array<VkDeviceSize, 5U> stock_native_abi_uniform_ranges{};
    VkImageView stock_native_abi_diffuse_view = VK_NULL_HANDLE;
    VkSampler stock_native_abi_linear_sampler = VK_NULL_HANDLE;
    std::array<VulkanDepthAttachment*, 3U> stock_native_abi_shadow_attachments{};
    std::array<VkImageView, 3U> stock_native_abi_shadow_views{};
    VkSampler stock_native_abi_shadow_sampler = VK_NULL_HANDLE;
};

// The portable resource ABI uses one sampled image, one sampler, and an
// optional material/frame constants buffers. Descriptor objects are transient because indexed
// batches are synchronous and the backend waits for the submitted fence before
// returning.
struct VulkanTransientSampledDescriptors {
    std::shared_ptr<VulkanContext> context;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool includes_material_buffer = false;
    bool includes_frame_buffer = false;
    bool includes_normal_binding = false;
    bool includes_maps_binding = false;
    bool includes_detail_binding = false;
    bool includes_normal_detail_binding = false;
    bool includes_damage_binding = false;
    bool includes_damage_mask_binding = false;
    bool includes_directional_shadow_binding = false;
    bool includes_multimap_reflection_binding = false;
    bool includes_alpha_tested_binding = false;

    VulkanTransientSampledDescriptors() = default;
    VulkanTransientSampledDescriptors(const VulkanTransientSampledDescriptors&) = delete;
    VulkanTransientSampledDescriptors& operator=(const VulkanTransientSampledDescriptors&) = delete;
    VulkanTransientSampledDescriptors(VulkanTransientSampledDescriptors&& other) noexcept
        : context(std::move(other.context)), layout(std::exchange(other.layout, VK_NULL_HANDLE)),
          pool(std::exchange(other.pool, VK_NULL_HANDLE)),
          includes_material_buffer(std::exchange(other.includes_material_buffer, false)),
          includes_frame_buffer(std::exchange(other.includes_frame_buffer, false)),
          includes_normal_binding(std::exchange(other.includes_normal_binding, false)),
          includes_maps_binding(std::exchange(other.includes_maps_binding, false)),
          includes_detail_binding(std::exchange(other.includes_detail_binding, false)),
          includes_normal_detail_binding(std::exchange(other.includes_normal_detail_binding, false)),
          includes_damage_binding(std::exchange(other.includes_damage_binding, false)),
          includes_damage_mask_binding(std::exchange(other.includes_damage_mask_binding, false)),
          includes_directional_shadow_binding(
              std::exchange(other.includes_directional_shadow_binding, false)),
          includes_multimap_reflection_binding(
              std::exchange(other.includes_multimap_reflection_binding, false)),
          includes_alpha_tested_binding(
              std::exchange(other.includes_alpha_tested_binding, false)) {}
    VulkanTransientSampledDescriptors& operator=(VulkanTransientSampledDescriptors&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        layout = std::exchange(other.layout, VK_NULL_HANDLE);
        pool = std::exchange(other.pool, VK_NULL_HANDLE);
        includes_material_buffer = std::exchange(other.includes_material_buffer, false);
        includes_frame_buffer = std::exchange(other.includes_frame_buffer, false);
        includes_normal_binding = std::exchange(other.includes_normal_binding, false);
        includes_maps_binding = std::exchange(other.includes_maps_binding, false);
        includes_detail_binding = std::exchange(other.includes_detail_binding, false);
        includes_normal_detail_binding = std::exchange(other.includes_normal_detail_binding, false);
        includes_damage_binding = std::exchange(other.includes_damage_binding, false);
        includes_damage_mask_binding = std::exchange(other.includes_damage_mask_binding, false);
        includes_directional_shadow_binding =
            std::exchange(other.includes_directional_shadow_binding, false);
        includes_multimap_reflection_binding =
            std::exchange(other.includes_multimap_reflection_binding, false);
        includes_alpha_tested_binding =
            std::exchange(other.includes_alpha_tested_binding, false);
        return *this;
    }
    ~VulkanTransientSampledDescriptors() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(context->device, pool, nullptr);
            if (layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(context->device, layout, nullptr);
        }
        pool = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        includes_material_buffer = false;
        includes_frame_buffer = false;
        includes_normal_binding = false;
        includes_maps_binding = false;
        includes_detail_binding = false;
        includes_normal_detail_binding = false;
        includes_damage_binding = false;
        includes_damage_mask_binding = false;
        includes_directional_shadow_binding = false;
        includes_multimap_reflection_binding = false;
        includes_alpha_tested_binding = false;
        context.reset();
    }
};

// The selected pass has one fragment uniform at set 0/binding 0. Keep this
// layout separate from the stock material set, whose binding zero is a sampled
// image. Both descriptor lifetimes end after the synchronous batch fence.
struct VulkanTransientSelectedDescriptors {
    std::shared_ptr<VulkanContext> context;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    VulkanTransientSelectedDescriptors() = default;
    VulkanTransientSelectedDescriptors(
        const VulkanTransientSelectedDescriptors&) = delete;
    VulkanTransientSelectedDescriptors& operator=(
        const VulkanTransientSelectedDescriptors&) = delete;
    ~VulkanTransientSelectedDescriptors() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(context->device, pool, nullptr);
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(context->device, layout, nullptr);
        }
        pool = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        context.reset();
    }
};

// The recovered ksPerPixel Vulkan ABI uses three descriptor sets to preserve
// D3D's independent b/t/s namespaces. The layouts are shared by the batch,
// while each native-ABI draw owns a distinct set triplet. All objects are
// destroyed after the synchronous fence completes.
struct VulkanTransientStockNativeAbiDescriptors {
    std::shared_ptr<VulkanContext> context;
    std::array<VkDescriptorSetLayout, 3U> layouts{};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    std::vector<std::size_t> draw_set_offsets;

    VulkanTransientStockNativeAbiDescriptors() = default;
    VulkanTransientStockNativeAbiDescriptors(const VulkanTransientStockNativeAbiDescriptors&) = delete;
    VulkanTransientStockNativeAbiDescriptors& operator=(const VulkanTransientStockNativeAbiDescriptors&) = delete;
    ~VulkanTransientStockNativeAbiDescriptors() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(context->device, pool, nullptr);
            for (VkDescriptorSetLayout layout : layouts)
                if (layout != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(context->device, layout, nullptr);
        }
        layouts.fill(VK_NULL_HANDLE);
        pool = VK_NULL_HANDLE;
        sets.clear();
        draw_set_offsets.clear();
        context.reset();
    }

    const VkDescriptorSet* sets_for_draw(std::size_t draw_index) const noexcept {
        if (draw_index >= draw_set_offsets.size()) return nullptr;
        const std::size_t offset = draw_set_offsets[draw_index];
        if (offset == std::numeric_limits<std::size_t>::max() ||
            offset > sets.size() || sets.size() - offset < layouts.size())
            return nullptr;
        return sets.data() + offset;
    }
};

bool prepare_vulkan_sampled_binding(const IndexedStaticMeshDrawRequest& request,
                                    const std::shared_ptr<VulkanContext>& context,
                                    VulkanIndexedBatchDraw& draw,
                                    Diagnostic& diagnostic);
bool prepare_vulkan_stock_ks_per_pixel_native_abi_binding(
    const IndexedStaticMeshDrawRequest& request,
    const std::shared_ptr<VulkanContext>& context,
    VulkanIndexedBatchDraw& draw,
    Diagnostic& diagnostic);

struct VulkanBatchPipeline {
    std::shared_ptr<VulkanContext> context;
    std::vector<VkShaderModule> shader_modules;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VulkanBatchPipeline() = default;
    VulkanBatchPipeline(const VulkanBatchPipeline&) = delete;
    VulkanBatchPipeline& operator=(const VulkanBatchPipeline&) = delete;
    VulkanBatchPipeline(VulkanBatchPipeline&& other) noexcept
        : context(std::move(other.context)), shader_modules(std::move(other.shader_modules)),
          layout(std::exchange(other.layout, VK_NULL_HANDLE)), pipeline(std::exchange(other.pipeline, VK_NULL_HANDLE)) {}
    VulkanBatchPipeline& operator=(VulkanBatchPipeline&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        shader_modules = std::move(other.shader_modules);
        layout = std::exchange(other.layout, VK_NULL_HANDLE);
        pipeline = std::exchange(other.pipeline, VK_NULL_HANDLE);
        return *this;
    }
    ~VulkanBatchPipeline() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(context->device, pipeline, nullptr);
            if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(context->device, layout, nullptr);
            for (VkShaderModule module : shader_modules)
                if (module != VK_NULL_HANDLE) vkDestroyShaderModule(context->device, module, nullptr);
        }
        pipeline = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        shader_modules.clear();
        context.reset();
    }
};

// The portable sky is the first draw in an indexed scene batch. Its one
// fragment UBO is intentionally separate from material descriptors so a sky
// draw cannot accidentally inherit a material binding ABI.
struct VulkanTransientSkyDescriptors {
    std::shared_ptr<VulkanContext> context;
    RawVulkanBuffer constants;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    VulkanTransientSkyDescriptors() = default;
    VulkanTransientSkyDescriptors(const VulkanTransientSkyDescriptors&) = delete;
    VulkanTransientSkyDescriptors& operator=(const VulkanTransientSkyDescriptors&) = delete;
    ~VulkanTransientSkyDescriptors() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(context->device, pool, nullptr);
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(context->device, layout, nullptr);
        }
        pool = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        set = VK_NULL_HANDLE;
        constants.reset();
        context.reset();
    }
};

// Cloud descriptors are transient because each ordered texture run may name a
// different sampled image. The batch is synchronous, so one descriptor set per
// run is sufficient and avoids rewriting a set that earlier recorded draws
// still reference.
struct VulkanTransientCloudDescriptors {
    std::shared_ptr<VulkanContext> context;
    RawVulkanBuffer constants;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;

    VulkanTransientCloudDescriptors() = default;
    VulkanTransientCloudDescriptors(const VulkanTransientCloudDescriptors&) = delete;
    VulkanTransientCloudDescriptors& operator=(const VulkanTransientCloudDescriptors&) = delete;
    ~VulkanTransientCloudDescriptors() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(context->device, pool, nullptr);
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(context->device, layout, nullptr);
        }
        pool = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        sets.clear();
        constants.reset();
        context.reset();
    }
};

// Concrete Vulkan ownership is resolved after VulkanTexture/VulkanSampler are
// complete. The recording helper consumes only these raw handles and tracked
// layout pointers, so it remains valid above the concrete resource classes.
struct VulkanPortableCloudRun {
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout* tracked_layout = nullptr;
    VkImageLayout original_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t mip_levels = 1U;
};

struct VulkanPortableCloudResources {
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VulkanPortableCloudRun> runs;
};

// Grass is one contiguous expanded vertex stream and one sampled atlas. Keep
// the tracked atlas layout alongside the raw handles so the synchronous batch
// restores the caller's sampled-image state on both success and drain paths.
struct VulkanPortableGrassResources {
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    std::uint32_t vertex_count = 0U;
    VkImage atlas_image = VK_NULL_HANDLE;
    VkImageView atlas_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageLayout* tracked_layout = nullptr;
    VkImageLayout original_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t mip_levels = 1U;
};

struct VulkanTransientGrassDescriptors {
    std::shared_ptr<VulkanContext> context;
    RawVulkanBuffer constants;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    VulkanTransientGrassDescriptors() = default;
    VulkanTransientGrassDescriptors(const VulkanTransientGrassDescriptors&) = delete;
    VulkanTransientGrassDescriptors& operator=(const VulkanTransientGrassDescriptors&) = delete;
    ~VulkanTransientGrassDescriptors() { reset(); }

    void reset() noexcept {
        if (context && context->device != VK_NULL_HANDLE) {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(context->device, pool, nullptr);
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(context->device, layout, nullptr);
        }
        pool = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        set = VK_NULL_HANDLE;
        constants.reset();
        context.reset();
    }
};

bool create_portable_cloud_descriptors(
    const std::shared_ptr<VulkanContext>& context,
    const PortableCloudShaderConstants& cloud_constants,
    const VulkanPortableCloudResources& resources,
    VulkanTransientCloudDescriptors& descriptors,
    Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    BufferDescription buffer_description;
    buffer_description.size_bytes = sizeof(PortableCloudShaderConstants);
    buffer_description.usage = BufferUsage::uniform;
    buffer_description.memory = BufferMemory::host_visible;
    buffer_description.mutability = BufferMutability::mutable_data;
    if (!create_raw_buffer(context, buffer_description,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           BufferMemory::host_visible, descriptors.constants,
                           diagnostic)) {
        diagnostic.code = diagnostic.code.empty()
                              ? "vulkan_portable_cloud_uniform_failed"
                              : diagnostic.code;
        descriptors.reset();
        return false;
    }
    if (!write_host_buffer(
            context, descriptors.constants, 0U,
            std::as_bytes(std::span(&cloud_constants, 1U)), diagnostic)) {
        diagnostic.code = diagnostic.code.empty()
                              ? "vulkan_portable_cloud_uniform_failed"
                              : diagnostic.code;
        descriptors.reset();
        return false;
    }
    std::array<VkDescriptorSetLayoutBinding, 2U> bindings{};
    bindings[0] = {0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    bindings[1] = {1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(
        context->device, &layout_info, nullptr, &descriptors.layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorSetLayout(portable clouds)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_cloud_descriptor_failed";
        descriptors.reset();
        return false;
    }
    std::array<VkDescriptorPoolSize, 2U> pool_sizes{};
    // Each set has its own uniform-buffer descriptor, even though all sets
    // point at the same transient allocation.
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     static_cast<std::uint32_t>(resources.runs.size())};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     static_cast<std::uint32_t>(resources.runs.size())};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = static_cast<std::uint32_t>(resources.runs.size());
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    result = vkCreateDescriptorPool(context->device, &pool_info, nullptr,
                                    &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(portable clouds)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_cloud_descriptor_failed";
        descriptors.reset();
        return false;
    }
    descriptors.sets.resize(resources.runs.size(), VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(resources.runs.size(), descriptors.layout);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    allocation.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(context->device, &allocation,
                                      descriptors.sets.data());
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(portable clouds)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_cloud_descriptor_failed";
        descriptors.reset();
        return false;
    }
    VkDescriptorBufferInfo buffer_info{descriptors.constants.buffer, 0U,
                                       sizeof(PortableCloudShaderConstants)};
    for (std::size_t index = 0U; index < resources.runs.size(); ++index) {
        const VulkanPortableCloudRun& run = resources.runs[index];
        VkDescriptorImageInfo image_info{resources.sampler, run.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2U> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptors.sets[index], 0U, 0U, 1U,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffer_info,
                     nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptors.sets[index], 1U, 0U, 1U,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info,
                     nullptr, nullptr};
        vkUpdateDescriptorSets(context->device,
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0U, nullptr);
    }
    return true;
}

bool create_portable_grass_descriptors(
    const std::shared_ptr<VulkanContext>& context,
    const PortableGrassShaderConstants& grass_constants,
    const VulkanPortableGrassResources& resources,
    VulkanTransientGrassDescriptors& descriptors,
    Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    if (sizeof(PortableGrassShaderConstants) > properties.limits.maxUniformBufferRange) {
        diagnostic = {"vulkan_portable_grass_uniform_limit",
                      "Portable grass constants exceed the Vulkan uniform-buffer range"};
        descriptors.reset();
        return false;
    }
    BufferDescription buffer_description;
    buffer_description.size_bytes = sizeof(PortableGrassShaderConstants);
    buffer_description.usage = BufferUsage::uniform;
    buffer_description.memory = BufferMemory::host_visible;
    buffer_description.mutability = BufferMutability::mutable_data;
    if (!create_raw_buffer(context, buffer_description,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           BufferMemory::host_visible, descriptors.constants,
                           diagnostic) ||
        !write_host_buffer(context, descriptors.constants, 0U,
                           std::as_bytes(std::span(&grass_constants, 1U)),
                           diagnostic)) {
        diagnostic.code = diagnostic.code.empty()
                              ? "vulkan_portable_grass_uniform_failed"
                              : diagnostic.code;
        descriptors.reset();
        return false;
    }
    std::array<VkDescriptorSetLayoutBinding, 3U> bindings{};
    bindings[0] = {0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    bindings[1] = {1U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2U, VK_DESCRIPTOR_TYPE_SAMPLER, 1U,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(
        context->device, &layout_info, nullptr, &descriptors.layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorSetLayout(portable grass)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_grass_descriptor_failed";
        descriptors.reset();
        return false;
    }
    std::array<VkDescriptorPoolSize, 3U> pool_sizes{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U};
    pool_sizes[2] = {VK_DESCRIPTOR_TYPE_SAMPLER, 1U};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1U;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    result = vkCreateDescriptorPool(context->device, &pool_info, nullptr,
                                    &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(portable grass)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_grass_descriptor_failed";
        descriptors.reset();
        return false;
    }
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = 1U;
    allocation.pSetLayouts = &descriptors.layout;
    result = vkAllocateDescriptorSets(context->device, &allocation,
                                      &descriptors.set);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(portable grass)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_grass_descriptor_failed";
        descriptors.reset();
        return false;
    }
    VkDescriptorBufferInfo buffer_info{descriptors.constants.buffer, 0U,
                                       sizeof(PortableGrassShaderConstants)};
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE, resources.atlas_view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler_info{resources.sampler, VK_NULL_HANDLE,
                                       VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkWriteDescriptorSet, 3U> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descriptors.set, 0U, 0U, 1U,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffer_info,
                 nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descriptors.set, 1U, 0U, 1U,
                 VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_info,
                 nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descriptors.set, 2U, 0U, 1U,
                 VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_info,
                 nullptr, nullptr};
    vkUpdateDescriptorSets(context->device,
                           static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0U, nullptr);
    return true;
}

bool create_portable_sky_descriptors(
    const std::shared_ptr<VulkanContext>& context,
    const PortableSkyShaderConstants& sky_constants,
    VulkanTransientSkyDescriptors& descriptors,
    Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    BufferDescription buffer_description;
    buffer_description.size_bytes = sizeof(PortableSkyShaderConstants);
    buffer_description.usage = BufferUsage::uniform;
    buffer_description.memory = BufferMemory::host_visible;
    buffer_description.mutability = BufferMutability::mutable_data;
    if (!create_raw_buffer(context, buffer_description,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           BufferMemory::host_visible, descriptors.constants,
                           diagnostic)) {
        diagnostic.code = diagnostic.code.empty()
                              ? "vulkan_portable_sky_uniform_failed"
                              : diagnostic.code;
        descriptors.reset();
        return false;
    }
    if (!write_host_buffer(
            context, descriptors.constants, 0U,
            std::as_bytes(std::span(&sky_constants, 1U)), diagnostic)) {
        diagnostic.code = diagnostic.code.empty()
                              ? "vulkan_portable_sky_uniform_failed"
                              : diagnostic.code;
        descriptors.reset();
        return false;
    }
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0U;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1U;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1U;
    layout_info.pBindings = &binding;
    VkResult result = vkCreateDescriptorSetLayout(
        context->device, &layout_info, nullptr, &descriptors.layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorSetLayout(portable sky)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_sky_descriptor_failed";
        descriptors.reset();
        return false;
    }
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount = 1U;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1U;
    pool_info.poolSizeCount = 1U;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(context->device, &pool_info, nullptr,
                                    &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(portable sky)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_sky_descriptor_failed";
        descriptors.reset();
        return false;
    }
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = 1U;
    allocation.pSetLayouts = &descriptors.layout;
    result = vkAllocateDescriptorSets(context->device, &allocation,
                                      &descriptors.set);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(portable sky)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_portable_sky_descriptor_failed";
        descriptors.reset();
        return false;
    }
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = descriptors.constants.buffer;
    buffer_info.offset = 0U;
    buffer_info.range = sizeof(PortableSkyShaderConstants);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptors.set;
    write.dstBinding = 0U;
    write.descriptorCount = 1U;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(context->device, 1U, &write, 0U, nullptr);
    return true;
}

bool create_stock_native_abi_descriptors(
    const std::shared_ptr<VulkanContext>& context,
    std::span<const VulkanIndexedBatchDraw> draws,
    VulkanTransientStockNativeAbiDescriptors& descriptors,
    Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    const std::size_t native_draw_count = static_cast<std::size_t>(std::count_if(
        draws.begin(), draws.end(), [](const VulkanIndexedBatchDraw& draw) {
            return draw.has_stock_ks_per_pixel_vulkan_native_abi;
        }));
    if (native_draw_count == 0U) return true;
    if (native_draw_count > std::numeric_limits<std::uint32_t>::max() / 5U) {
        diagnostic = {"vulkan_stock_native_abi_descriptor_batch_limit",
                      "The Vulkan ksPerPixel native-ABI batch is too large for descriptor allocation"};
        descriptors.reset();
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    if (properties.limits.maxBoundDescriptorSets < 3U ||
        properties.limits.maxDescriptorSetUniformBuffers < 5U ||
        properties.limits.maxPerStageDescriptorUniformBuffers < 4U ||
        properties.limits.maxDescriptorSetSampledImages < 4U ||
        properties.limits.maxPerStageDescriptorSampledImages < 4U ||
        properties.limits.maxDescriptorSetSamplers < 2U ||
        properties.limits.maxPerStageDescriptorSamplers < 2U) {
        diagnostic = {"vulkan_stock_native_abi_descriptor_limit_unsupported",
                      "The selected Vulkan device cannot bind the recovered three-set descriptor manifest"};
        descriptors.reset();
        return false;
    }
    constexpr std::array<VkDescriptorSetLayoutBinding, 5U> uniform_bindings = {{
        {0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    constexpr std::array<VkDescriptorSetLayoutBinding, 4U> image_bindings = {{
        {0U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {8U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    constexpr std::array<VkDescriptorSetLayoutBinding, 2U> sampler_bindings = {{
        {0U, VK_DESCRIPTOR_TYPE_SAMPLER, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1U, VK_DESCRIPTOR_TYPE_SAMPLER, 1U, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    const std::array<std::span<const VkDescriptorSetLayoutBinding>, 3U> bindings = {
        uniform_bindings, image_bindings, sampler_bindings};
    for (std::size_t set = 0U; set < descriptors.layouts.size(); ++set) {
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings[set].size());
        layout_info.pBindings = bindings[set].data();
        const VkResult result = vkCreateDescriptorSetLayout(
            context->device, &layout_info, nullptr, &descriptors.layouts[set]);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkCreateDescriptorSetLayout(stock native ABI)", result);
            diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                               : "vulkan_descriptor_layout_failed";
            descriptors.reset();
            return false;
        }
    }
    const std::uint32_t native_draw_count_u32 =
        static_cast<std::uint32_t>(native_draw_count);
    const std::array<VkDescriptorPoolSize, 3U> pool_sizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5U * native_draw_count_u32},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4U * native_draw_count_u32},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2U * native_draw_count_u32}}};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 3U * native_draw_count_u32;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    VkResult result = vkCreateDescriptorPool(context->device, &pool_info, nullptr,
                                              &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(stock native ABI)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                           : "vulkan_descriptor_pool_failed";
        descriptors.reset();
        return false;
    }
    std::vector<VkDescriptorSetLayout> allocation_layouts;
    allocation_layouts.reserve(native_draw_count * descriptors.layouts.size());
    for (std::size_t draw_index = 0U; draw_index < native_draw_count; ++draw_index)
        allocation_layouts.insert(allocation_layouts.end(), descriptors.layouts.begin(),
                                  descriptors.layouts.end());
    descriptors.sets.resize(allocation_layouts.size(), VK_NULL_HANDLE);
    descriptors.draw_set_offsets.assign(
        draws.size(), std::numeric_limits<std::size_t>::max());
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = static_cast<std::uint32_t>(allocation_layouts.size());
    allocation.pSetLayouts = allocation_layouts.data();
    result = vkAllocateDescriptorSets(context->device, &allocation, descriptors.sets.data());
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(stock native ABI)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                           : "vulkan_descriptor_allocation_failed";
        descriptors.reset();
        return false;
    }
    const std::array<std::uint32_t, 3U> shadow_bindings = {6U, 7U, 8U};
    std::size_t native_draw_index = 0U;
    for (std::size_t draw_index = 0U; draw_index < draws.size(); ++draw_index) {
        const VulkanIndexedBatchDraw& draw = draws[draw_index];
        if (!draw.has_stock_ks_per_pixel_vulkan_native_abi) continue;
        const std::size_t set_offset = native_draw_index * descriptors.layouts.size();
        descriptors.draw_set_offsets[draw_index] = set_offset;
        const VkDescriptorSet* draw_sets = descriptors.sets.data() + set_offset;
        std::array<VkDescriptorBufferInfo, 5U> uniform_infos{};
        std::array<VkDescriptorImageInfo, 3U> shadow_infos{};
        VkDescriptorImageInfo diffuse_info{};
        VkDescriptorImageInfo linear_info{};
        VkDescriptorImageInfo sampler_info{};
        for (std::size_t index = 0U; index < uniform_infos.size(); ++index) {
            uniform_infos[index] = {draw.stock_native_abi_uniform_buffers[index],
                                    draw.stock_native_abi_uniform_offsets[index],
                                    draw.stock_native_abi_uniform_ranges[index]};
        }
        diffuse_info.imageView = draw.stock_native_abi_diffuse_view;
        diffuse_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        linear_info.sampler = draw.stock_native_abi_linear_sampler;
        sampler_info.sampler = draw.stock_native_abi_shadow_sampler;
        for (std::size_t index = 0U; index < shadow_infos.size(); ++index) {
            shadow_infos[index].imageView = draw.stock_native_abi_shadow_views[index];
            shadow_infos[index].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        std::array<VkWriteDescriptorSet, 11U> writes{};
        std::size_t write_count = 0U;
        for (std::size_t index = 0U; index < uniform_infos.size(); ++index) {
            writes[write_count] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, draw_sets[0],
                static_cast<std::uint32_t>(index), 0U, 1U,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniform_infos[index], nullptr};
            ++write_count;
        }
        writes[write_count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                               draw_sets[1], 0U, 0U, 1U,
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &diffuse_info, nullptr, nullptr};
        ++write_count;
        for (std::size_t index = 0U; index < shadow_infos.size(); ++index) {
            writes[write_count] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, draw_sets[1],
                shadow_bindings[index], 0U, 1U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                &shadow_infos[index], nullptr, nullptr};
            ++write_count;
        }
        writes[write_count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                               draw_sets[2], 0U, 0U, 1U,
                               VK_DESCRIPTOR_TYPE_SAMPLER, &linear_info, nullptr, nullptr};
        ++write_count;
        writes[write_count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                               draw_sets[2], 1U, 0U, 1U,
                               VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_info, nullptr, nullptr};
        ++write_count;
        vkUpdateDescriptorSets(context->device, static_cast<std::uint32_t>(write_count),
                               writes.data(), 0U, nullptr);
        ++native_draw_index;
    }
    return true;
}

bool create_batch_pipeline(const std::shared_ptr<VulkanContext>& context,
                           VkRenderPass render_pass,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t samples,
                           const PipelineProgram& program,
                           bool has_color,
                           bool has_depth,
                           std::span<const VkDescriptorSetLayout> descriptor_layouts,
                           bool use_draw_matrices,
                           VulkanBatchPipeline& output,
                           Diagnostic& diagnostic) {
    output.reset();
    output.context = context;
    std::vector<std::vector<std::uint32_t>> shader_words;
    shader_words.reserve(program.shaders.size());
    output.shader_modules.reserve(program.shaders.size());
    for (const PipelineShaderModule& shader : program.shaders) {
        if (shader.format != PipelineShaderFormat::spirv || shader.bytes.size() % sizeof(std::uint32_t) != 0U) {
            diagnostic = {"vulkan_draw_shader_format_unsupported",
                          "Vulkan draw execution requires aligned SPIR-V shaders"};
            output.reset();
            return false;
        }
        shader_words.emplace_back(shader.bytes.size() / sizeof(std::uint32_t));
        std::memcpy(shader_words.back().data(), shader.bytes.data(), shader.bytes.size());
        if (shader_words.back().empty() || shader_words.back()[0] != 0x07230203U) {
            diagnostic = {"vulkan_draw_shader_invalid", "Vulkan draw shader has an invalid SPIR-V signature"};
            output.reset();
            return false;
        }
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = shader.bytes.size();
        module_info.pCode = shader_words.back().data();
        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult module_result = vkCreateShaderModule(context->device, &module_info, nullptr, &module);
        if (module_result != VK_SUCCESS) {
            diagnostic = vk_error("vkCreateShaderModule(batch)", module_result);
            diagnostic.code = module_result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
            output.reset();
            return false;
        }
        output.shader_modules.push_back(module);
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPushConstantRange push_constant_range{};
    if (use_draw_matrices) {
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_constant_range.offset = 0U;
        push_constant_range.size = static_cast<std::uint32_t>(sizeof(DrawMatrices));
        layout_info.pushConstantRangeCount = 1U;
        layout_info.pPushConstantRanges = &push_constant_range;
    }
    if (!descriptor_layouts.empty()) {
        layout_info.setLayoutCount = static_cast<std::uint32_t>(descriptor_layouts.size());
        layout_info.pSetLayouts = descriptor_layouts.data();
    }
    VkResult result = vkCreatePipelineLayout(context->device, &layout_info, nullptr, &output.layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreatePipelineLayout(batch)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        output.reset();
        return false;
    }

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(program.shaders.size());
    for (std::size_t index = 0; index < program.shaders.size(); ++index) {
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = vk_pipeline_stage(program.shaders[index].stage);
        stage.module = output.shader_modules[index];
        stage.pName = "main";
        stages.push_back(stage);
    }
    const auto& attributes = program.vertex_layout.attributes;
    std::vector<VkVertexInputAttributeDescription> vertex_attributes;
    vertex_attributes.reserve(attributes.size());
    for (const PipelineVertexAttribute& attribute : attributes)
        vertex_attributes.push_back({attribute.location, 0U, vk_pipeline_vertex_format(attribute.format), attribute.offset});
    VkVertexInputBindingDescription vertex_binding{0U, program.vertex_layout.stride,
                                                   VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = attributes.empty() ? 0U : 1U;
    vertex_input.pVertexBindingDescriptions = attributes.empty() ? nullptr : &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = vk_pipeline_topology(program.raster.fill);
    VkViewport viewport{0.0F, 0.0F, static_cast<float>(width),
                        static_cast<float>(height), 0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1U;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1U;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = program.raster.cull == PipelineCullMode::none ? VK_CULL_MODE_NONE
                      : program.raster.cull == PipelineCullMode::front ? VK_CULL_MODE_FRONT_BIT
                                                                         : VK_CULL_MODE_BACK_BIT;
    raster.frontFace = program.raster.front_face == PipelineFrontFace::clockwise
                           ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    const VkSampleCountFlagBits sample_count = vk_sample_count(samples);
    if (sample_count == 0U) {
        diagnostic = {"vulkan_draw_samples_unsupported",
                      "Vulkan draws support only one or four samples in the bounded contract"};
        output.reset();
        return false;
    }
    multisample.rasterizationSamples = sample_count;
    multisample.alphaToCoverageEnable = program.blend.alpha_to_coverage ? VK_TRUE : VK_FALSE;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = program.blend.enabled ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = vk_pipeline_blend_factor(program.blend.source_color);
    blend.dstColorBlendFactor = vk_pipeline_blend_factor(program.blend.destination_color);
    blend.colorBlendOp = vk_pipeline_blend_operation(program.blend.color_operation);
    blend.srcAlphaBlendFactor = vk_pipeline_blend_factor(program.blend.source_alpha);
    blend.dstAlphaBlendFactor = vk_pipeline_blend_factor(program.blend.destination_alpha);
    blend.alphaBlendOp = vk_pipeline_blend_operation(program.blend.alpha_operation);
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = has_color ? 1U : 0U;
    color_blend.pAttachments = has_color ? &blend : nullptr;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = has_depth && program.depth.test_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = has_depth && program.depth.write_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = vk_pipeline_compare(program.depth.compare);
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDepthStencilState = has_depth ? &depth_stencil : nullptr;
    pipeline_info.layout = output.layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0U;
    result = vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr,
                                       &output.pipeline);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateGraphicsPipelines(batch)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        output.reset();
        return false;
    }
    return true;
}

bool create_sampled_descriptor_layout(const std::shared_ptr<VulkanContext>& context,
                                      VulkanTransientSampledDescriptors& descriptors,
                                      bool includes_material_buffer,
                                      bool includes_frame_buffer,
                                      bool includes_normal_binding,
                                      bool includes_maps_binding,
                                      bool includes_detail_binding,
                                      bool includes_normal_detail_binding,
                                      bool includes_damage_binding,
                                      bool includes_damage_mask_binding,
                                      bool includes_directional_shadow_binding,
                                      bool includes_multimap_reflection_binding,
                                      bool includes_alpha_tested_binding,
                                      Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    // The normal/maps ABIs are intentionally exact: bindings 4/5 and 6/7
    // are only meaningful when the material and frame records at 2/3 are
    // present. Keep descriptor accounting safe even if malformed input
    // reaches this backend before neutral validation.
    const bool includes_detail_stack = includes_detail_binding || includes_normal_detail_binding;
    const bool includes_damage_stack = includes_damage_binding || includes_damage_mask_binding;
    descriptors.includes_material_buffer = includes_material_buffer || includes_normal_binding || includes_maps_binding ||
                                           includes_detail_stack || includes_damage_stack;
    descriptors.includes_frame_buffer = includes_frame_buffer || includes_normal_binding || includes_maps_binding ||
                                       includes_detail_stack || includes_damage_stack;
    descriptors.includes_normal_binding = includes_normal_binding || includes_maps_binding ||
                                          includes_detail_stack || includes_damage_stack;
    descriptors.includes_maps_binding = includes_maps_binding || includes_detail_stack ||
                                        includes_damage_stack;
    descriptors.includes_detail_binding = includes_detail_stack;
    descriptors.includes_normal_detail_binding = includes_normal_detail_binding;
    descriptors.includes_damage_binding = includes_damage_binding;
    descriptors.includes_damage_mask_binding = includes_damage_mask_binding;
    descriptors.includes_directional_shadow_binding =
        includes_directional_shadow_binding;
    descriptors.includes_multimap_reflection_binding =
        includes_multimap_reflection_binding;
    descriptors.includes_alpha_tested_binding = includes_alpha_tested_binding;
    if (includes_alpha_tested_binding) {
        std::array<VkDescriptorSetLayoutBinding, 3> alpha_bindings{};
        alpha_bindings[0] = {0U, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U,
                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        alpha_bindings[1] = {3U, VK_DESCRIPTOR_TYPE_SAMPLER, 1U,
                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        alpha_bindings[2] = {4U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo alpha_layout_info{};
        alpha_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        alpha_layout_info.bindingCount = static_cast<std::uint32_t>(alpha_bindings.size());
        alpha_layout_info.pBindings = alpha_bindings.data();
        const VkResult alpha_result = vkCreateDescriptorSetLayout(
            context->device, &alpha_layout_info, nullptr, &descriptors.layout);
        if (alpha_result != VK_SUCCESS) {
            diagnostic = vk_error("vkCreateDescriptorSetLayout(depth alpha)", alpha_result);
            diagnostic.code = alpha_result == VK_ERROR_DEVICE_LOST
                                  ? "vulkan_device_lost"
                                  : "vulkan_descriptor_layout_failed";
            descriptors.reset();
            return false;
        }
        return true;
    }
    std::array<VkDescriptorSetLayoutBinding, 24> bindings{};
    bindings[0].binding = 0U;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1U;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1U;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1U;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2U;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1U;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3U;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1U;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = 4U;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[4].descriptorCount = 1U;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].binding = 5U;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[5].descriptorCount = 1U;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[6].binding = 6U;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[6].descriptorCount = 1U;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[7].binding = 7U;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[7].descriptorCount = 1U;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[8].binding = 8U;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[8].descriptorCount = 1U;
    bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[9].binding = 9U;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[9].descriptorCount = 1U;
    bindings[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[10].binding = 10U;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[10].descriptorCount = 1U;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[11].binding = 11U;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[11].descriptorCount = 1U;
    bindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[12] = bindings[8];
    bindings[12].binding = 12U;
    bindings[13] = bindings[9];
    bindings[13].binding = 13U;
    bindings[14] = bindings[10];
    bindings[14].binding = 14U;
    bindings[15] = bindings[11];
    bindings[15].binding = 15U;
    bindings[16] = bindings[0];
    bindings[16].binding = 16U;
    bindings[17] = bindings[16];
    bindings[17].binding = 17U;
    bindings[18] = bindings[16];
    bindings[18].binding = 18U;
    bindings[19] = bindings[1];
    bindings[19].binding = 19U;
    bindings[20] = bindings[3];
    bindings[20].binding = 20U;
    bindings[21] = bindings[0];
    bindings[21].binding = portable_multimap_cube_texture_binding;
    bindings[22] = bindings[1];
    bindings[22].binding = portable_multimap_cube_sampler_binding;
    bindings[23] = bindings[3];
    bindings[23].binding = portable_multimap_reflection_constants_binding;
    if (includes_damage_stack && !includes_detail_stack)
        std::copy(bindings.begin() + 12, bindings.begin() + 16,
                  bindings.begin() + 8);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    const std::uint32_t material_sampled_descriptor_count =
        (includes_damage_stack && includes_detail_stack) ? 7U
        : includes_damage_stack ? 5U : includes_normal_detail_binding ? 5U
        : includes_detail_stack ? 5U : includes_maps_binding ? 3U : includes_normal_binding ? 2U : 1U;
    const std::uint32_t sampled_image_descriptor_count =
        (includes_directional_shadow_binding
             ? 10U
             : material_sampled_descriptor_count) +
        (includes_multimap_reflection_binding ? 1U : 0U);
    const std::uint32_t sampler_descriptor_count =
        (includes_directional_shadow_binding
             ? 8U
             : material_sampled_descriptor_count) +
        (includes_multimap_reflection_binding ? 1U : 0U);
    const std::uint32_t uniform_descriptor_count =
        (includes_directional_shadow_binding
             ? 3U
             : descriptors.includes_frame_buffer
                   ? 2U
                   : descriptors.includes_material_buffer ? 1U : 0U) +
        (includes_multimap_reflection_binding ? 1U : 0U);
    if (sampled_image_descriptor_count > properties.limits.maxPerStageDescriptorSampledImages ||
        sampled_image_descriptor_count > properties.limits.maxDescriptorSetSampledImages ||
        sampler_descriptor_count > properties.limits.maxPerStageDescriptorSamplers ||
        sampler_descriptor_count > properties.limits.maxDescriptorSetSamplers ||
        uniform_descriptor_count > properties.limits.maxPerStageDescriptorUniformBuffers ||
        uniform_descriptor_count > properties.limits.maxDescriptorSetUniformBuffers) {
        diagnostic = {"vulkan_descriptor_layout_limit",
                      "The indexed material descriptor layout exceeds Vulkan device descriptor limits"};
        descriptors.reset();
        return false;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    const std::uint32_t base_binding_count =
        includes_directional_shadow_binding
            ? 21U
            : (includes_detail_stack && includes_damage_stack)
                  ? 16U
                  : (includes_detail_stack || includes_damage_stack)
                        ? 12U
                        : includes_maps_binding
                              ? 8U
                              : includes_normal_binding
                                    ? 6U
                                    : includes_frame_buffer
                                          ? 4U
                                          : includes_material_buffer ? 3U : 2U;
    if (includes_multimap_reflection_binding && base_binding_count != 21U)
        std::copy(bindings.begin() + 21U, bindings.end(),
                  bindings.begin() + base_binding_count);
    layout_info.bindingCount =
        base_binding_count + (includes_multimap_reflection_binding ? 3U : 0U);
    layout_info.pBindings = bindings.data();
    const VkResult result = vkCreateDescriptorSetLayout(context->device, &layout_info, nullptr,
                                                        &descriptors.layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorSetLayout(indexed resources)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_descriptor_layout_failed";
        descriptors.reset();
        return false;
    }
    return true;
}

bool create_selected_descriptor_layout(
    const std::shared_ptr<VulkanContext>& context,
    VulkanTransientSelectedDescriptors& descriptors,
    Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0U;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1U;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1U;
    layout_info.pBindings = &binding;
    const VkResult result = vkCreateDescriptorSetLayout(
        context->device, &layout_info, nullptr, &descriptors.layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorSetLayout(selected mesh)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_selected_descriptor_layout_failed";
        descriptors.reset();
        return false;
    }
    return true;
}

bool allocate_selected_descriptor_sets(
    const std::shared_ptr<VulkanContext>& context,
    VulkanTransientSelectedDescriptors& descriptors,
    std::span<const VulkanIndexedBatchDraw> draws,
    std::vector<VkDescriptorSet>& sets,
    Diagnostic& diagnostic) {
    const std::size_t descriptor_count = static_cast<std::size_t>(
        std::count_if(draws.begin(), draws.end(),
                      [](const VulkanIndexedBatchDraw& draw) {
                          return draw.has_selected_color;
                      }));
    if (descriptor_count == 0U) return true;
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount = static_cast<std::uint32_t>(descriptor_count);
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = static_cast<std::uint32_t>(descriptor_count);
    pool_info.poolSizeCount = 1U;
    pool_info.pPoolSizes = &pool_size;
    VkResult result = vkCreateDescriptorPool(
        context->device, &pool_info, nullptr, &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(selected mesh)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_selected_descriptor_pool_failed";
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(descriptor_count,
                                                descriptors.layout);
    std::vector<VkDescriptorSet> allocated(descriptor_count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = static_cast<std::uint32_t>(descriptor_count);
    allocation.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(context->device, &allocation,
                                      allocated.data());
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(selected mesh)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST
                              ? "vulkan_device_lost"
                              : "vulkan_selected_descriptor_allocation_failed";
        return false;
    }
    if (sets.empty()) {
        sets.assign(draws.size(), VK_NULL_HANDLE);
    } else if (sets.size() != draws.size()) {
        diagnostic = {"vulkan_selected_descriptor_index_invalid",
                      "Selected-mesh descriptor slots do not match the merged draw count"};
        return false;
    }
    std::size_t next = 0U;
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const VulkanIndexedBatchDraw& draw = draws[index];
        if (!draw.has_selected_color) continue;
        const VkDescriptorSet set = allocated[next++];
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = draw.selected_color_buffer;
        buffer_info.offset = draw.selected_color_offset;
        buffer_info.range = draw.selected_color_range;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0U;
        write.descriptorCount = 1U;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(context->device, 1U, &write, 0U, nullptr);
        sets[index] = set;
    }
    return true;
}

bool allocate_sampled_descriptor_sets(const std::shared_ptr<VulkanContext>& context,
                                      VulkanTransientSampledDescriptors& descriptors,
                                      std::span<const VulkanIndexedBatchDraw> draws,
                                      std::vector<VkDescriptorSet>& sets,
                                      Diagnostic& diagnostic) {
    std::size_t descriptor_count = 0U;
    for (const VulkanIndexedBatchDraw& draw : draws) {
        if (draw.has_sampled_binding || draw.has_normal_binding || draw.has_maps_binding ||
            draw.has_detail_binding || draw.has_normal_detail_binding ||
            draw.has_damage_binding || draw.has_damage_mask_binding ||
            draw.has_material_binding || draw.has_frame_binding ||
            draw.has_directional_shadow_binding ||
            draw.has_multimap_reflection_binding)
            ++descriptor_count;
    }
    if (descriptor_count == 0U) return true;
    if (descriptor_count > max_indexed_static_mesh_batch_draws ||
        descriptor_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed descriptor count exceeds the bounded batch limit"};
        return false;
    }
    // The layout always contains the diffuse bindings. Pool accounting must
    // reserve every descriptor in every allocated set, even when a particular
    // shader does not read one of the optional bindings.
    VkDescriptorPoolSize pool_sizes[3]{};
    const bool includes_damage_descriptors = descriptors.includes_damage_binding ||
                                             descriptors.includes_damage_mask_binding;
    const bool includes_detail_descriptors = descriptors.includes_detail_binding ||
                                             descriptors.includes_normal_detail_binding;
    const std::size_t material_sampled_descriptors_per_set =
        (includes_damage_descriptors && includes_detail_descriptors) ? 7U
                                                        : descriptors.includes_damage_mask_binding
                                                        ? 5U
                                                        : descriptors.includes_damage_binding ? 5U
                                                        : descriptors.includes_normal_detail_binding
                                                        ? 5U
                                                        : descriptors.includes_detail_binding ? 5U
                                                        : descriptors.includes_maps_binding ? 3U
                                                        : descriptors.includes_normal_binding ? 2U : 1U;
    const std::size_t sampled_images_per_set =
        (descriptors.includes_directional_shadow_binding
             ? 10U
             : material_sampled_descriptors_per_set) +
        (descriptors.includes_multimap_reflection_binding ? 1U : 0U);
    const std::size_t samplers_per_set =
        (descriptors.includes_directional_shadow_binding
             ? 8U
             : material_sampled_descriptors_per_set) +
        (descriptors.includes_multimap_reflection_binding ? 1U : 0U);
    if (descriptor_count > std::numeric_limits<std::size_t>::max() / sampled_images_per_set ||
        descriptor_count > std::numeric_limits<std::size_t>::max() / samplers_per_set) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed sampled-image descriptor count overflows host arithmetic"};
        return false;
    }
    const std::size_t sampled_image_descriptor_count =
        descriptor_count * sampled_images_per_set;
    const std::size_t sampler_descriptor_count = descriptor_count * samplers_per_set;
    if (sampled_image_descriptor_count >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        sampler_descriptor_count >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed sampled-image descriptor count exceeds Vulkan's bounded descriptor limit"};
        return false;
    }
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[0].descriptorCount =
        static_cast<std::uint32_t>(sampled_image_descriptor_count);
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[1].descriptorCount = static_cast<std::uint32_t>(sampler_descriptor_count);
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    // A binding-3 layout includes binding 2 as well, so frame-only batches
    // still consume two uniform descriptors per allocated set even though the
    // frame-only shader writes only binding 3.
    const std::size_t uniform_bindings_per_set =
        (descriptors.includes_directional_shadow_binding
             ? 3U
             : descriptors.includes_frame_buffer
                   ? 2U
                   : descriptors.includes_material_buffer ? 1U : 0U) +
        (descriptors.includes_multimap_reflection_binding ? 1U : 0U);
    if (uniform_bindings_per_set != 0U &&
        descriptor_count > std::numeric_limits<std::size_t>::max() / uniform_bindings_per_set) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed uniform descriptor count overflows host arithmetic"};
        return false;
    }
    const std::size_t uniform_descriptor_count = descriptor_count * uniform_bindings_per_set;
    if (uniform_descriptor_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed uniform descriptor count exceeds Vulkan's bounded descriptor limit"};
        return false;
    }
    pool_sizes[2].descriptorCount = static_cast<std::uint32_t>(uniform_descriptor_count);
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = static_cast<std::uint32_t>(descriptor_count);
    pool_info.poolSizeCount =
        (descriptors.includes_material_buffer || descriptors.includes_frame_buffer ||
         descriptors.includes_directional_shadow_binding ||
         descriptors.includes_multimap_reflection_binding)
            ? 3U : 2U;
    pool_info.pPoolSizes = pool_sizes;
    VkResult result = vkCreateDescriptorPool(context->device, &pool_info, nullptr, &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(diffuse)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_descriptor_pool_failed";
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(descriptor_count, descriptors.layout);
    std::vector<VkDescriptorSet> allocated(descriptor_count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = static_cast<std::uint32_t>(descriptor_count);
    allocation.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(context->device, &allocation, allocated.data());
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(diffuse)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_descriptor_allocation_failed";
        return false;
    }
    sets.assign(draws.size(), VK_NULL_HANDLE);
    std::size_t next = 0U;
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const VulkanIndexedBatchDraw& draw = draws[index];
        if (!draw.has_sampled_binding && !draw.has_normal_binding && !draw.has_maps_binding &&
            !draw.has_detail_binding && !draw.has_normal_detail_binding &&
            !draw.has_damage_binding && !draw.has_damage_mask_binding &&
            !draw.has_material_binding && !draw.has_frame_binding &&
            !draw.has_directional_shadow_binding &&
            !draw.has_multimap_reflection_binding)
            continue;
        const VkDescriptorSet set = allocated[next++];
        VkDescriptorImageInfo image_info{};
        image_info.imageView = draw.sampled_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo sampler_info{};
        sampler_info.sampler = draw.sampled_sampler;
        VkDescriptorImageInfo normal_image_info{};
        normal_image_info.imageView = draw.normal_view;
        normal_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo normal_sampler_info{};
        normal_sampler_info.sampler = draw.normal_sampler;
        VkDescriptorImageInfo maps_image_info{};
        maps_image_info.imageView = draw.maps_view;
        maps_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo maps_sampler_info{};
        maps_sampler_info.sampler = draw.maps_sampler;
        VkDescriptorImageInfo detail_image_info{};
        detail_image_info.imageView = draw.detail_view;
        detail_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo detail_sampler_info{};
        detail_sampler_info.sampler = draw.detail_sampler;
        VkDescriptorImageInfo normal_detail_image_info{};
        normal_detail_image_info.imageView = draw.normal_detail_view;
        normal_detail_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo normal_detail_sampler_info{};
        normal_detail_sampler_info.sampler = draw.normal_detail_sampler;
        VkDescriptorImageInfo damage_image_info{};
        damage_image_info.imageView = draw.damage_view;
        damage_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo damage_sampler_info{};
        damage_sampler_info.sampler = draw.damage_sampler;
        VkDescriptorImageInfo damage_mask_image_info{};
        damage_mask_image_info.imageView = draw.damage_mask_view;
        damage_mask_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo damage_mask_sampler_info{};
        damage_mask_sampler_info.sampler = draw.damage_mask_sampler;
        VkDescriptorBufferInfo material_info{};
        material_info.buffer = draw.material_buffer;
        material_info.offset = draw.material_offset;
        material_info.range = draw.material_range;
        VkDescriptorBufferInfo frame_info{};
        frame_info.buffer = draw.frame_buffer;
        frame_info.offset = draw.frame_offset;
        frame_info.range = draw.frame_range;
        std::array<VkDescriptorImageInfo,
                   indexed_directional_shadow_cascade_count> shadow_image_infos{};
        for (std::size_t cascade = 0U;
             cascade < indexed_directional_shadow_cascade_count; ++cascade) {
            shadow_image_infos[cascade].imageView = draw.shadow_views[cascade];
            shadow_image_infos[cascade].imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        VkDescriptorImageInfo shadow_sampler_info{};
        shadow_sampler_info.sampler = draw.shadow_sampler;
        VkDescriptorBufferInfo shadow_constants_info{};
        shadow_constants_info.buffer = draw.shadow_constants_buffer;
        shadow_constants_info.offset = draw.shadow_constants_offset;
        shadow_constants_info.range = draw.shadow_constants_range;
        VkDescriptorImageInfo multimap_cube_image_info{};
        multimap_cube_image_info.imageView = draw.multimap_cube_view;
        multimap_cube_image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo multimap_cube_sampler_info{};
        multimap_cube_sampler_info.sampler = draw.multimap_cube_sampler;
        VkDescriptorBufferInfo multimap_reflection_info{};
        multimap_reflection_info.buffer = draw.multimap_reflection_buffer;
        multimap_reflection_info.offset = draw.multimap_reflection_offset;
        multimap_reflection_info.range = draw.multimap_reflection_range;
        std::array<VkWriteDescriptorSet, 24> writes{};
        std::uint32_t write_count = 0U;
        if (draw.has_sampled_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 0U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 1U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &sampler_info;
            ++write_count;
        }
        if (draw.has_normal_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 4U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &normal_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 5U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &normal_sampler_info;
            ++write_count;
        }
        if (draw.has_maps_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 6U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &maps_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 7U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &maps_sampler_info;
            ++write_count;
        }
        if (draw.has_detail_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 8U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &detail_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 9U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &detail_sampler_info;
            ++write_count;
        }
        if (draw.has_normal_detail_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 10U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &normal_detail_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 11U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &normal_detail_sampler_info;
            ++write_count;
        }
        if (draw.has_damage_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 12U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &damage_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 13U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &damage_sampler_info;
            ++write_count;
        }
        if (draw.has_damage_mask_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 14U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &damage_mask_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 15U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &damage_mask_sampler_info;
            ++write_count;
        }
        if (draw.has_material_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 2U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[write_count].pBufferInfo = &material_info;
            ++write_count;
        }
        if (draw.has_frame_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 3U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[write_count].pBufferInfo = &frame_info;
            ++write_count;
        }
        if (draw.has_directional_shadow_binding) {
            for (std::uint32_t cascade = 0U;
                 cascade < indexed_directional_shadow_cascade_count; ++cascade) {
                writes[write_count].sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[write_count].dstSet = set;
                writes[write_count].dstBinding = 16U + cascade;
                writes[write_count].descriptorCount = 1U;
                writes[write_count].descriptorType =
                    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[write_count].pImageInfo = &shadow_image_infos[cascade];
                ++write_count;
            }
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 19U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &shadow_sampler_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding = 20U;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[write_count].pBufferInfo = &shadow_constants_info;
            ++write_count;
        }
        if (draw.has_multimap_reflection_binding) {
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding =
                portable_multimap_cube_texture_binding;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType =
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[write_count].pImageInfo = &multimap_cube_image_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding =
                portable_multimap_cube_sampler_binding;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[write_count].pImageInfo = &multimap_cube_sampler_info;
            ++write_count;
            writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[write_count].dstSet = set;
            writes[write_count].dstBinding =
                portable_multimap_reflection_constants_binding;
            writes[write_count].descriptorCount = 1U;
            writes[write_count].descriptorType =
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[write_count].pBufferInfo = &multimap_reflection_info;
            ++write_count;
        }
        vkUpdateDescriptorSets(context->device, write_count, writes.data(), 0U, nullptr);
        sets[index] = set;
    }
    return true;
}

bool allocate_depth_alpha_descriptor_sets(
    const std::shared_ptr<VulkanContext>& context,
    VulkanTransientSampledDescriptors& descriptors,
    std::span<const VulkanIndexedBatchDraw> draws,
    std::vector<VkDescriptorSet>& sets,
    Diagnostic& diagnostic) {
    std::size_t count = 0U;
    for (const VulkanIndexedBatchDraw& draw : draws)
        if (draw.has_alpha_tested_binding) ++count;
    if (count == 0U) {
        sets.assign(draws.size(), VK_NULL_HANDLE);
        return true;
    }
    if (count > max_indexed_static_mesh_batch_draws ||
        count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        diagnostic = {"vulkan_depth_alpha_descriptor_count_limit",
                      "The depth alpha descriptor count exceeds the bounded batch limit"};
        return false;
    }
    std::array<VkDescriptorPoolSize, 3> pool_sizes{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<std::uint32_t>(count)};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(count)};
    pool_sizes[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<std::uint32_t>(count)};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = static_cast<std::uint32_t>(count);
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    VkResult result = vkCreateDescriptorPool(context->device, &pool_info, nullptr, &descriptors.pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(depth alpha)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_descriptor_pool_failed";
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(count, descriptors.layout);
    std::vector<VkDescriptorSet> allocated(count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors.pool;
    allocation.descriptorSetCount = static_cast<std::uint32_t>(count);
    allocation.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(context->device, &allocation, allocated.data());
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(depth alpha)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_descriptor_allocation_failed";
        return false;
    }
    sets.assign(draws.size(), VK_NULL_HANDLE);
    std::size_t next = 0U;
    for (std::size_t index = 0U; index < draws.size(); ++index) {
        const VulkanIndexedBatchDraw& draw = draws[index];
        if (!draw.has_alpha_tested_binding) continue;
        const VkDescriptorSet set = allocated[next++];
        VkDescriptorImageInfo image{};
        image.imageView = draw.alpha_view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo sampler{};
        sampler.sampler = draw.alpha_sampler;
        VkDescriptorBufferInfo material{};
        material.buffer = draw.alpha_material_buffer;
        material.offset = draw.alpha_material_offset;
        material.range = draw.alpha_material_range;
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0U, 0U, 1U,
                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3U, 0U, 1U,
                     VK_DESCRIPTOR_TYPE_SAMPLER, &sampler, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 4U, 0U, 1U,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &material, nullptr};
        vkUpdateDescriptorSets(context->device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0U, nullptr);
        sets[index] = set;
    }
    return true;
}

bool draw_graphics_and_readback(const std::shared_ptr<VulkanContext>& context,
                                RawVulkanImage& image,
                                const TextureDescription& description,
                                const PipelineProgram& program,
                                const VulkanDrawGeometry& geometry,
                                const DrawMatrices* draw_matrices,
                                VulkanDepthAttachment* depth_attachment,
                                bool load_color,
                                bool clear_depth,
                                float depth_clear_value,
                                const std::array<float, 4>& clear_color,
                                VkImageLayout& current_layout,
                                std::vector<std::byte>& output,
                                Diagnostic& diagnostic) {
    std::lock_guard command_guard(context->command_mutex);
    const VkSampleCountFlagBits sample_count = vk_sample_count(description.samples);
    if (sample_count == 0U) {
        diagnostic = {"vulkan_draw_samples_unsupported",
                      "Vulkan draws support only one or four samples in the bounded contract"};
        return false;
    }
    if (depth_attachment != nullptr) {
        if (depth_attachment->context().get() != context.get()) {
            diagnostic = {"indexed_depth_attachment_context_mismatch",
                          "The depth attachment belongs to another Vulkan device"};
            return false;
        }
        if (depth_attachment->info().description.samples != description.samples) {
            diagnostic = {"vulkan_depth_samples_mismatch",
                          "Vulkan color and depth attachments must use the same sample count"};
            return false;
        }
        if (!clear_depth && !depth_attachment->initialized()) {
            diagnostic = {"indexed_depth_load_before_clear",
                          "A depth load was requested before the depth attachment was cleared"};
            return false;
        }
    }
    if (draw_matrices != nullptr) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(context->physical_device, &properties);
        if (properties.limits.maxPushConstantsSize < sizeof(DrawMatrices)) {
            diagnostic = {"vulkan_draw_transform_limit_unsupported",
                          "Vulkan device push-constant limit is smaller than the draw-matrices contract"};
            return false;
        }
    }
    const std::uint64_t output_size_u64 = static_cast<std::uint64_t>(description.width) *
                                          static_cast<std::uint64_t>(description.height) * 4U;
    if (output_size_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        diagnostic = {"vulkan_draw_output_size_limit", "Vulkan draw readback size exceeds host addressability"};
        return false;
    }
    const std::size_t output_size = static_cast<std::size_t>(output_size_u64);
    BufferDescription readback_description;
    readback_description.size_bytes = output_size;
    readback_description.usage = BufferUsage::transfer_destination;
    readback_description.memory = BufferMemory::host_visible;
    RawVulkanBuffer readback;
    if (!create_raw_buffer(context, readback_description, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           BufferMemory::host_visible, readback, diagnostic)) {
        diagnostic.code = "vulkan_draw_execution_failed";
        return false;
    }
    RawVulkanImage resolve_image;
    if (description.samples != 1U) {
        TextureDescription resolve_description = description;
        resolve_description.samples = 1U;
        resolve_description.usage = TextureUsage::color_attachment | TextureUsage::transfer_source;
        if (!create_raw_image(context, resolve_description, resolve_image, diagnostic)) {
            diagnostic.code = diagnostic.code.empty() ? "vulkan_resolve_attachment_failed" : diagnostic.code;
            return false;
        }
    }

    std::vector<VkShaderModule> shader_modules;
    std::vector<std::vector<std::uint32_t>> shader_words;
    shader_modules.reserve(program.shaders.size());
    shader_words.reserve(program.shaders.size());
    VkResult result = VK_SUCCESS;
    for (const PipelineShaderModule& shader : program.shaders) {
        if (shader.format != PipelineShaderFormat::spirv || shader.bytes.size() % sizeof(std::uint32_t) != 0U) {
            diagnostic = {"vulkan_draw_shader_format_unsupported", "Vulkan draw execution requires aligned SPIR-V shaders"};
            return false;
        }
        shader_words.emplace_back(shader.bytes.size() / sizeof(std::uint32_t));
        std::memcpy(shader_words.back().data(), shader.bytes.data(), shader.bytes.size());
        if (shader_words.back().empty() || shader_words.back()[0] != 0x07230203U) {
            diagnostic = {"vulkan_draw_shader_invalid", "Vulkan draw shader has an invalid SPIR-V signature"};
            return false;
        }
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = shader.bytes.size();
        module_info.pCode = shader_words.back().data();
        VkShaderModule module = VK_NULL_HANDLE;
        result = vkCreateShaderModule(context->device, &module_info, nullptr, &module);
        if (result != VK_SUCCESS) break;
        shader_modules.push_back(module);
    }
    auto destroy_modules = [&] {
        for (VkShaderModule module : shader_modules) vkDestroyShaderModule(context->device, module, nullptr);
        shader_modules.clear();
    };
    if (result != VK_SUCCESS) {
        destroy_modules();
        diagnostic = vk_error("vkCreateShaderModule(draw)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }

    VkAttachmentDescription color_attachment{};
    color_attachment.format = vk_texture_format(description.format);
    color_attachment.samples = sample_count;
    color_attachment.loadOp = load_color ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription depth_attachment_description{};
    if (depth_attachment != nullptr) {
        depth_attachment_description.format = VK_FORMAT_D32_SFLOAT;
        depth_attachment_description.samples = sample_count;
        depth_attachment_description.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment_description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_attachment_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment_description.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment_description.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentDescription resolve_attachment{};
    if (description.samples != 1U) {
        resolve_attachment.format = vk_texture_format(description.format);
        resolve_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolve_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    std::array<VkAttachmentDescription, 3> attachments{};
    attachments[0] = color_attachment;
    const std::uint32_t resolve_attachment_index = depth_attachment == nullptr ? 1U : 2U;
    const std::uint32_t attachment_count =
        (depth_attachment == nullptr ? 1U : 2U) + (description.samples == 1U ? 0U : 1U);
    if (depth_attachment != nullptr) attachments[1] = depth_attachment_description;
    if (description.samples != 1U) attachments[resolve_attachment_index] = resolve_attachment;
    VkAttachmentReference color_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve_reference{resolve_attachment_index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = depth_attachment == nullptr ? nullptr : &depth_reference;
    subpass.pResolveAttachments = description.samples == 1U ? nullptr : &resolve_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = attachment_count;
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    result = vkCreateRenderPass(context->device, &render_pass_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
        destroy_modules();
        diagnostic = vk_error("vkCreateRenderPass(draw)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    std::array<VkImageView, 3> framebuffer_attachments{};
    framebuffer_attachments[0] = image.view;
    if (depth_attachment != nullptr) framebuffer_attachments[1] = depth_attachment->view();
    if (description.samples != 1U) framebuffer_attachments[resolve_attachment_index] = resolve_image.view;
    framebuffer_info.attachmentCount = render_pass_info.attachmentCount;
    framebuffer_info.pAttachments = framebuffer_attachments.data();
    framebuffer_info.width = description.width;
    framebuffer_info.height = description.height;
    framebuffer_info.layers = 1U;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    result = vkCreateFramebuffer(context->device, &framebuffer_info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) {
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        destroy_modules();
        diagnostic = vk_error("vkCreateFramebuffer(draw)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPushConstantRange push_constant_range{};
    if (draw_matrices != nullptr) {
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_constant_range.offset = 0U;
        push_constant_range.size = static_cast<std::uint32_t>(sizeof(DrawMatrices));
        layout_info.pushConstantRangeCount = 1U;
        layout_info.pPushConstantRanges = &push_constant_range;
    }
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    result = vkCreatePipelineLayout(context->device, &layout_info, nullptr, &pipeline_layout);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (result == VK_SUCCESS) {
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.reserve(program.shaders.size());
        for (std::size_t index = 0; index < program.shaders.size(); ++index) {
            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = vk_pipeline_stage(program.shaders[index].stage);
            stage.module = shader_modules[index];
            stage.pName = "main";
            stages.push_back(stage);
        }
        const auto& attributes = program.vertex_layout.attributes;
        std::vector<VkVertexInputAttributeDescription> vertex_attributes;
        vertex_attributes.reserve(attributes.size());
        for (const PipelineVertexAttribute& attribute : attributes)
            vertex_attributes.push_back({attribute.location, 0U, vk_pipeline_vertex_format(attribute.format), attribute.offset});
        VkVertexInputBindingDescription vertex_binding{0U, program.vertex_layout.stride,
                                                       VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1U;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertex_attributes.size());
        vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = vk_pipeline_topology(program.raster.fill);
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(description.width),
                            static_cast<float>(description.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, {description.width, description.height}};
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1U;
        viewport_state.pViewports = &viewport;
        viewport_state.scissorCount = 1U;
        viewport_state.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = program.raster.cull == PipelineCullMode::none ? VK_CULL_MODE_NONE
                          : program.raster.cull == PipelineCullMode::front ? VK_CULL_MODE_FRONT_BIT
                                                                                      : VK_CULL_MODE_BACK_BIT;
        raster.frontFace = program.raster.front_face == PipelineFrontFace::clockwise
                               ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = sample_count;
        multisample.alphaToCoverageEnable = program.blend.alpha_to_coverage ? VK_TRUE : VK_FALSE;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = program.blend.enabled ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = vk_pipeline_blend_factor(program.blend.source_color);
        blend.dstColorBlendFactor = vk_pipeline_blend_factor(program.blend.destination_color);
        blend.colorBlendOp = vk_pipeline_blend_operation(program.blend.color_operation);
        blend.srcAlphaBlendFactor = vk_pipeline_blend_factor(program.blend.source_alpha);
        blend.dstAlphaBlendFactor = vk_pipeline_blend_factor(program.blend.destination_alpha);
        blend.alphaBlendOp = vk_pipeline_blend_operation(program.blend.alpha_operation);
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = 1U;
        color_blend.pAttachments = &blend;
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = depth_attachment != nullptr && program.depth.test_enabled ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = depth_attachment != nullptr && program.depth.write_enabled ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = vk_pipeline_compare(program.depth.compare);
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDepthStencilState = depth_attachment == nullptr ? nullptr : &depth_stencil;
        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = render_pass;
        pipeline_info.subpass = 0U;
        result = vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr, &pipeline);
    }
    if (result != VK_SUCCESS) {
        if (pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(context->device, pipeline_layout, nullptr);
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        destroy_modules();
        diagnostic = vk_error("vkCreateGraphicsPipelines(draw)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }

    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1U;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
        if (result == VK_SUCCESS) {
            if (current_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = current_layout;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = description.mip_levels;
                barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(
                    texture_physical_array_layers(description));
                barrier.srcAccessMask = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                            ? 0U
                                            : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(command,
                                     current_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            if (depth_attachment != nullptr &&
                depth_attachment->layout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                VkImageMemoryBarrier depth_barrier{};
                depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                depth_barrier.oldLayout = depth_attachment->layout();
                depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                depth_barrier.image = depth_attachment->image();
                depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                depth_barrier.subresourceRange.levelCount = 1U;
                depth_barrier.subresourceRange.layerCount = 1U;
                depth_barrier.srcAccessMask = depth_attachment->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                                                   ? 0U
                                                   : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(
                    command,
                    depth_attachment->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    0U, 0U, nullptr, 0U, nullptr, 1U, &depth_barrier);
            }
            if (description.samples != 1U) {
                VkImageMemoryBarrier resolve_barrier{};
                resolve_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                resolve_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                resolve_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                resolve_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                resolve_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                resolve_barrier.image = resolve_image.image;
                resolve_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                resolve_barrier.subresourceRange.levelCount = 1U;
                resolve_barrier.subresourceRange.layerCount = 1U;
                resolve_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0U, 0U, nullptr,
                                     0U, nullptr, 1U, &resolve_barrier);
            }
            std::array<VkClearValue, 3> clear_values{};
            for (std::size_t index = 0; index < 4U; ++index) clear_values[0].color.float32[index] = clear_color[index];
            if (depth_attachment != nullptr) clear_values[1].depthStencil.depth = depth_clear_value;
            VkRenderPassBeginInfo render_begin{};
            render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_begin.renderPass = render_pass;
            render_begin.framebuffer = framebuffer;
            render_begin.renderArea.extent = {description.width, description.height};
            render_begin.clearValueCount = render_pass_info.attachmentCount;
            render_begin.pClearValues = clear_values.data();
            vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (draw_matrices != nullptr) {
                vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0U,
                                   static_cast<std::uint32_t>(sizeof(DrawMatrices)), draw_matrices);
            }
            vkCmdBindVertexBuffers(command, 0U, 1U, &geometry.vertices.buffer, &geometry.vertex_offset);
            if (geometry.indices != nullptr) {
                vkCmdBindIndexBuffer(command, geometry.indices->buffer, geometry.index_offset, geometry.index_type);
                vkCmdDrawIndexed(command, geometry.index_count, 1U, 0U, 0, 0U);
            } else {
                vkCmdDraw(command, geometry.vertex_count, 1U, 0U, 0U);
            }
            vkCmdEndRenderPass(command);
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = description.samples == 1U ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                           : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = description.samples == 1U ? image.image : resolve_image.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = description.samples == 1U ? description.mip_levels : 1U;
            barrier.subresourceRange.layerCount = description.samples == 1U
                                                      ? static_cast<std::uint32_t>(
                                                            texture_physical_array_layers(description))
                                                      : 1U;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1U;
            copy.imageExtent = {description.width, description.height, 1U};
            vkCmdCopyImageToBuffer(command, description.samples == 1U ? image.image : resolve_image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buffer, 1U, &copy);
            if (description.samples == 1U) {
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = texture_final_layout(
                    description.usage, description.access_policy);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     0, 0, nullptr, 0, nullptr, 1U, &barrier);
            }
            result = vkEndCommandBuffer(command);
        }
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1U, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1U, &fence, VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain == VK_SUCCESS) {
            current_layout = texture_final_layout(
                description.usage, description.access_policy);
            if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (depth_attachment != nullptr) depth_attachment->mark_invalid();
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(
            description.usage, description.access_policy);
        if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
    }
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(context->device, context->command_pool, 1U, &command);
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(context->device, pipeline, nullptr);
    if (pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(context->device, pipeline_layout, nullptr);
    vkDestroyFramebuffer(context->device, framebuffer, nullptr);
    vkDestroyRenderPass(context->device, render_pass, nullptr);
    destroy_modules();
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan draw execution", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }
    void* mapped = nullptr;
    result = vkMapMemory(context->device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result == VK_SUCCESS && (readback.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = readback.memory;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(context->device, 1U, &range);
    }
    if (result == VK_SUCCESS) {
        try { output.assign(static_cast<const std::byte*>(mapped), static_cast<const std::byte*>(mapped) + output_size); }
        catch (const std::bad_alloc&) { result = VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    if (mapped != nullptr) vkUnmapMemory(context->device, readback.memory);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan draw readback", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        output.clear();
        return false;
    }
    if (description.format == TextureFormat::bgra8_unorm || description.format == TextureFormat::bgra8_srgb)
        for (std::size_t index = 0; index < output.size(); index += 4U) std::swap(output[index], output[index + 2U]);
    return true;
}

bool draw_triangle_and_readback(const std::shared_ptr<VulkanContext>& context,
                                RawVulkanImage& image,
                                const TextureDescription& description,
                                const TriangleDrawRequest& request,
                                VkImageLayout& current_layout,
                                std::vector<std::byte>& output,
                                Diagnostic& diagnostic) {
    BufferDescription vertex_description;
    vertex_description.size_bytes = request.vertex_data.size();
    vertex_description.usage = BufferUsage::vertex;
    vertex_description.memory = BufferMemory::host_visible;
    RawVulkanBuffer vertices;
    if (!create_raw_buffer(context, vertex_description, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           BufferMemory::host_visible, vertices, diagnostic) ||
        !write_host_buffer(context, vertices, 0U, request.vertex_data, diagnostic)) {
        diagnostic.code = "triangle_execution_failed";
        return false;
    }
    const VulkanDrawGeometry geometry{vertices, 0U, nullptr, 0U, request.vertex_count, 0U,
                                      VK_INDEX_TYPE_UINT16};
    if (!draw_graphics_and_readback(context, image, description, *request.pipeline, geometry, nullptr,
                                    nullptr, false, false, 1.0F, request.clear_color, current_layout,
                                    output, diagnostic)) {
        if (diagnostic.code.rfind("vulkan_draw_", 0U) == 0U)
            diagnostic.code = "triangle_execution_failed";
        return false;
    }
    return true;
}

// Record every indexed request in one render pass. This deliberately shares
// the color/depth load operations and readback with the complete batch while
// retaining a pipeline bind, push-constant update, and indexed draw per item.
bool draw_indexed_batch_and_readback(
    const std::shared_ptr<VulkanContext>& context,
    RawVulkanImage& image,
    const TextureDescription& description,
    const TextureTargetSubresource& target_subresource,
    std::span<const VulkanIndexedBatchDraw> draws,
    const PortableSkyShaderConstants* sky_constants,
    const PortableCloudShaderConstants* cloud_constants,
    const VulkanPortableCloudResources* cloud_resources,
    const PortableGrassShaderConstants* grass_constants,
    const VulkanPortableGrassResources* grass_resources,
    VulkanDepthAttachment* depth_attachment,
    bool load_color,
    bool clear_depth,
    float depth_clear_value,
    const std::array<float, 4>& clear_color,
    VkImageLayout& current_layout,
    RawVulkanImage* retained_resolve_image,
    VkImageLayout* retained_resolve_layout,
    bool* retained_resolve_initialized,
    VkImageLayout retained_resolve_final_layout,
    std::uint32_t retained_resolve_mip_levels,
    bool capture_rgba8,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    std::lock_guard command_guard(context->command_mutex);
    const VkSampleCountFlagBits sample_count = vk_sample_count(description.samples);
    if (sample_count == 0U) {
        diagnostic = {"vulkan_draw_samples_unsupported",
                      "Vulkan draws support only one or four samples in the bounded contract"};
        return false;
    }
    const std::uint32_t target_physical_layer = static_cast<std::uint32_t>(
        texture_target_physical_array_layer(description, target_subresource));
    VkImageView target_view = image.view;
    ScopedVulkanImageView target_face_view;
    ScopedVulkanImageView target_mip_view;
    const auto create_mip_zero_attachment_view =
        [&](VkImage source_image, std::uint32_t array_layer,
            ScopedVulkanImageView& owned_view, const char* operation) -> bool {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = source_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = vk_texture_format(description.format);
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0U;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.baseArrayLayer = array_layer;
        view_info.subresourceRange.layerCount = 1U;
        const VkResult view_result = vkCreateImageView(
            context->device, &view_info, nullptr, &owned_view.view);
        if (view_result != VK_SUCCESS) {
            diagnostic = vk_error(operation, view_result);
            diagnostic.code = view_result == VK_ERROR_DEVICE_LOST
                                  ? "vulkan_device_lost"
                                  : "vulkan_draw_execution_failed";
            return false;
        }
        owned_view.device = context->device;
        return true;
    };
    if (description.shape == TextureShape::texture_cube) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = vk_texture_format(description.format);
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = target_subresource.mip_level;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.baseArrayLayer = target_physical_layer;
        view_info.subresourceRange.layerCount = 1U;
        const VkResult view_result = vkCreateImageView(
            context->device, &view_info, nullptr, &target_face_view.view);
        if (view_result != VK_SUCCESS) {
            diagnostic = vk_error("vkCreateImageView(indexed cube face)", view_result);
            diagnostic.code = view_result == VK_ERROR_DEVICE_LOST
                                  ? "vulkan_device_lost"
                                  : "vulkan_draw_execution_failed";
            return false;
        }
        target_face_view.device = context->device;
        target_view = target_face_view.view;
    } else if (description.mip_levels > 1U) {
        if (!create_mip_zero_attachment_view(
                image.image, target_physical_layer, target_mip_view,
                "vkCreateImageView(indexed mip-zero target)"))
            return false;
        target_view = target_mip_view.view;
    }
    if (depth_attachment != nullptr) {
        if (depth_attachment->context().get() != context.get()) {
            diagnostic = {"indexed_depth_attachment_context_mismatch",
                          "The depth attachment belongs to another Vulkan device"};
            return false;
        }
        if (depth_attachment->info().description.samples != description.samples) {
            diagnostic = {"vulkan_depth_samples_mismatch",
                          "Vulkan color and depth attachments must use the same sample count"};
            return false;
        }
        if (!clear_depth && !depth_attachment->initialized()) {
            diagnostic = {"indexed_depth_load_before_clear",
                          "A depth load was requested before the depth attachment was cleared"};
            return false;
        }
    }
    const bool has_stock_native_abi = std::any_of(
        draws.begin(), draws.end(), [](const VulkanIndexedBatchDraw& draw) {
            return draw.has_stock_ks_per_pixel_vulkan_native_abi;
        });
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    if (!has_stock_native_abi &&
        properties.limits.maxPushConstantsSize < sizeof(DrawMatrices)) {
        diagnostic = {"vulkan_draw_transform_limit_unsupported",
                      "Vulkan device push-constant limit is smaller than the draw-matrices contract"};
        return false;
    }
    const std::uint64_t output_size_u64 = static_cast<std::uint64_t>(description.width) *
                                          static_cast<std::uint64_t>(description.height) * 4U;
    if (capture_rgba8 &&
        output_size_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        diagnostic = {"vulkan_draw_output_size_limit", "Vulkan draw readback size exceeds host addressability"};
        return false;
    }
    BufferDescription readback_description;
    readback_description.size_bytes = static_cast<std::size_t>(output_size_u64);
    readback_description.usage = BufferUsage::transfer_destination;
    readback_description.memory = BufferMemory::host_visible;
    RawVulkanBuffer readback;
    if (capture_rgba8 &&
        !create_raw_buffer(context, readback_description, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           BufferMemory::host_visible, readback, diagnostic)) {
        diagnostic.code = "vulkan_draw_execution_failed";
        return false;
    }
    RawVulkanImage resolve_image;
    ScopedVulkanImageView resolve_mip_view;
    RawVulkanImage* resolved_image = retained_resolve_image;
    if (description.samples != 1U && resolved_image == nullptr) {
        TextureDescription resolve_description = description;
        resolve_description.samples = 1U;
        resolve_description.usage = TextureUsage::color_attachment | TextureUsage::transfer_source;
        if (!create_raw_image(context, resolve_description, resolve_image, diagnostic)) {
            diagnostic.code = diagnostic.code.empty() ? "vulkan_resolve_attachment_failed" : diagnostic.code;
            return false;
        }
        resolved_image = &resolve_image;
    }
    if (description.samples != 1U && retained_resolve_image != nullptr &&
        retained_resolve_mip_levels > 1U &&
        !create_mip_zero_attachment_view(
            resolved_image->image, 0U, resolve_mip_view,
            "vkCreateImageView(indexed mip-zero resolve)"))
        return false;

    bool has_sampled_binding = false;
    bool has_material_binding = false;
    bool has_frame_binding = false;
    bool has_normal_binding = false;
    bool has_maps_binding = false;
    bool has_detail_binding = false;
    bool has_normal_detail_binding = false;
    bool has_damage_binding = false;
    bool has_damage_mask_binding = false;
    bool has_directional_shadow_binding = false;
    bool has_multimap_reflection_binding = false;
    bool has_selected_color = false;
    for (const VulkanIndexedBatchDraw& draw : draws) {
        has_sampled_binding = has_sampled_binding || draw.has_sampled_binding;
        has_material_binding = has_material_binding || draw.has_material_binding;
        has_frame_binding = has_frame_binding || draw.has_frame_binding;
        has_normal_binding = has_normal_binding || draw.has_normal_binding;
        has_maps_binding = has_maps_binding || draw.has_maps_binding;
        has_detail_binding = has_detail_binding || draw.has_detail_binding;
        has_normal_detail_binding = has_normal_detail_binding || draw.has_normal_detail_binding;
        has_damage_binding = has_damage_binding || draw.has_damage_binding;
        has_damage_mask_binding = has_damage_mask_binding || draw.has_damage_mask_binding;
        has_directional_shadow_binding = has_directional_shadow_binding ||
                                         draw.has_directional_shadow_binding;
        has_multimap_reflection_binding =
            has_multimap_reflection_binding ||
            draw.has_multimap_reflection_binding;
        has_selected_color = has_selected_color || draw.has_selected_color;
    }
    std::vector<VulkanDepthAttachment*> sampled_shadow_attachments;
    if (has_directional_shadow_binding || std::any_of(
            draws.begin(), draws.end(), [](const VulkanIndexedBatchDraw& draw) {
                return draw.has_stock_ks_per_pixel_vulkan_native_abi;
            })) {
        sampled_shadow_attachments.reserve(
            draws.size() * indexed_directional_shadow_cascade_count);
        for (const VulkanIndexedBatchDraw& draw : draws) {
            if (!draw.has_directional_shadow_binding &&
                !draw.has_stock_ks_per_pixel_vulkan_native_abi) continue;
            const auto& attachments = draw.has_stock_ks_per_pixel_vulkan_native_abi
                                          ? draw.stock_native_abi_shadow_attachments
                                          : draw.shadow_attachments;
            for (VulkanDepthAttachment* attachment : attachments) {
                if (attachment != nullptr &&
                    std::find(sampled_shadow_attachments.begin(),
                              sampled_shadow_attachments.end(), attachment) ==
                        sampled_shadow_attachments.end())
                    sampled_shadow_attachments.push_back(attachment);
            }
        }
    }
    std::vector<VkImageLayout> sampled_shadow_original_layouts;
    sampled_shadow_original_layouts.reserve(sampled_shadow_attachments.size());
    for (const VulkanDepthAttachment* attachment : sampled_shadow_attachments)
        sampled_shadow_original_layouts.push_back(attachment->layout());
    VulkanTransientSampledDescriptors descriptors;
    if ((has_sampled_binding || has_normal_binding || has_maps_binding || has_detail_binding ||
         has_normal_detail_binding || has_damage_binding || has_damage_mask_binding ||
         has_material_binding || has_frame_binding ||
         has_directional_shadow_binding || has_multimap_reflection_binding) &&
        !create_sampled_descriptor_layout(context, descriptors, has_material_binding, has_frame_binding,
                                          has_normal_binding, has_maps_binding, has_detail_binding,
                                          has_normal_detail_binding, has_damage_binding,
                                          has_damage_mask_binding,
                                          has_directional_shadow_binding,
                                          has_multimap_reflection_binding,
                                          false, diagnostic))
        return false;
    VulkanTransientSelectedDescriptors selected_descriptors;
    if (has_selected_color &&
        !create_selected_descriptor_layout(context, selected_descriptors,
                                           diagnostic))
        return false;
    VulkanTransientStockNativeAbiDescriptors stock_native_abi_descriptors;
    if (has_stock_native_abi &&
        !create_stock_native_abi_descriptors(context, draws,
                                             stock_native_abi_descriptors,
                                             diagnostic))
        return false;

    VkAttachmentDescription color_attachment{};
    color_attachment.format = vk_texture_format(description.format);
    color_attachment.samples = sample_count;
    color_attachment.loadOp = load_color ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription depth_description{};
    if (depth_attachment != nullptr) {
        depth_description.format = VK_FORMAT_D32_SFLOAT;
        depth_description.samples = sample_count;
        depth_description.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_description.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_description.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentDescription resolve_attachment{};
    if (description.samples != 1U) {
        resolve_attachment.format = vk_texture_format(description.format);
        resolve_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolve_attachment.finalLayout =
            capture_rgba8 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                          : retained_resolve_final_layout;
    }
    std::array<VkAttachmentDescription, 3> attachments{};
    attachments[0] = color_attachment;
    const std::uint32_t resolve_attachment_index = depth_attachment == nullptr ? 1U : 2U;
    const std::uint32_t attachment_count =
        (depth_attachment == nullptr ? 1U : 2U) + (description.samples == 1U ? 0U : 1U);
    if (depth_attachment != nullptr) attachments[1] = depth_description;
    if (description.samples != 1U) attachments[resolve_attachment_index] = resolve_attachment;
    VkAttachmentReference color_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve_reference{resolve_attachment_index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = depth_attachment == nullptr ? nullptr : &depth_reference;
    subpass.pResolveAttachments = description.samples == 1U ? nullptr : &resolve_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = attachment_count;
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkResult result = vkCreateRenderPass(context->device, &render_pass_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateRenderPass(batch)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }
    std::array<VkImageView, 3> framebuffer_attachments{};
    framebuffer_attachments[0] = target_view;
    if (depth_attachment != nullptr) framebuffer_attachments[1] = depth_attachment->view();
    if (description.samples != 1U)
        framebuffer_attachments[resolve_attachment_index] =
            resolve_mip_view.view != VK_NULL_HANDLE ? resolve_mip_view.view
                                                    : resolved_image->view;
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = render_pass_info.attachmentCount;
    framebuffer_info.pAttachments = framebuffer_attachments.data();
    framebuffer_info.width = description.width;
    framebuffer_info.height = description.height;
    framebuffer_info.layers = 1U;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    result = vkCreateFramebuffer(context->device, &framebuffer_info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) {
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        diagnostic = vk_error("vkCreateFramebuffer(batch)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }

    VulkanTransientSkyDescriptors sky_descriptors;
    VulkanBatchPipeline sky_pipeline;
    if (sky_constants != nullptr) {
        if (!create_portable_sky_descriptors(
                context, *sky_constants, sky_descriptors, diagnostic)) {
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            return false;
        }
        const auto shader_bytes = [](const std::uint32_t* words,
                                     std::size_t word_count) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(words);
            return std::vector<std::uint8_t>(bytes, bytes + word_count * sizeof(std::uint32_t));
        };
        PipelineProgram sky_program;
        sky_program.name = "portable-sky-webgl-aligned";
        sky_program.shaders = {
            {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
             shader_bytes(generated::portable_sky_vertex_spirv,
                          generated::portable_sky_vertex_spirv_size /
                              sizeof(std::uint32_t)),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
             shader_bytes(generated::portable_sky_fragment_spirv,
                          generated::portable_sky_fragment_spirv_size /
                              sizeof(std::uint32_t)),
             PipelineShaderProvenance::source_equivalent},
        };
        sky_program.raster.cull = PipelineCullMode::none;
        sky_program.depth.test_enabled = false;
        sky_program.depth.write_enabled = false;
        sky_program.resources = {
            {PipelineResourceKind::uniform_buffer, 0U, 0U,
             "portableSkyConstants"}};
        const std::array<VkDescriptorSetLayout, 1U> sky_layouts = {
            sky_descriptors.layout};
        if (!create_batch_pipeline(
                context, render_pass, description.width, description.height,
                description.samples, sky_program, true,
                depth_attachment != nullptr, sky_layouts, false,
                sky_pipeline, diagnostic)) {
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            return false;
        }
    }

    std::vector<VulkanBatchPipeline> pipelines;
    pipelines.reserve(draws.size());
    for (const VulkanIndexedBatchDraw& draw : draws) {
        if (draw.program == nullptr || draw.vertices == nullptr ||
            (draw.indexed && draw.indices == nullptr)) {
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            diagnostic = {"indexed_static_mesh_batch_resource_invalid",
                          "The Vulkan indexed batch contains an invalid draw resource"};
            return false;
        }
        pipelines.emplace_back();
        std::span<const VkDescriptorSetLayout> draw_descriptor_layouts;
        if (draw.has_stock_ks_per_pixel_vulkan_native_abi) {
            draw_descriptor_layouts = stock_native_abi_descriptors.layouts;
        } else if (draw.has_selected_color) {
            draw_descriptor_layouts =
                std::span<const VkDescriptorSetLayout>(&selected_descriptors.layout, 1U);
        } else if (descriptors.layout != VK_NULL_HANDLE) {
            draw_descriptor_layouts =
                std::span<const VkDescriptorSetLayout>(&descriptors.layout, 1U);
        }
        if (!create_batch_pipeline(context, render_pass, description.width, description.height,
                                   description.samples, *draw.program, true,
                                   depth_attachment != nullptr, draw_descriptor_layouts,
                                   !draw.has_stock_ks_per_pixel_vulkan_native_abi,
                                   pipelines.back(), diagnostic)) {
            pipelines.clear();
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            return false;
        }
    }

    std::vector<VkDescriptorSet> descriptor_sets;
    if ((has_sampled_binding || has_normal_binding || has_maps_binding || has_detail_binding ||
         has_normal_detail_binding || has_damage_binding || has_damage_mask_binding ||
         has_material_binding || has_frame_binding ||
         has_directional_shadow_binding || has_multimap_reflection_binding) &&
        !allocate_sampled_descriptor_sets(
                                  context, descriptors, draws, descriptor_sets, diagnostic)) {
        pipelines.clear();
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        return false;
    }
    if (has_selected_color &&
        !allocate_selected_descriptor_sets(context, selected_descriptors,
                                           draws, descriptor_sets,
                                           diagnostic)) {
        pipelines.clear();
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        return false;
    }

    VulkanTransientCloudDescriptors cloud_descriptors;
    VulkanBatchPipeline cloud_pipeline;
    const std::span<const VulkanPortableCloudRun> cloud_runs =
        cloud_resources != nullptr ? std::span<const VulkanPortableCloudRun>(
                                         cloud_resources->runs)
                                    : std::span<const VulkanPortableCloudRun>{};
    const auto cleanup_batch_setup = [&]() {
        pipelines.clear();
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
    };
    if (cloud_constants != nullptr || cloud_resources != nullptr) {
        if (cloud_constants == nullptr || cloud_resources == nullptr ||
            cloud_runs.size() > portable_cloud_max_count ||
            (!cloud_runs.empty() &&
             (cloud_resources->vertex_buffer == VK_NULL_HANDLE ||
              cloud_resources->sampler == VK_NULL_HANDLE))) {
            diagnostic = {"vulkan_portable_cloud_request_invalid",
                          "The Vulkan cloud batch contains an incomplete or oversized request"};
            cleanup_batch_setup();
            return false;
        }
        if (!cloud_runs.empty()) {
            if (!create_portable_cloud_descriptors(
                    context, *cloud_constants, *cloud_resources,
                    cloud_descriptors, diagnostic)) {
                cleanup_batch_setup();
                return false;
            }
            const auto shader_bytes = [](const unsigned char* bytes,
                                         std::size_t byte_count) {
                return std::vector<std::uint8_t>(bytes, bytes + byte_count);
            };
            PipelineProgram cloud_program;
            cloud_program.name = "portable-clouds-webgl-aligned";
            cloud_program.shaders = {
                {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
                shader_bytes(generated::portable_clouds_vertex_spirv,
                             generated::portable_clouds_vertex_spirv_size),
                 PipelineShaderProvenance::source_equivalent},
                {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
                shader_bytes(generated::portable_clouds_fragment_spirv,
                             generated::portable_clouds_fragment_spirv_size),
                 PipelineShaderProvenance::source_equivalent},
            };
            cloud_program.vertex_layout.stride = static_cast<std::uint32_t>(
                portable_cloud_vertex_stride_bytes);
            cloud_program.vertex_layout.attributes = {
                {PipelineVertexSemantic::position,
                 PipelineVertexAttributeFormat::float32x2, 0U, 0U},
                {PipelineVertexSemantic::position,
                 PipelineVertexAttributeFormat::float32x3, 1U,
                 2U * sizeof(float)},
                {PipelineVertexSemantic::texcoord0,
                 PipelineVertexAttributeFormat::float32x2, 2U,
                 5U * sizeof(float)},
                {PipelineVertexSemantic::texcoord0,
                 PipelineVertexAttributeFormat::float32, 3U,
                 7U * sizeof(float)},
            };
            cloud_program.raster.cull = PipelineCullMode::front;
            cloud_program.depth.test_enabled = false;
            cloud_program.depth.write_enabled = false;
            cloud_program.blend.enabled = true;
            cloud_program.blend.source_color = PipelineBlendFactor::source_alpha;
            cloud_program.blend.destination_color =
                PipelineBlendFactor::one_minus_source_alpha;
            cloud_program.blend.source_alpha = PipelineBlendFactor::source_alpha;
            cloud_program.blend.destination_alpha =
                PipelineBlendFactor::one_minus_source_alpha;
            cloud_program.resources = {
                {PipelineResourceKind::uniform_buffer, 0U, 0U,
                 "portableCloudConstants"},
                {PipelineResourceKind::sampled_texture, 0U, 1U,
                 "portableCloudTexture"},
            };
            const std::array<VkDescriptorSetLayout, 1U> cloud_layouts = {
                cloud_descriptors.layout};
            if (!create_batch_pipeline(
                    context, render_pass, description.width, description.height,
                    description.samples, cloud_program, true,
                    depth_attachment != nullptr, cloud_layouts, false,
                    cloud_pipeline, diagnostic)) {
                cleanup_batch_setup();
                return false;
            }
        }
    }

    VulkanTransientGrassDescriptors grass_descriptors;
    VulkanBatchPipeline grass_pipeline;
    const bool draw_grass = grass_constants != nullptr &&
                            grass_resources != nullptr &&
                            grass_resources->vertex_count != 0U;
    const bool grass_atlas_shared_with_cloud =
        draw_grass && std::any_of(
            cloud_runs.begin(), cloud_runs.end(),
            [&](const VulkanPortableCloudRun& run) {
                return run.image == grass_resources->atlas_image &&
                       run.tracked_layout == grass_resources->tracked_layout &&
                       run.original_layout == grass_resources->original_layout;
            });
    if (grass_constants != nullptr || grass_resources != nullptr) {
        if (grass_constants == nullptr || grass_resources == nullptr ||
            grass_resources->vertex_count > portable_grass_max_vertex_count ||
            grass_resources->vertex_count % portable_grass_vertices_per_blade != 0U ||
            (draw_grass &&
             (grass_resources->vertex_buffer == VK_NULL_HANDLE ||
              grass_resources->atlas_image == VK_NULL_HANDLE ||
              grass_resources->atlas_view == VK_NULL_HANDLE ||
              grass_resources->sampler == VK_NULL_HANDLE ||
              grass_resources->tracked_layout == nullptr ||
              grass_resources->original_layout == VK_IMAGE_LAYOUT_UNDEFINED))) {
            diagnostic = {"vulkan_portable_grass_request_invalid",
                          "The Vulkan grass batch contains an incomplete or oversized request"};
            cleanup_batch_setup();
            return false;
        }
        if (draw_grass &&
            !create_portable_grass_descriptors(
                context, *grass_constants, *grass_resources,
                grass_descriptors, diagnostic)) {
            cleanup_batch_setup();
            return false;
        }
        if (draw_grass) {
            const auto shader_bytes = [](const unsigned char* bytes,
                                         std::size_t byte_count) {
                return std::vector<std::uint8_t>(bytes, bytes + byte_count);
            };
            PipelineProgram grass_program;
            grass_program.name = "portable-grass-expanded-webgl-aligned";
            grass_program.shaders = {
                {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
                 shader_bytes(generated::portable_grass_vertex_spirv,
                              generated::portable_grass_vertex_spirv_size),
                 PipelineShaderProvenance::portable},
                {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
                 shader_bytes(generated::portable_grass_fragment_spirv,
                              generated::portable_grass_fragment_spirv_size),
                 PipelineShaderProvenance::portable},
            };
            grass_program.vertex_layout.stride = static_cast<std::uint32_t>(
                portable_grass_vertex_stride_bytes);
            grass_program.vertex_layout.attributes = {
                {PipelineVertexSemantic::position,
                 PipelineVertexAttributeFormat::float32x3, 0U, 0U},
                {PipelineVertexSemantic::normal,
                 PipelineVertexAttributeFormat::float32x3, 1U,
                 3U * sizeof(float)},
                {PipelineVertexSemantic::texcoord0,
                 PipelineVertexAttributeFormat::float32x2, 2U,
                 6U * sizeof(float)},
                {PipelineVertexSemantic::color,
                 PipelineVertexAttributeFormat::float32x4, 3U,
                 8U * sizeof(float)},
                {PipelineVertexSemantic::texcoord1,
                 PipelineVertexAttributeFormat::float32, 4U,
                 12U * sizeof(float)},
                {PipelineVertexSemantic::texcoord1,
                 PipelineVertexAttributeFormat::float32, 5U,
                 13U * sizeof(float)},
            };
            grass_program.raster.cull = PipelineCullMode::none;
            grass_program.depth.test_enabled = true;
            grass_program.depth.write_enabled = true;
            grass_program.depth.compare = PipelineCompareOperation::less_or_equal;
            grass_program.blend.alpha_to_coverage = description.samples == 4U;
            grass_program.resources = {
                {PipelineResourceKind::uniform_buffer, 0U, 0U,
                 "portableGrassConstants"},
                {PipelineResourceKind::sampled_texture, 0U, 1U,
                 "portableGrassAtlas"},
                {PipelineResourceKind::sampler, 0U, 2U,
                 "portableGrassSampler"},
            };
            const std::array<VkDescriptorSetLayout, 1U> grass_layouts = {
                grass_descriptors.layout};
            if (!create_batch_pipeline(
                    context, render_pass, description.width, description.height,
                    description.samples, grass_program, true,
                    depth_attachment != nullptr, grass_layouts, false,
                    grass_pipeline, diagnostic)) {
                cleanup_batch_setup();
                return false;
            }
        }
    }

    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1U;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
    }
    if (result == VK_SUCCESS) {
        if (current_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = current_layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = description.mip_levels;
            barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(
                texture_physical_array_layers(description));
            barrier.srcAccessMask = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                        ? 0U : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(command,
                                 current_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0U, 0U, nullptr,
                                 0U, nullptr, 1U, &barrier);
        }
        if (depth_attachment != nullptr &&
            depth_attachment->layout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = depth_attachment->layout();
            barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = depth_attachment->image();
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier.subresourceRange.levelCount = 1U;
            barrier.subresourceRange.layerCount = 1U;
            barrier.srcAccessMask = depth_attachment->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                                        ? 0U : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(command,
                                 depth_attachment->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                                     ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
        }
        for (VulkanDepthAttachment* shadow : sampled_shadow_attachments) {
            if (shadow->layout() ==
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                continue;
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = shadow->layout();
            barrier.newLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = shadow->image();
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier.subresourceRange.levelCount = 1U;
            barrier.subresourceRange.layerCount = 1U;
            barrier.srcAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(
                command,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, nullptr,
                0U, nullptr, 1U, &barrier);
        }
        if (description.samples != 1U) {
            const std::uint32_t resolve_mip_levels =
                retained_resolve_image != nullptr
                    ? std::max(1U, retained_resolve_mip_levels)
                    : 1U;
            VkImageMemoryBarrier resolve_barrier{};
            resolve_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            resolve_barrier.oldLayout = retained_resolve_layout != nullptr
                                            ? *retained_resolve_layout
                                            : VK_IMAGE_LAYOUT_UNDEFINED;
            resolve_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            resolve_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            resolve_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            resolve_barrier.image = resolved_image->image;
            resolve_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            resolve_barrier.subresourceRange.levelCount = resolve_mip_levels;
            resolve_barrier.subresourceRange.layerCount = 1U;
            resolve_barrier.srcAccessMask = resolve_barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                                                ? 0U
                                                : VK_ACCESS_MEMORY_READ_BIT |
                                                      VK_ACCESS_MEMORY_WRITE_BIT;
            resolve_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(command,
                                 resolve_barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                                     ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0U, 0U, nullptr,
                                 0U, nullptr, 1U, &resolve_barrier);
        }
        if (!cloud_runs.empty()) {
            std::vector<VkImageMemoryBarrier> barriers;
            barriers.reserve(cloud_runs.size());
            bool has_non_shader_read_source = false;
            for (std::size_t index = 0U; index < cloud_runs.size(); ++index) {
                const VulkanPortableCloudRun& run = cloud_runs[index];
                const bool already_seen = std::any_of(
                    cloud_runs.begin(), cloud_runs.begin() +
                        static_cast<std::span<const VulkanPortableCloudRun>::difference_type>(index),
                    [&](const VulkanPortableCloudRun& prior) {
                        return prior.image == run.image &&
                               prior.tracked_layout == run.tracked_layout;
                    });
                if (already_seen) continue;
                const VkImageLayout original = run.original_layout;
                if (original == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
                has_non_shader_read_source = true;
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = original;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = run.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount =
                    run.mip_levels;
                barrier.subresourceRange.layerCount = 1U;
                barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                        VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers.push_back(barrier);
            }
            if (!barriers.empty()) {
                vkCmdPipelineBarrier(
                    command,
                    has_non_shader_read_source ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, nullptr, 0U,
                    nullptr, static_cast<std::uint32_t>(barriers.size()),
                    barriers.data());
            }
        }
        if (draw_grass && !grass_atlas_shared_with_cloud &&
            grass_resources->original_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = grass_resources->original_layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = grass_resources->atlas_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = grass_resources->mip_levels;
            barrier.subresourceRange.layerCount = 1U;
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                    VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U,
                                 0U, nullptr, 0U, nullptr, 1U, &barrier);
        }
        std::array<VkClearValue, 3> clear_values{};
        for (std::size_t index = 0; index < 4U; ++index) clear_values[0].color.float32[index] = clear_color[index];
        if (depth_attachment != nullptr) clear_values[1].depthStencil.depth = depth_clear_value;
        VkRenderPassBeginInfo render_begin{};
        render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_begin.renderPass = render_pass;
        render_begin.framebuffer = framebuffer;
        render_begin.renderArea.extent = {description.width, description.height};
        render_begin.clearValueCount = render_pass_info.attachmentCount;
        render_begin.pClearValues = clear_values.data();
        vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
        if (sky_constants != nullptr) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              sky_pipeline.pipeline);
            vkCmdBindDescriptorSets(
                command, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline.layout,
                0U, 1U, &sky_descriptors.set, 0U, nullptr);
            // The inverted clip-space Y in portable_sky.vert preserves the
            // production WebGL top/bottom NDC orientation on Vulkan.
            vkCmdDraw(command, 3U, 1U, 0U, 0U);
        }
        if (!cloud_runs.empty()) {
            // Cloud runs are ordered exactly as produced by the portable
            // layout builder. A missing texture slot skips only that run.
            const VkBuffer cloud_vertex_buffer = cloud_resources->vertex_buffer;
            const VkDeviceSize cloud_vertex_offset = 0U;
            for (std::size_t index = 0U; index < cloud_runs.size(); ++index) {
                const VulkanPortableCloudRun& run = cloud_runs[index];
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  cloud_pipeline.pipeline);
                vkCmdBindDescriptorSets(
                    command, VK_PIPELINE_BIND_POINT_GRAPHICS, cloud_pipeline.layout,
                    0U, 1U, &cloud_descriptors.sets[index], 0U, nullptr);
                vkCmdBindVertexBuffers(command, 0U, 1U, &cloud_vertex_buffer,
                                       &cloud_vertex_offset);
                vkCmdDraw(command, run.vertex_count, 1U, run.first_vertex, 0U);
            }
        }
        const auto record_grass_draw = [&]() {
            if (!draw_grass) return;
            const VkDeviceSize grass_vertex_offset = 0U;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              grass_pipeline.pipeline);
            vkCmdBindDescriptorSets(
                command, VK_PIPELINE_BIND_POINT_GRAPHICS, grass_pipeline.layout,
                0U, 1U, &grass_descriptors.set, 0U, nullptr);
            vkCmdBindVertexBuffers(command, 0U, 1U,
                                   &grass_resources->vertex_buffer,
                                   &grass_vertex_offset);
            vkCmdDraw(command, grass_resources->vertex_count, 1U, 0U, 0U);
        };
        bool grass_recorded = false;
        for (std::size_t index = 0; index < draws.size(); ++index) {
            const VulkanIndexedBatchDraw& draw = draws[index];
            if (!grass_recorded && draw.transparent) {
                record_grass_draw();
                grass_recorded = true;
            }
            const VulkanBatchPipeline& pipeline = pipelines[index];
            const VkBuffer vertex_buffer = draw.vertices->raw().buffer;
            const VkDeviceSize vertex_offset = draw.vertex_offset;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            if (!draw.has_stock_ks_per_pixel_vulkan_native_abi)
                vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0U,
                                   static_cast<std::uint32_t>(sizeof(DrawMatrices)), &draw.matrices);
            if (draw.has_stock_ks_per_pixel_vulkan_native_abi) {
                const VkDescriptorSet* draw_sets =
                    stock_native_abi_descriptors.sets_for_draw(index);
                if (draw_sets == nullptr) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    break;
                }
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline.layout, 0U, 3U,
                                        draw_sets, 0U, nullptr);
            } else if (!descriptor_sets.empty() && descriptor_sets[index] != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                                        0U, 1U, &descriptor_sets[index], 0U, nullptr);
            }
            vkCmdBindVertexBuffers(command, 0U, 1U, &vertex_buffer, &vertex_offset);
            if (draw.indexed) {
                vkCmdBindIndexBuffer(command, draw.indices->raw().buffer,
                                     draw.index_offset, VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(command, draw.index_count, 1U, 0U, 0, 0U);
            } else {
                vkCmdDraw(command, draw.vertex_count, 1U, 0U, 0U);
            }
        }
        if (!grass_recorded) record_grass_draw();
        vkCmdEndRenderPass(command);
        if (!cloud_runs.empty()) {
            std::vector<VkImageMemoryBarrier> barriers;
            barriers.reserve(cloud_runs.size());
            for (std::size_t index = 0U; index < cloud_runs.size(); ++index) {
                const VulkanPortableCloudRun& run = cloud_runs[index];
                const bool already_seen = std::any_of(
                    cloud_runs.begin(), cloud_runs.begin() +
                        static_cast<std::span<const VulkanPortableCloudRun>::difference_type>(index),
                    [&](const VulkanPortableCloudRun& prior) {
                        return prior.image == run.image &&
                               prior.tracked_layout == run.tracked_layout;
                    });
                if (already_seen) continue;
                const VkImageLayout original = run.original_layout;
                if (original == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout = original;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = run.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount =
                    run.mip_levels;
                barrier.subresourceRange.layerCount = 1U;
                barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                        VK_ACCESS_MEMORY_WRITE_BIT;
                barriers.push_back(barrier);
            }
            if (!barriers.empty()) {
                vkCmdPipelineBarrier(
                    command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U,
                    nullptr, static_cast<std::uint32_t>(barriers.size()),
                    barriers.data());
            }
        }
        if (draw_grass && !grass_atlas_shared_with_cloud &&
            grass_resources->original_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = grass_resources->original_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = grass_resources->atlas_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = grass_resources->mip_levels;
            barrier.subresourceRange.layerCount = 1U;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                    VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U,
                                 nullptr, 0U, nullptr, 1U, &barrier);
        }
        if (description.samples != 1U && retained_resolve_image != nullptr &&
            retained_resolve_mip_levels > 1U &&
            retained_resolve_final_layout !=
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier resolve_mip_barrier{};
            resolve_mip_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            resolve_mip_barrier.oldLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            resolve_mip_barrier.newLayout = retained_resolve_final_layout;
            resolve_mip_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            resolve_mip_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            resolve_mip_barrier.image = resolved_image->image;
            resolve_mip_barrier.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            resolve_mip_barrier.subresourceRange.baseMipLevel = 1U;
            resolve_mip_barrier.subresourceRange.levelCount =
                retained_resolve_mip_levels - 1U;
            resolve_mip_barrier.subresourceRange.layerCount = 1U;
            resolve_mip_barrier.srcAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            resolve_mip_barrier.dstAccessMask =
                retained_resolve_final_layout ==
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    ? VK_ACCESS_SHADER_READ_BIT
                    : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(
                command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                retained_resolve_final_layout ==
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                    : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0U, 0U, nullptr, 0U, nullptr, 1U, &resolve_mip_barrier);
        }
        for (std::size_t index = 0U;
             index < sampled_shadow_attachments.size(); ++index) {
            const VkImageLayout original =
                sampled_shadow_original_layouts[index];
            if (original == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                continue;
            VkImageMemoryBarrier shadow_barrier{};
            shadow_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            shadow_barrier.oldLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            shadow_barrier.newLayout = original;
            shadow_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            shadow_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            shadow_barrier.image = sampled_shadow_attachments[index]->image();
            shadow_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            shadow_barrier.subresourceRange.levelCount = 1U;
            shadow_barrier.subresourceRange.layerCount = 1U;
            shadow_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            shadow_barrier.dstAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(
                command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                0U, 0U, nullptr, 0U, nullptr, 1U, &shadow_barrier);
        }
        if (capture_rgba8) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = description.samples == 1U ? image.image : resolved_image->image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = description.samples == 1U ? description.mip_levels : 1U;
            barrier.subresourceRange.layerCount = description.samples == 1U
                                                      ? static_cast<std::uint32_t>(
                                                            texture_physical_array_layers(description))
                                                      : 1U;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = target_subresource.mip_level;
            copy.imageSubresource.baseArrayLayer = target_physical_layer;
            copy.imageSubresource.layerCount = 1U;
            copy.imageExtent = {description.width, description.height, 1U};
            vkCmdCopyImageToBuffer(command,
                                   description.samples == 1U ? image.image : resolved_image->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buffer, 1U, &copy);
            if (description.samples == 1U || retained_resolve_image != nullptr) {
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = description.samples == 1U
                                        ? texture_final_layout(
                                              description.usage,
                                              description.access_policy)
                                        : retained_resolve_final_layout;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr,
                                     0U, nullptr, 1U, &barrier);
            }
        } else if (description.samples == 1U) {
            const VkImageLayout final_layout = texture_final_layout(
                description.usage, description.access_policy);
            if (final_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = final_layout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = description.mip_levels;
                barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(
                    texture_physical_array_layers(description));
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask =
                    final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        ? VK_ACCESS_SHADER_READ_BIT
                        : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                vkCmdPipelineBarrier(
                    command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                        : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
            }
        }
        result = vkEndCommandBuffer(command);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1U, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (result == VK_SUCCESS) {
                result = vkWaitForFences(context->device, 1U, &fence, VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain == VK_SUCCESS) {
            current_layout = texture_final_layout(
                description.usage, description.access_policy);
            if (retained_resolve_layout != nullptr)
                *retained_resolve_layout = retained_resolve_final_layout;
            if (retained_resolve_initialized != nullptr)
                *retained_resolve_initialized = true;
            if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
            for (std::size_t index = 0U;
                 index < sampled_shadow_attachments.size(); ++index)
                sampled_shadow_attachments[index]->mark_layout(
                    sampled_shadow_original_layouts[index]);
            for (const VulkanPortableCloudRun& run : cloud_runs)
                if (run.tracked_layout != nullptr)
                    *run.tracked_layout = run.original_layout;
            if (draw_grass && grass_resources->tracked_layout != nullptr)
                *grass_resources->tracked_layout = grass_resources->original_layout;
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (retained_resolve_layout != nullptr)
                *retained_resolve_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (retained_resolve_initialized != nullptr)
                *retained_resolve_initialized = false;
            if (depth_attachment != nullptr) depth_attachment->mark_invalid();
            for (VulkanDepthAttachment* shadow : sampled_shadow_attachments)
                shadow->mark_invalid();
            for (const VulkanPortableCloudRun& run : cloud_runs)
                if (run.tracked_layout != nullptr)
                    *run.tracked_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (draw_grass && grass_resources->tracked_layout != nullptr)
                *grass_resources->tracked_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(
            description.usage, description.access_policy);
        if (retained_resolve_layout != nullptr)
            *retained_resolve_layout = retained_resolve_final_layout;
        if (retained_resolve_initialized != nullptr)
            *retained_resolve_initialized = true;
        if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
        for (std::size_t index = 0U;
             index < sampled_shadow_attachments.size(); ++index)
            sampled_shadow_attachments[index]->mark_layout(
                sampled_shadow_original_layouts[index]);
        for (const VulkanPortableCloudRun& run : cloud_runs)
            if (run.tracked_layout != nullptr)
                *run.tracked_layout = run.original_layout;
        if (draw_grass && grass_resources->tracked_layout != nullptr)
            *grass_resources->tracked_layout = grass_resources->original_layout;
    }
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(context->device, context->command_pool, 1U, &command);
    vkDestroyFramebuffer(context->device, framebuffer, nullptr);
    vkDestroyRenderPass(context->device, render_pass, nullptr);
    pipelines.clear();
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan indexed batch execution", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        return false;
    }
    if (!capture_rgba8) {
        output.clear();
        diagnostic = {};
        return true;
    }
    void* mapped = nullptr;
    result = vkMapMemory(context->device, readback.memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (result == VK_SUCCESS && (readback.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = readback.memory;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(context->device, 1U, &range);
    }
    if (result == VK_SUCCESS) {
        try {
            output.assign(static_cast<const std::byte*>(mapped),
                          static_cast<const std::byte*>(mapped) + static_cast<std::size_t>(output_size_u64));
        } catch (const std::bad_alloc&) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    if (mapped != nullptr) vkUnmapMemory(context->device, readback.memory);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan indexed batch readback", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_draw_execution_failed";
        output.clear();
        return false;
    }
    if (description.format == TextureFormat::bgra8_unorm || description.format == TextureFormat::bgra8_srgb)
        for (std::size_t index = 0; index < output.size(); index += 4U) std::swap(output[index], output[index + 2U]);
    return true;
}

// Execute a resource-free depth pass without manufacturing a color target or
// performing a color readback. The attachment stays in the persistent D32
// layout so a later bounded readback or receiver pass can consume it.
bool draw_depth_only_indexed_batch(
    const std::shared_ptr<VulkanContext>& context,
    VulkanDepthAttachment& depth_attachment,
    std::span<const VulkanIndexedBatchDraw> draws,
    bool clear_depth,
    float depth_clear_value,
    Diagnostic& diagnostic) {
    std::lock_guard command_guard(context->command_mutex);
    if (depth_attachment.context().get() != context.get()) {
        diagnostic = {"depth_only_indexed_depth_context_mismatch",
                      "The depth attachment belongs to another Vulkan device"};
        return false;
    }
    if (!clear_depth && !depth_attachment.initialized()) {
        diagnostic = {"depth_only_indexed_depth_load_before_clear",
                      "A depth load was requested before the depth attachment was initialized"};
        return false;
    }
    const DepthAttachmentDescription& description = depth_attachment.info().description;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    if (properties.limits.maxPushConstantsSize < sizeof(DrawMatrices)) {
        diagnostic = {"vulkan_draw_transform_limit_unsupported",
                      "Vulkan device push-constant limit is smaller than the draw-matrices contract"};
        return false;
    }

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_D32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_reference{0U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0U;
    subpass.pColorAttachments = nullptr;
    subpass.pDepthStencilAttachment = &depth_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1U;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkResult result = vkCreateRenderPass(context->device, &render_pass_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateRenderPass(depth-only batch)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_depth_only_execution_failed";
        return false;
    }

    const VkImageView framebuffer_attachment = depth_attachment.view();
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1U;
    framebuffer_info.pAttachments = &framebuffer_attachment;
    framebuffer_info.width = description.width;
    framebuffer_info.height = description.height;
    framebuffer_info.layers = 1U;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    result = vkCreateFramebuffer(context->device, &framebuffer_info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) {
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        diagnostic = vk_error("vkCreateFramebuffer(depth-only batch)", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_depth_only_execution_failed";
        return false;
    }

    const bool has_alpha_tested_binding = std::any_of(
        draws.begin(), draws.end(), [](const VulkanIndexedBatchDraw& draw) {
            return draw.has_alpha_tested_binding;
        });
    VulkanTransientSampledDescriptors descriptors;
    if (has_alpha_tested_binding &&
        !create_sampled_descriptor_layout(context, descriptors, false, false,
                                           false, false, false, false, false,
                                           false, false, false, true,
                                           diagnostic)) {
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        return false;
    }
    std::vector<VkDescriptorSet> descriptor_sets;
    if (has_alpha_tested_binding &&
        !allocate_depth_alpha_descriptor_sets(context, descriptors, draws, descriptor_sets,
                                              diagnostic)) {
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
        return false;
    }

    std::vector<VulkanBatchPipeline> pipelines;
    pipelines.reserve(draws.size());
    for (const VulkanIndexedBatchDraw& draw : draws) {
        if (draw.program == nullptr || draw.vertices == nullptr || draw.indices == nullptr) {
            pipelines.clear();
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            diagnostic = {"depth_only_indexed_static_mesh_batch_resource_invalid",
                          "The Vulkan depth-only batch contains an invalid draw resource"};
            return false;
        }
        if (draw.has_alpha_tested_binding &&
            (draw.alpha_image == VK_NULL_HANDLE || draw.alpha_view == VK_NULL_HANDLE ||
             draw.alpha_sampler == VK_NULL_HANDLE || draw.alpha_material_buffer == VK_NULL_HANDLE ||
             draw.alpha_material_range == 0U || draw.alpha_layout == nullptr)) {
            pipelines.clear();
            descriptors.reset();
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            diagnostic = {"vulkan_depth_alpha_binding_invalid",
                          "The Vulkan depth-only alpha draw contains an incomplete descriptor binding"};
            return false;
        }
        pipelines.emplace_back();
        if (!create_batch_pipeline(context, render_pass, description.width, description.height,
                                   description.samples, *draw.program, false, true,
                                   has_alpha_tested_binding
                                       ? std::span<const VkDescriptorSetLayout>(&descriptors.layout, 1U)
                                       : std::span<const VkDescriptorSetLayout>{},
                                   true,
                                   pipelines.back(), diagnostic)) {
            pipelines.clear();
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            return false;
        }
    }

    struct AlphaImageState {
        VkImage image = VK_NULL_HANDLE;
        const VkImageLayout* tracked_layout = nullptr;
        VkImageLayout before = VK_IMAGE_LAYOUT_UNDEFINED;
        std::uint32_t mip_levels = 0U;
    };
    std::vector<AlphaImageState> alpha_images;
    if (has_alpha_tested_binding) {
        alpha_images.reserve(draws.size());
        for (const VulkanIndexedBatchDraw& draw : draws) {
            if (!draw.has_alpha_tested_binding) continue;
            const auto found = std::find_if(
                alpha_images.begin(), alpha_images.end(), [&](const AlphaImageState& state) {
                    return state.image == draw.alpha_image &&
                           state.tracked_layout == draw.alpha_layout;
                });
            if (found == alpha_images.end())
                alpha_images.push_back({draw.alpha_image, draw.alpha_layout,
                                        *draw.alpha_layout, draw.alpha_mip_levels});
        }
    }

    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1U;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(context->device, &allocation, &command);
    bool submitted = false;
    bool completed = false;
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
    }
    if (result == VK_SUCCESS &&
        depth_attachment.layout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = depth_attachment.layout();
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = depth_attachment.image();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.layerCount = 1U;
        barrier.srcAccessMask = depth_attachment.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                                    ? 0U
                                : depth_attachment.layout() ==
                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command,
                             depth_attachment.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                                 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                             : depth_attachment.layout() ==
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                 : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
    }
    if (result == VK_SUCCESS && !alpha_images.empty()) {
        std::vector<VkImageMemoryBarrier> barriers;
        barriers.reserve(alpha_images.size());
        for (const AlphaImageState& state : alpha_images) {
            if (state.before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = state.before;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = state.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = state.mip_levels;
            barrier.subresourceRange.layerCount = 1U;
            barrier.srcAccessMask = state.before == VK_IMAGE_LAYOUT_UNDEFINED
                                        ? 0U
                                        : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers.push_back(barrier);
        }
        if (!barriers.empty()) {
            const VkPipelineStageFlags source_stage = std::any_of(
                alpha_images.begin(), alpha_images.end(), [](const AlphaImageState& state) {
                    return state.before != VK_IMAGE_LAYOUT_UNDEFINED &&
                           state.before != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                })
                                                         ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            vkCmdPipelineBarrier(command, source_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0U, 0U, nullptr, 0U, nullptr,
                                 static_cast<std::uint32_t>(barriers.size()), barriers.data());
        }
    }
    if (result == VK_SUCCESS) {
        VkClearValue clear_value{};
        clear_value.depthStencil.depth = depth_clear_value;
        VkRenderPassBeginInfo render_begin{};
        render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_begin.renderPass = render_pass;
        render_begin.framebuffer = framebuffer;
        render_begin.renderArea.extent = {description.width, description.height};
        render_begin.clearValueCount = 1U;
        render_begin.pClearValues = &clear_value;
        vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
        for (std::size_t index = 0U; index < draws.size(); ++index) {
            const VulkanIndexedBatchDraw& draw = draws[index];
            const VulkanBatchPipeline& pipeline = pipelines[index];
            const VkBuffer vertex_buffer = draw.vertices->raw().buffer;
            const VkDeviceSize vertex_offset = draw.vertex_offset;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0U,
                               static_cast<std::uint32_t>(sizeof(DrawMatrices)), &draw.matrices);
            if (descriptor_sets.size() == draws.size() &&
                descriptor_sets[index] != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                                        0U, 1U, &descriptor_sets[index], 0U, nullptr);
            }
            vkCmdBindVertexBuffers(command, 0U, 1U, &vertex_buffer, &vertex_offset);
            vkCmdBindIndexBuffer(command, draw.indices->raw().buffer, draw.index_offset,
                                 VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(command, draw.index_count, 1U, 0U, 0, 0U);
        }
        vkCmdEndRenderPass(command);
        if (!alpha_images.empty()) {
            std::vector<VkImageMemoryBarrier> barriers;
            barriers.reserve(alpha_images.size());
            for (const AlphaImageState& state : alpha_images) {
                if (state.before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout = state.before;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = state.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = state.mip_levels;
                barrier.subresourceRange.layerCount = 1U;
                barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.dstAccessMask = state.before == VK_IMAGE_LAYOUT_UNDEFINED
                                            ? 0U
                                            : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barriers.push_back(barrier);
            }
            if (!barriers.empty()) {
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U,
                                     nullptr, static_cast<std::uint32_t>(barriers.size()),
                                     barriers.data());
            }
        }
        result = vkEndCommandBuffer(command);
    }
    VkFence fence = VK_NULL_HANDLE;
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            result = vkQueueSubmit(context->queue, 1U, &submit, fence);
            submitted = result == VK_SUCCESS;
            if (submitted) {
                result = vkWaitForFences(context->device, 1U, &fence, VK_TRUE, UINT64_MAX);
                completed = result == VK_SUCCESS;
            }
        }
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain == VK_SUCCESS) {
            depth_attachment.mark_rendered(clear_depth);
        } else {
            depth_attachment.mark_invalid();
            result = drain;
        }
    } else if (completed) {
        depth_attachment.mark_rendered(clear_depth);
    }
    if (fence != VK_NULL_HANDLE) vkDestroyFence(context->device, fence, nullptr);
    if (command != VK_NULL_HANDLE)
        vkFreeCommandBuffers(context->device, context->command_pool, 1U, &command);
    pipelines.clear();
    vkDestroyFramebuffer(context->device, framebuffer, nullptr);
    vkDestroyRenderPass(context->device, render_pass, nullptr);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan depth-only indexed batch execution", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_depth_only_execution_failed";
        return false;
    }
    diagnostic = {};
    return true;
}

class VulkanTexture final : public Texture {
public:
    VulkanTexture(std::shared_ptr<VulkanContext> context,
                  RawVulkanImage raw,
                  TextureDescription description,
                  VkImageLayout layout,
                  bool initialized)
        : context_(std::move(context)), raw_(std::move(raw)), info_({description}),
          layout_(layout), initialized_(initialized),
          base_mip_initialized_(
              static_cast<std::size_t>(
                  texture_physical_array_layers(description)),
              initialized ? 1U : 0U) {}

    ~VulkanTexture() override = default;
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const TextureInfo& info() const noexcept override { return info_; }
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }
    VkImage image() const noexcept { return raw_.image; }
    VkImageView view() const noexcept { return raw_.view; }
    VkImageLayout layout() const noexcept { return layout_; }
    const VkImageLayout* layout_ptr() const noexcept { return &layout_; }
    VkImageLayout* mutable_layout_ptr() noexcept { return &layout_; }
    bool initialized() const noexcept { return initialized_; }
    void mark_initialized() noexcept {
        initialized_ = true;
        if (!base_mip_initialized_.empty()) base_mip_initialized_[0U] = 1U;
    }
    void mark_layout(VkImageLayout layout) noexcept { layout_ = layout; }
    bool base_mip_initialized() const noexcept {
        return !base_mip_initialized_.empty() &&
               std::all_of(base_mip_initialized_.begin(),
                           base_mip_initialized_.end(),
                           [](std::uint8_t value) { return value != 0U; });
    }

    bool upload(const TextureUploadPlan& uploads, Diagnostic& diagnostic) {
        const bool uploaded = copy_texture_uploads(context_, raw_, info_.description, uploads,
                                                   layout_, diagnostic);
        initialized_ = initialized_ || (uploaded && !uploads.subresources.empty());
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

    bool generate_mips(Diagnostic& diagnostic) {
        return generate_texture_mips(context_, raw_, info_.description,
                                     layout_, diagnostic);
    }

    bool clear_readback(const TextureClearReadbackRequest& request,
                        std::vector<std::byte>& output,
                        Diagnostic& diagnostic) {
        const bool cleared = clear_texture_and_readback(context_, raw_, info_.description, request,
                                                        layout_, output, diagnostic);
        initialized_ = initialized_ || cleared;
        if (cleared && !base_mip_initialized_.empty())
            base_mip_initialized_[0U] = 1U;
        return cleared;
    }

    bool draw_triangle(const TriangleDrawRequest& request,
                       std::vector<std::byte>& output,
                       Diagnostic& diagnostic) {
        const bool drawn = draw_triangle_and_readback(context_, raw_, info_.description, request,
                                                      layout_, output, diagnostic);
        initialized_ = initialized_ || drawn;
        return drawn;
    }

    bool draw_indexed_static_mesh(const IndexedStaticMeshDrawRequest& request,
                                  const VulkanBuffer& vertex_buffer,
                                  const VulkanBuffer& index_buffer,
                                  std::vector<std::byte>& output,
                                  Diagnostic& diagnostic) {
        if (request.load_color && !initialized_) {
            diagnostic = {"indexed_color_load_before_clear",
                          "A color load was requested before the color attachment was initialized"};
            return false;
        }
        const DrawPacket& packet = *request.packet;
        const std::uint64_t vertex_offset = static_cast<std::uint64_t>(packet.vertex_offset) *
                                            request.pipeline->vertex_layout.stride;
        const std::uint64_t index_offset = static_cast<std::uint64_t>(packet.index_offset) * sizeof(std::uint16_t);
        if (vertex_offset > std::numeric_limits<VkDeviceSize>::max() ||
            index_offset > std::numeric_limits<VkDeviceSize>::max()) {
            diagnostic = {"indexed_static_mesh_offset_overflow", "Indexed static-mesh offsets exceed Vulkan addressability"};
            return false;
        }
        const VulkanDrawGeometry geometry{vertex_buffer.raw(),
                                          static_cast<VkDeviceSize>(vertex_offset),
                                          &index_buffer.raw(),
                                          static_cast<VkDeviceSize>(index_offset), 0U,
                                          packet.index_count, VK_INDEX_TYPE_UINT16};
        const bool stock_vulkan_source =
            request.shader_authority ==
            IndexedShaderAuthority::
                explicit_stock_ks_per_pixel_vulkan_source_equivalent;
        const bool stock_native_abi = stock_vulkan_source ||
            request.shader_authority ==
                IndexedShaderAuthority::
                    explicit_stock_ks_per_pixel_vulkan_abi_probe;
        DrawMatrices draw_matrices{};
        if (!stock_native_abi)
            draw_matrices = {packet.world_matrix, request.camera_frame->view_projection};
        VulkanDepthAttachment* depth_attachment = nullptr;
        if (request.depth_attachment != nullptr) {
            depth_attachment = dynamic_cast<VulkanDepthAttachment*>(request.depth_attachment);
            if (depth_attachment == nullptr) {
                diagnostic = {"indexed_depth_attachment_type_unsupported",
                              "The Vulkan device received an unknown depth attachment handle"};
                return false;
            }
            if (depth_attachment->context().get() != context_.get()) {
                diagnostic = {"indexed_depth_attachment_context_mismatch",
                              "The depth attachment belongs to another Vulkan device"};
                return false;
            }
        }
        if (stock_native_abi) {
            VulkanIndexedBatchDraw draw;
            draw.program = request.pipeline;
            draw.vertices = &vertex_buffer;
            draw.indices = &index_buffer;
            draw.vertex_offset = static_cast<VkDeviceSize>(vertex_offset);
            draw.index_offset = static_cast<VkDeviceSize>(index_offset);
            draw.index_count = packet.index_count;
            if (!prepare_vulkan_stock_ks_per_pixel_native_abi_binding(
                    request, context_, draw, diagnostic)) {
                if (stock_vulkan_source) {
                    constexpr std::string_view prefix =
                        "vulkan_stock_abi_probe_";
                    if (diagnostic.code.starts_with(prefix)) {
                        diagnostic.code = "vulkan_stock_source_" +
                            diagnostic.code.substr(prefix.size());
                    }
                }
                return false;
            }
            const std::array<VulkanIndexedBatchDraw, 1> draws = {draw};
            const bool drawn = draw_indexed_batch_and_readback(
                context_, raw_, info_.description, TextureTargetSubresource{},
                draws, nullptr, nullptr, nullptr, nullptr, nullptr, depth_attachment, request.load_color,
                request.clear_depth, request.depth_clear_value, request.clear_color, layout_,
                nullptr, nullptr, nullptr,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1U, true, output,
                diagnostic);
            initialized_ = initialized_ || drawn;
            if (drawn && !base_mip_initialized_.empty())
                base_mip_initialized_[0U] = 1U;
            return drawn;
        }
        if (!request.pipeline->resources.empty()) {
            VulkanIndexedBatchDraw draw;
            draw.program = request.pipeline;
            draw.vertices = &vertex_buffer;
            draw.indices = &index_buffer;
            draw.vertex_offset = static_cast<VkDeviceSize>(vertex_offset);
            draw.index_offset = static_cast<VkDeviceSize>(index_offset);
            draw.index_count = packet.index_count;
            draw.matrices = {packet.world_matrix, request.camera_frame->view_projection};
            if (!prepare_vulkan_sampled_binding(request, context_, draw, diagnostic)) return false;
            const std::array<VulkanIndexedBatchDraw, 1> draws = {draw};
            const bool drawn = draw_indexed_batch_and_readback(
                context_, raw_, info_.description, TextureTargetSubresource{},
                draws, nullptr, nullptr, nullptr, nullptr, nullptr, depth_attachment, request.load_color,
                request.clear_depth, request.depth_clear_value, request.clear_color, layout_,
                nullptr, nullptr, nullptr,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1U, true, output,
                diagnostic);
            initialized_ = initialized_ || drawn;
            if (drawn && !base_mip_initialized_.empty())
                base_mip_initialized_[0U] = 1U;
            return drawn;
        }
        const bool drawn = draw_graphics_and_readback(
            context_, raw_, info_.description, *request.pipeline, geometry, &draw_matrices,
            depth_attachment, request.load_color, request.clear_depth, request.depth_clear_value,
            request.clear_color, layout_, output, diagnostic);
        initialized_ = initialized_ || drawn;
        if (drawn && !base_mip_initialized_.empty())
            base_mip_initialized_[0U] = 1U;
        return drawn;
    }

    bool draw_indexed_static_mesh_batch(const IndexedStaticMeshBatchDescription& description,
                                        VulkanDepthAttachment* depth_attachment,
                                        VulkanTexture* resolve_target,
                                        std::vector<std::byte>& output,
                                        Diagnostic& diagnostic) {
        if (description.load_color && !initialized_) {
            diagnostic = {"indexed_color_load_before_clear",
                          "A color load was requested before the color attachment was initialized"};
            return false;
        }
        std::vector<VulkanIndexedBatchDraw> draws;
        draws.reserve(description.draws.size() +
                      description.selected_mesh_draws.size() +
                      description.overlay_draws.size());
        const auto append_scene_draw = [&](const IndexedStaticMeshDrawRequest& request) {
            auto* vertices = dynamic_cast<const VulkanBuffer*>(request.vertex_buffer);
            auto* indices = dynamic_cast<const VulkanBuffer*>(request.index_buffer);
            const bool stock_vulkan_source =
                request.shader_authority ==
                IndexedShaderAuthority::
                    explicit_stock_ks_per_pixel_vulkan_source_equivalent;
            const bool stock_native_abi = stock_vulkan_source ||
                request.shader_authority ==
                    IndexedShaderAuthority::
                        explicit_stock_ks_per_pixel_vulkan_abi_probe;
            if (vertices == nullptr || indices == nullptr || request.packet == nullptr || request.pipeline == nullptr ||
                (!stock_native_abi && !request.camera_frame.has_value())) {
                diagnostic = {"indexed_static_mesh_batch_resource_invalid",
                              "The Vulkan indexed batch contains an unknown or incomplete draw resource"};
                return false;
            }
            if (vertices->context() != context_ || indices->context() != context_) {
                diagnostic = {"indexed_static_mesh_context_mismatch",
                              "Indexed static-mesh resources must be owned by the same Vulkan device"};
                return false;
            }
            const DrawPacket& packet = *request.packet;
            const std::uint64_t vertex_offset = static_cast<std::uint64_t>(packet.vertex_offset) *
                                                request.pipeline->vertex_layout.stride;
            const std::uint64_t index_offset = static_cast<std::uint64_t>(packet.index_offset) * sizeof(std::uint16_t);
            if (vertex_offset > std::numeric_limits<VkDeviceSize>::max() ||
                index_offset > std::numeric_limits<VkDeviceSize>::max()) {
                diagnostic = {"indexed_static_mesh_offset_overflow",
                              "Indexed static-mesh offsets exceed Vulkan addressability"};
                return false;
            }
            VulkanIndexedBatchDraw draw;
            draw.program = request.pipeline;
            draw.vertices = vertices;
            draw.indices = indices;
            draw.vertex_offset = static_cast<VkDeviceSize>(vertex_offset);
            draw.index_offset = static_cast<VkDeviceSize>(index_offset);
            draw.index_count = packet.index_count;
            draw.transparent = packet.flags.transparent ||
                               packet.flags.blend_enabled ||
                               request.pipeline->blend.enabled;
            if (stock_native_abi) {
                if (!prepare_vulkan_stock_ks_per_pixel_native_abi_binding(
                        request, context_, draw, diagnostic)) {
                    if (stock_vulkan_source) {
                        constexpr std::string_view prefix =
                            "vulkan_stock_abi_probe_";
                        if (diagnostic.code.starts_with(prefix)) {
                            diagnostic.code = "vulkan_stock_source_" +
                                diagnostic.code.substr(prefix.size());
                        }
                    }
                    return false;
                }
            } else {
                draw.matrices = {packet.world_matrix,
                                 request.camera_frame->view_projection};
                if (!prepare_vulkan_sampled_binding(
                        request, context_, draw, diagnostic))
                    return false;
            }
            draws.push_back(draw);
            return true;
        };
        const auto append_selected_draw = [&](const SelectedMeshDrawRequest& request) {
            auto* vertices = dynamic_cast<const VulkanBuffer*>(request.vertex_buffer);
            auto* indices = dynamic_cast<const VulkanBuffer*>(request.index_buffer);
            auto* color = dynamic_cast<const VulkanBuffer*>(request.color_buffer);
            if (vertices == nullptr || indices == nullptr || request.packet == nullptr ||
                request.pipeline == nullptr || color == nullptr) {
                diagnostic = {"selected_mesh_resource_invalid",
                              "The Vulkan selected-mesh draw contains an unknown or incomplete resource"};
                return false;
            }
            if (vertices->context() != context_ || indices->context() != context_ ||
                color->context() != context_) {
                diagnostic = {"selected_mesh_context_mismatch",
                              "Selected-mesh resources must belong to the same Vulkan device"};
                return false;
            }
            const BufferDescription& color_description = color->info().description;
            if (request.color_offset_bytes %
                        context_->min_uniform_buffer_offset_alignment !=
                    0U ||
                request.color_range_bytes > context_->max_uniform_buffer_range ||
                request.color_offset_bytes > color_description.size_bytes ||
                request.color_range_bytes >
                    color_description.size_bytes - request.color_offset_bytes ||
                color->raw().buffer == VK_NULL_HANDLE) {
                diagnostic = {"selected_mesh_uniform_device_limit",
                              "The selected-mesh color view exceeds Vulkan uniform-buffer limits"};
                return false;
            }
            const std::uint64_t vertex_offset =
                static_cast<std::uint64_t>(request.packet->vertex_offset) *
                (11U * sizeof(float));
            const std::uint64_t index_offset =
                static_cast<std::uint64_t>(request.packet->index_offset) *
                sizeof(std::uint16_t);
            if (vertex_offset > std::numeric_limits<VkDeviceSize>::max() ||
                index_offset > std::numeric_limits<VkDeviceSize>::max()) {
                diagnostic = {"selected_mesh_offset_overflow",
                              "Selected-mesh offsets exceed Vulkan addressability"};
                return false;
            }
            VulkanIndexedBatchDraw draw;
            draw.program = request.pipeline;
            draw.vertices = vertices;
            draw.indices = indices;
            draw.vertex_offset = static_cast<VkDeviceSize>(vertex_offset);
            draw.index_offset = static_cast<VkDeviceSize>(index_offset);
            draw.index_count = request.packet->index_count;
            draw.matrices = request.matrices;
            draw.has_selected_color = true;
            draw.selected_color_buffer = color->raw().buffer;
            draw.selected_color_offset =
                static_cast<VkDeviceSize>(request.color_offset_bytes);
            draw.selected_color_range =
                static_cast<VkDeviceSize>(request.color_range_bytes);
            draws.push_back(draw);
            return true;
        };
        const auto append_overlay_draw = [&](const OverlayLineDrawRequest& request) {
            auto* vertices = dynamic_cast<const VulkanBuffer*>(request.vertex_buffer);
            if (vertices == nullptr || request.pipeline == nullptr) {
                diagnostic = {"overlay_line_resource_invalid",
                              "The Vulkan overlay batch contains an unknown or incomplete resource"};
                return false;
            }
            if (vertices->context() != context_) {
                diagnostic = {"overlay_line_context_mismatch",
                              "Overlay line buffers must be owned by the same Vulkan device"};
                return false;
            }
            if (request.vertex_offset_bytes >
                std::numeric_limits<VkDeviceSize>::max()) {
                diagnostic = {"overlay_line_offset_overflow",
                              "Overlay line offset exceeds Vulkan addressability"};
                return false;
            }
            VulkanIndexedBatchDraw draw;
            draw.program = request.pipeline;
            draw.vertices = vertices;
            draw.vertex_offset =
                static_cast<VkDeviceSize>(request.vertex_offset_bytes);
            draw.vertex_count = request.vertex_count;
            draw.indexed = false;
            draw.matrices = request.matrices;
            draws.push_back(draw);
            return true;
        };
        if (!visit_indexed_static_mesh_batch_draws(
                description, append_scene_draw, append_selected_draw,
                append_overlay_draw))
            return false;
        std::optional<PortableSkyShaderConstants> sky_constants;
        if (description.sky.has_value()) {
            sky_constants.emplace();
            if (!build_portable_sky_shader_constants(
                    *description.sky, *sky_constants, diagnostic))
                return false;
        }
        std::optional<PortableCloudShaderConstants> cloud_constants;
        if (description.clouds.has_value()) {
            cloud_constants.emplace();
            if (!build_portable_cloud_shader_constants(
                    *description.clouds, *cloud_constants, diagnostic))
                return false;
        }
        std::optional<VulkanPortableCloudResources> cloud_resources;
        if (description.clouds.has_value()) {
            const PortableCloudParameters& parameters = *description.clouds;
            const VulkanBuffer* vertices = nullptr;
            if (!parameters.texture_runs.empty())
                vertices = dynamic_cast<const VulkanBuffer*>(
                    parameters.vertex_buffer);
            if (parameters.texture_runs.size() > portable_cloud_max_count ||
                (!parameters.texture_runs.empty() &&
                 (vertices == nullptr ||
                  vertices->raw().buffer == VK_NULL_HANDLE ||
                  (static_cast<std::uint32_t>(
                       vertices->info().description.usage) &
                   static_cast<std::uint32_t>(BufferUsage::vertex)) == 0U))) {
                diagnostic = {"vulkan_portable_cloud_resource_invalid",
                              "Vulkan cloud vertices must be an owned vertex resource"};
                return false;
            }
            cloud_resources.emplace();
            if (vertices != nullptr)
                cloud_resources->vertex_buffer = vertices->raw().buffer;
            cloud_resources->runs.reserve(parameters.texture_runs.size());
            for (const Texture* requested_texture : parameters.textures) {
                if (requested_texture == nullptr)
                    continue;
                const auto* texture = dynamic_cast<const VulkanTexture*>(
                    requested_texture);
                if (texture == nullptr || texture->context() != context_) {
                    diagnostic = {"vulkan_portable_cloud_texture_invalid",
                                  "Vulkan cloud textures must be owned Vulkan resources"};
                    return false;
                }
            }
            for (const PortableCloudTextureRun& source_run : parameters.texture_runs) {
                if (source_run.vertex_count == 0U ||
                    source_run.first_vertex >
                        std::numeric_limits<std::uint32_t>::max() -
                            source_run.vertex_count ||
                    static_cast<std::uint64_t>(source_run.first_vertex +
                                               source_run.vertex_count) *
                            portable_cloud_vertex_stride_bytes >
                        vertices->info().description.size_bytes) {
                    diagnostic = {"vulkan_portable_cloud_vertex_range_invalid",
                                  "A Vulkan cloud texture run exceeds its vertex buffer"};
                    return false;
                }
                if (source_run.texture >= parameters.textures.size()) {
                    diagnostic = {"vulkan_portable_cloud_texture_slot_invalid",
                                  "A Vulkan cloud texture run names an invalid texture slot"};
                    return false;
                }
                const auto* texture = dynamic_cast<const VulkanTexture*>(
                    parameters.textures[source_run.texture]);
                if (texture == nullptr) {
                    // Null slots skip only the run that names them.
                    if (parameters.textures[source_run.texture] != nullptr) {
                        diagnostic = {"vulkan_portable_cloud_texture_invalid",
                                      "Vulkan cloud textures must be Vulkan resources"};
                        return false;
                    }
                    continue;
                }
                const TextureDescription& texture_description =
                    texture->info().description;
                if (texture->context() != context_ || !texture->initialized() ||
                    texture->image() == VK_NULL_HANDLE ||
                    texture->view() == VK_NULL_HANDLE ||
                    texture->layout() == VK_IMAGE_LAYOUT_UNDEFINED ||
                    texture_description.samples != 1U ||
                    texture_description.array_layers != 1U ||
                    texture_description.mip_levels == 0U ||
                    (static_cast<std::uint32_t>(texture_description.usage) &
                     static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                    diagnostic = {"vulkan_portable_cloud_texture_invalid",
                                  "Vulkan cloud textures must be initialized single-layer sampled images"};
                    return false;
                }
                cloud_resources->runs.push_back({
                    source_run.first_vertex, source_run.vertex_count,
                    texture->image(), texture->view(),
                    const_cast<VulkanTexture*>(texture)->mutable_layout_ptr(),
                    texture->layout(), texture_description.mip_levels});
            }
            if (!cloud_resources->runs.empty()) {
                VkSampler raw_sampler = VK_NULL_HANDLE;
                if (!extract_vulkan_sampler(parameters.sampler, context_,
                                            raw_sampler, diagnostic) ||
                    raw_sampler == VK_NULL_HANDLE) {
                    diagnostic = {"vulkan_portable_cloud_resource_invalid",
                                  "Drawable Vulkan clouds require an owned sampler"};
                    return false;
                }
                cloud_resources->sampler = raw_sampler;
            }
        }
        std::optional<PortableGrassShaderConstants> grass_constants;
        if (description.grass.has_value()) {
            grass_constants.emplace();
            if (!build_portable_grass_shader_constants(
                    *description.grass, *grass_constants, diagnostic))
                return false;
        }
        std::optional<VulkanPortableGrassResources> grass_resources;
        if (description.grass.has_value()) {
            const PortableGrassParameters& parameters = *description.grass;
            grass_resources.emplace();
            grass_resources->vertex_count = parameters.vertex_count;
            if (parameters.vertex_count != 0U) {
                const auto* vertices = dynamic_cast<const VulkanBuffer*>(
                    parameters.vertex_buffer);
                if (vertices == nullptr || vertices->context() != context_ ||
                    vertices->raw().buffer == VK_NULL_HANDLE ||
                    !any(vertices->info().description.usage & BufferUsage::vertex)) {
                    diagnostic = {"vulkan_portable_grass_vertex_resource_invalid",
                                  "Vulkan grass vertices must be an owned vertex resource"};
                    return false;
                }
                const std::uint64_t required_bytes =
                    static_cast<std::uint64_t>(parameters.vertex_count) *
                    portable_grass_vertex_stride_bytes;
                if (required_bytes > vertices->info().description.size_bytes) {
                    diagnostic = {"vulkan_portable_grass_vertex_range_invalid",
                                  "Vulkan grass vertices exceed their buffer range"};
                    return false;
                }
                grass_resources->vertex_buffer = vertices->raw().buffer;

                const auto* atlas = dynamic_cast<const VulkanTexture*>(
                    parameters.atlas);
                if (atlas == nullptr || atlas->context() != context_ ||
                    !atlas->initialized() || atlas->image() == VK_NULL_HANDLE ||
                    atlas->view() == VK_NULL_HANDLE ||
                    atlas->layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
                    diagnostic = {"vulkan_portable_grass_atlas_invalid",
                                  "Vulkan grass requires an initialized owned atlas"};
                    return false;
                }
                const TextureDescription& atlas_description =
                    atlas->info().description;
                const bool sampled_color_format =
                    atlas_description.format == TextureFormat::rgba8_unorm ||
                    atlas_description.format == TextureFormat::rgba8_srgb ||
                    atlas_description.format == TextureFormat::bgra8_unorm ||
                    atlas_description.format == TextureFormat::bgra8_srgb;
                if (atlas_description.shape != TextureShape::texture_2d ||
                    atlas_description.array_layers != 1U ||
                    atlas_description.samples != 1U ||
                    atlas_description.mip_levels == 0U ||
                    (static_cast<std::uint32_t>(atlas_description.usage) &
                     static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
                    !sampled_color_format) {
                    diagnostic = {"vulkan_portable_grass_atlas_invalid",
                                  "Vulkan grass atlas must be a single-layer sampled color image"};
                    return false;
                }
                grass_resources->atlas_image = atlas->image();
                grass_resources->atlas_view = atlas->view();
                grass_resources->tracked_layout =
                    const_cast<VulkanTexture*>(atlas)->mutable_layout_ptr();
                grass_resources->original_layout = atlas->layout();
                grass_resources->mip_levels = atlas_description.mip_levels;
                VkSampler raw_sampler = VK_NULL_HANDLE;
                if (!extract_vulkan_sampler(parameters.sampler, context_,
                                            raw_sampler, diagnostic) ||
                    raw_sampler == VK_NULL_HANDLE) {
                    diagnostic = {"vulkan_portable_grass_sampler_invalid",
                                  "Drawable Vulkan grass requires an owned sampler"};
                    return false;
                }
                grass_resources->sampler = raw_sampler;
            }
        }
        const bool drawn = draw_indexed_batch_and_readback(
            context_, raw_, info_.description, description.target_subresource,
            draws, sky_constants.has_value() ? &*sky_constants : nullptr,
            cloud_constants.has_value() ? &*cloud_constants : nullptr,
            cloud_resources.has_value() ? &*cloud_resources : nullptr,
            grass_constants.has_value() ? &*grass_constants : nullptr,
            grass_resources.has_value() ? &*grass_resources : nullptr,
            depth_attachment, description.load_color,
            description.clear_depth, description.depth_clear_value, description.clear_color,
            layout_, resolve_target != nullptr ? &resolve_target->raw_ : nullptr,
            resolve_target != nullptr ? &resolve_target->layout_ : nullptr,
            resolve_target != nullptr ? &resolve_target->initialized_ : nullptr,
            resolve_target != nullptr
                ? texture_final_layout(
                      resolve_target->info().description.usage,
                      resolve_target->info().description.access_policy)
                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            resolve_target != nullptr
                ? resolve_target->info().description.mip_levels
                : 1U,
            description.capture_rgba8, output, diagnostic);
        initialized_ = initialized_ || drawn;
        if (drawn) {
            const std::size_t layer = static_cast<std::size_t>(
                texture_target_physical_array_layer(
                    info_.description, description.target_subresource));
            if (layer < base_mip_initialized_.size())
                base_mip_initialized_[layer] = 1U;
            if (resolve_target != nullptr &&
                !resolve_target->base_mip_initialized_.empty())
                resolve_target->base_mip_initialized_[0U] = 1U;
        }
        return drawn;
    }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanImage raw_;
    TextureInfo info_;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized_ = false;
    std::vector<std::uint8_t> base_mip_initialized_;
};

struct VulkanBloomTargets {
    // Each retained level has a ping-pong pair: A is the downsample/vertical
    // target and B is the horizontal blur target.
    std::array<RawVulkanImage, 10U> images;
    std::array<std::uint32_t, 5U> widths{};
    std::array<std::uint32_t, 5U> heights{};
};

struct VulkanBloomUniform {
    std::array<float, 4U> mode_exposure_threshold_remap{};
    std::array<float, 4U> direction_sample_count_texel_size{};
    std::array<std::array<float, 4U>, 4U> offsets{};
    std::array<std::array<float, 4U>, 4U> weights{};
};
static_assert(sizeof(VulkanBloomUniform) == 160U);

bool generate_vulkan_bloom(const std::shared_ptr<VulkanContext>& context,
                           VulkanTexture& source,
                           float source_exposure,
                           const HdrBloomParameters& parameters,
                           VulkanBloomTargets& output,
                           Diagnostic& diagnostic) {
    constexpr std::uint32_t level_count = 5U;
    constexpr std::uint32_t pass_count = 15U;
    if (context == nullptr || context->device == VK_NULL_HANDLE ||
        context->queue == VK_NULL_HANDLE || context->command_pool == VK_NULL_HANDLE) {
        diagnostic = {"vulkan_hdr_bloom_uninitialized",
                      "HDR bloom requires an initialized Vulkan device context"};
        return false;
    }
    if (source.image() == VK_NULL_HANDLE || source.view() == VK_NULL_HANDLE ||
        source.layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
        diagnostic = {"hdr_bloom_source_uninitialized",
                      "HDR bloom requires an initialized source texture"};
        return false;
    }
    if (!validate_hdr_bloom_resource_budget(
            source.info().description.width, source.info().description.height,
            0U, diagnostic)) {
        diagnostic.message = "Vulkan " + diagnostic.message;
        return false;
    }

    TextureDescription bloom_description;
    bloom_description.width = std::max(1U, (source.info().description.width + 3U) / 4U);
    bloom_description.height = std::max(1U, (source.info().description.height + 3U) / 4U);
    bloom_description.format = TextureFormat::rgba16_sfloat;
    bloom_description.usage = TextureUsage::sampled | TextureUsage::color_attachment;
    bloom_description.mutability = TextureMutability::mutable_data;
    bloom_description.access_policy = TextureAccessPolicy::render_then_sample;
    for (std::uint32_t level = 0U; level < level_count; ++level) {
        output.widths[level] = bloom_description.width;
        output.heights[level] = bloom_description.height;
        for (std::uint32_t side = 0U; side < 2U; ++side) {
            const std::size_t index = static_cast<std::size_t>(level * 2U + side);
            if (!create_raw_image(context, bloom_description, output.images[index], diagnostic)) {
                diagnostic.code = diagnostic.code.empty() ? "vulkan_hdr_bloom_unsupported" : diagnostic.code;
                return false;
            }
        }
        bloom_description.width = std::max(1U, (bloom_description.width + 1U) / 2U);
        bloom_description.height = std::max(1U, (bloom_description.height + 1U) / 2U);
    }

    VkShaderModule shader = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::array<VkFramebuffer, level_count * 2U> framebuffers{};
    std::array<VkDescriptorSet, pass_count> descriptor_sets{};
    std::array<RawVulkanBuffer, pass_count> uniforms;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(context->device, fence, nullptr);
        if (command != VK_NULL_HANDLE)
            vkFreeCommandBuffers(context->device, context->command_pool, 1U, &command);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(context->device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(context->device, pipeline_layout, nullptr);
        for (VkFramebuffer framebuffer : framebuffers)
            if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        if (render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(context->device, render_pass, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(context->device, descriptor_pool, nullptr);
        if (descriptor_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(context->device, descriptor_layout, nullptr);
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(context->device, sampler, nullptr);
        if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(context->device, shader, nullptr);
        if (vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(context->device, vertex_shader, nullptr);
    };
    const auto create_shader = [&](const std::uint32_t* bytes, std::size_t size) -> VkResult {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = bytes;
        return vkCreateShaderModule(context->device, &info, nullptr, &shader);
    };
    VkResult result = create_shader(generated::hdr_bloom_frag_spirv,
                                    generated::hdr_bloom_frag_spirv_size);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateShaderModule(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    VkShaderModuleCreateInfo vertex_shader_info{};
    vertex_shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertex_shader_info.codeSize = generated::hdr_tone_map_vertex_spirv_size;
    vertex_shader_info.pCode = reinterpret_cast<const std::uint32_t*>(
        generated::hdr_tone_map_vertex_spirv);
    result = vkCreateShaderModule(context->device, &vertex_shader_info, nullptr, &vertex_shader);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateShaderModule(hdr bloom vertex)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    VkFormatProperties bloom_format_properties{};
    vkGetPhysicalDeviceFormatProperties(context->physical_device,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        &bloom_format_properties);
    const VkFilter bloom_filter =
        (bloom_format_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U
            ? VK_FILTER_LINEAR
            : VK_FILTER_NEAREST;
    sampler_info.magFilter = bloom_filter;
    sampler_info.minFilter = bloom_filter;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0F;
    result = vkCreateSampler(context->device, &sampler_info, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateSampler(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 2U> bindings{};
    bindings[0] = {0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    descriptor_info.pBindings = bindings.data();
    result = vkCreateDescriptorSetLayout(context->device, &descriptor_info, nullptr,
                                         &descriptor_layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorSetLayout(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    std::array<VkDescriptorPoolSize, 2U> pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pass_count},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, pass_count}};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = pass_count;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    result = vkCreateDescriptorPool(context->device, &pool_info, nullptr, &descriptor_pool);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateDescriptorPool(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    std::array<VkDescriptorSetLayout, pass_count> layouts{};
    layouts.fill(descriptor_layout);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptor_pool;
    allocation.descriptorSetCount = pass_count;
    allocation.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(context->device, &allocation, descriptor_sets.data());
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkAllocateDescriptorSets(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    VkBufferUsageFlags uniform_usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    BufferDescription uniform_description;
    uniform_description.size_bytes = sizeof(VulkanBloomUniform);
    uniform_description.usage = BufferUsage::uniform;
    uniform_description.memory = BufferMemory::host_visible;
    uniform_description.mutability = BufferMutability::mutable_data;
    for (RawVulkanBuffer& uniform : uniforms) {
        if (!create_raw_buffer(context, uniform_description, uniform_usage,
                               BufferMemory::host_visible, uniform, diagnostic)) {
            diagnostic.code = diagnostic.code.empty() ? "vulkan_hdr_bloom_uniform_failed" : diagnostic.code;
            cleanup();
            return false;
        }
    }

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo render_info{};
    render_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_info.attachmentCount = 1U;
    render_info.pAttachments = &attachment;
    render_info.subpassCount = 1U;
    render_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(context->device, &render_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateRenderPass(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    for (std::uint32_t level = 0U; level < level_count; ++level) {
        for (std::uint32_t side = 0U; side < 2U; ++side) {
            const std::size_t index = static_cast<std::size_t>(level * 2U + side);
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = render_pass;
            framebuffer_info.attachmentCount = 1U;
            framebuffer_info.pAttachments = &output.images[index].view;
            framebuffer_info.width = output.widths[level];
            framebuffer_info.height = output.heights[level];
            framebuffer_info.layers = 1U;
            result = vkCreateFramebuffer(context->device, &framebuffer_info, nullptr,
                                         &framebuffers[index]);
            if (result != VK_SUCCESS) {
                diagnostic = vk_error("vkCreateFramebuffer(hdr bloom)", result);
                diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
                cleanup();
                return false;
            }
        }
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1U;
    layout_info.pSetLayouts = &descriptor_layout;
    result = vkCreatePipelineLayout(context->device, &layout_info, nullptr, &pipeline_layout);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreatePipelineLayout(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1U;
    viewport_state.scissorCount = 1U;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1U;
    blend.pAttachments = &blend_attachment;
    const std::array<VkDynamicState, 2U> dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2U;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    result = vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1U, &pipeline_info,
                                       nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateGraphicsPipelines(hdr bloom)", result);
        diagnostic.code = "vulkan_hdr_bloom_pipeline_failed";
        cleanup();
        return false;
    }

    const auto state_for = [](VkImageLayout layout) {
        if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        if (layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        return std::pair<VkPipelineStageFlags, VkAccessFlags>{
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U};
    };
    const auto transition = [&](VkCommandBuffer target, VkImage image,
                                VkImageLayout old_layout, VkImageLayout new_layout) {
        if (old_layout == new_layout) return;
        const auto [source_stage, source_access] = state_for(old_layout);
        const auto [destination_stage, destination_access] = state_for(new_layout);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.layerCount = 1U;
        vkCmdPipelineBarrier(target, source_stage, destination_stage, 0U, 0U, nullptr,
                             0U, nullptr, 1U, &barrier);
    };
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = context->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1U;
    result = vkAllocateCommandBuffers(context->device, &command_info, &command);
    if (result == VK_SUCCESS) {
    VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
    }
    const VkImageLayout source_old_layout = source.layout();
    std::array<VkImageLayout, level_count * 2U> output_layouts{};
    output_layouts.fill(VK_IMAGE_LAYOUT_UNDEFINED);
    if (result == VK_SUCCESS) {
        transition(command, source.image(), source_old_layout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        std::uint32_t descriptor_index = 0U;
        const auto record_pass = [&](std::uint32_t level, std::uint32_t side,
                                     VkImageView input_view, std::uint32_t mode,
                                     float exposure, float direction_x, float direction_y,
                                     const BloomKernelMetadata* kernel) -> bool {
            VulkanBloomUniform values{};
            values.mode_exposure_threshold_remap = {
                static_cast<float>(mode), exposure, parameters.threshold, parameters.remap};
            values.direction_sample_count_texel_size = {
                direction_x, direction_y,
                kernel == nullptr ? 0.0F : static_cast<float>(kernel->sample_count), 0.0F};
            if (kernel != nullptr) {
                for (std::size_t i = 0U; i < kernel->offsets.size(); ++i) {
                    values.offsets[i / 4U][i & 3U] = kernel->offsets[i];
                    values.weights[i / 4U][i & 3U] = kernel->weights[i];
                }
            }
            if (!write_host_buffer(context, uniforms[descriptor_index], 0U,
                                   std::span<const std::byte>(
                                       reinterpret_cast<const std::byte*>(&values), sizeof(values)),
                                   diagnostic)) return false;
            VkDescriptorImageInfo source_info{sampler, input_view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorBufferInfo buffer_info{uniforms[descriptor_index].buffer, 0U, sizeof(values)};
            std::array<VkWriteDescriptorSet, 2U> writes{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptor_sets[descriptor_index], 0U, 0U, 1U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source_info, nullptr, nullptr};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptor_sets[descriptor_index], 1U, 0U, 1U,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buffer_info, nullptr};
            vkUpdateDescriptorSets(context->device, writes.size(), writes.data(), 0U, nullptr);
            const std::size_t output_index = static_cast<std::size_t>(level * 2U + side);
            transition(command, output.images[output_index].image,
                       output_layouts[output_index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderPassBeginInfo render_begin{};
            render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_begin.renderPass = render_pass;
            render_begin.framebuffer = framebuffers[output_index];
            render_begin.renderArea = {{0, 0}, {output.widths[level], output.heights[level]}};
            vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{0.0F, 0.0F, static_cast<float>(output.widths[level]),
                                static_cast<float>(output.heights[level]), 0.0F, 1.0F};
            VkRect2D scissor{{0, 0}, {output.widths[level], output.heights[level]}};
            vkCmdSetViewport(command, 0U, 1U, &viewport);
            vkCmdSetScissor(command, 0U, 1U, &scissor);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                    0U, 1U, &descriptor_sets[descriptor_index], 0U, nullptr);
            vkCmdDraw(command, 3U, 1U, 0U, 0U);
            vkCmdEndRenderPass(command);
            output_layouts[output_index] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ++descriptor_index;
            return true;
        };
        if (record_pass(0U, 0U, source.view(), 0U, source_exposure, 0.0F, 0.0F,
                        nullptr)) {
            for (std::uint32_t level = 0U; level < level_count; ++level) {
                if (level != 0U &&
                    !record_pass(level, 0U, output.images[(level - 1U) * 2U].view,
                                 1U, 0.0F, 0.0F, 0.0F, nullptr)) break;
                const BloomKernelMetadata kernel = ksEditorBloomGaussianKernel(
                    static_cast<std::int32_t>(level), parameters.kernel_threshold,
                    parameters.radius_scale, parameters.source_level,
                    parameters.display_scale);
                if (!record_pass(level, 1U, output.images[level * 2U].view,
                                 2U, 0.0F, 1.0F, 0.0F, &kernel)) break;
                if (!record_pass(level, 0U, output.images[level * 2U + 1U].view,
                                 2U, 0.0F, 0.0F, 1.0F, &kernel)) break;
            }
        }
        if (descriptor_index != pass_count) {
            if (diagnostic.code.empty())
                diagnostic = {"vulkan_hdr_bloom_record_failed", "Vulkan HDR bloom pass recording failed"};
            result = VK_ERROR_INITIALIZATION_FAILED;
        } else {
            transition(command, source.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       source_old_layout);
            result = vkEndCommandBuffer(command);
        }
    }
    if (result == VK_SUCCESS) {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &command;
            result = vkQueueSubmit(context->queue, 1U, &submit, fence);
            if (result == VK_SUCCESS) result = vkWaitForFences(context->device, 1U, &fence, VK_TRUE, UINT64_MAX);
        }
    }
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan HDR bloom execution", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "vulkan_hdr_bloom_failed";
        cleanup();
        return false;
    }
    source.mark_layout(source_old_layout);
    cleanup();
    diagnostic = {};
    return true;
}

bool tone_map_vulkan_texture(VulkanTexture& source, VulkanTexture& destination,
                             const HdrToneMapParameters& parameters,
                             Diagnostic& diagnostic) {
    const std::shared_ptr<VulkanContext>& context = source.context();
    std::lock_guard command_guard(context->command_mutex);
    if (context->device == VK_NULL_HANDLE || context->queue == VK_NULL_HANDLE ||
        context->command_pool == VK_NULL_HANDLE) {
        diagnostic = {"vulkan_hdr_tone_map_uninitialized",
                      "HDR tone mapping requires an initialized Vulkan device context"};
        return false;
    }
    if (!source.initialized() || source.image() == VK_NULL_HANDLE ||
        source.view() == VK_NULL_HANDLE || source.layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
        diagnostic = {"hdr_tone_map_source_uninitialized",
                      "HDR tone mapping requires an initialized source texture"};
        return false;
    }
    if (destination.image() == VK_NULL_HANDLE || destination.view() == VK_NULL_HANDLE) {
        diagnostic = {"hdr_tone_map_destination_uninitialized",
                      "HDR tone mapping requires an allocated destination texture"};
        return false;
    }

    const VkFormat destination_format =
        vk_texture_format(destination.info().description.format);
    const VkImageLayout source_old_layout = source.layout();
    const VkImageLayout destination_old_layout = destination.layout();
    const VkImageLayout destination_final_layout = texture_final_layout(
        destination.info().description.usage,
        destination.info().description.access_policy);

    std::optional<VulkanBloomTargets> bloom_targets;
    if (parameters.bloom.enabled) {
        bloom_targets.emplace();
        if (!generate_vulkan_bloom(context, source, parameters.exposure, parameters.bloom,
                                   *bloom_targets, diagnostic))
            return false;
    }

    struct PushConstants {
        float exposure;
        float gamma_value;
        float saturation;
        float curve_scale;
        float curve_shoulder;
        std::uint32_t srgb_destination;
        float dither_scale;
    } push_constants{parameters.exposure,
                     parameters.gamma,
                     parameters.saturation,
                     parameters.curve_scale,
                     parameters.curve_shoulder,
                     (destination.info().description.format == TextureFormat::rgba8_srgb ||
                      destination.info().description.format == TextureFormat::bgra8_srgb)
                         ? 1U
                         : 0U,
                     parameters.dither_scale};
    static_assert(sizeof(PushConstants) == 28U);
    struct BloomPushConstants {
        float exposure;
        float gamma_value;
        float saturation;
        float curve_scale;
        float curve_shoulder;
        std::uint32_t srgb_destination;
        float dither_scale;
        float bloom_scale;
    } bloom_push_constants{parameters.exposure,
                           parameters.gamma,
                           parameters.saturation,
                           parameters.curve_scale,
                           parameters.curve_shoulder,
                           push_constants.srgb_destination,
                           parameters.dither_scale,
                           parameters.bloom.composite_scale};
    static_assert(sizeof(BloomPushConstants) == 32U);
    const bool bloom_enabled = bloom_targets.has_value();

    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    const auto cleanup = [&] {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(context->device, fence, nullptr);
        if (command != VK_NULL_HANDLE)
            vkFreeCommandBuffers(context->device, context->command_pool, 1U, &command);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(context->device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(context->device, pipeline_layout, nullptr);
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        if (render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(context->device, render_pass, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(context->device, descriptor_pool, nullptr);
        if (descriptor_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(context->device, descriptor_set_layout, nullptr);
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(context->device, sampler, nullptr);
        if (fragment_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(context->device, fragment_shader, nullptr);
        if (vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(context->device, vertex_shader, nullptr);
    };
    const auto fail = [&](const char* operation, VkResult result) {
        diagnostic = vk_error(operation, result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_hdr_tone_map_failed";
        cleanup();
        return false;
    };

    const auto create_shader = [&](const void* bytes, std::size_t size,
                                   VkShaderModule& output) -> VkResult {
        if (size == 0U || size % sizeof(std::uint32_t) != 0U ||
            bytes == nullptr || reinterpret_cast<std::uintptr_t>(bytes) % alignof(std::uint32_t) != 0U)
            return VK_ERROR_INVALID_SHADER_NV;
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = reinterpret_cast<const std::uint32_t*>(bytes);
        return vkCreateShaderModule(context->device, &info, nullptr, &output);
    };
    VkResult result = create_shader(generated::hdr_tone_map_vertex_spirv,
                                    generated::hdr_tone_map_vertex_spirv_size,
                                    vertex_shader);
    if (result != VK_SUCCESS) return fail("vkCreateShaderModule(hdr tone-map vertex)", result);
    result = bloom_enabled
                 ? create_shader(generated::hdr_tone_map_bloom_frag_spirv,
                                 generated::hdr_tone_map_bloom_frag_spirv_size,
                                 fragment_shader)
                 : create_shader(generated::hdr_tone_map_fragment_spirv,
                                 generated::hdr_tone_map_fragment_spirv_size,
                                 fragment_shader);
    if (result != VK_SUCCESS) return fail("vkCreateShaderModule(hdr tone-map fragment)", result);

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // The one-to-one fullscreen pass does not need interpolation. Nearest
    // filtering keeps this path valid on devices that expose sampled RGBA16F
    // without the optional linear-filter feature.
    VkFormatProperties source_format_properties{};
    vkGetPhysicalDeviceFormatProperties(context->physical_device,
                                        vk_texture_format(source.info().description.format),
                                        &source_format_properties);
    const VkFilter tone_map_filter =
        bloom_enabled &&
                (source_format_properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U
            ? VK_FILTER_LINEAR
            : VK_FILTER_NEAREST;
    sampler_info.magFilter = tone_map_filter;
    sampler_info.minFilter = tone_map_filter;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0F;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    result = vkCreateSampler(context->device, &sampler_info, nullptr, &sampler);
    if (result != VK_SUCCESS) return fail("vkCreateSampler(hdr tone-map)", result);

    std::array<VkDescriptorSetLayoutBinding, 6U> sampler_bindings{};
    const std::uint32_t sampler_binding_count = bloom_enabled ? 6U : 1U;
    for (std::uint32_t binding = 0U; binding < sampler_binding_count; ++binding) {
        sampler_bindings[binding].binding = binding;
        sampler_bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_bindings[binding].descriptorCount = 1U;
        sampler_bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info{};
    descriptor_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_layout_info.bindingCount = sampler_binding_count;
    descriptor_layout_info.pBindings = sampler_bindings.data();
    result = vkCreateDescriptorSetLayout(context->device, &descriptor_layout_info, nullptr,
                                          &descriptor_set_layout);
    if (result != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout(hdr tone-map)", result);

    VkDescriptorPoolSize descriptor_pool_size{};
    descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_pool_size.descriptorCount = sampler_binding_count;
    VkDescriptorPoolCreateInfo descriptor_pool_info{};
    descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_info.maxSets = 1U;
    descriptor_pool_info.poolSizeCount = 1U;
    descriptor_pool_info.pPoolSizes = &descriptor_pool_size;
    result = vkCreateDescriptorPool(context->device, &descriptor_pool_info, nullptr,
                                     &descriptor_pool);
    if (result != VK_SUCCESS) return fail("vkCreateDescriptorPool(hdr tone-map)", result);

    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo descriptor_allocation{};
    descriptor_allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_allocation.descriptorPool = descriptor_pool;
    descriptor_allocation.descriptorSetCount = 1U;
    descriptor_allocation.pSetLayouts = &descriptor_set_layout;
    result = vkAllocateDescriptorSets(context->device, &descriptor_allocation, &descriptor_set);
    if (result != VK_SUCCESS)
        return fail("vkAllocateDescriptorSets(hdr tone-map)", result);
    std::array<VkDescriptorImageInfo, 6U> image_infos{};
    image_infos[0] = {sampler, source.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (bloom_enabled) {
        for (std::uint32_t level = 0U; level < 5U; ++level)
            image_infos[level + 1U] = {
                sampler, bloom_targets->images[level * 2U].view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    std::array<VkWriteDescriptorSet, 6U> descriptor_writes{};
    for (std::uint32_t binding = 0U; binding < sampler_binding_count; ++binding) {
        descriptor_writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[binding].dstSet = descriptor_set;
        descriptor_writes[binding].dstBinding = binding;
        descriptor_writes[binding].descriptorCount = 1U;
        descriptor_writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_writes[binding].pImageInfo = &image_infos[binding];
    }
    vkUpdateDescriptorSets(context->device, sampler_binding_count, descriptor_writes.data(),
                           0U, nullptr);

    VkAttachmentDescription color_attachment{};
    color_attachment.format = destination_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &color_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1U;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(context->device, &render_pass_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) return fail("vkCreateRenderPass(hdr tone-map)", result);

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1U;
    framebuffer_info.width = destination.info().description.width;
    framebuffer_info.height = destination.info().description.height;
    framebuffer_info.layers = 1U;
    // pAttachments must point at storage, while VulkanTexture::view() returns by value.
    const VkImageView destination_view = destination.view();
    framebuffer_info.pAttachments = &destination_view;
    result = vkCreateFramebuffer(context->device, &framebuffer_info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) return fail("vkCreateFramebuffer(hdr tone-map)", result);

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0U;
    push_range.size = bloom_enabled ? sizeof(BloomPushConstants) : sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1U;
    pipeline_layout_info.pSetLayouts = &descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1U;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(context->device, &pipeline_layout_info, nullptr,
                                    &pipeline_layout);
    if (result != VK_SUCCESS) return fail("vkCreatePipelineLayout(hdr tone-map)", result);

    VkPipelineShaderStageCreateInfo shader_stages[2]{};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vertex_shader;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = fragment_shader;
    shader_stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.0F,
                        0.0F,
                        static_cast<float>(destination.info().description.width),
                        static_cast<float>(destination.info().description.height),
                        0.0F,
                        1.0F};
    VkRect2D scissor{{0, 0},
                     {destination.info().description.width,
                      destination.info().description.height}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1U;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1U;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1U;
    blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2U;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0U;
    pipeline_info.basePipelineIndex = -1;
    result = vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1U, &pipeline_info,
                                       nullptr, &pipeline);
    if (result != VK_SUCCESS)
        return fail("vkCreateGraphicsPipelines(hdr tone-map)", result);

    VkCommandBufferAllocateInfo command_allocation{};
    command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocation.commandPool = context->command_pool;
    command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocation.commandBufferCount = 1U;
    result = vkAllocateCommandBuffers(context->device, &command_allocation, &command);
    if (result != VK_SUCCESS)
        return fail("vkAllocateCommandBuffers(hdr tone-map)", result);
    VkCommandBufferBeginInfo command_begin{};
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command, &command_begin);
    if (result != VK_SUCCESS) return fail("vkBeginCommandBuffer(hdr tone-map)", result);

    struct LayoutState {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags access = 0U;
    };
    const auto old_state = [](VkImageLayout layout) {
        switch (layout) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        case VK_IMAGE_LAYOUT_GENERAL:
            return LayoutState{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return LayoutState{VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT};
        default:
            return LayoutState{};
        }
    };
    const auto transition = [&](VkImage image, VkImageLayout old_layout,
                                VkImageLayout new_layout, VkAccessFlags destination_access,
                                VkPipelineStageFlags destination_stage) {
        if (old_layout == new_layout) return;
        const LayoutState source_state = old_state(old_layout);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source_state.access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.layerCount = 1U;
        vkCmdPipelineBarrier(command, source_state.stage, destination_stage, 0U, 0U, nullptr,
                             0U, nullptr, 1U, &barrier);
    };
    transition(source.image(), source_old_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    transition(destination.image(), destination_old_layout,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea = scissor;
    vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0U, 1U,
                            &descriptor_set, 0U, nullptr);
    if (bloom_enabled)
        vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0U,
                           sizeof(bloom_push_constants), &bloom_push_constants);
    else
        vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0U,
                           sizeof(push_constants), &push_constants);
    vkCmdDraw(command, 3U, 1U, 0U, 0U);
    vkCmdEndRenderPass(command);

    transition(source.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, source_old_layout,
               old_state(source_old_layout).access, old_state(source_old_layout).stage);
    transition(destination.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               destination_final_layout,
               old_state(destination_final_layout).access,
               old_state(destination_final_layout).stage);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) return fail("vkEndCommandBuffer(hdr tone-map)", result);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) return fail("vkCreateFence(hdr tone-map)", result);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &command;
    result = vkQueueSubmit(context->queue, 1U, &submit, fence);
    if (result != VK_SUCCESS) return fail("vkQueueSubmit(hdr tone-map)", result);
    result = vkWaitForFences(context->device, 1U, &fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain != VK_SUCCESS) {
            source.mark_layout(VK_IMAGE_LAYOUT_UNDEFINED);
            destination.mark_layout(VK_IMAGE_LAYOUT_UNDEFINED);
            return fail("vkWaitForFences(hdr tone-map)", drain);
        }
    }
    source.mark_layout(source_old_layout);
    destination.mark_layout(destination_final_layout);
    destination.mark_initialized();
    cleanup();
    diagnostic = {};
    return true;
}

bool apply_fxaa_vulkan_texture(VulkanTexture& source, VulkanTexture& destination,
                               Diagnostic& diagnostic) {
    const std::shared_ptr<VulkanContext>& context = source.context();
    std::lock_guard command_guard(context->command_mutex);
    if (context->device == VK_NULL_HANDLE || context->queue == VK_NULL_HANDLE ||
        context->command_pool == VK_NULL_HANDLE) {
        diagnostic = {"vulkan_fxaa_uninitialized",
                      "FXAA requires an initialized Vulkan device context"};
        return false;
    }
    if (!source.initialized() || source.image() == VK_NULL_HANDLE ||
        source.view() == VK_NULL_HANDLE || source.layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
        diagnostic = {"fxaa_source_uninitialized",
                      "FXAA requires an initialized source texture"};
        return false;
    }
    if (destination.image() == VK_NULL_HANDLE || destination.view() == VK_NULL_HANDLE) {
        diagnostic = {"fxaa_destination_uninitialized",
                      "FXAA requires an allocated destination texture"};
        return false;
    }

    const VkFormat destination_format =
        vk_texture_format(destination.info().description.format);
    const VkImageLayout source_old_layout = source.layout();
    const VkImageLayout destination_old_layout = destination.layout();
    const VkImageLayout destination_final_layout = texture_final_layout(
        destination.info().description.usage,
        destination.info().description.access_policy);
    struct PushConstants {
        float inverse_width;
        float inverse_height;
        std::uint32_t srgb_destination;
    } push_constants{
        1.0F / static_cast<float>(source.info().description.width),
        1.0F / static_cast<float>(source.info().description.height),
        (destination.info().description.format == TextureFormat::rgba8_srgb ||
         destination.info().description.format == TextureFormat::bgra8_srgb)
            ? 1U
            : 0U};
    static_assert(sizeof(PushConstants) == 12U);

    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    bool completed = false;

    const auto cleanup = [&] {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(context->device, fence, nullptr);
        if (command != VK_NULL_HANDLE)
            vkFreeCommandBuffers(context->device, context->command_pool, 1U, &command);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(context->device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(context->device, pipeline_layout, nullptr);
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        if (render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(context->device, render_pass, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(context->device, descriptor_pool, nullptr);
        if (descriptor_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(context->device, descriptor_set_layout, nullptr);
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(context->device, sampler, nullptr);
        if (fragment_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(context->device, fragment_shader, nullptr);
        if (vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(context->device, vertex_shader, nullptr);
    };
    const auto fail = [&](const char* operation, VkResult result) {
        diagnostic = vk_error(operation, result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_fxaa_failed";
        cleanup();
        return false;
    };
    const auto create_shader = [&](const void* bytes, std::size_t size,
                                   VkShaderModule& output) -> VkResult {
        if (size == 0U || size % sizeof(std::uint32_t) != 0U || bytes == nullptr ||
            reinterpret_cast<std::uintptr_t>(bytes) % alignof(std::uint32_t) != 0U)
            return VK_ERROR_INVALID_SHADER_NV;
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = reinterpret_cast<const std::uint32_t*>(bytes);
        return vkCreateShaderModule(context->device, &info, nullptr, &output);
    };

    VkResult result = create_shader(generated::hdr_tone_map_vertex_spirv,
                                    generated::hdr_tone_map_vertex_spirv_size,
                                    vertex_shader);
    if (result != VK_SUCCESS)
        return fail("vkCreateShaderModule(fxaa vertex)", result);
    result = create_shader(generated::fxaa_fragment_spirv,
                           generated::fxaa_fragment_spirv_size,
                           fragment_shader);
    if (result != VK_SUCCESS)
        return fail("vkCreateShaderModule(fxaa fragment)", result);

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // The recovered runtime uses anisotropic repeat filtering. A bounded
    // linear-repeat sampler is the portable Vulkan approximation; the shader
    // explicitly samples mip zero for the full-resolution FXAA source.
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = 0.0F;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    result = vkCreateSampler(context->device, &sampler_info, nullptr, &sampler);
    if (result != VK_SUCCESS) return fail("vkCreateSampler(fxaa)", result);

    VkDescriptorSetLayoutBinding sampler_binding{};
    sampler_binding.binding = 0U;
    sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_binding.descriptorCount = 1U;
    sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info{};
    descriptor_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_layout_info.bindingCount = 1U;
    descriptor_layout_info.pBindings = &sampler_binding;
    result = vkCreateDescriptorSetLayout(context->device, &descriptor_layout_info, nullptr,
                                          &descriptor_set_layout);
    if (result != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout(fxaa)", result);

    VkDescriptorPoolSize descriptor_pool_size{};
    descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_pool_size.descriptorCount = 1U;
    VkDescriptorPoolCreateInfo descriptor_pool_info{};
    descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_info.maxSets = 1U;
    descriptor_pool_info.poolSizeCount = 1U;
    descriptor_pool_info.pPoolSizes = &descriptor_pool_size;
    result = vkCreateDescriptorPool(context->device, &descriptor_pool_info, nullptr,
                                     &descriptor_pool);
    if (result != VK_SUCCESS) return fail("vkCreateDescriptorPool(fxaa)", result);

    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo descriptor_allocation{};
    descriptor_allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_allocation.descriptorPool = descriptor_pool;
    descriptor_allocation.descriptorSetCount = 1U;
    descriptor_allocation.pSetLayouts = &descriptor_set_layout;
    result = vkAllocateDescriptorSets(context->device, &descriptor_allocation, &descriptor_set);
    if (result != VK_SUCCESS)
        return fail("vkAllocateDescriptorSets(fxaa)", result);
    VkDescriptorImageInfo image_info{sampler, source.view(),
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet descriptor_write{};
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptor_set;
    descriptor_write.dstBinding = 0U;
    descriptor_write.descriptorCount = 1U;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(context->device, 1U, &descriptor_write, 0U, nullptr);

    VkAttachmentDescription color_attachment{};
    color_attachment.format = destination_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &color_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1U;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(context->device, &render_pass_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) return fail("vkCreateRenderPass(fxaa)", result);

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1U;
    framebuffer_info.width = destination.info().description.width;
    framebuffer_info.height = destination.info().description.height;
    framebuffer_info.layers = 1U;
    const VkImageView destination_view = destination.view();
    framebuffer_info.pAttachments = &destination_view;
    result = vkCreateFramebuffer(context->device, &framebuffer_info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) return fail("vkCreateFramebuffer(fxaa)", result);

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0U;
    push_range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1U;
    pipeline_layout_info.pSetLayouts = &descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1U;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(context->device, &pipeline_layout_info, nullptr,
                                    &pipeline_layout);
    if (result != VK_SUCCESS) return fail("vkCreatePipelineLayout(fxaa)", result);

    VkPipelineShaderStageCreateInfo shader_stages[2]{};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vertex_shader;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = fragment_shader;
    shader_stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.0F,
                        0.0F,
                        static_cast<float>(destination.info().description.width),
                        static_cast<float>(destination.info().description.height),
                        0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, {destination.info().description.width,
                              destination.info().description.height}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1U;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1U;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1U;
    blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2U;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0U;
    pipeline_info.basePipelineIndex = -1;
    result = vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1U, &pipeline_info,
                                       nullptr, &pipeline);
    if (result != VK_SUCCESS) return fail("vkCreateGraphicsPipelines(fxaa)", result);

    VkCommandBufferAllocateInfo command_allocation{};
    command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocation.commandPool = context->command_pool;
    command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocation.commandBufferCount = 1U;
    result = vkAllocateCommandBuffers(context->device, &command_allocation, &command);
    if (result != VK_SUCCESS) return fail("vkAllocateCommandBuffers(fxaa)", result);
    VkCommandBufferBeginInfo command_begin{};
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command, &command_begin);
    if (result != VK_SUCCESS) return fail("vkBeginCommandBuffer(fxaa)", result);

    struct LayoutState {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags access = 0U;
    };
    const auto old_state = [](VkImageLayout layout) {
        switch (layout) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return LayoutState{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        case VK_IMAGE_LAYOUT_GENERAL:
            return LayoutState{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return LayoutState{VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT};
        default:
            return LayoutState{};
        }
    };
    const auto transition = [&](VkImage image, VkImageLayout old_layout,
                                VkImageLayout new_layout, VkAccessFlags destination_access,
                                VkPipelineStageFlags destination_stage) {
        if (old_layout == new_layout) return;
        const LayoutState source_state = old_state(old_layout);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source_state.access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.layerCount = 1U;
        vkCmdPipelineBarrier(command, source_state.stage, destination_stage, 0U, 0U, nullptr,
                             0U, nullptr, 1U, &barrier);
    };
    transition(source.image(), source_old_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    transition(destination.image(), destination_old_layout,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea = scissor;
    vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0U, 1U,
                            &descriptor_set, 0U, nullptr);
    vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0U,
                       sizeof(push_constants), &push_constants);
    vkCmdDraw(command, 3U, 1U, 0U, 0U);
    vkCmdEndRenderPass(command);
    transition(source.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, source_old_layout,
               old_state(source_old_layout).access, old_state(source_old_layout).stage);
    transition(destination.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               destination_final_layout, old_state(destination_final_layout).access,
               old_state(destination_final_layout).stage);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) return fail("vkEndCommandBuffer(fxaa)", result);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(context->device, &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) return fail("vkCreateFence(fxaa)", result);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &command;
    result = vkQueueSubmit(context->queue, 1U, &submit, fence);
    submitted = result == VK_SUCCESS;
    if (result == VK_SUCCESS) {
        result = vkWaitForFences(context->device, 1U, &fence, VK_TRUE, UINT64_MAX);
        completed = result == VK_SUCCESS;
    }
    if (submitted && !completed) {
        const VkResult drain = vkDeviceWaitIdle(context->device);
        if (drain != VK_SUCCESS) {
            source.mark_layout(VK_IMAGE_LAYOUT_UNDEFINED);
            destination.mark_layout(VK_IMAGE_LAYOUT_UNDEFINED);
            return fail("vkWaitForFences(fxaa)", drain);
        }
        completed = true;
    }
    if (result != VK_SUCCESS && !completed) return fail("vkQueueSubmit(fxaa)", result);
    source.mark_layout(source_old_layout);
    destination.mark_layout(destination_final_layout);
    destination.mark_initialized();
    cleanup();
    diagnostic = {};
    return true;
}

struct RawVulkanSampler {
    std::shared_ptr<VulkanContext> context;
    VkSampler sampler = VK_NULL_HANDLE;

    ~RawVulkanSampler() { reset(); }
    RawVulkanSampler() = default;
    RawVulkanSampler(const RawVulkanSampler&) = delete;
    RawVulkanSampler& operator=(const RawVulkanSampler&) = delete;
    RawVulkanSampler(RawVulkanSampler&& other) noexcept
        : context(std::move(other.context)), sampler(std::exchange(other.sampler, VK_NULL_HANDLE)) {}
    RawVulkanSampler& operator=(RawVulkanSampler&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        sampler = std::exchange(other.sampler, VK_NULL_HANDLE);
        return *this;
    }
    void reset() noexcept {
        if (context && sampler != VK_NULL_HANDLE) vkDestroySampler(context->device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
        context.reset();
    }
};

struct RawVulkanShaderModule {
    std::shared_ptr<VulkanContext> context;
    VkShaderModule module = VK_NULL_HANDLE;

    ~RawVulkanShaderModule() { reset(); }
    RawVulkanShaderModule() = default;
    RawVulkanShaderModule(const RawVulkanShaderModule&) = delete;
    RawVulkanShaderModule& operator=(const RawVulkanShaderModule&) = delete;
    RawVulkanShaderModule(RawVulkanShaderModule&& other) noexcept
        : context(std::move(other.context)), module(std::exchange(other.module, VK_NULL_HANDLE)) {}
    RawVulkanShaderModule& operator=(RawVulkanShaderModule&& other) noexcept {
        if (this == &other) return *this;
        reset();
        context = std::move(other.context);
        module = std::exchange(other.module, VK_NULL_HANDLE);
        return *this;
    }
    void reset() noexcept {
        if (context && module != VK_NULL_HANDLE) vkDestroyShaderModule(context->device, module, nullptr);
        module = VK_NULL_HANDLE;
        context.reset();
    }
};

class VulkanSampler final : public Sampler {
public:
    VulkanSampler(std::shared_ptr<VulkanContext> context, RawVulkanSampler raw,
                  SamplerDescription description)
        : context_(std::move(context)), raw_(std::move(raw)), info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const SamplerInfo& info() const noexcept override { return info_; }
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }
    VkSampler sampler() const noexcept { return raw_.sampler; }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanSampler raw_;
    SamplerInfo info_;
};

bool extract_vulkan_sampler(const Sampler* sampler,
                            const std::shared_ptr<VulkanContext>& context,
                            VkSampler& raw_sampler,
                            Diagnostic& diagnostic) {
    const auto* vulkan_sampler = dynamic_cast<const VulkanSampler*>(sampler);
    if (vulkan_sampler == nullptr || vulkan_sampler->context() != context ||
        vulkan_sampler->sampler() == VK_NULL_HANDLE) {
        diagnostic = {"vulkan_portable_cloud_resource_invalid",
                      "Vulkan cloud vertices and sampler must be owned vertex and sampler resources"};
        return false;
    }
    raw_sampler = vulkan_sampler->sampler();
    return true;
}

bool prepare_vulkan_sampled_binding(const IndexedStaticMeshDrawRequest& request,
                                    const std::shared_ptr<VulkanContext>& context,
                                    VulkanIndexedBatchDraw& draw,
                                    Diagnostic& diagnostic) {
    if (request.pipeline == nullptr || request.pipeline->resources.empty()) return true;
    bool sampled_declaration = false;
    bool sampler_declaration = false;
    bool material_declaration = false;
    bool frame_declaration = false;
    bool normal_texture_declaration = false;
    bool normal_sampler_declaration = false;
    bool maps_texture_declaration = false;
    bool maps_sampler_declaration = false;
    bool detail_texture_declaration = false;
    bool detail_sampler_declaration = false;
    bool normal_detail_texture_declaration = false;
    bool normal_detail_sampler_declaration = false;
    bool damage_texture_declaration = false;
    bool damage_sampler_declaration = false;
    bool damage_mask_texture_declaration = false;
    bool damage_mask_sampler_declaration = false;
    for (const PipelineResourceBinding& resource : request.pipeline->resources) {
        if (resource.set != 0U) continue;
        if (resource.binding == 0U && resource.kind == PipelineResourceKind::sampled_texture)
            sampled_declaration = true;
        else if (resource.binding == 1U && resource.kind == PipelineResourceKind::sampler)
            sampler_declaration = true;
        else if (resource.binding == 2U && resource.kind == PipelineResourceKind::uniform_buffer)
            material_declaration = true;
        else if (resource.binding == 3U && resource.kind == PipelineResourceKind::uniform_buffer)
            frame_declaration = true;
        else if (resource.binding == 4U && resource.kind == PipelineResourceKind::sampled_texture)
            normal_texture_declaration = true;
        else if (resource.binding == 5U && resource.kind == PipelineResourceKind::sampler)
            normal_sampler_declaration = true;
        else if (resource.binding == 6U && resource.kind == PipelineResourceKind::sampled_texture)
            maps_texture_declaration = true;
        else if (resource.binding == 7U && resource.kind == PipelineResourceKind::sampler)
            maps_sampler_declaration = true;
        else if (resource.binding == 8U && resource.kind == PipelineResourceKind::sampled_texture)
            detail_texture_declaration = true;
        else if (resource.binding == 9U && resource.kind == PipelineResourceKind::sampler)
            detail_sampler_declaration = true;
        else if (resource.binding == 10U && resource.kind == PipelineResourceKind::sampled_texture)
            normal_detail_texture_declaration = true;
        else if (resource.binding == 11U && resource.kind == PipelineResourceKind::sampler)
            normal_detail_sampler_declaration = true;
        else if (resource.binding == 12U && resource.kind == PipelineResourceKind::sampled_texture)
            damage_texture_declaration = true;
        else if (resource.binding == 13U && resource.kind == PipelineResourceKind::sampler)
            damage_sampler_declaration = true;
        else if (resource.binding == 14U && resource.kind == PipelineResourceKind::sampled_texture)
            damage_mask_texture_declaration = true;
        else if (resource.binding == 15U && resource.kind == PipelineResourceKind::sampler)
            damage_mask_sampler_declaration = true;
    }
    if (!sampled_declaration || !sampler_declaration) {
        diagnostic = {"vulkan_indexed_resource_layout_unsupported",
                      "Vulkan indexed resources require set 0 texture and sampler bindings"};
        return false;
    }
    if (normal_texture_declaration != normal_sampler_declaration) {
        diagnostic = {"vulkan_indexed_normal_layout_unsupported",
                      "Vulkan indexed normal resources require both set 0 texture and sampler bindings"};
        return false;
    }
    const bool normal_declaration = normal_texture_declaration;
    if (normal_declaration && (!material_declaration || !frame_declaration)) {
        diagnostic = {"vulkan_indexed_normal_layout_unsupported",
                      "Vulkan indexed normal resources require material and frame bindings at 2 and 3"};
        return false;
    }
    if (maps_texture_declaration != maps_sampler_declaration) {
        diagnostic = {"vulkan_indexed_maps_layout_unsupported",
                      "Vulkan indexed maps resources require both set 0 texture and sampler bindings"};
        return false;
    }
    const bool maps_declaration = maps_texture_declaration;
    if (maps_declaration && (!normal_declaration || !material_declaration || !frame_declaration)) {
        diagnostic = {"vulkan_indexed_maps_layout_unsupported",
                      "Vulkan indexed maps resources require normal, material, and frame bindings"};
        return false;
    }
    if (!maps_declaration &&
        (request.maps_binding.texture != nullptr || request.maps_binding.sampler != nullptr)) {
        diagnostic = {"vulkan_indexed_maps_binding_unexpected",
                      "A Vulkan indexed pipeline without maps declarations cannot receive maps resources"};
        return false;
    }
    if (detail_texture_declaration != detail_sampler_declaration) {
        diagnostic = {"vulkan_indexed_detail_layout_unsupported",
                      "Vulkan indexed detail resources require both set 0 texture and sampler bindings"};
        return false;
    }
    if (normal_detail_texture_declaration != normal_detail_sampler_declaration) {
        diagnostic = {"vulkan_indexed_normal_detail_layout_unsupported",
                      "Vulkan indexed normal-detail resources require both set 0 texture and sampler bindings"};
        return false;
    }
    const bool detail_declaration = detail_texture_declaration;
    const bool normal_detail_declaration = normal_detail_texture_declaration;
    const bool damage_dust_layout =
        classify_indexed_portable_resource_layout(*request.pipeline) ==
        IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
    if ((!damage_dust_layout && detail_declaration != normal_detail_declaration) ||
        (!damage_dust_layout && detail_declaration &&
         (!maps_declaration || !normal_declaration || !material_declaration || !frame_declaration))) {
        diagnostic = {"vulkan_indexed_detail_layout_unsupported",
                      "Vulkan indexed detail stack requires diffuse, normal, maps, material, and frame bindings"};
        return false;
    }
    if (!detail_declaration &&
        (request.detail_binding.texture != nullptr || request.detail_binding.sampler != nullptr ||
         request.normal_detail_binding.texture != nullptr || request.normal_detail_binding.sampler != nullptr)) {
        diagnostic = {"vulkan_indexed_detail_binding_unexpected",
                      "A Vulkan indexed pipeline without detail declarations cannot receive detail resources"};
        return false;
    }
    if (damage_texture_declaration != damage_sampler_declaration ||
        damage_mask_texture_declaration != damage_mask_sampler_declaration ||
        damage_texture_declaration != damage_mask_texture_declaration) {
        diagnostic = {"vulkan_indexed_damage_layout_unsupported",
                      "Vulkan indexed damage resources require complete texture and sampler pairs"};
        return false;
    }
    const bool damage_declaration = damage_texture_declaration;
    const bool multimap_reflection_declaration =
        pipeline_declares_multimap_reflection(*request.pipeline);
    if (damage_declaration &&
        (!maps_declaration || !normal_declaration || !material_declaration ||
         !frame_declaration || (detail_declaration && !damage_dust_layout))) {
        diagnostic = {"vulkan_indexed_damage_layout_unsupported",
                      "Vulkan indexed damage resources require diffuse, normal, maps, material, and frame bindings without the detail stack"};
        return false;
    }
    if (!damage_declaration &&
        (request.damage_binding.texture != nullptr || request.damage_binding.sampler != nullptr ||
         request.damage_mask_binding.texture != nullptr ||
         request.damage_mask_binding.sampler != nullptr)) {
        diagnostic = {"vulkan_indexed_damage_binding_unexpected",
                      "A Vulkan indexed pipeline without damage declarations cannot receive damage resources"};
        return false;
    }
    auto* sampled_texture = dynamic_cast<const VulkanTexture*>(request.sampled_binding.texture);
    auto* sampler = dynamic_cast<const VulkanSampler*>(request.sampled_binding.sampler);
    if (sampled_texture == nullptr || sampler == nullptr) {
        diagnostic = {"vulkan_indexed_resource_type_unsupported",
                      "Vulkan indexed diffuse resources must be Vulkan texture and sampler handles"};
        return false;
    }
    if (sampled_texture->context() != context || sampler->context() != context) {
        diagnostic = {"vulkan_indexed_resource_context_mismatch",
                      "Vulkan indexed diffuse resources must belong to the draw device"};
        return false;
    }
    if (!sampled_texture->initialized()) {
        diagnostic = {"vulkan_indexed_resource_uninitialized",
                      "Vulkan indexed diffuse texture must contain an uploaded image before sampling"};
        return false;
    }
    if (sampled_texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        diagnostic = {"vulkan_indexed_resource_layout_invalid",
                      "Vulkan indexed diffuse texture is not in shader-read-only layout"};
        return false;
    }
    if (sampled_texture->image() == VK_NULL_HANDLE || sampled_texture->view() == VK_NULL_HANDLE ||
        sampler->sampler() == VK_NULL_HANDLE) {
        diagnostic = {"vulkan_indexed_resource_handle_invalid",
                      "Vulkan indexed diffuse resources contain a null native handle"};
        return false;
    }
    draw.has_sampled_binding = true;
    draw.sampled_image = sampled_texture->image();
    draw.sampled_view = sampled_texture->view();
    draw.sampled_sampler = sampler->sampler();

    if (normal_declaration) {
        const auto* normal_texture = dynamic_cast<const VulkanTexture*>(request.normal_binding.texture);
        const auto* normal_sampler = dynamic_cast<const VulkanSampler*>(request.normal_binding.sampler);
        if (normal_texture == nullptr || normal_sampler == nullptr) {
            diagnostic = {"vulkan_indexed_normal_resource_type_unsupported",
                          "Vulkan indexed normal resources must be Vulkan texture and sampler handles"};
            return false;
        }
        if (normal_texture->context() != context || normal_sampler->context() != context) {
            diagnostic = {"vulkan_indexed_normal_resource_context_mismatch",
                          "Vulkan indexed normal resources must belong to the draw device"};
            return false;
        }
        const TextureDescription& normal_description = normal_texture->info().description;
        const std::uint32_t normal_usage = static_cast<std::uint32_t>(normal_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((normal_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (normal_usage & forbidden_usage) != 0U) {
            diagnostic = {"vulkan_indexed_normal_resource_usage_invalid",
                          "Vulkan indexed normal textures require sampled-only image usage"};
            return false;
        }
        if (normal_description.width == 0U || normal_description.height == 0U ||
            normal_description.mip_levels == 0U || normal_description.array_layers != 1U ||
            (normal_description.format != TextureFormat::rgba8_unorm &&
             normal_description.format != TextureFormat::rgba8_srgb &&
             normal_description.format != TextureFormat::bgra8_unorm &&
             normal_description.format != TextureFormat::bgra8_srgb)) {
            diagnostic = {"vulkan_indexed_normal_resource_format_unsupported",
                          "Vulkan indexed normal textures require one-layer RGBA8 or BGRA8 image data"};
            return false;
        }
        if (!normal_texture->initialized()) {
            diagnostic = {"vulkan_indexed_normal_resource_uninitialized",
                          "Vulkan indexed normal texture must contain an uploaded image before sampling"};
            return false;
        }
        if (normal_texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            diagnostic = {"vulkan_indexed_normal_resource_layout_invalid",
                          "Vulkan indexed normal texture is not in shader-read-only layout"};
            return false;
        }
        if (normal_texture->image() == VK_NULL_HANDLE || normal_texture->view() == VK_NULL_HANDLE ||
            normal_sampler->sampler() == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_normal_resource_handle_invalid",
                          "Vulkan indexed normal resources contain a null native handle"};
            return false;
        }
        draw.has_normal_binding = true;
        draw.normal_image = normal_texture->image();
        draw.normal_view = normal_texture->view();
        draw.normal_sampler = normal_sampler->sampler();
    }

    if (maps_declaration) {
        const auto* maps_texture = dynamic_cast<const VulkanTexture*>(request.maps_binding.texture);
        const auto* maps_sampler = dynamic_cast<const VulkanSampler*>(request.maps_binding.sampler);
        if (maps_texture == nullptr || maps_sampler == nullptr) {
            diagnostic = {"vulkan_indexed_maps_resource_type_unsupported",
                          "Vulkan indexed maps resources must be Vulkan texture and sampler handles"};
            return false;
        }
        if (maps_texture->context() != context || maps_sampler->context() != context) {
            diagnostic = {"vulkan_indexed_maps_resource_context_mismatch",
                          "Vulkan indexed maps resources must belong to the draw device"};
            return false;
        }
        const TextureDescription& maps_description = maps_texture->info().description;
        const std::uint32_t maps_usage = static_cast<std::uint32_t>(maps_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((maps_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (maps_usage & forbidden_usage) != 0U) {
            diagnostic = {"vulkan_indexed_maps_resource_usage_invalid",
                          "Vulkan indexed maps textures require sampled-only image usage"};
            return false;
        }
        if (maps_description.width == 0U || maps_description.height == 0U ||
            maps_description.mip_levels == 0U || maps_description.array_layers != 1U ||
            (maps_description.format != TextureFormat::rgba8_unorm &&
             maps_description.format != TextureFormat::bgra8_unorm &&
             maps_description.format != TextureFormat::bc1_unorm &&
             maps_description.format != TextureFormat::bc3_unorm &&
             maps_description.format != TextureFormat::bc7_unorm)) {
            diagnostic = {"vulkan_indexed_maps_resource_format_unsupported",
                          "Vulkan indexed maps textures require one-layer linear RGBA8, BGRA8, BC1, BC3, or BC7 UNORM image data"};
            return false;
        }
        if (!maps_texture->initialized()) {
            diagnostic = {"vulkan_indexed_maps_resource_uninitialized",
                          "Vulkan indexed maps texture must contain an uploaded image before sampling"};
            return false;
        }
        if (maps_texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            diagnostic = {"vulkan_indexed_maps_resource_layout_invalid",
                          "Vulkan indexed maps texture is not in shader-read-only layout"};
            return false;
        }
        if (maps_texture->image() == VK_NULL_HANDLE || maps_texture->view() == VK_NULL_HANDLE ||
            maps_sampler->sampler() == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_maps_resource_handle_invalid",
                          "Vulkan indexed maps resources contain a null native handle"};
            return false;
        }
        Diagnostic maps_sampler_diagnostic;
        const SamplerStatus maps_sampler_status = validate_sampler_description(
            maps_sampler->info().description, maps_sampler_diagnostic);
        if (maps_sampler_status != SamplerStatus::ready) {
            diagnostic = {maps_sampler_diagnostic.code.empty()
                              ? "vulkan_indexed_maps_sampler_invalid"
                              : "vulkan_indexed_maps_" + maps_sampler_diagnostic.code,
                          maps_sampler_diagnostic.message};
            return false;
        }
        draw.has_maps_binding = true;
        draw.maps_image = maps_texture->image();
        draw.maps_view = maps_texture->view();
        draw.maps_sampler = maps_sampler->sampler();
    }

    if (detail_declaration) {
        const auto* detail_texture = dynamic_cast<const VulkanTexture*>(request.detail_binding.texture);
        const auto* detail_sampler = dynamic_cast<const VulkanSampler*>(request.detail_binding.sampler);
        if (detail_texture == nullptr || detail_sampler == nullptr) {
            diagnostic = {"vulkan_indexed_detail_resource_type_unsupported",
                          "Vulkan indexed detail resources must be Vulkan texture and sampler handles"};
            return false;
        }
        if (detail_texture->context() != context || detail_sampler->context() != context) {
            diagnostic = {"vulkan_indexed_detail_resource_context_mismatch",
                          "Vulkan indexed detail resources must belong to the draw device"};
            return false;
        }
        const TextureDescription& detail_description = detail_texture->info().description;
        const std::uint32_t detail_usage = static_cast<std::uint32_t>(detail_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((detail_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (detail_usage & forbidden_usage) != 0U) {
            diagnostic = {"vulkan_indexed_detail_resource_usage_invalid",
                          "Vulkan indexed detail textures require sampled-only image usage"};
            return false;
        }
        if (detail_description.width == 0U || detail_description.height == 0U ||
            detail_description.mip_levels == 0U || detail_description.array_layers != 1U ||
            (detail_description.format != TextureFormat::rgba8_unorm &&
             detail_description.format != TextureFormat::rgba8_srgb &&
             detail_description.format != TextureFormat::bgra8_unorm &&
             detail_description.format != TextureFormat::bgra8_srgb &&
             detail_description.format != TextureFormat::bc1_unorm &&
             detail_description.format != TextureFormat::bc1_srgb &&
             detail_description.format != TextureFormat::bc3_unorm &&
             detail_description.format != TextureFormat::bc3_srgb &&
             detail_description.format != TextureFormat::bc7_unorm &&
             detail_description.format != TextureFormat::bc7_srgb)) {
            diagnostic = {"vulkan_indexed_detail_resource_format_unsupported",
                          "Vulkan indexed detail textures require one-layer RGBA8, BGRA8, BC1, BC3, or BC7 image data"};
            return false;
        }
        if (!detail_texture->initialized()) {
            diagnostic = {"vulkan_indexed_detail_resource_uninitialized",
                          "Vulkan indexed detail texture must contain an uploaded image before sampling"};
            return false;
        }
        if (detail_texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            diagnostic = {"vulkan_indexed_detail_resource_layout_invalid",
                          "Vulkan indexed detail texture is not in shader-read-only layout"};
            return false;
        }
        if (detail_texture->image() == VK_NULL_HANDLE || detail_texture->view() == VK_NULL_HANDLE ||
            detail_sampler->sampler() == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_detail_resource_handle_invalid",
                          "Vulkan indexed detail resources contain a null native handle"};
            return false;
        }
        Diagnostic detail_sampler_diagnostic;
        const SamplerStatus detail_sampler_status = validate_sampler_description(
            detail_sampler->info().description, detail_sampler_diagnostic);
        if (detail_sampler_status != SamplerStatus::ready) {
            diagnostic = {detail_sampler_diagnostic.code.empty()
                              ? "vulkan_indexed_detail_sampler_invalid"
                              : "vulkan_indexed_detail_" + detail_sampler_diagnostic.code,
                          detail_sampler_diagnostic.message};
            return false;
        }
        draw.has_detail_binding = true;
        draw.detail_image = detail_texture->image();
        draw.detail_view = detail_texture->view();
        draw.detail_sampler = detail_sampler->sampler();
    }

    if (normal_detail_declaration) {
        const auto* normal_detail_texture =
            dynamic_cast<const VulkanTexture*>(request.normal_detail_binding.texture);
        const auto* normal_detail_sampler =
            dynamic_cast<const VulkanSampler*>(request.normal_detail_binding.sampler);
        if (normal_detail_texture == nullptr || normal_detail_sampler == nullptr) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_type_unsupported",
                          "Vulkan indexed normal-detail resources must be Vulkan texture and sampler handles"};
            return false;
        }
        if (normal_detail_texture->context() != context || normal_detail_sampler->context() != context) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_context_mismatch",
                          "Vulkan indexed normal-detail resources must belong to the draw device"};
            return false;
        }
        const TextureDescription& normal_detail_description = normal_detail_texture->info().description;
        const std::uint32_t normal_detail_usage = static_cast<std::uint32_t>(normal_detail_description.usage);
        const std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((normal_detail_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (normal_detail_usage & forbidden_usage) != 0U) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_usage_invalid",
                          "Vulkan indexed normal-detail textures require sampled-only image usage"};
            return false;
        }
        if (normal_detail_description.width == 0U || normal_detail_description.height == 0U ||
            normal_detail_description.mip_levels == 0U || normal_detail_description.array_layers != 1U ||
            (normal_detail_description.format != TextureFormat::rgba8_unorm &&
             normal_detail_description.format != TextureFormat::bgra8_unorm)) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_format_unsupported",
                          "Vulkan indexed normal-detail textures require one-layer linear RGBA8 or BGRA8 UNORM image data"};
            return false;
        }
        if (!normal_detail_texture->initialized()) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_uninitialized",
                          "Vulkan indexed normal-detail texture must contain an uploaded image before sampling"};
            return false;
        }
        if (normal_detail_texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_layout_invalid",
                          "Vulkan indexed normal-detail texture is not in shader-read-only layout"};
            return false;
        }
        if (normal_detail_texture->image() == VK_NULL_HANDLE || normal_detail_texture->view() == VK_NULL_HANDLE ||
            normal_detail_sampler->sampler() == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_normal_detail_resource_handle_invalid",
                          "Vulkan indexed normal-detail resources contain a null native handle"};
            return false;
        }
        Diagnostic normal_detail_sampler_diagnostic;
        const SamplerStatus normal_detail_sampler_status = validate_sampler_description(
            normal_detail_sampler->info().description, normal_detail_sampler_diagnostic);
        if (normal_detail_sampler_status != SamplerStatus::ready) {
            diagnostic = {normal_detail_sampler_diagnostic.code.empty()
                              ? "vulkan_indexed_normal_detail_sampler_invalid"
                              : "vulkan_indexed_normal_detail_" + normal_detail_sampler_diagnostic.code,
                          normal_detail_sampler_diagnostic.message};
            return false;
        }
        draw.has_normal_detail_binding = true;
        draw.normal_detail_image = normal_detail_texture->image();
        draw.normal_detail_view = normal_detail_texture->view();
        draw.normal_detail_sampler = normal_detail_sampler->sampler();
    }

    const auto prepare_damage_binding =
        [&](const IndexedSampledTextureBinding& binding, std::string_view role,
            bool allow_srgb, VkImage& output_image, VkImageView& output_view,
            VkSampler& output_sampler) {
            const auto* source_texture = dynamic_cast<const VulkanTexture*>(binding.texture);
            const auto* source_sampler = dynamic_cast<const VulkanSampler*>(binding.sampler);
            const std::string prefix = "vulkan_indexed_" + std::string(role);
            if (source_texture == nullptr || source_sampler == nullptr) {
                diagnostic = {prefix + "_resource_type_unsupported",
                              "Vulkan indexed damage resources must use Vulkan texture and sampler handles"};
                return false;
            }
            if (source_texture->context() != context || source_sampler->context() != context) {
                diagnostic = {prefix + "_resource_context_mismatch",
                              "Vulkan indexed damage resources must belong to the draw device"};
                return false;
            }
            const TextureDescription& source = source_texture->info().description;
            const bool format_supported =
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
            if (!format_supported) {
                diagnostic = {prefix + "_resource_format_unsupported",
                              "Vulkan indexed damage resources use unsupported texture data"};
                return false;
            }
            if (!source_texture->initialized() ||
                source_texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                source_texture->image() == VK_NULL_HANDLE ||
                source_texture->view() == VK_NULL_HANDLE ||
                source_sampler->sampler() == VK_NULL_HANDLE) {
                diagnostic = {prefix + "_resource_invalid",
                              "Vulkan indexed damage resources must be initialized shader-readable handles"};
                return false;
            }
            output_image = source_texture->image();
            output_view = source_texture->view();
            output_sampler = source_sampler->sampler();
            return true;
        };
    if (damage_declaration) {
        if (!prepare_damage_binding(request.damage_binding, "damage", true,
                                    draw.damage_image, draw.damage_view,
                                    draw.damage_sampler) ||
            !prepare_damage_binding(request.damage_mask_binding, "damage_mask", false,
                                    draw.damage_mask_image, draw.damage_mask_view,
                                    draw.damage_mask_sampler))
            return false;
        draw.has_damage_binding = true;
        draw.has_damage_mask_binding = true;
    }

    if (material_declaration) {
        const auto* material_buffer = dynamic_cast<const VulkanBuffer*>(request.material_binding.buffer);
        if (material_buffer == nullptr) {
            diagnostic = {"vulkan_indexed_material_buffer_type_unsupported",
                          "Vulkan indexed material constants must use a Vulkan uniform buffer"};
            return false;
        }
        if (material_buffer->context() != context) {
            diagnostic = {"vulkan_indexed_material_buffer_context_mismatch",
                          "Vulkan indexed material constants must belong to the draw device"};
            return false;
        }
        const BufferDescription& description = material_buffer->info().description;
        if (description.usage != BufferUsage::uniform) {
            diagnostic = {"vulkan_indexed_material_buffer_usage_invalid",
                          "Vulkan indexed material constants require exclusive uniform buffer usage"};
            return false;
        }
        const std::uint64_t offset = request.material_binding.offset_bytes;
        const std::uint64_t range = request.material_binding.range_bytes;
        if (request.material_binding.range_bytes != portable_material_buffer_view_bytes) {
            diagnostic = {"vulkan_indexed_material_buffer_range_invalid",
                          "Vulkan indexed material constants require a 256-byte descriptor range"};
            return false;
        }
        if (offset % portable_material_buffer_view_bytes != 0U ||
            offset % context->min_uniform_buffer_offset_alignment != 0U) {
            diagnostic = {"vulkan_indexed_material_buffer_alignment_invalid",
                          "Vulkan indexed material constants offset violates the device uniform-buffer alignment"};
            return false;
        }
        if (range > context->max_uniform_buffer_range || offset > description.size_bytes ||
            range > description.size_bytes - offset || offset > std::numeric_limits<VkDeviceSize>::max()) {
            diagnostic = {"vulkan_indexed_material_buffer_range_invalid",
                          "Vulkan indexed material constants exceed the device or buffer range"};
            return false;
        }
        if (material_buffer->raw().buffer == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_material_buffer_handle_invalid",
                          "Vulkan indexed material constants contain a null buffer handle"};
            return false;
        }
        draw.has_material_binding = true;
        draw.material_buffer = material_buffer->raw().buffer;
        draw.material_offset = static_cast<VkDeviceSize>(offset);
        draw.material_range = static_cast<VkDeviceSize>(range);
    }
    if (frame_declaration) {
        const auto* frame_buffer = dynamic_cast<const VulkanBuffer*>(request.frame_binding.buffer);
        if (frame_buffer == nullptr) {
            diagnostic = {"vulkan_indexed_frame_buffer_type_unsupported",
                          "Vulkan indexed frame constants must use a Vulkan uniform buffer"};
            return false;
        }
        if (frame_buffer->context() != context) {
            diagnostic = {"vulkan_indexed_frame_buffer_context_mismatch",
                          "Vulkan indexed frame constants must belong to the draw device"};
            return false;
        }
        const BufferDescription& description = frame_buffer->info().description;
        if (description.usage != BufferUsage::uniform) {
            diagnostic = {"vulkan_indexed_frame_buffer_usage_invalid",
                          "Vulkan indexed frame constants require exclusive uniform buffer usage"};
            return false;
        }
        const std::uint64_t offset = request.frame_binding.offset_bytes;
        const std::uint64_t range = request.frame_binding.range_bytes;
        if (request.frame_binding.range_bytes != portable_frame_buffer_view_bytes) {
            diagnostic = {"vulkan_indexed_frame_buffer_range_invalid",
                          "Vulkan indexed frame constants require a 256-byte descriptor range"};
            return false;
        }
        if (offset % portable_frame_buffer_view_bytes != 0U ||
            offset % context->min_uniform_buffer_offset_alignment != 0U) {
            diagnostic = {"vulkan_indexed_frame_buffer_alignment_invalid",
                          "Vulkan indexed frame constants offset violates the device uniform-buffer alignment"};
            return false;
        }
        if (range > context->max_uniform_buffer_range || offset > description.size_bytes ||
            range > description.size_bytes - offset || offset > std::numeric_limits<VkDeviceSize>::max()) {
            diagnostic = {"vulkan_indexed_frame_buffer_range_invalid",
                          "Vulkan indexed frame constants exceed the device or buffer range"};
            return false;
        }
        if (frame_buffer->raw().buffer == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_frame_buffer_handle_invalid",
                          "Vulkan indexed frame constants contain a null buffer handle"};
            return false;
        }
        draw.has_frame_binding = true;
        draw.frame_buffer = frame_buffer->raw().buffer;
        draw.frame_offset = static_cast<VkDeviceSize>(offset);
        draw.frame_range = static_cast<VkDeviceSize>(range);
    }
    if (multimap_reflection_declaration) {
        const auto* cube = dynamic_cast<const VulkanTexture*>(
            request.multimap_reflection_binding.cube.texture);
        const auto* cube_sampler = dynamic_cast<const VulkanSampler*>(
            request.multimap_reflection_binding.cube.sampler);
        const auto* constants = dynamic_cast<const VulkanBuffer*>(
            request.multimap_reflection_binding.constants.buffer);
        if (cube == nullptr || cube_sampler == nullptr || constants == nullptr) {
            diagnostic = {
                "vulkan_indexed_multimap_reflection_resource_type_unsupported",
                "Vulkan MultiMap reflection requires Vulkan cube, sampler, and uniform-buffer handles"};
            return false;
        }
        if (cube->context() != context || cube_sampler->context() != context ||
            constants->context() != context) {
            diagnostic = {
                "vulkan_indexed_multimap_reflection_resource_context_mismatch",
                "Vulkan MultiMap reflection resources must belong to the draw device"};
            return false;
        }
        const TextureDescription& cube_description = cube->info().description;
        const auto cube_usage =
            static_cast<std::uint32_t>(cube_description.usage);
        const bool format_supported =
            cube_description.format == TextureFormat::rgba16_sfloat ||
            cube_description.format == TextureFormat::rgba8_unorm ||
            cube_description.format == TextureFormat::rgba8_srgb ||
            cube_description.format == TextureFormat::bgra8_unorm ||
            cube_description.format == TextureFormat::bgra8_srgb ||
            cube_description.format == TextureFormat::bc1_unorm ||
            cube_description.format == TextureFormat::bc1_srgb ||
            cube_description.format == TextureFormat::bc3_unorm ||
            cube_description.format == TextureFormat::bc3_srgb ||
            cube_description.format == TextureFormat::bc7_unorm ||
            cube_description.format == TextureFormat::bc7_srgb;
        if (cube_description.shape != TextureShape::texture_cube ||
            cube_description.array_layers != 1U ||
            cube_description.width == 0U ||
            cube_description.width != cube_description.height ||
            cube_description.mip_levels == 0U ||
            cube_description.samples != 1U || !format_supported ||
            (cube_usage &
             static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
            (cube_usage &
             static_cast<std::uint32_t>(TextureUsage::storage)) != 0U) {
            diagnostic = {
                "vulkan_indexed_multimap_reflection_cube_unsupported",
                "Vulkan MultiMap reflection requires one explicit sampled square cube"};
            return false;
        }
        if (!cube->initialized() ||
            cube->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
            cube->image() == VK_NULL_HANDLE || cube->view() == VK_NULL_HANDLE ||
            cube_sampler->sampler() == VK_NULL_HANDLE) {
            diagnostic = {
                "vulkan_indexed_multimap_reflection_cube_uninitialized",
                "Vulkan MultiMap reflection requires initialized shader-readable cube handles"};
            return false;
        }
        Diagnostic sampler_diagnostic;
        if (validate_sampler_description(cube_sampler->info().description,
                                         sampler_diagnostic) !=
                SamplerStatus::ready ||
            cube_sampler->info().description.compare !=
                SamplerCompare::disabled) {
            diagnostic = {
                "vulkan_indexed_multimap_reflection_sampler_invalid",
                sampler_diagnostic.message.empty()
                    ? "Vulkan MultiMap reflection requires comparison disabled"
                    : sampler_diagnostic.message};
            return false;
        }
        const IndexedMaterialBufferBinding& view =
            request.multimap_reflection_binding.constants;
        const BufferDescription& buffer_description =
            constants->info().description;
        const std::uint64_t offset = view.offset_bytes;
        const std::uint64_t range = view.range_bytes;
        if (buffer_description.usage != BufferUsage::uniform ||
            range != portable_multimap_reflection_buffer_view_bytes ||
            offset % portable_multimap_reflection_buffer_view_bytes != 0U ||
            offset % context->min_uniform_buffer_offset_alignment != 0U ||
            range > context->max_uniform_buffer_range ||
            offset > buffer_description.size_bytes ||
            range > buffer_description.size_bytes - offset ||
            offset > std::numeric_limits<VkDeviceSize>::max() ||
            constants->raw().buffer == VK_NULL_HANDLE) {
            diagnostic = {
                "vulkan_indexed_multimap_reflection_constants_invalid",
                "Vulkan MultiMap reflection constants violate the uniform-buffer view contract"};
            return false;
        }
        draw.has_multimap_reflection_binding = true;
        draw.multimap_cube_image = cube->image();
        draw.multimap_cube_view = cube->view();
        draw.multimap_cube_sampler = cube_sampler->sampler();
        draw.multimap_reflection_buffer = constants->raw().buffer;
        draw.multimap_reflection_offset = static_cast<VkDeviceSize>(offset);
        draw.multimap_reflection_range = static_cast<VkDeviceSize>(range);
    }
    if (pipeline_declares_directional_shadow_receiver(*request.pipeline)) {
        const auto* shadow_sampler =
            dynamic_cast<const VulkanSampler*>(
                request.directional_shadow_binding.sampler);
        const auto* shadow_constants =
            dynamic_cast<const VulkanBuffer*>(
                request.directional_shadow_binding.constants);
        if (shadow_sampler == nullptr || shadow_constants == nullptr) {
            diagnostic = {"vulkan_indexed_directional_shadow_resource_type_unsupported",
                          "Vulkan directional shadows require Vulkan depth, sampler, and uniform-buffer handles"};
            return false;
        }
        if (shadow_sampler->context() != context ||
            shadow_constants->context() != context) {
            diagnostic = {"vulkan_indexed_directional_shadow_resource_context_mismatch",
                          "Vulkan directional-shadow resources must belong to the draw device"};
            return false;
        }
        for (std::size_t cascade = 0U;
             cascade < indexed_directional_shadow_cascade_count; ++cascade) {
            auto* attachment = dynamic_cast<VulkanDepthAttachment*>(
                const_cast<DepthAttachment*>(
                    request.directional_shadow_binding.maps[cascade]));
            if (attachment == nullptr || attachment->context().get() != context.get()) {
                diagnostic = {"vulkan_indexed_directional_shadow_map_context_mismatch",
                              "Vulkan directional-shadow maps must belong to the draw device"};
                return false;
            }
            if (!attachment->initialized() ||
                attachment->layout() == VK_IMAGE_LAYOUT_UNDEFINED ||
                attachment->image() == VK_NULL_HANDLE ||
                attachment->view() == VK_NULL_HANDLE) {
                diagnostic = {"vulkan_indexed_directional_shadow_map_uninitialized",
                              "Vulkan directional-shadow maps must be initialized before sampling"};
                return false;
            }
            draw.shadow_attachments[cascade] = attachment;
            draw.shadow_views[cascade] = attachment->view();
        }
        const std::uint64_t offset =
            request.directional_shadow_binding.constants_offset_bytes;
        const std::uint64_t range =
            request.directional_shadow_binding.constants_range_bytes;
        if (range > context->max_uniform_buffer_range ||
            offset % context->min_uniform_buffer_offset_alignment != 0U ||
            offset > shadow_constants->info().description.size_bytes ||
            range > shadow_constants->info().description.size_bytes - offset ||
            offset > std::numeric_limits<VkDeviceSize>::max() ||
            shadow_constants->raw().buffer == VK_NULL_HANDLE ||
            shadow_sampler->sampler() == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_indexed_directional_shadow_resource_invalid",
                          "Vulkan directional-shadow sampler or constants are invalid"};
            return false;
        }
        draw.has_directional_shadow_binding = true;
        draw.shadow_sampler = shadow_sampler->sampler();
        draw.shadow_constants_buffer = shadow_constants->raw().buffer;
        draw.shadow_constants_offset = static_cast<VkDeviceSize>(offset);
        draw.shadow_constants_range = static_cast<VkDeviceSize>(range);
    }
    return true;
}

bool prepare_vulkan_stock_ks_per_pixel_native_abi_binding(
    const IndexedStaticMeshDrawRequest& request,
    const std::shared_ptr<VulkanContext>& context,
    VulkanIndexedBatchDraw& draw,
    Diagnostic& diagnostic) {
    const StockKsPerPixelVulkanNativeAbiDrawBinding* binding =
        request.stock_ks_per_pixel_vulkan_source != nullptr
            ? &request.stock_ks_per_pixel_vulkan_source->resources
            : request.stock_ks_per_pixel_vulkan_abi_probe;
    if (binding == nullptr) {
        diagnostic = {"vulkan_stock_abi_probe_binding_missing",
                      "The Vulkan ksPerPixel ABI probe binding is missing"};
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    for (std::size_t index = 0U; index < binding->uniform_buffers.size(); ++index) {
        const StockKsPerPixelVulkanAbiUniformBufferView& view =
            binding->uniform_buffers[index];
        const auto* buffer = dynamic_cast<const VulkanBuffer*>(
            view.buffer);
        if (buffer == nullptr || buffer->raw().buffer == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_stock_abi_probe_uniform_resource_invalid",
                          "The Vulkan ksPerPixel ABI probe requires Vulkan uniform-buffer handles"};
            return false;
        }
        if (buffer->context() != context) {
            diagnostic = {"vulkan_stock_abi_probe_context_mismatch",
                          "Every Vulkan ksPerPixel ABI probe resource must belong to the target device"};
            return false;
        }
        if (view.offset_bytes % properties.limits.minUniformBufferOffsetAlignment != 0U ||
            view.range_bytes > properties.limits.maxUniformBufferRange) {
            diagnostic = {"vulkan_stock_abi_probe_uniform_limit_unsupported",
                          "A Vulkan ksPerPixel ABI probe uniform view exceeds the selected device limits"};
            return false;
        }
        draw.stock_native_abi_uniform_buffers[index] = buffer->raw().buffer;
        draw.stock_native_abi_uniform_offsets[index] = view.offset_bytes;
        draw.stock_native_abi_uniform_ranges[index] = view.range_bytes;
    }
    const auto* diffuse = dynamic_cast<const VulkanTexture*>(binding->diffuse_texture);
    const auto* linear = dynamic_cast<const VulkanSampler*>(binding->linear_sampler);
    const auto* shadow_sampler = dynamic_cast<const VulkanSampler*>(binding->shadow_sampler);
    if (diffuse == nullptr || linear == nullptr || shadow_sampler == nullptr ||
        diffuse->image() == VK_NULL_HANDLE ||
        diffuse->view() == VK_NULL_HANDLE || linear->sampler() == VK_NULL_HANDLE ||
        shadow_sampler->sampler() == VK_NULL_HANDLE ||
        !diffuse->initialized() ||
        diffuse->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        diagnostic = {"vulkan_stock_abi_probe_resource_invalid",
                      "The Vulkan ksPerPixel ABI probe requires initialized shader-readable Vulkan resources"};
        return false;
    }
    if (diffuse->context() != context || linear->context() != context ||
        shadow_sampler->context() != context) {
        diagnostic = {"vulkan_stock_abi_probe_context_mismatch",
                      "Every Vulkan ksPerPixel ABI probe resource must belong to the target device"};
        return false;
    }
    draw.stock_native_abi_diffuse_view = diffuse->view();
    draw.stock_native_abi_linear_sampler = linear->sampler();
    draw.stock_native_abi_shadow_sampler = shadow_sampler->sampler();
    for (std::size_t index = 0U; index < binding->shadow_maps.size(); ++index) {
        auto* shadow = dynamic_cast<VulkanDepthAttachment*>(
            const_cast<DepthAttachment*>(binding->shadow_maps[index]));
        if (shadow == nullptr || !shadow->initialized() ||
            shadow->image() == VK_NULL_HANDLE || shadow->view() == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_stock_abi_probe_shadow_resource_invalid",
                          "The Vulkan ksPerPixel ABI probe requires initialized Vulkan shadow maps"};
            return false;
        }
        if (shadow->context() != context) {
            diagnostic = {"vulkan_stock_abi_probe_context_mismatch",
                          "Every Vulkan ksPerPixel ABI probe resource must belong to the target device"};
            return false;
        }
        draw.stock_native_abi_shadow_attachments[index] = shadow;
        draw.stock_native_abi_shadow_views[index] = shadow->view();
    }
    draw.has_stock_ks_per_pixel_vulkan_native_abi = true;
    return true;
}

class VulkanShaderModule final : public ShaderModule {
public:
    VulkanShaderModule(std::shared_ptr<VulkanContext> context, RawVulkanShaderModule raw,
                       ShaderModuleInfo info)
        : context_(std::move(context)), raw_(std::move(raw)), info_(info) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const ShaderModuleInfo& info() const noexcept override { return info_; }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanShaderModule raw_;
    ShaderModuleInfo info_;
};

VkFilter vk_sampler_filter(SamplerFilter filter) noexcept {
    return filter == SamplerFilter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode vk_sampler_mipmap(SamplerFilter filter) noexcept {
    return filter == SamplerFilter::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode vk_sampler_address(SamplerAddressMode mode) noexcept {
    switch (mode) {
    case SamplerAddressMode::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case SamplerAddressMode::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case SamplerAddressMode::clamp_to_edge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case SamplerAddressMode::clamp_to_border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkCompareOp vk_sampler_compare(SamplerCompare compare) noexcept {
    switch (compare) {
    case SamplerCompare::less: return VK_COMPARE_OP_LESS;
    case SamplerCompare::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case SamplerCompare::greater: return VK_COMPARE_OP_GREATER;
    case SamplerCompare::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case SamplerCompare::equal: return VK_COMPARE_OP_EQUAL;
    case SamplerCompare::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
    case SamplerCompare::always: return VK_COMPARE_OP_ALWAYS;
    case SamplerCompare::never: return VK_COMPARE_OP_NEVER;
    case SamplerCompare::disabled: break;
    }
    return VK_COMPARE_OP_ALWAYS;
}

class VulkanPresentationTarget final : public PresentationTarget {
public:
    VulkanPresentationTarget(std::shared_ptr<VulkanContext> context,
                             const PresentationTargetDescription& description,
                             Diagnostic& diagnostic)
        : context_(std::move(context)), info_({description}) {
        valid_ = create(diagnostic);
        if (!valid_) reset();
    }

    ~VulkanPresentationTarget() override { reset(); }

    Backend backend() const noexcept override { return Backend::Vulkan; }
    const PresentationTargetInfo& info() const noexcept override { return info_; }
    bool valid() const noexcept { return valid_; }
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }

    PresentationFrameResult clear_and_present(const std::array<float, 4>& clear_color) {
        for (const float value : clear_color) {
            if (!std::isfinite(value)) {
                return {PresentationFrameStatus::invalid_request,
                        {"presentation_clear_color_non_finite",
                         "Presentation clear colors must contain finite values"}};
            }
        }
        if (context_ == nullptr || swapchain_ == VK_NULL_HANDLE || command_buffer_ == VK_NULL_HANDLE ||
            !usable_) {
            return {PresentationFrameStatus::execution_failed,
                    {"vulkan_presentation_target_invalid",
                     "The Vulkan presentation target is not initialized"}};
        }

        std::lock_guard command_guard(context_->command_mutex);
        VkResult result = vkWaitForFences(context_->device, 1U, &frame_fence_, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) return failure("vkWaitForFences(presentation)", result);
        result = vkAcquireNextImageKHR(context_->device, swapchain_, UINT64_MAX,
                                       image_available_, VK_NULL_HANDLE, &image_index_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return {PresentationFrameStatus::execution_failed,
                    {"vulkan_presentation_target_out_of_date",
                     "The Vulkan presentation target requires swapchain recreation"}};
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return failure("vkAcquireNextImageKHR", result);
        result = vkResetCommandBuffer(command_buffer_, 0U);
        if (result != VK_SUCCESS) return failure("vkResetCommandBuffer(presentation)", result);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin);
        if (result == VK_SUCCESS) {
            VkClearValue clear{};
            for (std::size_t index = 0U; index < clear_color.size(); ++index)
                clear.color.float32[index] = clear_color[index];
            VkRenderPassBeginInfo render_begin{};
            render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_begin.renderPass = render_pass_;
            render_begin.framebuffer = framebuffers_[image_index_];
            render_begin.renderArea.extent = extent_;
            render_begin.clearValueCount = 1U;
            render_begin.pClearValues = &clear;
            vkCmdBeginRenderPass(command_buffer_, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(command_buffer_);
            result = vkEndCommandBuffer(command_buffer_);
        }
        if (result != VK_SUCCESS) return failure("Vulkan presentation command recording", result);

        result = vkResetFences(context_->device, 1U, &frame_fence_);
        if (result != VK_SUCCESS) {
            usable_ = false;
            return failure("vkResetFences(presentation)", result);
        }
        usable_ = false;

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1U;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command_buffer_;
        submit.signalSemaphoreCount = 1U;
        submit.pSignalSemaphores = &render_finished_;
        result = vkQueueSubmit(context_->queue, 1U, &submit, frame_fence_);
        if (result != VK_SUCCESS) return failure("vkQueueSubmit(presentation)", result);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1U;
        present.pWaitSemaphores = &render_finished_;
        present.swapchainCount = 1U;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index_;
        const VkResult present_result = vkQueuePresentKHR(context_->queue, &present);
        const VkResult wait_result = vkWaitForFences(context_->device, 1U, &frame_fence_, VK_TRUE, UINT64_MAX);
        if (wait_result != VK_SUCCESS) return failure("vkWaitForFences(presentation completion)", wait_result);
        usable_ = true;
        if (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR)
            return {PresentationFrameStatus::ready, {}};
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR)
            return {PresentationFrameStatus::execution_failed,
                    {"vulkan_presentation_target_out_of_date",
                     "The Vulkan presentation target requires swapchain recreation"}};
        return failure("vkQueuePresentKHR", present_result);
    }

    PresentationFrameResult present_texture(VulkanTexture& source) {
        if (context_ == nullptr || swapchain_ == VK_NULL_HANDLE || command_buffer_ == VK_NULL_HANDLE ||
            !usable_) {
            return {PresentationFrameStatus::execution_failed,
                    {"vulkan_presentation_target_invalid",
                     "The Vulkan presentation target is not initialized"}};
        }
        if (!source.initialized()) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_uninitialized",
                     "The presentation source must be rendered or initialized before use"}};
        }

        std::lock_guard command_guard(context_->command_mutex);
        VkResult result = vkWaitForFences(context_->device, 1U, &frame_fence_, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) return failure("vkWaitForFences(presentation texture)", result);
        result = vkAcquireNextImageKHR(context_->device, swapchain_, UINT64_MAX,
                                       image_available_, VK_NULL_HANDLE, &image_index_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return {PresentationFrameStatus::execution_failed,
                    {"vulkan_presentation_target_out_of_date",
                     "The Vulkan presentation target requires swapchain recreation"}};
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return failure("vkAcquireNextImageKHR", result);
        result = vkResetCommandBuffer(command_buffer_, 0U);
        if (result != VK_SUCCESS) return failure("vkResetCommandBuffer(presentation texture)", result);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin);
        const VkImageLayout source_layout = source.layout();
        if (result == VK_SUCCESS) {
            VkImageMemoryBarrier source_to_copy{};
            source_to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            source_to_copy.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            source_to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            source_to_copy.oldLayout = source_layout;
            source_to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            source_to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            source_to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            source_to_copy.image = source.image();
            source_to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            source_to_copy.subresourceRange.levelCount = 1U;
            source_to_copy.subresourceRange.layerCount = 1U;

            VkImageMemoryBarrier swapchain_to_copy{};
            swapchain_to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            swapchain_to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            swapchain_to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            swapchain_to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            swapchain_to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapchain_to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapchain_to_copy.image = images_[image_index_];
            swapchain_to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            swapchain_to_copy.subresourceRange.levelCount = 1U;
            swapchain_to_copy.subresourceRange.layerCount = 1U;
            const std::array<VkImageMemoryBarrier, 2U> before_copy = {
                source_to_copy, swapchain_to_copy};
            vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                                 nullptr, static_cast<std::uint32_t>(before_copy.size()),
                                 before_copy.data());

            VkImageCopy copy{};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1U;
            copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.dstSubresource.layerCount = 1U;
            copy.extent = {extent_.width, extent_.height, 1U};
            vkCmdCopyImage(command_buffer_, source.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           images_[image_index_], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);

            VkImageMemoryBarrier source_from_copy = source_to_copy;
            source_from_copy.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            source_from_copy.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            source_from_copy.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            source_from_copy.newLayout = source_layout;
            VkImageMemoryBarrier swapchain_to_present = swapchain_to_copy;
            swapchain_to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            swapchain_to_present.dstAccessMask = 0U;
            swapchain_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            swapchain_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            const std::array<VkImageMemoryBarrier, 2U> after_copy = {
                source_from_copy, swapchain_to_present};
            vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U,
                                 nullptr, static_cast<std::uint32_t>(after_copy.size()),
                                 after_copy.data());
            result = vkEndCommandBuffer(command_buffer_);
        }
        if (result != VK_SUCCESS)
            return failure("Vulkan texture presentation command recording", result);

        result = vkResetFences(context_->device, 1U, &frame_fence_);
        if (result != VK_SUCCESS) {
            usable_ = false;
            return failure("vkResetFences(presentation texture)", result);
        }
        usable_ = false;
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1U;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command_buffer_;
        submit.signalSemaphoreCount = 1U;
        submit.pSignalSemaphores = &render_finished_;
        result = vkQueueSubmit(context_->queue, 1U, &submit, frame_fence_);
        if (result != VK_SUCCESS) return failure("vkQueueSubmit(presentation texture)", result);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1U;
        present.pWaitSemaphores = &render_finished_;
        present.swapchainCount = 1U;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index_;
        const VkResult present_result = vkQueuePresentKHR(context_->queue, &present);
        const VkResult wait_result =
            vkWaitForFences(context_->device, 1U, &frame_fence_, VK_TRUE, UINT64_MAX);
        if (wait_result != VK_SUCCESS)
            return failure("vkWaitForFences(presentation texture completion)", wait_result);
        usable_ = true;
        if (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR)
            return {PresentationFrameStatus::ready, {}};
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR)
            return {PresentationFrameStatus::execution_failed,
                    {"vulkan_presentation_target_out_of_date",
                     "The Vulkan presentation target requires swapchain recreation"}};
        return failure("vkQueuePresentKHR", present_result);
    }

private:
    static PresentationFrameResult failure(const char* operation, VkResult result) {
        Diagnostic diagnostic = vk_error(operation, result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost"
                                                          : "vulkan_presentation_execution_failed";
        return {PresentationFrameStatus::execution_failed, std::move(diagnostic)};
    }

    static bool has_composite_alpha(VkCompositeAlphaFlagsKHR supported,
                                    VkCompositeAlphaFlagBitsKHR value) noexcept {
        return (supported & static_cast<VkCompositeAlphaFlagsKHR>(value)) != 0U;
    }

    bool create(Diagnostic& diagnostic) {
        if (context_ == nullptr || context_->presentation_surface == VK_NULL_HANDLE) {
            diagnostic = {"vulkan_presentation_surface_unavailable",
                          "The Vulkan device has no initialized presentation surface"};
            return false;
        }
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            context_->physical_device, context_->presentation_surface, &capabilities);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
            diagnostic.code = "vulkan_presentation_capabilities_failed";
            return false;
        }
        constexpr VkImageUsageFlags required_usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((capabilities.supportedUsageFlags & required_usage) != required_usage) {
            diagnostic = {"vulkan_presentation_image_usage_unsupported",
                          "The presentation surface does not support color and transfer-destination swapchain images"};
            return false;
        }

        VkSurfaceFormatKHR format{};
        std::uint32_t format_count = 0U;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(context_->physical_device,
                                                      context_->presentation_surface,
                                                      &format_count, nullptr);
        if (result != VK_SUCCESS || format_count == 0U || format_count > 4096U) {
            diagnostic = {"vulkan_presentation_formats_unavailable",
                          "The presentation surface returned no bounded surface formats"};
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(context_->physical_device,
                                                      context_->presentation_surface,
                                                      &format_count, formats.data());
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkGetPhysicalDeviceSurfaceFormatsKHR", result);
            diagnostic.code = "vulkan_presentation_formats_unavailable";
            return false;
        }
        formats.resize(format_count);
        const VkFormat requested_format = vk_texture_format(info_.description.format);
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if ((candidate.format == requested_format || candidate.format == VK_FORMAT_UNDEFINED) &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = candidate;
                if (candidate.format == VK_FORMAT_UNDEFINED) format.format = requested_format;
                break;
            }
        }
        if (format.format == VK_FORMAT_UNDEFINED) {
            diagnostic = {"vulkan_presentation_format_unsupported",
                          "The requested presentation format is not exposed by the presentation surface"};
            return false;
        }

        std::uint32_t present_mode_count = 0U;
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(context_->physical_device,
                                                            context_->presentation_surface,
                                                            &present_mode_count, nullptr);
        if (result != VK_SUCCESS || present_mode_count == 0U || present_mode_count > 4096U) {
            diagnostic = {"vulkan_presentation_modes_unavailable",
                          "The presentation surface returned no bounded present modes"};
            return false;
        }
        std::vector<VkPresentModeKHR> present_modes(present_mode_count);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(context_->physical_device,
                                                           context_->presentation_surface,
                                                           &present_mode_count, present_modes.data());
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkGetPhysicalDeviceSurfacePresentModesKHR", result);
            diagnostic.code = "vulkan_presentation_modes_unavailable";
            return false;
        }
        present_modes.resize(present_mode_count);
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        if (info_.description.vsync) {
            if (std::find(present_modes.begin(), present_modes.end(), present_mode) == present_modes.end()) {
                diagnostic = {"vulkan_presentation_fifo_unsupported",
                              "The presentation surface does not expose the required FIFO present mode"};
                return false;
            }
        } else {
            present_mode = VK_PRESENT_MODE_FIFO_KHR;
            for (const VkPresentModeKHR candidate : {VK_PRESENT_MODE_MAILBOX_KHR,
                                                     VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                     VK_PRESENT_MODE_FIFO_KHR}) {
                if (std::find(present_modes.begin(), present_modes.end(), candidate) != present_modes.end()) {
                    present_mode = candidate;
                    break;
                }
            }
        }

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != UINT32_MAX) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp(info_.description.width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(info_.description.height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        if (extent.width == 0U || extent.height == 0U) {
            diagnostic = {"vulkan_presentation_extent_invalid",
                          "The presentation surface returned a zero presentation extent"};
            return false;
        }
        std::uint32_t image_count = std::max(info_.description.image_count, capabilities.minImageCount);
        if ((capabilities.maxImageCount != 0U && image_count > capabilities.maxImageCount) ||
            image_count > max_presentation_image_count) {
            diagnostic = {"vulkan_presentation_image_count_unsupported",
                          "The requested presentation image count exceeds the surface limits"};
            return false;
        }
        VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (const VkCompositeAlphaFlagBitsKHR candidate : {
                 VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                 VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
            if (has_composite_alpha(capabilities.supportedCompositeAlpha, candidate)) {
                composite_alpha = candidate;
                break;
            }
        }
        if (!has_composite_alpha(capabilities.supportedCompositeAlpha, composite_alpha)) {
            diagnostic = {"vulkan_presentation_composite_alpha_unsupported",
                          "The presentation surface exposes no supported composite-alpha mode"};
            return false;
        }

        VkSwapchainCreateInfoKHR swapchain_info{};
        swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchain_info.surface = context_->presentation_surface;
        swapchain_info.minImageCount = image_count;
        swapchain_info.imageFormat = format.format;
        swapchain_info.imageColorSpace = format.colorSpace;
        swapchain_info.imageExtent = extent;
        swapchain_info.imageArrayLayers = 1U;
        swapchain_info.imageUsage = required_usage;
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.preTransform = capabilities.currentTransform;
        swapchain_info.compositeAlpha = composite_alpha;
        swapchain_info.presentMode = present_mode;
        swapchain_info.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(context_->device, &swapchain_info, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkCreateSwapchainKHR", result);
            diagnostic.code = "vulkan_presentation_swapchain_creation_failed";
            return false;
        }
        std::uint32_t image_count_result = 0U;
        result = vkGetSwapchainImagesKHR(context_->device, swapchain_, &image_count_result, nullptr);
        if (result != VK_SUCCESS || image_count_result == 0U || image_count_result > 4096U) {
            diagnostic = {"vulkan_presentation_images_unavailable",
                          "The Vulkan swapchain returned no bounded images"};
            return false;
        }
        images_.resize(image_count_result);
        result = vkGetSwapchainImagesKHR(context_->device, swapchain_, &image_count_result, images_.data());
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkGetSwapchainImagesKHR", result);
            diagnostic.code = "vulkan_presentation_images_unavailable";
            return false;
        }
        images_.resize(image_count_result);
        views_.resize(images_.size(), VK_NULL_HANDLE);
        for (std::size_t index = 0U; index < images_.size(); ++index) {
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = images_[index];
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = format.format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            result = vkCreateImageView(context_->device, &view_info, nullptr, &views_[index]);
            if (result != VK_SUCCESS) {
                diagnostic = vk_error("vkCreateImageView(presentation)", result);
                diagnostic.code = "vulkan_presentation_target_allocation_failed";
                return false;
            }
        }
        VkAttachmentDescription attachment{};
        attachment.format = format.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1U;
        subpass.pColorAttachments = &color_reference;
        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1U;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1U;
        render_pass_info.pSubpasses = &subpass;
        result = vkCreateRenderPass(context_->device, &render_pass_info, nullptr, &render_pass_);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkCreateRenderPass(presentation)", result);
            diagnostic.code = "vulkan_presentation_target_allocation_failed";
            return false;
        }
        framebuffers_.resize(views_.size(), VK_NULL_HANDLE);
        for (std::size_t index = 0U; index < views_.size(); ++index) {
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = render_pass_;
            framebuffer_info.attachmentCount = 1U;
            framebuffer_info.pAttachments = &views_[index];
            framebuffer_info.width = extent.width;
            framebuffer_info.height = extent.height;
            framebuffer_info.layers = 1U;
            result = vkCreateFramebuffer(context_->device, &framebuffer_info, nullptr,
                                         &framebuffers_[index]);
            if (result != VK_SUCCESS) {
                diagnostic = vk_error("vkCreateFramebuffer(presentation)", result);
                diagnostic.code = "vulkan_presentation_target_allocation_failed";
                return false;
            }
        }
        VkCommandBufferAllocateInfo command_allocation{};
        command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocation.commandPool = context_->command_pool;
        command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocation.commandBufferCount = 1U;
        result = vkAllocateCommandBuffers(context_->device, &command_allocation, &command_buffer_);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("vkAllocateCommandBuffers(presentation)", result);
            diagnostic.code = "vulkan_presentation_target_allocation_failed";
            return false;
        }
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result = vkCreateSemaphore(context_->device, &semaphore_info, nullptr, &image_available_);
        if (result == VK_SUCCESS)
            result = vkCreateSemaphore(context_->device, &semaphore_info, nullptr, &render_finished_);
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (result == VK_SUCCESS)
            result = vkCreateFence(context_->device, &fence_info, nullptr, &frame_fence_);
        if (result != VK_SUCCESS) {
            diagnostic = vk_error("Vulkan presentation synchronization", result);
            diagnostic.code = "vulkan_presentation_target_allocation_failed";
            return false;
        }
        format_ = format.format;
        extent_ = extent;
        info_.description.width = extent.width;
        info_.description.height = extent.height;
        return true;
    }

    void reset() noexcept {
        if (context_ != nullptr && context_->device != VK_NULL_HANDLE) {
            (void)context_->wait_idle();
            if (command_buffer_ != VK_NULL_HANDLE)
                vkFreeCommandBuffers(context_->device, context_->command_pool, 1U, &command_buffer_);
            if (frame_fence_ != VK_NULL_HANDLE) vkDestroyFence(context_->device, frame_fence_, nullptr);
            if (render_finished_ != VK_NULL_HANDLE)
                vkDestroySemaphore(context_->device, render_finished_, nullptr);
            if (image_available_ != VK_NULL_HANDLE)
                vkDestroySemaphore(context_->device, image_available_, nullptr);
            for (VkFramebuffer framebuffer : framebuffers_)
                if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(context_->device, framebuffer, nullptr);
            if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(context_->device, render_pass_, nullptr);
            for (VkImageView view : views_)
                if (view != VK_NULL_HANDLE) vkDestroyImageView(context_->device, view, nullptr);
            if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(context_->device, swapchain_, nullptr);
        }
        command_buffer_ = VK_NULL_HANDLE;
        frame_fence_ = VK_NULL_HANDLE;
        render_finished_ = VK_NULL_HANDLE;
        image_available_ = VK_NULL_HANDLE;
        framebuffers_.clear();
        views_.clear();
        images_.clear();
        render_pass_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        if (context_ != nullptr) context_->presentation_target_active = false;
        context_.reset();
    }

    std::shared_ptr<VulkanContext> context_;
    PresentationTargetInfo info_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_ = VK_NULL_HANDLE;
    VkFence frame_fence_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0U, 0U};
    std::uint32_t image_index_ = 0U;
    bool valid_ = false;
    bool usable_ = true;
};

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(std::shared_ptr<VulkanContext> context) : context_(std::move(context)) {}

    const DeviceInfo& info() const noexcept override { return context_->info; }
    bool owns_resource(const Texture& texture) const noexcept override {
        const auto* native = dynamic_cast<const VulkanTexture*>(&texture);
        return native != nullptr && native->context().get() == context_.get();
    }
    bool owns_resource(
        const DepthAttachment& attachment) const noexcept override {
        const auto* native =
            dynamic_cast<const VulkanDepthAttachment*>(&attachment);
        return native != nullptr && native->context().get() == context_.get();
    }
    bool owns_resource(const Sampler& sampler) const noexcept override {
        const auto* native = dynamic_cast<const VulkanSampler*>(&sampler);
        return native != nullptr && native->context().get() == context_.get();
    }

    PresentationCapabilities presentation_capabilities() const noexcept override {
        return context_->presentation_capabilities;
    }

    PresentationTargetResult create_presentation_target(
        const PresentationTargetDescription& description) override {
        Diagnostic diagnostic;
        const PresentationTargetStatus validation =
            validate_presentation_target_description(description, diagnostic);
        if (validation != PresentationTargetStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        if (!context_->presentation_capabilities.swapchain_api_available ||
            context_->presentation_surface == VK_NULL_HANDLE ||
            (!context_->presentation_capabilities.headless_surface_api_available &&
             context_->native_surface_destroy == nullptr)) {
            return {PresentationTargetStatus::unsupported,
                    {"vulkan_presentation_unsupported",
                     "This Vulkan device was not created with a presentation surface"},
                    nullptr};
        }
        if (context_->presentation_target_active) {
            return {PresentationTargetStatus::unsupported,
                    {"vulkan_presentation_target_active",
                     "This Vulkan device already owns a presentation target"},
                    nullptr};
        }
        context_->presentation_target_active = true;
        auto target = std::make_unique<VulkanPresentationTarget>(context_, description, diagnostic);
        if (!target->valid())
            return {PresentationTargetStatus::allocation_failed, std::move(diagnostic), nullptr};
        return {PresentationTargetStatus::ready, {}, std::move(target)};
    }

    PresentationFrameResult clear_and_present(
        PresentationTarget& target, const std::array<float, 4>& clear_color) override {
        if (target.backend() != Backend::Vulkan) {
            return {PresentationFrameStatus::unsupported,
                    {"presentation_target_backend_mismatch",
                     "The Vulkan device received a target from another backend"}};
        }
        auto* vulkan_target = dynamic_cast<VulkanPresentationTarget*>(&target);
        if (vulkan_target == nullptr || vulkan_target->context().get() != context_.get()) {
            return {PresentationFrameStatus::unsupported,
                    {"presentation_target_device_mismatch",
                     "The Vulkan device received an unknown presentation target"}};
        }
        return vulkan_target->clear_and_present(clear_color);
    }

    PresentationFrameResult present_texture(
        PresentationTarget& target, Texture& source) override {
        if (target.backend() != Backend::Vulkan || source.backend() != Backend::Vulkan) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_backend_mismatch",
                     "The presentation target and source texture must use Vulkan"}};
        }
        auto* vulkan_target = dynamic_cast<VulkanPresentationTarget*>(&target);
        auto* vulkan_source = dynamic_cast<VulkanTexture*>(&source);
        if (vulkan_target == nullptr || vulkan_source == nullptr ||
            vulkan_target->context().get() != context_.get() ||
            vulkan_source->context().get() != context_.get()) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_device_mismatch",
                     "The presentation target and source texture must belong to this Vulkan device"}};
        }
        const PresentationTargetDescription& target_description = target.info().description;
        const TextureDescription& source_description = source.info().description;
        if (source_description.width != target_description.width ||
            source_description.height != target_description.height) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_dimensions_mismatch",
                     "The presentation source dimensions must match the target exactly"}};
        }
        if (source_description.format != target_description.format) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_format_mismatch",
                     "The presentation source format must match the target exactly"}};
        }
        if (source_description.mip_levels != 1U || source_description.array_layers != 1U ||
            source_description.samples != 1U) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_shape_unsupported",
                     "The presentation source must have one mip, one layer, and one sample"}};
        }
        constexpr TextureUsage required_usage =
            TextureUsage::color_attachment | TextureUsage::transfer_source;
        if ((source_description.usage & required_usage) != required_usage) {
            return {PresentationFrameStatus::invalid_request,
                    {"presentation_texture_usage_invalid",
                     "The presentation source must support color attachment and transfer source usage"}};
        }
        return vulkan_target->present_texture(*vulkan_source);
    }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        Diagnostic diagnostic;
        if (!valid_buffer_description(description, initial_data.size(), diagnostic))
            return {BufferStatus::invalid_description, std::move(diagnostic), nullptr};
        VkBufferUsageFlags usage = vk_usage(description.usage);
        const bool device_local = description.memory == BufferMemory::device_local;
        if (device_local && (!initial_data.empty() || description.mutability == BufferMutability::mutable_data))
            usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        RawVulkanBuffer raw;
        if (!create_raw_buffer(context_, description, usage, description.memory, raw, diagnostic))
            return {BufferStatus::allocation_failed, std::move(diagnostic), nullptr};
        auto buffer = std::make_unique<VulkanBuffer>(context_, std::move(raw), description);
        bool uploaded = true;
        if (!initial_data.empty()) {
            uploaded = device_local ? buffer->write_device(0, initial_data, diagnostic)
                                    : buffer->write_host(0, initial_data, diagnostic);
        }
        if (!uploaded) {
            return {BufferStatus::upload_failed, std::move(diagnostic), nullptr};
        }
        return {BufferStatus::ready, {}, std::move(buffer)};
    }

    BufferUpdateResult update_buffer(Buffer& buffer,
                                      std::uint64_t offset,
                                      std::span<const std::byte> data) override {
        Diagnostic diagnostic;
        if (buffer.backend() != Backend::Vulkan) {
            return {BufferStatus::unsupported, {"buffer_backend_mismatch", "The buffer belongs to another graphics backend"}};
        }
        auto* vulkan_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        if (vulkan_buffer == nullptr) {
            return {BufferStatus::unsupported, {"buffer_type_unsupported", "The Vulkan device received an unknown buffer handle"}};
        }
        if (vulkan_buffer->context().get() != context_.get()) {
            return {BufferStatus::unsupported,
                    {"buffer_context_mismatch", "The buffer belongs to another Vulkan device"}};
        }
        if (!valid_buffer_update(buffer, offset, data.size(), diagnostic))
            return {BufferStatus::invalid_description, std::move(diagnostic)};
        const bool host_visible = vulkan_buffer->info().description.memory == BufferMemory::host_visible;
        const bool updated = host_visible ? vulkan_buffer->write_host(offset, data, diagnostic)
                                          : vulkan_buffer->write_device(offset, data, diagnostic);
        return updated ? BufferUpdateResult{BufferStatus::ready, {}}
                       : BufferUpdateResult{BufferStatus::upload_failed, std::move(diagnostic)};
    }

    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan& initial_uploads) override {
        Diagnostic diagnostic;
        if (!valid_texture_description(description, initial_uploads, diagnostic)) {
            const TextureStatus status = validate_texture_description(description, initial_uploads, diagnostic);
            return {status, std::move(diagnostic), nullptr};
        }
        if (description.memory != TextureMemory::device_local) {
            return {TextureStatus::unsupported,
                    {"vulkan_host_visible_texture_unsupported",
                     "Vulkan textures use optimal device-local tiling; host-visible texture memory is unsupported"},
                    nullptr};
        }
        RawVulkanImage raw;
        if (!create_raw_image(context_, description, raw, diagnostic)) {
            const bool unsupported = diagnostic.code == "texture_format_unsupported" ||
                                     diagnostic.code == "vulkan_compressed_format_unsupported" ||
                                     diagnostic.code == "vulkan_rgba_float_format_unsupported" ||
                                     diagnostic.code == "vulkan_rgba_float_samples_unsupported" ||
                                     diagnostic.code == "vulkan_texture_samples_unsupported" ||
                                     diagnostic.code == "vulkan_cube_array_unsupported";
            return {unsupported ? TextureStatus::unsupported : TextureStatus::allocation_failed,
                    std::move(diagnostic), nullptr};
        }
        auto texture = std::make_unique<VulkanTexture>(context_, std::move(raw), description,
                                                       VK_IMAGE_LAYOUT_UNDEFINED, false);
        if (!texture->upload(initial_uploads, diagnostic))
            return {TextureStatus::upload_failed, std::move(diagnostic), nullptr};
        return {TextureStatus::ready, {}, std::move(texture)};
    }

    TextureUpdateResult update_texture(Texture& texture,
                                       const TextureUploadPlan& uploads) override {
        Diagnostic diagnostic;
        if (texture.backend() != Backend::Vulkan)
            return {TextureStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}};
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        if (vulkan_texture == nullptr)
            return {TextureStatus::unsupported,
                    {"texture_type_unsupported", "The Vulkan device received an unknown texture handle"}};
        if (vulkan_texture->context().get() != context_.get())
            return {TextureStatus::unsupported,
                    {"texture_context_mismatch", "The texture belongs to another Vulkan device"}};
        if (!valid_texture_update(texture, uploads, diagnostic))
            return {TextureStatus::invalid_description, std::move(diagnostic)};
        return vulkan_texture->upload(uploads, diagnostic)
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
        if (texture.backend() != Backend::Vulkan)
            return {TextureStatus::unsupported,
                    {"texture_backend_mismatch",
                     "The texture belongs to another graphics backend"}};
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        if (vulkan_texture == nullptr)
            return {TextureStatus::unsupported,
                    {"texture_type_unsupported",
                     "The Vulkan device received an unknown texture handle"}};
        if (vulkan_texture->context().get() != context_.get())
            return {TextureStatus::unsupported,
                    {"texture_context_mismatch",
                     "The texture belongs to another Vulkan device"}};
        if (!vulkan_texture->base_mip_initialized())
            return {TextureStatus::invalid_description,
                    {"texture_mip_generation_base_uninitialized",
                     "Texture mip generation requires initialized mip-zero data for every physical layer"}};
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(
            context_->physical_device,
            vk_texture_format(texture.info().description.format),
            &properties);
        constexpr VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
            return {TextureStatus::unsupported,
                    {"vulkan_texture_mip_filter_unsupported",
                     "The Vulkan device cannot linearly blit the requested texture format"}};
        return vulkan_texture->generate_mips(diagnostic)
                   ? TextureUpdateResult{TextureStatus::ready, {}}
                   : TextureUpdateResult{TextureStatus::upload_failed,
                                         std::move(diagnostic)};
    }

    HdrLuminanceResult measure_hdr_luminance(Texture& source) override {
        Diagnostic diagnostic;
        const HdrLuminanceStatus validation =
            validate_hdr_luminance_request(source, diagnostic);
        if (validation != HdrLuminanceStatus::ready)
            return {validation, std::move(diagnostic), 0.0F};
        if (source.backend() != Backend::Vulkan)
            return {HdrLuminanceStatus::unsupported,
                    {"hdr_luminance_backend_mismatch",
                     "HDR luminance measurement requires a Vulkan texture"},
                    0.0F};
        auto* vulkan_source = dynamic_cast<VulkanTexture*>(&source);
        if (vulkan_source == nullptr)
            return {HdrLuminanceStatus::unsupported,
                    {"hdr_luminance_texture_type_unsupported",
                     "The Vulkan device received an unknown HDR luminance texture handle"},
                    0.0F};
        if (vulkan_source->context().get() != context_.get())
            return {HdrLuminanceStatus::unsupported,
                    {"hdr_luminance_context_mismatch",
                     "HDR luminance textures must belong to this Vulkan device"},
                    0.0F};
        if (!vulkan_source->initialized() ||
            !vulkan_source->base_mip_initialized())
            return {HdrLuminanceStatus::invalid_request,
                    {"hdr_luminance_source_uninitialized",
                     "HDR luminance measurement requires initialized mip-zero data"},
                    0.0F};

        if (vulkan_source->info().description.mip_levels > 1U &&
            !vulkan_source->generate_mips(diagnostic))
            return {HdrLuminanceStatus::execution_failed, std::move(diagnostic),
                    0.0F};

        float luminance = 0.0F;
        if (!read_hdr_luminance(context_, vulkan_source->image(),
                                vulkan_source->info().description,
                                *vulkan_source->mutable_layout_ptr(), luminance,
                                diagnostic))
            return {HdrLuminanceStatus::execution_failed, std::move(diagnostic),
                    0.0F};
        return {HdrLuminanceStatus::ready, {}, luminance};
    }

    HdrToneMapResult tone_map_hdr_texture(
        Texture& source, Texture& destination,
        const HdrToneMapParameters& parameters = {}) override {
        Diagnostic diagnostic;
        const HdrToneMapStatus validation = validate_hdr_tone_map_request(
            source, destination, parameters, diagnostic);
        if (validation != HdrToneMapStatus::ready)
            return {validation, std::move(diagnostic)};
        if (source.backend() != Backend::Vulkan || destination.backend() != Backend::Vulkan)
            return {HdrToneMapStatus::unsupported,
                    {"hdr_tone_map_backend_mismatch",
                     "HDR tone mapping requires Vulkan textures on this device"}};
        auto* vulkan_source = dynamic_cast<VulkanTexture*>(&source);
        auto* vulkan_destination = dynamic_cast<VulkanTexture*>(&destination);
        if (vulkan_source == nullptr || vulkan_destination == nullptr)
            return {HdrToneMapStatus::unsupported,
                    {"hdr_tone_map_texture_type_unsupported",
                     "The Vulkan device received an unknown HDR tone-map texture handle"}};
        if (vulkan_source->context().get() != context_.get() ||
            vulkan_destination->context().get() != context_.get())
            return {HdrToneMapStatus::unsupported,
                    {"hdr_tone_map_context_mismatch",
                     "HDR tone-map textures must belong to this Vulkan device"}};
        if (!vulkan_source->initialized())
            return {HdrToneMapStatus::invalid_request,
                    {"hdr_tone_map_source_uninitialized",
                     "HDR tone mapping requires an initialized source texture"}};
        if (context_->device == VK_NULL_HANDLE || context_->queue == VK_NULL_HANDLE ||
            context_->command_pool == VK_NULL_HANDLE)
            return {HdrToneMapStatus::unsupported,
                    {"vulkan_hdr_tone_map_uninitialized",
                     "HDR tone mapping requires an initialized Vulkan device context"}};
        if (!tone_map_vulkan_texture(*vulkan_source, *vulkan_destination, parameters,
                                     diagnostic))
            return {HdrToneMapStatus::execution_failed, std::move(diagnostic)};
        return {HdrToneMapStatus::ready, {}};
    }

    FxaaResult apply_fxaa(Texture& source, Texture& destination) override {
        Diagnostic diagnostic;
        const FxaaStatus validation = validate_fxaa_request(source, destination, diagnostic);
        if (validation != FxaaStatus::ready)
            return {validation, std::move(diagnostic)};
        if (source.backend() != Backend::Vulkan || destination.backend() != Backend::Vulkan)
            return {FxaaStatus::unsupported,
                    {"fxaa_backend_mismatch",
                     "FXAA requires Vulkan textures on this device"}};
        auto* vulkan_source = dynamic_cast<VulkanTexture*>(&source);
        auto* vulkan_destination = dynamic_cast<VulkanTexture*>(&destination);
        if (vulkan_source == nullptr || vulkan_destination == nullptr)
            return {FxaaStatus::unsupported,
                    {"fxaa_texture_type_unsupported",
                     "The Vulkan device received an unknown FXAA texture handle"}};
        if (vulkan_source->context().get() != context_.get() ||
            vulkan_destination->context().get() != context_.get())
            return {FxaaStatus::unsupported,
                    {"fxaa_context_mismatch",
                     "FXAA textures must belong to this Vulkan device"}};
        if (!vulkan_source->initialized())
            return {FxaaStatus::invalid_request,
                    {"fxaa_source_uninitialized",
                     "FXAA requires an initialized source texture"}};
        if (context_->device == VK_NULL_HANDLE || context_->queue == VK_NULL_HANDLE ||
            context_->command_pool == VK_NULL_HANDLE)
            return {FxaaStatus::unsupported,
                    {"vulkan_fxaa_uninitialized",
                     "FXAA requires an initialized Vulkan device context"}};
        if (!apply_fxaa_vulkan_texture(*vulkan_source, *vulkan_destination, diagnostic))
            return {FxaaStatus::execution_failed, std::move(diagnostic)};
        return {FxaaStatus::ready, {}};
    }

    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription& description) override {
        Diagnostic diagnostic;
        const DepthAttachmentStatus validation =
            validate_depth_attachment_description(description, diagnostic);
        if (validation != DepthAttachmentStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        RawVulkanDepthAttachment raw;
        if (!create_raw_depth_attachment(context_, description, raw, diagnostic)) {
            const DepthAttachmentStatus status = diagnostic.code == "depth_attachment_format_unsupported"
                                                     ? DepthAttachmentStatus::unsupported
                                                     : DepthAttachmentStatus::allocation_failed;
            return {status, std::move(diagnostic), nullptr};
        }
        return {DepthAttachmentStatus::ready, {},
                std::make_unique<VulkanDepthAttachment>(context_, std::move(raw), description)};
    }

    DepthAttachmentReadbackResult read_depth_attachment(
        DepthAttachment& attachment,
        const DepthAttachmentReadbackRequest& request) override {
        Diagnostic diagnostic;
        const DepthAttachmentReadbackStatus validation =
            validate_depth_attachment_readback(attachment, request, diagnostic);
        if (validation != DepthAttachmentReadbackStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (attachment.backend() != Backend::Vulkan)
            return {DepthAttachmentReadbackStatus::unsupported,
                    {"depth_attachment_backend_mismatch",
                     "The Vulkan device received a depth attachment from another backend"},
                    {}};
        auto* vulkan_depth = dynamic_cast<VulkanDepthAttachment*>(&attachment);
        if (vulkan_depth == nullptr)
            return {DepthAttachmentReadbackStatus::unsupported,
                    {"depth_attachment_type_unsupported",
                     "The Vulkan device received an unknown depth attachment handle"},
                    {}};
        if (vulkan_depth->context().get() != context_.get())
            return {DepthAttachmentReadbackStatus::unsupported,
                    {"depth_attachment_context_mismatch",
                     "The depth attachment belongs to another Vulkan device"},
                    {}};
        if (!vulkan_depth->initialized())
            return {DepthAttachmentReadbackStatus::invalid_request,
                    {"depth_attachment_uninitialized",
                     "D32 depth readback requires an initialized depth attachment"},
                    {}};
        std::vector<float> output;
        if (!vulkan_depth->readback(request, output, diagnostic))
            return {DepthAttachmentReadbackStatus::execution_failed, std::move(diagnostic), {}};
        return {DepthAttachmentReadbackStatus::ready, {}, std::move(output)};
    }

    TextureClearReadbackResult clear_texture_and_readback(
        Texture& texture, const TextureClearReadbackRequest& request) override {
        Diagnostic diagnostic;
        const TextureReadbackStatus validation = validate_texture_clear_readback(texture, request, diagnostic);
        if (validation != TextureReadbackStatus::ready)
            return {validation, std::move(diagnostic), {}};
        if (texture.backend() != Backend::Vulkan)
            return {TextureReadbackStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}, {}};
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        if (vulkan_texture == nullptr)
            return {TextureReadbackStatus::unsupported,
                    {"texture_type_unsupported", "The Vulkan device received an unknown texture handle"}, {}};
        if (vulkan_texture->context().get() != context_.get())
            return {TextureReadbackStatus::unsupported,
                    {"texture_context_mismatch", "The texture belongs to another Vulkan device"}, {}};
        std::vector<std::byte> output;
        if (!vulkan_texture->clear_readback(request, output, diagnostic))
            return {TextureReadbackStatus::execution_failed, std::move(diagnostic), {}};
        return {TextureReadbackStatus::ready, {}, std::move(output)};
    }

    TriangleDrawResult draw_triangle_and_readback(
        Texture& texture, const TriangleDrawRequest& request) override {
        Diagnostic diagnostic;
        const TriangleDrawStatus validation = validate_triangle_draw_request(texture, request, diagnostic);
        if (validation != TriangleDrawStatus::ready) return {validation, std::move(diagnostic), {}};
        if (texture.backend() != Backend::Vulkan)
            return {TriangleDrawStatus::unsupported,
                    {"texture_backend_mismatch", "The texture belongs to another graphics backend"}, {}};
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        if (vulkan_texture == nullptr)
            return {TriangleDrawStatus::unsupported,
                    {"texture_type_unsupported", "The Vulkan device received an unknown texture handle"}, {}};
        if (vulkan_texture->context().get() != context_.get())
            return {TriangleDrawStatus::unsupported,
                    {"texture_context_mismatch", "The texture belongs to another Vulkan device"}, {}};
        std::vector<std::byte> output;
        if (!vulkan_texture->draw_triangle(request, output, diagnostic))
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
        if (texture.backend() != Backend::Vulkan || request.vertex_buffer->backend() != Backend::Vulkan ||
            request.index_buffer->backend() != Backend::Vulkan) {
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_backend_mismatch",
                     "Indexed static-mesh resources must belong to the Vulkan backend"}, {}};
        }
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        auto* vulkan_vertices = dynamic_cast<const VulkanBuffer*>(request.vertex_buffer);
        auto* vulkan_indices = dynamic_cast<const VulkanBuffer*>(request.index_buffer);
        if (vulkan_texture == nullptr || vulkan_vertices == nullptr || vulkan_indices == nullptr) {
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_type_unsupported",
                     "The Vulkan device received an unknown indexed static-mesh resource handle"}, {}};
        }
        if (vulkan_texture->context() != vulkan_vertices->context() ||
            vulkan_texture->context() != vulkan_indices->context()) {
            return {IndexedStaticMeshDrawStatus::unsupported,
                    {"indexed_static_mesh_context_mismatch",
                     "Indexed static-mesh resources must be owned by the same Vulkan device"}, {}};
        }
        std::vector<std::byte> output;
        if (!vulkan_texture->draw_indexed_static_mesh(request, *vulkan_vertices, *vulkan_indices,
                                                      output, diagnostic))
            return {IndexedStaticMeshDrawStatus::execution_failed, std::move(diagnostic), {}};
        return {IndexedStaticMeshDrawStatus::ready, {}, std::move(output)};
    }

    IndexedStaticMeshBatchResult draw_indexed_static_mesh_batch_and_readback(
        Texture& texture, const IndexedStaticMeshBatchDescription& description) override {
        Diagnostic diagnostic;
        const IndexedStaticMeshBatchStatus validation =
            validate_indexed_static_mesh_batch_description(texture, description, diagnostic);
        if (validation != IndexedStaticMeshBatchStatus::ready) {
            IndexedStaticMeshBatchResult result;
            result.status = validation;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
        if (texture.backend() != Backend::Vulkan) {
            IndexedStaticMeshBatchResult result;
            result.status = IndexedStaticMeshBatchStatus::unsupported;
            result.diagnostic = {"indexed_static_mesh_backend_mismatch",
                                 "Indexed static-mesh resources must belong to the Vulkan backend"};
            return result;
        }
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        if (vulkan_texture == nullptr || vulkan_texture->context() != context_) {
            IndexedStaticMeshBatchResult result;
            result.status = IndexedStaticMeshBatchStatus::unsupported;
            result.diagnostic = {"indexed_static_mesh_type_unsupported",
                                 "The Vulkan device received an unknown indexed batch texture handle"};
            return result;
        }
        VulkanDepthAttachment* depth_attachment = nullptr;
        if (description.depth_attachment != nullptr) {
            depth_attachment = dynamic_cast<VulkanDepthAttachment*>(description.depth_attachment);
            if (depth_attachment == nullptr) {
                IndexedStaticMeshBatchResult result;
                result.status = IndexedStaticMeshBatchStatus::unsupported;
                result.diagnostic = {"indexed_depth_attachment_type_unsupported",
                                     "The Vulkan device received an unknown depth attachment handle"};
                return result;
            }
            if (depth_attachment->context() != context_) {
                IndexedStaticMeshBatchResult result;
                result.status = IndexedStaticMeshBatchStatus::unsupported;
                result.diagnostic = {"indexed_depth_attachment_context_mismatch",
                                     "The depth attachment belongs to another Vulkan device"};
                return result;
            }
        }
        VulkanTexture* resolve_target = nullptr;
        if (description.resolve_target != nullptr) {
            resolve_target = dynamic_cast<VulkanTexture*>(description.resolve_target);
            if (resolve_target == nullptr || resolve_target->context() != context_) {
                IndexedStaticMeshBatchResult result;
                result.status = IndexedStaticMeshBatchStatus::unsupported;
                result.diagnostic = {
                    "indexed_static_mesh_batch_resolve_context_mismatch",
                    "The resolve target does not belong to this Vulkan device"};
                return result;
            }
        }
        std::vector<std::byte> output;
        if (!vulkan_texture->draw_indexed_static_mesh_batch(
                description, depth_attachment, resolve_target, output, diagnostic)) {
            IndexedStaticMeshBatchResult result;
            result.status = IndexedStaticMeshBatchStatus::execution_failed;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
        IndexedStaticMeshBatchResult result;
        result.status = IndexedStaticMeshBatchStatus::ready;
        result.rgba8 = std::move(output);
        return result;
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
        const DepthOnlyIndexedStaticMeshBatchResult result =
            draw_depth_only_indexed_static_mesh_batch(batch);
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
        const DepthOnlyIndexedStaticMeshBatchDescription& description) override {
        Diagnostic diagnostic;
        const auto validation =
            validate_depth_only_indexed_static_mesh_batch_description(description, diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic)};
        auto* depth_attachment =
            dynamic_cast<VulkanDepthAttachment*>(description.depth_attachment);
        if (depth_attachment == nullptr || depth_attachment->context() != context_) {
            return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                    {"depth_only_indexed_depth_context_mismatch",
                     "The depth attachment does not belong to this Vulkan device"}};
        }

        std::vector<VulkanIndexedBatchDraw> draws;
        draws.reserve(description.draws.size());
        for (const DepthOnlyIndexedStaticMeshDrawRequest& request : description.draws) {
            auto* vertices = dynamic_cast<const VulkanBuffer*>(request.vertex_buffer);
            auto* indices = dynamic_cast<const VulkanBuffer*>(request.index_buffer);
            if (vertices == nullptr || indices == nullptr || request.packet == nullptr ||
                request.pipeline == nullptr || !request.camera_frame.has_value()) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                        {"depth_only_indexed_static_mesh_batch_resource_invalid",
                         "The Vulkan depth-only batch contains an unknown or incomplete resource"}};
            }
            if (vertices->context() != context_ || indices->context() != context_) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                        {"depth_only_indexed_static_mesh_context_mismatch",
                         "Depth-only indexed resources must belong to this Vulkan device"}};
            }
            const DrawPacket& packet = *request.packet;
            const std::uint64_t vertex_offset = static_cast<std::uint64_t>(packet.vertex_offset) *
                                                request.pipeline->vertex_layout.stride;
            const std::uint64_t index_offset = static_cast<std::uint64_t>(packet.index_offset) *
                                               sizeof(std::uint16_t);
            if (vertex_offset > std::numeric_limits<VkDeviceSize>::max() ||
                index_offset > std::numeric_limits<VkDeviceSize>::max()) {
                return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                        {"depth_only_indexed_static_mesh_offset_overflow",
                         "Depth-only indexed offsets exceed Vulkan addressability"}};
            }
            VulkanIndexedBatchDraw draw;
            draw.program = request.pipeline;
            draw.vertices = vertices;
            draw.indices = indices;
            draw.vertex_offset = static_cast<VkDeviceSize>(vertex_offset);
            draw.index_offset = static_cast<VkDeviceSize>(index_offset);
            draw.index_count = packet.index_count;
            draw.matrices = {packet.world_matrix, request.camera_frame->view_projection};
            if (request.material_mode ==
                DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::stock_alpha_tested) {
                const auto* alpha_texture =
                    dynamic_cast<const VulkanTexture*>(request.alpha_tested_diffuse_binding.texture);
                const auto* alpha_sampler =
                    dynamic_cast<const VulkanSampler*>(request.alpha_tested_diffuse_binding.sampler);
                const auto* alpha_material =
                    dynamic_cast<const VulkanBuffer*>(request.alpha_tested_material_binding.buffer);
                if (alpha_texture == nullptr || alpha_sampler == nullptr || alpha_material == nullptr) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                            {"vulkan_depth_alpha_resource_type_unsupported",
                             "Vulkan alpha-tested draws require Vulkan texture, sampler, and material handles"}};
                }
                if (alpha_texture->context() != context_ || alpha_sampler->context() != context_ ||
                    alpha_material->context() != context_) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                            {"vulkan_depth_alpha_context_mismatch",
                             "Vulkan alpha-tested resources must belong to this device"}};
                }
                const TextureDescription& alpha_description = alpha_texture->info().description;
                constexpr std::uint32_t sampled_usage =
                    static_cast<std::uint32_t>(TextureUsage::sampled);
                constexpr std::uint32_t forbidden_usage =
                    static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                    static_cast<std::uint32_t>(TextureUsage::storage) |
                    static_cast<std::uint32_t>(TextureUsage::transfer_destination);
                const std::uint32_t usage = static_cast<std::uint32_t>(alpha_description.usage);
                if (!alpha_texture->initialized() || alpha_texture->image() == VK_NULL_HANDLE ||
                    alpha_texture->view() == VK_NULL_HANDLE || alpha_texture->layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                            {"vulkan_depth_alpha_texture_uninitialized",
                             "Vulkan alpha-tested diffuse texture must be initialized before the depth draw"}};
                }
                if ((usage & sampled_usage) == 0U || (usage & forbidden_usage) != 0U) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                            {"vulkan_depth_alpha_texture_usage_invalid",
                             "Vulkan alpha-tested diffuse texture must have sampled-only usage"}};
                }
                const BufferDescription& material_description = alpha_material->info().description;
                if (material_description.usage != BufferUsage::uniform ||
                    alpha_material->raw().buffer == VK_NULL_HANDLE ||
                    request.alpha_tested_material_binding.offset_bytes %
                            stock_shadow_caster_buffer_alignment !=
                        0U ||
                    request.alpha_tested_material_binding.range_bytes !=
                        stock_shadow_caster_material_bytes ||
                    request.alpha_tested_material_binding.offset_bytes > material_description.size_bytes ||
                    static_cast<std::uint64_t>(request.alpha_tested_material_binding.range_bytes) >
                        material_description.size_bytes -
                            request.alpha_tested_material_binding.offset_bytes) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                            {"vulkan_depth_alpha_material_binding_invalid",
                             "Vulkan alpha-tested material binding does not fit its uniform buffer contract"}};
                }
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(context_->physical_device, &properties);
                if (properties.limits.minUniformBufferOffsetAlignment != 0U &&
                    request.alpha_tested_material_binding.offset_bytes %
                            properties.limits.minUniformBufferOffsetAlignment !=
                        0U) {
                    return {DepthOnlyIndexedStaticMeshBatchStatus::invalid_request,
                            {"vulkan_depth_alpha_material_offset_alignment_invalid",
                             "Vulkan alpha-tested material offset violates the device uniform-buffer alignment"}};
                }
                draw.has_alpha_tested_binding = true;
                draw.alpha_image = alpha_texture->image();
                draw.alpha_view = alpha_texture->view();
                draw.alpha_sampler = alpha_sampler->sampler();
                draw.alpha_material_buffer = alpha_material->raw().buffer;
                draw.alpha_material_offset = static_cast<VkDeviceSize>(
                    request.alpha_tested_material_binding.offset_bytes);
                draw.alpha_material_range = static_cast<VkDeviceSize>(
                    request.alpha_tested_material_binding.range_bytes);
                draw.alpha_mip_levels = alpha_description.mip_levels;
                draw.alpha_layout = alpha_texture->layout_ptr();
            }
            draws.push_back(draw);
        }
        if (!draw_depth_only_indexed_batch(context_, *depth_attachment, draws,
                                           description.clear_depth,
                                           description.depth_clear_value, diagnostic)) {
            return {DepthOnlyIndexedStaticMeshBatchStatus::execution_failed,
                    std::move(diagnostic)};
        }
        return {DepthOnlyIndexedStaticMeshBatchStatus::ready, {}};
    }

    SamplerResult create_sampler(const SamplerDescription& description) override {
        Diagnostic diagnostic;
        if (!valid_sampler_description(description, diagnostic))
            return {SamplerStatus::invalid_description, std::move(diagnostic), nullptr};
        const bool anisotropic = description.min_filter == SamplerFilter::anisotropic ||
                                 description.mag_filter == SamplerFilter::anisotropic ||
                                 description.mip_filter == SamplerFilter::anisotropic;
        if (anisotropic && !context_->sampler_anisotropy)
            return {SamplerStatus::unsupported,
                    {"vulkan_sampler_anisotropy_unsupported", "The Vulkan device does not expose sampler anisotropy"},
                    nullptr};
        if (anisotropic && description.max_anisotropy > context_->max_sampler_anisotropy)
            return {SamplerStatus::unsupported,
                    {"vulkan_sampler_anisotropy_limit", "The requested sampler anisotropy exceeds the physical device limit"},
                    nullptr};
        VkPhysicalDeviceProperties sampler_properties{};
        vkGetPhysicalDeviceProperties(context_->physical_device,
                                      &sampler_properties);
        if (std::abs(description.mip_lod_bias) >
            sampler_properties.limits.maxSamplerLodBias)
            return {SamplerStatus::unsupported,
                    {"vulkan_sampler_lod_bias_limit",
                     "The requested sampler LOD bias exceeds the physical device limit"},
                    nullptr};
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = vk_sampler_filter(description.mag_filter);
        sampler_info.minFilter = vk_sampler_filter(description.min_filter);
        sampler_info.mipmapMode = vk_sampler_mipmap(description.mip_filter);
        sampler_info.addressModeU = vk_sampler_address(description.address_u);
        sampler_info.addressModeV = vk_sampler_address(description.address_v);
        sampler_info.addressModeW = vk_sampler_address(description.address_w);
        sampler_info.mipLodBias = description.mip_lod_bias;
        sampler_info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
        sampler_info.maxAnisotropy = description.max_anisotropy;
        sampler_info.compareEnable = description.compare == SamplerCompare::disabled ? VK_FALSE : VK_TRUE;
        sampler_info.compareOp = vk_sampler_compare(description.compare);
        sampler_info.minLod = description.min_lod;
        sampler_info.maxLod = description.max_lod;
        sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sampler_info.unnormalizedCoordinates = VK_FALSE;
        RawVulkanSampler raw;
        raw.context = context_;
        const VkResult result = vkCreateSampler(context_->device, &sampler_info, nullptr, &raw.sampler);
        if (result != VK_SUCCESS) {
            Diagnostic failure = vk_error("vkCreateSampler", result);
            failure.code = "sampler_allocation_failed";
            return {SamplerStatus::allocation_failed, std::move(failure), nullptr};
        }
        return {SamplerStatus::ready, {}, std::make_unique<VulkanSampler>(context_, std::move(raw), description)};
    }

    ShaderModuleResult create_shader_module(const ShaderModuleDescription& description) override {
        Diagnostic diagnostic;
        const auto validation = validate_shader_module_description(description, diagnostic);
        if (validation != ShaderModuleStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        ShaderBytecodeFormat format{};
        if (!shader_bytecode_format(description.bytecode, format) || format != ShaderBytecodeFormat::spirv)
            return {ShaderModuleStatus::unsupported,
                    {"vulkan_shader_format_unsupported", "Vulkan shader modules require SPIR-V bytecode"}, nullptr};
        std::vector<std::uint32_t> words(description.bytecode.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), description.bytecode.data(), description.bytecode.size());
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = description.bytecode.size();
        module_info.pCode = words.data();
        RawVulkanShaderModule raw;
        raw.context = context_;
        const VkResult result = vkCreateShaderModule(context_->device, &module_info, nullptr, &raw.module);
        if (result != VK_SUCCESS) {
            Diagnostic failure = vk_error("vkCreateShaderModule", result);
            failure.code = "shader_module_allocation_failed";
            return {ShaderModuleStatus::allocation_failed, std::move(failure), nullptr};
        }
        return {ShaderModuleStatus::ready, {},
                std::make_unique<VulkanShaderModule>(context_, std::move(raw),
                                                      ShaderModuleInfo{description.stage, format, description.bytecode.size()})};
    }

    void wait_idle() noexcept override {
        if (context_) (void)context_->wait_idle();
    }

private:
    std::shared_ptr<VulkanContext> context_;
};

} // namespace

AdapterResult enumerate_vulkan_adapters(const DeviceOptions& options) {
    Instance instance;
    AdapterResult created = make_instance(options, instance);
    if (!created.ok()) return created;
    RawVulkanSurface presentation_surface;
    if (options.enable_headless_presentation) {
        Diagnostic diagnostic;
        if (!create_headless_surface(instance.value, presentation_surface, diagnostic)) {
            AdapterResult result;
            result.status = DeviceStatus::initialization_failed;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
    } else if (!options.headless && options.native_surface.has_value()) {
        Diagnostic diagnostic;
        if (!create_native_surface(instance.value, *options.native_surface,
                                   presentation_surface, diagnostic)) {
            AdapterResult result;
            result.status = DeviceStatus::initialization_failed;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
    }
    const auto devices = physical_devices(instance.value, presentation_surface.surface);
    AdapterResult result;
    if (devices.empty()) {
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"vulkan_no_graphics_device",
                             options.enable_headless_presentation || !options.headless
                                 ? "Vulkan initialized, but no physical device exposes a graphics and present-capable queue"
                                 : "Vulkan initialized, but no physical device exposes a graphics queue"};
        return result;
    }
    result.status = DeviceStatus::ready;
    for (const auto& device : devices) {
        DeviceInfo info = info_from(device.properties);
        if (options.allow_software || !info.software) {
            result.adapters.push_back(std::move(info));
        }
    }
    if (options.prefer_software) {
        std::stable_sort(result.adapters.begin(), result.adapters.end(),
                         [](const DeviceInfo& left, const DeviceInfo& right) {
                             return left.software && !right.software;
                         });
    }
    if (result.adapters.empty()) {
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"vulkan_no_usable_device", "Vulkan devices were found, but all were software devices"};
    }
    return result;
}

DeviceResult create_vulkan_device(const DeviceOptions& options) {
    Instance instance;
    AdapterResult created = make_instance(options, instance);
    if (!created.ok()) {
        DeviceResult result;
        result.status = created.status;
        result.diagnostic = std::move(created.diagnostic);
        return result;
    }

    RawVulkanSurface presentation_surface;
    if (options.enable_headless_presentation) {
        Diagnostic diagnostic;
        if (!create_headless_surface(instance.value, presentation_surface, diagnostic)) {
            DeviceResult result;
            result.status = DeviceStatus::initialization_failed;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
    } else if (!options.headless && options.native_surface.has_value()) {
        Diagnostic diagnostic;
        if (!create_native_surface(instance.value, *options.native_surface,
                                   presentation_surface, diagnostic)) {
            DeviceResult result;
            result.status = DeviceStatus::initialization_failed;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
    }

    const auto devices = physical_devices(instance.value, presentation_surface.surface);
    std::vector<PhysicalDevice> usable;
    for (const auto& device : devices) {
        const bool software = device.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (options.allow_software || !software) {
            usable.push_back(device);
        }
    }
    if (options.prefer_software) {
        std::stable_sort(usable.begin(), usable.end(),
                         [](const PhysicalDevice& left, const PhysicalDevice& right) {
                             const bool left_software = left.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
                             const bool right_software = right.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
                             return left_software && !right_software;
                         });
    }
    if (usable.empty()) {
        DeviceResult result;
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"vulkan_no_usable_device",
                             options.enable_headless_presentation || !options.headless
                                 ? "Vulkan has no usable graphics and present-capable device"
                                 : "Vulkan has no usable graphics device"};
        return result;
    }
    if (options.adapter_index >= usable.size()) {
        DeviceResult result;
        result.status = DeviceStatus::invalid_options;
        result.diagnostic = {"adapter_index_out_of_range", "Vulkan adapter index is outside the enumerated device list"};
        return result;
    }

    const PhysicalDevice& selected = usable[options.adapter_index];
    const PresentationCapabilities presentation_capabilities =
        query_vulkan_presentation_capabilities(selected.handle);
    if ((options.enable_headless_presentation || !options.headless) &&
        (!presentation_capabilities.swapchain_api_available ||
         (!options.headless && !presentation_capabilities.native_surface_api_available) ||
         (options.headless && !presentation_capabilities.headless_surface_api_available))) {
        DeviceResult result;
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"vulkan_presentation_unsupported",
                             options.headless
                                 ? "The selected Vulkan device lacks the headless surface or swapchain extension"
                                 : "The selected Vulkan device lacks the native surface or swapchain extension"};
        return result;
    }
    std::vector<const char*> device_extensions;
    if (options.enable_headless_presentation || !options.headless)
        device_extensions.push_back("VK_KHR_swapchain");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = selected.graphics_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures supported_features{};
    vkGetPhysicalDeviceFeatures(selected.handle, &supported_features);
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = supported_features.samplerAnisotropy;
    features.imageCubeArray = supported_features.imageCubeArray;
    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames =
        device_extensions.empty() ? nullptr : device_extensions.data();
    create_info.pEnabledFeatures = &features;

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result_code = vkCreateDevice(selected.handle, &create_info, nullptr, &device);
    if (result_code != VK_SUCCESS) {
        DeviceResult result;
        result.status = DeviceStatus::initialization_failed;
        result.diagnostic = vk_error("vkCreateDevice", result_code);
        return result;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, selected.graphics_queue_family, 0, &queue);
    VkCommandPoolCreateInfo command_pool_info{};
    command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = selected.graphics_queue_family;
    auto context = std::make_shared<VulkanContext>(std::move(instance), selected.handle, device, queue,
                                                   selected.graphics_queue_family, info_from(selected.properties),
                                                   supported_features.samplerAnisotropy == VK_TRUE,
                                                   supported_features.imageCubeArray == VK_TRUE,
                                                   selected.properties.limits.maxSamplerAnisotropy,
                                                   selected.properties.limits.maxUniformBufferRange,
                                                   selected.properties.limits.minUniformBufferOffsetAlignment);
    context->presentation_surface = std::exchange(presentation_surface.surface, VK_NULL_HANDLE);
    context->native_surface_context = std::exchange(presentation_surface.callback_context, nullptr);
    context->native_surface_destroy =
        std::exchange(presentation_surface.destroy_callback, nullptr);
    context->presentation_capabilities = presentation_capabilities;
    const VkResult pool_result = vkCreateCommandPool(device, &command_pool_info, nullptr, &context->command_pool);
    if (pool_result != VK_SUCCESS) {
        DeviceResult failure;
        failure.status = DeviceStatus::initialization_failed;
        failure.diagnostic = vk_error("vkCreateCommandPool", pool_result);
        return failure;
    }
    DeviceResult result;
    result.status = DeviceStatus::ready;
    result.device = std::make_unique<VulkanDevice>(std::move(context));
    return result;
}

#else

AdapterResult enumerate_vulkan_adapters(const DeviceOptions&) {
    AdapterResult result;
    result.status = DeviceStatus::unavailable;
    result.diagnostic = {"vulkan_not_compiled", "Vulkan support is not compiled into this build"};
    return result;
}

DeviceResult create_vulkan_device(const DeviceOptions&) {
    DeviceResult result;
    result.status = DeviceStatus::unavailable;
    result.diagnostic = {"vulkan_not_compiled", "Vulkan support is not compiled into this build"};
    return result;
}

#endif

} // namespace apex::render
