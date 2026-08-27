#include "apex/platform/window.hpp"
#include "apex/render/device.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

int unavailable(const std::string_view code, const std::string_view message) {
    std::cout << "SKIP D3D12 presentation: " << code << ": " << message << '\n';
    return 77;
}

}  // namespace

int main() {
    try {
        using apex::platform::Window;
        using apex::platform::WindowDescription;
        using apex::platform::WindowStatus;
        using apex::render::Backend;
        using apex::render::DeviceOptions;
        using apex::render::DeviceStatus;
        using apex::render::PresentationFrameStatus;
        using apex::render::PresentationTargetDescription;
        using apex::render::PresentationTargetStatus;
        using apex::render::TextureDescription;
        using apex::render::TextureMutability;
        using apex::render::TextureReadbackStatus;
        using apex::render::TextureUsage;

        if (!Window::available()) return unavailable("window_backend_unavailable", "SDL3 is unavailable");

        WindowDescription window_description;
        window_description.title = "Apex D3D12 presentation test";
        window_description.width = 320U;
        window_description.height = 240U;
        window_description.resizable = false;
        const auto window_result = Window::create(window_description);
        if (window_result.status == WindowStatus::unavailable)
            return unavailable(window_result.diagnostic.code, window_result.diagnostic.message);
        require(window_result.ok(), "SDL window creation failed");
        const auto source = window_result.window->native_surface_source();
        require(source.win32Window != nullptr, "SDL did not provide a Win32 HWND");

        DeviceOptions options;
        options.headless = false;
        options.enable_validation = true;
        options.allow_software = true;
        options.prefer_software = true;  // Request the WARP adapter.
        options.native_surface = source;

        const auto adapters = apex::render::enumerate_adapters(Backend::D3D12, options);
        if (adapters.status == DeviceStatus::unavailable)
            return unavailable(adapters.diagnostic.code, adapters.diagnostic.message);
        require(adapters.ok() && !adapters.adapters.empty(), "WARP adapter enumeration failed");

        auto device_result = apex::render::create_device(Backend::D3D12, options);
        if (device_result.status == DeviceStatus::unavailable)
            return unavailable(device_result.diagnostic.code, device_result.diagnostic.message);
        require(device_result.ok() && device_result.device != nullptr,
                "WARP D3D12 device creation failed");

        PresentationTargetDescription target_description;
        target_description.width = 64U;
        target_description.height = 64U;
        auto target_result = device_result.device->create_presentation_target(target_description);
        require(target_result.ok(), "D3D12 presentation target creation failed");
        require(target_result.target->info().description.width == target_description.width &&
                    target_result.target->info().description.height == target_description.height,
                "D3D12 target dimensions were not retained");
        require(target_result.target->info().description.image_count >= 2U &&
                    target_result.target->info().description.image_count <= 8U,
                "D3D12 target image count is outside the bounded contract");

        const auto duplicate = device_result.device->create_presentation_target(target_description);
        require(duplicate.status == PresentationTargetStatus::unsupported &&
                    duplicate.diagnostic.code == "d3d12_presentation_target_active",
                "D3D12 rejects a second active presentation target");

        const auto clear = device_result.device->clear_and_present(
            *target_result.target, {0.125F, 0.25F, 0.5F, 1.0F});
        require(clear.status == PresentationFrameStatus::ready,
                "D3D12 finite clear/present failed");

        TextureDescription texture_description;
        texture_description.width = target_result.target->info().description.width;
        texture_description.height = target_result.target->info().description.height;
        texture_description.format = target_result.target->info().description.format;
        texture_description.usage = TextureUsage::color_attachment | TextureUsage::transfer_source;
        texture_description.mutability = TextureMutability::mutable_data;
        auto texture_result = device_result.device->create_texture(texture_description);
        require(texture_result.ok() && texture_result.texture != nullptr,
                "D3D12 presentation source texture creation failed");

        apex::render::TextureClearReadbackRequest readback_request;
        readback_request.clear_color = {0.25F, 0.5F, 0.75F, 1.0F};
        readback_request.output_width = texture_description.width;
        readback_request.output_height = texture_description.height;
        const auto initialized = device_result.device->clear_texture_and_readback(
            *texture_result.texture, readback_request);
        require(initialized.status == TextureReadbackStatus::ready &&
                    initialized.rgba8.size() == static_cast<std::size_t>(
                        texture_description.width) * texture_description.height * 4U,
                "D3D12 presentation source initialization/readback failed");

        const auto texture_frame = device_result.device->present_texture(
            *target_result.target, *texture_result.texture);
        require(texture_frame.status == PresentationFrameStatus::ready,
                "D3D12 initialized same-format texture presentation failed");

        target_result.target.reset();
        target_result = device_result.device->create_presentation_target(target_description);
        require(target_result.ok(), "D3D12 target recreation after destruction failed");
        const auto recreated_clear = device_result.device->clear_and_present(
            *target_result.target, {0.0F, 0.0F, 0.0F, 1.0F});
        require(recreated_clear.status == PresentationFrameStatus::ready,
                "D3D12 clear/present after target recreation failed");

        device_result.device->wait_idle();
        std::cout << "D3D12 presentation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 presentation tests failed: " << error.what() << '\n';
        return 1;
    }
}
