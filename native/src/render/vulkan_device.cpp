#include "backend_internal.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
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
    DeviceInfo info;

    VulkanContext(Instance instance_value,
                  VkPhysicalDevice physical,
                  VkDevice device_value,
                  VkQueue queue_value,
                  std::uint32_t family,
                  DeviceInfo info_value)
        : instance(std::move(instance_value)), physical_device(physical), device(device_value),
          queue(queue_value), queue_family(family), info(std::move(info_value)) {}

    ~VulkanContext() {
        if (device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(device);
            if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
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
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context->device, &allocation, &command);
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
            if (result == VK_SUCCESS) result = vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(context->device, fence, nullptr);
        }
    }
    vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    if (result != VK_SUCCESS) {
        diagnostic = vk_error("Vulkan buffer copy", result);
        diagnostic.code = "buffer_upload_failed";
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

    void wait_idle() noexcept override {
        if (context_ && context_->device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(context_->device);
        }
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
    VkPhysicalDeviceFeatures features{};
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
                                                   selected.graphics_queue_family, info_from(selected.properties));
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
