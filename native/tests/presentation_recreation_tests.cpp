#include "apex/app/presentation_recreation.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void test_backend_diagnostics() {
    require(apex::app::presentation_recreation_required(
                "vulkan_presentation_target_out_of_date"),
            "Vulkan out-of-date presentation status must be recoverable");
    require(apex::app::presentation_recreation_required(
                "d3d12_presentation_target_out_of_date"),
            "D3D12 out-of-date presentation status must be recoverable");
    require(!apex::app::presentation_recreation_required(
                "d3d12_device_removed"),
            "device removal must not be treated as a target recreation");
    require(!apex::app::presentation_recreation_required(""),
            "empty presentation status must not trigger recovery");
}

void test_resize_and_zero_size_decisions() {
    require(apex::app::presentation_surface_is_zero_sized(0U, 720U),
            "zero width must pause presentation");
    require(apex::app::presentation_surface_is_zero_sized(1280U, 0U),
            "zero height must pause presentation");
    require(!apex::app::presentation_surface_is_zero_sized(1280U, 720U),
            "positive surface dimensions must remain renderable");

    require(!apex::app::presentation_resize_requires_recreation(
                false, 1280U, 720U),
            "unchanged pixel dimensions must not recreate a target");
    require(!apex::app::presentation_resize_requires_recreation(
                true, 0U, 720U),
            "zero-sized resize must pause instead of creating a target");
    require(apex::app::presentation_resize_requires_recreation(
                true, 1280U, 720U),
            "restored pixel dimensions must recreate a target");
}

void test_bounded_recovery_and_reset() {
    apex::app::PresentationRecreationController controller;
    for (std::uint32_t attempt = 1U;
         attempt <= apex::app::max_presentation_recreation_attempts;
         ++attempt) {
        require(controller.begin_out_of_date_recreation(
                    "vulkan_presentation_target_out_of_date"),
                "out-of-date frame must be retried within the recovery budget");
        require(controller.attempts() == attempt,
                "recovery attempt count must be monotonic and bounded");
    }
    require(!controller.begin_out_of_date_recreation(
                "d3d12_presentation_target_out_of_date"),
            "out-of-date frame must stop after eight retries");
    require(controller.attempts() == apex::app::max_presentation_recreation_attempts,
            "retry budget must remain at its hard limit after exhaustion");

    controller.record_successful_frame();
    require(controller.attempts() == 0U,
            "successful frame must reset the recovery budget");
    require(controller.begin_out_of_date_recreation(
                "d3d12_presentation_target_out_of_date"),
            "D3D12 recovery must work after a successful frame reset");
}

}  // namespace

int main() {
    try {
        test_backend_diagnostics();
        test_resize_and_zero_size_decisions();
        test_bounded_recovery_and_reset();
        std::cout << "presentation recreation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "presentation recreation tests failed: " << error.what() << '\n';
        return 1;
    }
}
