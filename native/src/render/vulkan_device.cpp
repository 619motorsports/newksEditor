#include "backend_internal.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

#if defined(APEX_HAS_VULKAN) && APEX_HAS_VULKAN
#    include <vulkan/vulkan.h>
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

struct PhysicalDevice {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::uint32_t graphics_queue_family = UINT32_MAX;
};

AdapterResult make_instance(const DeviceOptions& options, Instance& instance) {
    std::vector<const char*> layers;
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

std::vector<PhysicalDevice> physical_devices(VkInstance instance) {
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
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                families[index].queueCount != 0) {
                graphics_family = index;
                break;
            }
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
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::mutex command_mutex;
    bool sampler_anisotropy = false;
    float max_sampler_anisotropy = 1.0F;
    DeviceInfo info;

    VulkanContext(Instance instance_value,
                  VkPhysicalDevice physical,
                  VkDevice device_value,
                  VkQueue queue_value,
                  std::uint32_t family,
                  DeviceInfo info_value,
                  bool sampler_anisotropy_value,
                  float max_sampler_anisotropy_value)
        : instance(std::move(instance_value)), physical_device(physical), device(device_value),
          queue(queue_value), queue_family(family), sampler_anisotropy(sampler_anisotropy_value),
          max_sampler_anisotropy(max_sampler_anisotropy_value), info(std::move(info_value)) {}

    ~VulkanContext() {
        if (device != VK_NULL_HANDLE) {
            (void)wait_idle();
            if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyDevice(device, nullptr);
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
    case TextureFormat::r5g6b5_unorm:
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    default:
        return VK_FORMAT_UNDEFINED;
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

VkImageLayout texture_final_layout(TextureUsage usage) {
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

bool create_raw_image(const std::shared_ptr<VulkanContext>& context,
                      const TextureDescription& description,
                      RawVulkanImage& raw,
                      Diagnostic& diagnostic) {
    raw.reset();
    raw.context = context;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = vk_texture_format(description.format);
    image_info.extent = {description.width, description.height, 1};
    image_info.mipLevels = description.mip_levels;
    image_info.arrayLayers = description.array_layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = vk_texture_usage(description.usage);
    // The first contract always permits synchronous uploads. A later
    // resource policy can remove this bit when immutable no-upload images are
    // introduced, but doing so now would make update validation backend-state
    // dependent.
    image_info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageFormatProperties format_properties{};
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        context->physical_device, image_info.format, image_info.imageType, image_info.tiling,
        image_info.usage, 0, &format_properties);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkGetPhysicalDeviceImageFormatProperties", result);
        diagnostic.code = "texture_format_unsupported";
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
    view_info.viewType = description.array_layers == 1U ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = image_info.format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = description.mip_levels;
    view_info.subresourceRange.layerCount = description.array_layers;
    result = vkCreateImageView(context->device, &view_info, nullptr, &raw.view);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("vkCreateImageView", result);
        diagnostic.code = "texture_allocation_failed";
        raw.reset();
        return false;
    }
    return true;
}

bool copy_texture_uploads(const std::shared_ptr<VulkanContext>& context,
                          RawVulkanImage& image,
                          const TextureDescription& description,
                          const TextureUploadPlan& uploads,
                          VkImageLayout& current_layout,
                          Diagnostic& diagnostic) {
    if (!texture_format_cpu_upload_supported(description.format)) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "Block-compressed texture uploads require a dedicated GPU upload path"};
        return false;
    }
    const auto bytes_per_pixel = texture_format_bytes_per_pixel(description.format);
    if (bytes_per_pixel == 0U) {
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
        copy.bufferRowLength = static_cast<std::uint32_t>(upload.row_pitch / bytes_per_pixel);
        copy.bufferImageHeight = upload.height;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = upload.mip_level;
        copy.imageSubresource.baseArrayLayer = upload.array_layer;
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
            barrier.subresourceRange.layerCount = description.array_layers;
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
            barrier.newLayout = texture_final_layout(description.usage);
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
                if (completed) current_layout = texture_final_layout(description.usage);
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
            current_layout = texture_final_layout(description.usage);
        }
    }
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    staging.reset();
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan texture upload", result);
        diagnostic.code = result == VK_ERROR_DEVICE_LOST ? "vulkan_device_lost" : "texture_upload_failed";
        return false;
    }
    if (!completed) current_layout = texture_final_layout(description.usage);
    return true;
}

