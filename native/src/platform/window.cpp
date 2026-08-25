#include "apex/platform/window.hpp"

#include <utility>

#if defined(APEX_HAS_SDL3)
#include <SDL3/SDL.h>
#if defined(APEX_HAS_VULKAN)
#include <SDL3/SDL_vulkan.h>
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
#endif

namespace apex::platform {
namespace {

[[nodiscard]] bool valid_description(const WindowDescription& description,
                                     WindowDiagnostic& diagnostic) {
    if (description.title.empty() || description.title.size() > max_window_title_bytes ||
        description.title.find('\0') != std::string::npos) {
        diagnostic = {"window_title_invalid", "window title is empty, contains NUL, or exceeds its byte limit"};
        return false;
    }
    if (description.width == 0U || description.height == 0U ||
        description.width > max_window_dimension || description.height > max_window_dimension ||
        static_cast<std::uint64_t>(description.width) * description.height > max_window_pixels) {
        diagnostic = {"window_dimensions_invalid", "window dimensions are outside their bounded range"};
        return false;
    }
    return true;
}

#if defined(APEX_HAS_SDL3)

std::size_t video_users = 0U;

[[nodiscard]] bool acquire_video() noexcept {
    if (video_users == 0U && !SDL_Init(SDL_INIT_VIDEO)) return false;
    ++video_users;
    return true;
}

void release_video() noexcept {
    if (video_users == 0U) return;
    --video_users;
    if (video_users == 0U) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

[[nodiscard]] std::uint32_t positive_dimension(const Sint32 value) noexcept {
    return value > 0 ? static_cast<std::uint32_t>(value) : 0U;
}

#endif

}  // namespace

#if defined(APEX_HAS_SDL3)
struct Window::Impl {
    SDL_Window* window = nullptr;
    SDL_WindowID id = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t pixel_width = 0U;
    std::uint32_t pixel_height = 0U;
    bool close_requested = false;
    bool video_acquired = false;
    NativeSurfaceSource surface_source;

