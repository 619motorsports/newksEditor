#include "backend_internal.hpp"
#include "apex/render/draw_packet.hpp"

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
    VkDeviceSize max_uniform_buffer_range = 0U;
    VkDeviceSize min_uniform_buffer_offset_alignment = 1U;
    DeviceInfo info;

    VulkanContext(Instance instance_value,
                  VkPhysicalDevice physical,
                  VkDevice device_value,
                  VkQueue queue_value,
                  std::uint32_t family,
                  DeviceInfo info_value,
                  bool sampler_anisotropy_value,
                  float max_sampler_anisotropy_value,
                  VkDeviceSize max_uniform_buffer_range_value,
                  VkDeviceSize min_uniform_buffer_offset_alignment_value)
        : instance(std::move(instance_value)), physical_device(physical), device(device_value),
          queue(queue_value), queue_family(family), sampler_anisotropy(sampler_anisotropy_value),
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
    case TextureFormat::r5g6b5_unorm:
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    default:
        return VK_FORMAT_UNDEFINED;
    }
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
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = vk_texture_format(description.format);
    image_info.extent = {description.width, description.height, 1};
    image_info.mipLevels = description.mip_levels;
    image_info.arrayLayers = description.array_layers;
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
    VkImageFormatProperties format_properties{};
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        context->physical_device, image_info.format, image_info.imageType, image_info.tiling,
        image_info.usage, 0, &format_properties);
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
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
    void mark_invalid() noexcept {
        raw_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        raw_.initialized = false;
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
        current_layout = texture_final_layout(description.usage);
        return true;
    }
    if (description.samples != 1U) {
        diagnostic = {"vulkan_multisample_texture_upload_unsupported",
                      "Multisampled Vulkan textures cannot receive transfer uploads"};
        return false;
    }
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

struct VulkanIndexedBatchDraw {
    const PipelineProgram* program = nullptr;
    const VulkanBuffer* vertices = nullptr;
    const VulkanBuffer* indices = nullptr;
    VkDeviceSize vertex_offset = 0U;
    VkDeviceSize index_offset = 0U;
    std::uint32_t index_count = 0U;
    DrawMatrices matrices{};
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
    bool has_material_binding = false;
    VkBuffer material_buffer = VK_NULL_HANDLE;
    VkDeviceSize material_offset = 0U;
    VkDeviceSize material_range = 0U;
    bool has_frame_binding = false;
    VkBuffer frame_buffer = VK_NULL_HANDLE;
    VkDeviceSize frame_offset = 0U;
    VkDeviceSize frame_range = 0U;
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
          includes_normal_detail_binding(std::exchange(other.includes_normal_detail_binding, false)) {}
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
        context.reset();
    }
};

