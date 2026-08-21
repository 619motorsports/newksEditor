#include "backend_internal.hpp"

#include <algorithm>
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

class VulkanDevice final : public Device {
public:
    VulkanDevice(Instance instance, VkDevice device, VkQueue queue, DeviceInfo info)
        : instance_(std::move(instance)), device_(device), queue_(queue), info_(std::move(info)) {}

    ~VulkanDevice() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            vkDestroyDevice(device_, nullptr);
        }
    }

    const DeviceInfo& info() const noexcept override { return info_; }

    void wait_idle() noexcept override {
        if (device_ != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(device_);
        }
    }

private:
    Instance instance_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    DeviceInfo info_;
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
    DeviceResult result;
    result.status = DeviceStatus::ready;
    result.device = std::make_unique<VulkanDevice>(std::move(instance), device, queue, info_from(selected.properties));
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