    ~Impl() {
        if (window != nullptr) SDL_DestroyWindow(window);
        if (video_acquired) release_video();
    }
};

namespace {

#if defined(APEX_HAS_VULKAN)

constexpr std::size_t max_vulkan_extensions = 64U;
constexpr std::size_t max_vulkan_extension_bytes = 256U;

[[nodiscard]] bool create_vulkan_surface(void* context, void* instance,
                                         void* surface_storage,
                                         std::string& diagnostic) {
    auto* implementation = static_cast<Window::Impl*>(context);
    if (implementation == nullptr || implementation->window == nullptr ||
        instance == nullptr || surface_storage == nullptr) {
        diagnostic = "window Vulkan surface callback received invalid storage";
        return false;
    }

    const auto* instance_value = static_cast<const VkInstance*>(instance);
    auto* surface = static_cast<VkSurfaceKHR*>(surface_storage);
    *surface = VkSurfaceKHR{};
    if (!SDL_Vulkan_CreateSurface(implementation->window, *instance_value, nullptr, surface)) {
        diagnostic = SDL_GetError();
        if (diagnostic.empty()) diagnostic = "SDL could not create a Vulkan surface";
        return false;
    }
    return true;
}

void destroy_vulkan_surface(void* context, void* instance,
                            void* surface_storage) noexcept {
    auto* implementation = static_cast<Window::Impl*>(context);
    if (implementation == nullptr || implementation->window == nullptr ||
        instance == nullptr || surface_storage == nullptr) {
        return;
    }
    const auto* instance_value = static_cast<const VkInstance*>(instance);
    auto* surface = static_cast<VkSurfaceKHR*>(surface_storage);
    if (*surface != VkSurfaceKHR{}) {
        SDL_Vulkan_DestroySurface(*instance_value, *surface, nullptr);
        *surface = VkSurfaceKHR{};
    }
}

void initialize_vulkan_source(Window::Impl& implementation, WindowDiagnostic& diagnostic) {
    Uint32 count = 0U;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr || count == 0U || count > max_vulkan_extensions) {
        diagnostic = {"window_vulkan_extensions_unavailable",
                      "SDL did not provide a bounded Vulkan instance extension list"};
        return;
    }
    implementation.surface_source.vulkanInstanceExtensions.reserve(count);
    for (Uint32 index = 0U; index < count; ++index) {
        if (extensions[index] == nullptr) {
            diagnostic = {"window_vulkan_extension_invalid", "SDL returned a null Vulkan extension name"};
            implementation.surface_source.vulkanInstanceExtensions.clear();
            return;
        }
        const std::string value(extensions[index]);
        if (value.empty() || value.size() > max_vulkan_extension_bytes) {
            diagnostic = {"window_vulkan_extension_invalid", "SDL returned an over-sized Vulkan extension name"};
            implementation.surface_source.vulkanInstanceExtensions.clear();
            return;
        }
        implementation.surface_source.vulkanInstanceExtensions.push_back(value);
    }
    implementation.surface_source.context = &implementation;
    implementation.surface_source.createVulkanSurface = &create_vulkan_surface;
    implementation.surface_source.destroyVulkanSurface = &destroy_vulkan_surface;
}

#endif

}  // namespace

#else

struct Window::Impl {};

#endif

Window::Window(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Window::Window(Window&& other) noexcept = default;
Window& Window::operator=(Window&& other) noexcept = default;
Window::~Window() = default;

bool Window::available() noexcept {
#if defined(APEX_HAS_SDL3)
    return true;
#else
    return false;
#endif
}

WindowResult Window::create(const WindowDescription& description) {
    WindowDiagnostic diagnostic;
    if (!valid_description(description, diagnostic))
        return {WindowStatus::invalid_description, std::move(diagnostic), nullptr};

#if !defined(APEX_HAS_SDL3)
    return {WindowStatus::unavailable,
            {"window_backend_unavailable", "SDL3 native window support is not compiled"}, nullptr};
#else
    if (!acquire_video()) {
        return {WindowStatus::unavailable,
                {"window_video_unavailable", SDL_GetError()}, nullptr};
    }

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (description.resizable) flags |= SDL_WINDOW_RESIZABLE;
#if !defined(APEX_HAS_VULKAN)
    if (description.vulkan) {
        release_video();
        return {WindowStatus::unavailable,
                {"window_vulkan_unavailable", "Vulkan headers and loader are not compiled into this build"}, nullptr};
    }
#else
    if (description.vulkan) flags |= SDL_WINDOW_VULKAN;
#endif
    SDL_Window* native_window = SDL_CreateWindow(
        description.title.c_str(), static_cast<int>(description.width),
        static_cast<int>(description.height), flags);
    if (native_window == nullptr) {
        const std::string error = SDL_GetError();
        release_video();
        return {WindowStatus::unavailable,
                {"window_creation_failed", error.empty() ? "SDL could not create a window" : error}, nullptr};
    }

    auto implementation = std::unique_ptr<Impl>(new Impl);
    implementation->window = native_window;
    implementation->id = SDL_GetWindowID(native_window);
    implementation->video_acquired = true;
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(native_window, &width, &height) || width <= 0 || height <= 0) {
        SDL_DestroyWindow(native_window);
        implementation->window = nullptr;
        release_video();
        implementation->video_acquired = false;
        return {WindowStatus::initialization_failed,
                {"window_size_unavailable", "SDL did not return a valid window size"}, nullptr};
    }
    implementation->width = static_cast<std::uint32_t>(width);
    implementation->height = static_cast<std::uint32_t>(height);
    if (!SDL_GetWindowSizeInPixels(native_window, &width, &height) || width <= 0 || height <= 0) {
        width = static_cast<int>(implementation->width);
        height = static_cast<int>(implementation->height);
    }
    implementation->pixel_width = static_cast<std::uint32_t>(width);
    implementation->pixel_height = static_cast<std::uint32_t>(height);

 #if defined(APEX_HAS_VULKAN)
    if (description.vulkan) {
        initialize_vulkan_source(*implementation, diagnostic);
        if (!diagnostic.code.empty()) {
            implementation.reset();
            return {WindowStatus::unavailable, std::move(diagnostic), nullptr};
        }
    }
 #endif

#if defined(_WIN32)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(native_window);
    const HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    implementation->surface_source.win32Window = hwnd;
#endif
    return {WindowStatus::ready, {}, std::unique_ptr<Window>(new Window(std::move(implementation)))};
#endif
}

std::size_t Window::poll_events(std::span<WindowEvent> events) noexcept {
#if !defined(APEX_HAS_SDL3)
    (void)events;
    return 0U;
#else
    if (implementation_ == nullptr || implementation_->window == nullptr || events.empty()) return 0U;
    std::size_t written = 0U;
    std::size_t polled = 0U;
    SDL_Event event{};
    while (written < events.size() && polled < max_window_poll_count && SDL_PollEvent(&event)) {
        ++polled;
        WindowEvent translated;
        bool relevant = false;
        switch (event.type) {
        case SDL_EVENT_QUIT:
            implementation_->close_requested = true;
            translated.type = WindowEventType::close_requested;
            relevant = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID != implementation_->id) break;
            implementation_->close_requested = true;
            translated.type = WindowEventType::close_requested;
            relevant = true;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            if (event.window.windowID != implementation_->id) break;
            implementation_->width = positive_dimension(event.window.data1);
            implementation_->height = positive_dimension(event.window.data2);
            translated.type = WindowEventType::resized;
            translated.width = event.window.data1;
            translated.height = event.window.data2;
            relevant = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (event.window.windowID != implementation_->id) break;
            implementation_->pixel_width = positive_dimension(event.window.data1);
            implementation_->pixel_height = positive_dimension(event.window.data2);
            translated.type = WindowEventType::pixel_size_changed;
            translated.width = event.window.data1;
            translated.height = event.window.data2;
            relevant = true;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event.key.windowID != implementation_->id) break;
            translated.type = event.type == SDL_EVENT_KEY_DOWN ? WindowEventType::key_down
                                                                : WindowEventType::key_up;
            translated.key = static_cast<std::uint32_t>(event.key.key);
            translated.modifiers = static_cast<std::uint32_t>(event.key.mod);
            translated.repeat = event.key.repeat;
            relevant = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.windowID != implementation_->id) break;
            translated.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                  ? WindowEventType::mouse_button_down
                                  : WindowEventType::mouse_button_up;
            translated.button = static_cast<std::uint32_t>(event.button.button);
            translated.x = event.button.x;
            translated.y = event.button.y;
            relevant = true;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.windowID != implementation_->id) break;
            translated.type = WindowEventType::mouse_motion;
            translated.x = event.motion.x;
            translated.y = event.motion.y;
            translated.x_relative = event.motion.xrel;
            translated.y_relative = event.motion.yrel;
            relevant = true;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.windowID != implementation_->id) break;
            translated.type = WindowEventType::mouse_wheel;
            translated.x_relative = event.wheel.x;
            translated.y_relative = event.wheel.y;
            relevant = true;
            break;
        default:
            break;
        }
        if (relevant) events[written++] = translated;
    }
    return written;
