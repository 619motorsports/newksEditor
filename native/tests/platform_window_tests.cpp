#include "apex/platform/window.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void test_invalid_descriptions() {
    using apex::platform::Window;
    using apex::platform::WindowDescription;
    using apex::platform::WindowStatus;

    WindowDescription empty_title;
    empty_title.title.clear();
    require(Window::create(empty_title).status == WindowStatus::invalid_description,
            "empty window title must be rejected");

    WindowDescription embedded_nul;
    embedded_nul.title = std::string("Apex\0Editor", 11);
    require(Window::create(embedded_nul).status == WindowStatus::invalid_description,
            "NUL-containing window title must be rejected");

    WindowDescription long_title;
    long_title.title.assign(apex::platform::max_window_title_bytes + 1U, 'x');
    require(Window::create(long_title).status == WindowStatus::invalid_description,
            "over-sized window title must be rejected");

    WindowDescription zero_width;
    zero_width.width = 0U;
    require(Window::create(zero_width).status == WindowStatus::invalid_description,
            "zero window width must be rejected");

    WindowDescription over_sized;
    over_sized.height = apex::platform::max_window_dimension + 1U;
    require(Window::create(over_sized).status == WindowStatus::invalid_description,
            "over-sized window dimension must be rejected");
}

void test_portable_mouse_semantics() {
    using apex::platform::WindowModifier;
    using apex::platform::WindowMouseButton;
    require(apex::platform::window_mouse_button_code(
                WindowMouseButton::left) == 1U &&
                apex::platform::window_mouse_button_code(
                    WindowMouseButton::middle) == 2U &&
                apex::platform::window_mouse_button_code(
                    WindowMouseButton::right) == 3U,
            "portable mouse buttons must match SDL button codes");
    const auto modifiers =
        static_cast<std::uint32_t>(WindowModifier::control) |
        static_cast<std::uint32_t>(WindowModifier::shift);
    require(apex::platform::window_modifier_active(
                modifiers, WindowModifier::control) &&
                apex::platform::window_modifier_active(
                    modifiers, WindowModifier::shift) &&
                !apex::platform::window_modifier_active(
                    0U, WindowModifier::control),
            "portable modifier test must preserve semantic bits");
}

int test_display_smoke() {
    using apex::platform::Window;
    using apex::platform::WindowDescription;
    using apex::platform::WindowEvent;
    using apex::platform::WindowStatus;

    if (!Window::available()) return 77;
    WindowDescription description;
    description.title = "Apex native window test";
    description.width = 320U;
    description.height = 240U;
    const auto result = Window::create(description);
    if (result.status == WindowStatus::unavailable) return 77;
    require(result.ok(), "SDL window smoke creation failed");
    require(result.window->width() > 0U && result.window->height() > 0U,
            "SDL window did not report a logical size");
    require(result.window->pixel_width() > 0U && result.window->pixel_height() > 0U,
            "SDL window did not report a pixel size");

    std::array<WindowEvent, 1> bounded_events{};
    require(result.window->poll_events(bounded_events) <= bounded_events.size(),
            "window event polling exceeded caller-provided bound");
    const auto source = result.window->native_surface_source();
    require(!source.supportsVulkan(), "non-Vulkan window unexpectedly exposed Vulkan callbacks");
    return 0;
}

int test_vulkan_source_smoke() {
    using apex::platform::Window;
    using apex::platform::WindowDescription;
    using apex::platform::WindowStatus;

    if (!Window::available()) return 77;
    WindowDescription description;
    description.title = "Apex native Vulkan window test";
    description.width = 320U;
    description.height = 240U;
    description.vulkan = true;
    const auto result = Window::create(description);
    // Vulkan support is optional even when the SDL window backend is present.
    // The plain display smoke above remains the availability gate for this test.
    if (result.status == WindowStatus::unavailable) return 0;
    require(result.ok(), "SDL Vulkan window smoke creation failed");
    const auto source = result.window->native_surface_source();
    require(source.supportsVulkan(), "Vulkan window did not expose source callbacks");
    require(source.vulkanInstanceExtensions.size() <= 64U,
            "Vulkan extension list exceeded its bound");
    return 0;
}

}  // namespace

int main() {
    try {
        test_invalid_descriptions();
        test_portable_mouse_semantics();
        const int display_status = test_display_smoke();
        if (display_status != 0) return display_status;
        const int vulkan_status = test_vulkan_source_smoke();
        if (vulkan_status != 0) return vulkan_status;
        std::cout << "platform window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "platform window tests failed: " << error.what() << '\n';
        return 1;
    }
}
