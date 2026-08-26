#pragma once

#include "apex/platform/native_surface.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace apex::platform {

inline constexpr std::size_t max_window_title_bytes = 256U;
inline constexpr std::uint32_t max_window_dimension = 16'384U;
inline constexpr std::uint64_t max_window_pixels =
    static_cast<std::uint64_t>(max_window_dimension) * max_window_dimension;
inline constexpr std::size_t max_window_poll_count = 256U;

struct WindowDescription {
    std::string title = "Apex Editor";
    std::uint32_t width = 1'280U;
    std::uint32_t height = 720U;
    bool resizable = true;
    bool vulkan = false;
};

enum class WindowStatus : std::uint8_t {
    ready,
    invalid_description,
    unavailable,
    initialization_failed,
};

struct WindowDiagnostic {
    std::string code;
    std::string message;
};

enum class WindowEventType : std::uint8_t {
    close_requested,
    resized,
    pixel_size_changed,
    focus_gained,
    focus_lost,
    key_down,
    key_up,
    mouse_button_down,
    mouse_button_up,
    mouse_motion,
    mouse_wheel,
};

// Portable semantic keys used by application input controllers. Platform
// backends translate their native key and scan codes at the window boundary.
enum class WindowKey : std::uint8_t {
    unknown,
    w,
    s,
    a,
    d,
    q,
    e,
    keypad_2,
    keypad_3,
    keypad_4,
    keypad_6,
    keypad_8,
    keypad_9,
    left_control,
    right_control,
};

struct WindowEvent {
    WindowEventType type = WindowEventType::close_requested;
    // Keep the native key code for existing consumers and diagnostics.
    std::uint32_t key = 0U;
    WindowKey semantic_key = WindowKey::unknown;
    std::uint32_t modifiers = 0U;
    std::uint32_t button = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    float x = 0.0F;
    float y = 0.0F;
    float x_relative = 0.0F;
    float y_relative = 0.0F;
    bool repeat = false;
};

struct WindowResult {
    WindowStatus status = WindowStatus::unavailable;
    WindowDiagnostic diagnostic;
    std::unique_ptr<class Window> window;

    [[nodiscard]] bool ok() const noexcept {
        return status == WindowStatus::ready && window != nullptr;
    }
};

class Window {
public:
    struct Impl;

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] static WindowResult create(const WindowDescription& description);

    // The supplied storage bounds the number of events returned and the fixed
    // poll budget bounds the number of SDL events consumed by this call. No
    // event queue is retained by this API between calls.
    [[nodiscard]] std::size_t poll_events(std::span<WindowEvent> events) noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint32_t pixel_width() const noexcept;
    [[nodiscard]] std::uint32_t pixel_height() const noexcept;
    [[nodiscard]] bool close_requested() const noexcept;

    // The returned callbacks borrow this Window. Keep the Window alive while
    // a render device or presentation target uses the source.
    [[nodiscard]] NativeSurfaceSource native_surface_source() const;

private:
    explicit Window(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

}  // namespace apex::platform