#endif
}

std::uint32_t Window::width() const noexcept {
#if defined(APEX_HAS_SDL3)
    return implementation_ == nullptr ? 0U : implementation_->width;
#else
    return 0U;
#endif
}

std::uint32_t Window::height() const noexcept {
#if defined(APEX_HAS_SDL3)
    return implementation_ == nullptr ? 0U : implementation_->height;
#else
    return 0U;
#endif
}

std::uint32_t Window::pixel_width() const noexcept {
#if defined(APEX_HAS_SDL3)
    return implementation_ == nullptr ? 0U : implementation_->pixel_width;
#else
    return 0U;
#endif
}

std::uint32_t Window::pixel_height() const noexcept {
#if defined(APEX_HAS_SDL3)
    return implementation_ == nullptr ? 0U : implementation_->pixel_height;
#else
    return 0U;
#endif
}

bool Window::close_requested() const noexcept {
#if defined(APEX_HAS_SDL3)
    return implementation_ != nullptr && implementation_->close_requested;
#else
    return false;
#endif
}

NativeSurfaceSource Window::native_surface_source() const {
#if defined(APEX_HAS_SDL3)
    if (implementation_ == nullptr) return {};
    NativeSurfaceSource source = implementation_->surface_source;
    source.context = implementation_.get();
    return source;
#else
    return {};
#endif
}

}  // namespace apex::platform