bool clear_texture_and_readback(const std::shared_ptr<VulkanContext>& context,
                                RawVulkanImage& image,
                                const TextureDescription& description,
                                const TextureClearReadbackRequest& request,
                                VkImageLayout& current_layout,
                                std::vector<std::byte>& output,
                                Diagnostic& diagnostic) {
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
            barrier.subresourceRange.layerCount = description.array_layers;
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
            barrier.newLayout = texture_final_layout(description.usage);
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
            current_layout = texture_final_layout(description.usage);
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(description.usage);
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

class VulkanTexture final : public Texture {
public:
    VulkanTexture(std::shared_ptr<VulkanContext> context,
                  RawVulkanImage raw,
                  TextureDescription description,
                  VkImageLayout layout)
        : context_(std::move(context)), raw_(std::move(raw)), info_({description}), layout_(layout) {}

    ~VulkanTexture() override = default;
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const TextureInfo& info() const noexcept override { return info_; }

    bool upload(const TextureUploadPlan& uploads, Diagnostic& diagnostic) {
        return copy_texture_uploads(context_, raw_, info_.description, uploads, layout_, diagnostic);
    }

    bool clear_readback(const TextureClearReadbackRequest& request,
                        std::vector<std::byte>& output,
                        Diagnostic& diagnostic) {
        return clear_texture_and_readback(context_, raw_, info_.description, request, layout_, output, diagnostic);
    }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanImage raw_;
    TextureInfo info_;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

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

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanSampler raw_;
    SamplerInfo info_;
};

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

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(std::shared_ptr<VulkanContext> context) : context_(std::move(context)) {}

    const DeviceInfo& info() const noexcept override { return context_->info; }

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
        if (!valid_buffer_update(buffer, offset, data.size(), diagnostic))
            return {BufferStatus::invalid_description, std::move(diagnostic)};
        auto* vulkan_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        if (vulkan_buffer == nullptr) {
            return {BufferStatus::unsupported, {"buffer_type_unsupported", "The Vulkan device received an unknown buffer handle"}};
        }
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
        if (!create_raw_image(context_, description, raw, diagnostic))
            return {TextureStatus::allocation_failed, std::move(diagnostic), nullptr};
        auto texture = std::make_unique<VulkanTexture>(context_, std::move(raw), description,
                                                       VK_IMAGE_LAYOUT_UNDEFINED);
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
        if (!valid_texture_update(texture, uploads, diagnostic))
            return {TextureStatus::invalid_description, std::move(diagnostic)};
        auto* vulkan_texture = dynamic_cast<VulkanTexture*>(&texture);
        if (vulkan_texture == nullptr)
            return {TextureStatus::unsupported,
                    {"texture_type_unsupported", "The Vulkan device received an unknown texture handle"}};
        return vulkan_texture->upload(uploads, diagnostic)
                   ? TextureUpdateResult{TextureStatus::ready, {}}
                   : TextureUpdateResult{TextureStatus::upload_failed, std::move(diagnostic)};
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
        std::vector<std::byte> output;
        if (!vulkan_texture->clear_readback(request, output, diagnostic))
            return {TextureReadbackStatus::execution_failed, std::move(diagnostic), {}};
        return {TextureReadbackStatus::ready, {}, std::move(output)};
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
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = vk_sampler_filter(description.mag_filter);
        sampler_info.minFilter = vk_sampler_filter(description.min_filter);
        sampler_info.mipmapMode = vk_sampler_mipmap(description.mip_filter);
        sampler_info.addressModeU = vk_sampler_address(description.address_u);
        sampler_info.addressModeV = vk_sampler_address(description.address_v);
        sampler_info.addressModeW = vk_sampler_address(description.address_w);
        sampler_info.mipLodBias = 0.0F;
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
    const auto devices = physical_devices(instance.value);
    AdapterResult result;
    if (devices.empty()) {
        result.status = DeviceStatus::unavailable;
        result.diagnostic = {"vulkan_no_graphics_device",
                             "Vulkan initialized, but no physical device exposes a graphics queue"};
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

    const auto devices = physical_devices(instance.value);
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
        result.diagnostic = {"vulkan_no_usable_device", "Vulkan has no usable graphics device"};
        return result;
    }
    if (options.adapter_index >= usable.size()) {
        DeviceResult result;
        result.status = DeviceStatus::invalid_options;
        result.diagnostic = {"adapter_index_out_of_range", "Vulkan adapter index is outside the enumerated device list"};
        return result;
    }

    const PhysicalDevice& selected = usable[options.adapter_index];
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
    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
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
                                                   selected.properties.limits.maxSamplerAnisotropy);
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