bool prepare_vulkan_sampled_binding(const IndexedStaticMeshDrawRequest& request,
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

bool create_batch_pipeline(const std::shared_ptr<VulkanContext>& context,
                           VkRenderPass render_pass,
                           const TextureDescription& description,
                           const PipelineProgram& program,
                           bool has_depth,
                           VkDescriptorSetLayout sampled_descriptor_layout,
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

    VkPushConstantRange push_constant_range{};
    push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_constant_range.offset = 0U;
    push_constant_range.size = static_cast<std::uint32_t>(sizeof(DrawMatrices));
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1U;
    layout_info.pPushConstantRanges = &push_constant_range;
    if (sampled_descriptor_layout != VK_NULL_HANDLE) {
        layout_info.setLayoutCount = 1U;
        layout_info.pSetLayouts = &sampled_descriptor_layout;
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
    const VkSampleCountFlagBits sample_count = vk_sample_count(description.samples);
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
    color_blend.attachmentCount = 1U;
    color_blend.pAttachments = &blend;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = has_depth && program.depth.test_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = has_depth && program.depth.write_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
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
                                      Diagnostic& diagnostic) {
    descriptors.reset();
    descriptors.context = context;
    // The normal/maps ABIs are intentionally exact: bindings 4/5 and 6/7
    // are only meaningful when the material and frame records at 2/3 are
    // present. Keep descriptor accounting safe even if malformed input
    // reaches this backend before neutral validation.
    const bool includes_detail_stack = includes_detail_binding || includes_normal_detail_binding;
    descriptors.includes_material_buffer = includes_material_buffer || includes_normal_binding || includes_maps_binding ||
                                           includes_detail_stack;
    descriptors.includes_frame_buffer = includes_frame_buffer || includes_normal_binding || includes_maps_binding ||
                                       includes_detail_stack;
    descriptors.includes_normal_binding = includes_normal_binding || includes_maps_binding || includes_detail_stack;
    descriptors.includes_maps_binding = includes_maps_binding;
    descriptors.includes_detail_binding = includes_detail_stack;
    descriptors.includes_normal_detail_binding = includes_normal_detail_binding;
    std::array<VkDescriptorSetLayoutBinding, 12> bindings{};
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
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    const std::uint32_t sampled_descriptor_count = includes_normal_detail_binding ? 5U
        : includes_detail_stack ? 5U : includes_maps_binding ? 3U : includes_normal_binding ? 2U : 1U;
    const std::uint32_t uniform_descriptor_count =
        (includes_frame_buffer || includes_normal_binding || includes_maps_binding || includes_detail_stack)
            ? 2U : includes_material_buffer ? 1U : 0U;
    if (sampled_descriptor_count > properties.limits.maxPerStageDescriptorSampledImages ||
        sampled_descriptor_count > properties.limits.maxDescriptorSetSampledImages ||
        sampled_descriptor_count > properties.limits.maxPerStageDescriptorSamplers ||
        sampled_descriptor_count > properties.limits.maxDescriptorSetSamplers ||
        uniform_descriptor_count > properties.limits.maxPerStageDescriptorUniformBuffers ||
        uniform_descriptor_count > properties.limits.maxDescriptorSetUniformBuffers) {
        diagnostic = {"vulkan_descriptor_layout_limit",
                      "The indexed material descriptor layout exceeds Vulkan device descriptor limits"};
        descriptors.reset();
        return false;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = includes_detail_stack
                                   ? static_cast<std::uint32_t>(bindings.size())
                                   : includes_maps_binding ? 8U
                                   : includes_normal_binding ? 6U
                                   : includes_frame_buffer ? 4U
                                   : includes_material_buffer ? 3U : 2U;
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

bool allocate_sampled_descriptor_sets(const std::shared_ptr<VulkanContext>& context,
                                      VulkanTransientSampledDescriptors& descriptors,
                                      std::span<const VulkanIndexedBatchDraw> draws,
                                      std::vector<VkDescriptorSet>& sets,
                                      Diagnostic& diagnostic) {
    std::size_t descriptor_count = 0U;
    for (const VulkanIndexedBatchDraw& draw : draws) {
        if (draw.has_sampled_binding || draw.has_normal_binding || draw.has_maps_binding ||
            draw.has_detail_binding || draw.has_normal_detail_binding ||
            draw.has_material_binding || draw.has_frame_binding)
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
    const std::size_t sampled_descriptors_per_set = descriptors.includes_normal_detail_binding
                                                        ? 5U
                                                        : descriptors.includes_detail_binding ? 5U
                                                        : descriptors.includes_maps_binding ? 3U
                                                        : descriptors.includes_normal_binding ? 2U : 1U;
    if (descriptor_count > std::numeric_limits<std::size_t>::max() / sampled_descriptors_per_set) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed sampled-image descriptor count overflows host arithmetic"};
        return false;
    }
    const std::size_t sampled_descriptor_count = descriptor_count * sampled_descriptors_per_set;
    if (sampled_descriptor_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        diagnostic = {"vulkan_descriptor_count_limit",
                      "The indexed sampled-image descriptor count exceeds Vulkan's bounded descriptor limit"};
        return false;
    }
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[0].descriptorCount = static_cast<std::uint32_t>(sampled_descriptor_count);
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[1].descriptorCount = static_cast<std::uint32_t>(sampled_descriptor_count);
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    // A binding-3 layout includes binding 2 as well, so frame-only batches
    // still consume two uniform descriptors per allocated set even though the
    // frame-only shader writes only binding 3.
    const std::size_t uniform_bindings_per_set = descriptors.includes_frame_buffer
                                                      ? 2U
                                                      : descriptors.includes_material_buffer ? 1U : 0U;
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
    pool_info.poolSizeCount = (descriptors.includes_material_buffer || descriptors.includes_frame_buffer) ? 3U : 2U;
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
            !draw.has_material_binding && !draw.has_frame_binding)
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
        VkDescriptorBufferInfo material_info{};
        material_info.buffer = draw.material_buffer;
        material_info.offset = draw.material_offset;
        material_info.range = draw.material_range;
        VkDescriptorBufferInfo frame_info{};
        frame_info.buffer = draw.frame_buffer;
        frame_info.offset = draw.frame_offset;
        frame_info.range = draw.frame_range;
        std::array<VkWriteDescriptorSet, 12> writes{};
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
        vkUpdateDescriptorSets(context->device, write_count, writes.data(), 0U, nullptr);
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
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
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
                barrier.subresourceRange.layerCount = description.array_layers;
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
            barrier.subresourceRange.layerCount = description.samples == 1U ? description.array_layers : 1U;
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
                barrier.newLayout = texture_final_layout(description.usage);
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
            current_layout = texture_final_layout(description.usage);
            if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (depth_attachment != nullptr) depth_attachment->mark_invalid();
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(description.usage);
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
    std::span<const VulkanIndexedBatchDraw> draws,
    VulkanDepthAttachment* depth_attachment,
    bool load_color,
    bool clear_depth,
    float depth_clear_value,
    const std::array<float, 4>& clear_color,
    VkImageLayout& current_layout,
    std::vector<std::byte>& output,
    Diagnostic& diagnostic) {
    std::lock_guard command_guard(context->command_mutex);
    if (draws.empty()) {
        diagnostic = {"indexed_static_mesh_batch_empty", "The Vulkan indexed batch contains no draws"};
        return false;
    }
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
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device, &properties);
    if (properties.limits.maxPushConstantsSize < sizeof(DrawMatrices)) {
        diagnostic = {"vulkan_draw_transform_limit_unsupported",
                      "Vulkan device push-constant limit is smaller than the draw-matrices contract"};
        return false;
    }
    const std::uint64_t output_size_u64 = static_cast<std::uint64_t>(description.width) *
                                          static_cast<std::uint64_t>(description.height) * 4U;
    if (output_size_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        diagnostic = {"vulkan_draw_output_size_limit", "Vulkan draw readback size exceeds host addressability"};
        return false;
    }
    BufferDescription readback_description;
    readback_description.size_bytes = static_cast<std::size_t>(output_size_u64);
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

    bool has_sampled_binding = false;
    bool has_material_binding = false;
    bool has_frame_binding = false;
    bool has_normal_binding = false;
    bool has_maps_binding = false;
    bool has_detail_binding = false;
    bool has_normal_detail_binding = false;
    for (const VulkanIndexedBatchDraw& draw : draws) {
        has_sampled_binding = has_sampled_binding || draw.has_sampled_binding;
        has_material_binding = has_material_binding || draw.has_material_binding;
        has_frame_binding = has_frame_binding || draw.has_frame_binding;
        has_normal_binding = has_normal_binding || draw.has_normal_binding;
        has_maps_binding = has_maps_binding || draw.has_maps_binding;
        has_detail_binding = has_detail_binding || draw.has_detail_binding;
        has_normal_detail_binding = has_normal_detail_binding || draw.has_normal_detail_binding;
    }
    VulkanTransientSampledDescriptors descriptors;
    if ((has_sampled_binding || has_normal_binding || has_maps_binding || has_detail_binding ||
         has_normal_detail_binding || has_material_binding || has_frame_binding) &&
        !create_sampled_descriptor_layout(context, descriptors, has_material_binding, has_frame_binding,
                                          has_normal_binding, has_maps_binding, has_detail_binding,
                                          has_normal_detail_binding, diagnostic))
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
        resolve_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
    framebuffer_attachments[0] = image.view;
    if (depth_attachment != nullptr) framebuffer_attachments[1] = depth_attachment->view();
    if (description.samples != 1U) framebuffer_attachments[resolve_attachment_index] = resolve_image.view;
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

    std::vector<VulkanBatchPipeline> pipelines;
    pipelines.reserve(draws.size());
    for (const VulkanIndexedBatchDraw& draw : draws) {
        if (draw.program == nullptr || draw.vertices == nullptr || draw.indices == nullptr) {
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            diagnostic = {"indexed_static_mesh_batch_resource_invalid",
                          "The Vulkan indexed batch contains an invalid draw resource"};
            return false;
        }
        pipelines.emplace_back();
        if (!create_batch_pipeline(context, render_pass, description, *draw.program,
                                   depth_attachment != nullptr, descriptors.layout,
                                   pipelines.back(), diagnostic)) {
            pipelines.clear();
            vkDestroyFramebuffer(context->device, framebuffer, nullptr);
            vkDestroyRenderPass(context->device, render_pass, nullptr);
            return false;
        }
    }

    std::vector<VkDescriptorSet> descriptor_sets;
    if ((has_sampled_binding || has_normal_binding || has_maps_binding || has_detail_binding ||
         has_normal_detail_binding || has_material_binding || has_frame_binding) &&
        !allocate_sampled_descriptor_sets(
                                  context, descriptors, draws, descriptor_sets, diagnostic)) {
        pipelines.clear();
        vkDestroyFramebuffer(context->device, framebuffer, nullptr);
        vkDestroyRenderPass(context->device, render_pass, nullptr);
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
            barrier.subresourceRange.layerCount = description.array_layers;
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
        for (std::size_t index = 0; index < draws.size(); ++index) {
            const VulkanIndexedBatchDraw& draw = draws[index];
            const VulkanBatchPipeline& pipeline = pipelines[index];
            const VkBuffer vertex_buffer = draw.vertices->raw().buffer;
            const VkDeviceSize vertex_offset = draw.vertex_offset;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0U,
                               static_cast<std::uint32_t>(sizeof(DrawMatrices)), &draw.matrices);
            if (!descriptor_sets.empty() && descriptor_sets[index] != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                                        0U, 1U, &descriptor_sets[index], 0U, nullptr);
            }
            vkCmdBindVertexBuffers(command, 0U, 1U, &vertex_buffer, &vertex_offset);
            vkCmdBindIndexBuffer(command, draw.indices->raw().buffer, draw.index_offset, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(command, draw.index_count, 1U, 0U, 0, 0U);
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
        barrier.subresourceRange.layerCount = description.samples == 1U ? description.array_layers : 1U;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1U;
        copy.imageExtent = {description.width, description.height, 1U};
        vkCmdCopyImageToBuffer(command, description.samples == 1U ? image.image : resolve_image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1U, &copy);
        if (description.samples == 1U) {
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = texture_final_layout(description.usage);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
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
            current_layout = texture_final_layout(description.usage);
            if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
        } else {
            current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (depth_attachment != nullptr) depth_attachment->mark_invalid();
            result = drain;
        }
    } else if (completed) {
        current_layout = texture_final_layout(description.usage);
        if (depth_attachment != nullptr) depth_attachment->mark_rendered(clear_depth);
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

class VulkanTexture final : public Texture {
public:
    VulkanTexture(std::shared_ptr<VulkanContext> context,
                  RawVulkanImage raw,
                  TextureDescription description,
                  VkImageLayout layout,
                  bool initialized)
        : context_(std::move(context)), raw_(std::move(raw)), info_({description}),
          layout_(layout), initialized_(initialized) {}

    ~VulkanTexture() override = default;
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const TextureInfo& info() const noexcept override { return info_; }
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }
    VkImage image() const noexcept { return raw_.image; }
    VkImageView view() const noexcept { return raw_.view; }
    VkImageLayout layout() const noexcept { return layout_; }
    bool initialized() const noexcept { return initialized_; }

    bool upload(const TextureUploadPlan& uploads, Diagnostic& diagnostic) {
        const bool uploaded = copy_texture_uploads(context_, raw_, info_.description, uploads,
                                                   layout_, diagnostic);
        initialized_ = initialized_ || (uploaded && !uploads.subresources.empty());
        return uploaded;
    }

    bool clear_readback(const TextureClearReadbackRequest& request,
                        std::vector<std::byte>& output,
                        Diagnostic& diagnostic) {
        const bool cleared = clear_texture_and_readback(context_, raw_, info_.description, request,
                                                        layout_, output, diagnostic);
        initialized_ = initialized_ || cleared;
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
        const DrawMatrices draw_matrices{packet.world_matrix, request.camera_frame->view_projection};
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
                context_, raw_, info_.description, draws, depth_attachment, request.load_color,
                request.clear_depth, request.depth_clear_value, request.clear_color, layout_, output,
                diagnostic);
            initialized_ = initialized_ || drawn;
            return drawn;
        }
        const bool drawn = draw_graphics_and_readback(
            context_, raw_, info_.description, *request.pipeline, geometry, &draw_matrices,
            depth_attachment, request.load_color, request.clear_depth, request.depth_clear_value,
            request.clear_color, layout_, output, diagnostic);
        initialized_ = initialized_ || drawn;
        return drawn;
    }

    bool draw_indexed_static_mesh_batch(const IndexedStaticMeshBatchDescription& description,
                                        VulkanDepthAttachment* depth_attachment,
                                        std::vector<std::byte>& output,
                                        Diagnostic& diagnostic) {
        if (description.load_color && !initialized_) {
            diagnostic = {"indexed_color_load_before_clear",
                          "A color load was requested before the color attachment was initialized"};
            return false;
        }
        std::vector<VulkanIndexedBatchDraw> draws;
        draws.reserve(description.draws.size());
        for (const IndexedStaticMeshDrawRequest& request : description.draws) {
            auto* vertices = dynamic_cast<const VulkanBuffer*>(request.vertex_buffer);
            auto* indices = dynamic_cast<const VulkanBuffer*>(request.index_buffer);
            if (vertices == nullptr || indices == nullptr || request.packet == nullptr || request.pipeline == nullptr ||
                !request.camera_frame.has_value()) {
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
            draw.matrices = {packet.world_matrix, request.camera_frame->view_projection};
            if (!prepare_vulkan_sampled_binding(request, context_, draw, diagnostic)) return false;
            draws.push_back(draw);
        }
        const bool drawn = draw_indexed_batch_and_readback(
            context_, raw_, info_.description, draws, depth_attachment, description.load_color,
            description.clear_depth, description.depth_clear_value, description.clear_color,
            layout_, output, diagnostic);
        initialized_ = initialized_ || drawn;
        return drawn;
    }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanImage raw_;
    TextureInfo info_;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized_ = false;
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
    const std::shared_ptr<VulkanContext>& context() const noexcept { return context_; }
    VkSampler sampler() const noexcept { return raw_.sampler; }

private:
    std::shared_ptr<VulkanContext> context_;
    RawVulkanSampler raw_;
    SamplerInfo info_;
};

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
    if (detail_declaration != normal_detail_declaration ||
        (detail_declaration &&
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
             maps_description.format != TextureFormat::bgra8_unorm)) {
            diagnostic = {"vulkan_indexed_maps_resource_format_unsupported",
                          "Vulkan indexed maps textures require one-layer linear RGBA8 or BGRA8 UNORM image data"};
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
             detail_description.format != TextureFormat::bgra8_srgb)) {
            diagnostic = {"vulkan_indexed_detail_resource_format_unsupported",
                          "Vulkan indexed detail textures require one-layer RGBA8 or BGRA8 image data"};
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
        std::vector<std::byte> output;
        if (!vulkan_texture->draw_indexed_static_mesh_batch(description, depth_attachment, output, diagnostic)) {
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
                                                   selected.properties.limits.maxSamplerAnisotropy,
                                                   selected.properties.limits.maxUniformBufferRange,
                                                   selected.properties.limits.minUniformBufferOffsetAlignment);
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
