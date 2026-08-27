#pragma once

#include <string>
#include <vector>

namespace apex::platform {

/**
 * Type-erased native presentation source.
 *
 * The platform layer owns context and the native window. They must outlive
 * every render device and presentation target created from this source.
 * Vulkan types stay private: instance points to a VkInstance value, and
 * surfaceStorage points to writable VkSurfaceKHR storage.
 */
struct NativeSurfaceSource {
    using CreateVulkanSurface = bool (*)(
        void* context, void* instance, void* surfaceStorage,
        std::string& diagnostic);
    using DestroyVulkanSurface = void (*)(
        void* context, void* instance, void* surfaceStorage) noexcept;

    std::vector<std::string> vulkanInstanceExtensions;
    void* context = nullptr;
    CreateVulkanSurface createVulkanSurface = nullptr;
    DestroyVulkanSurface destroyVulkanSurface = nullptr;
    void* win32Window = nullptr;

    [[nodiscard]] bool supportsVulkan() const noexcept {
        return createVulkanSurface != nullptr && destroyVulkanSurface != nullptr &&
               !vulkanInstanceExtensions.empty();
    }
};

}  // namespace apex::platform
