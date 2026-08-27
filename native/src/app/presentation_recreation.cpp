#include "apex/app/presentation_recreation.hpp"

namespace apex::app {

bool presentation_recreation_required(const std::string_view diagnostic_code) noexcept {
    return diagnostic_code == "vulkan_presentation_target_out_of_date" ||
           diagnostic_code == "d3d12_presentation_target_out_of_date";
}

bool presentation_surface_is_zero_sized(const std::uint32_t pixel_width,
                                        const std::uint32_t pixel_height) noexcept {
    return pixel_width == 0U || pixel_height == 0U;
}

bool presentation_resize_requires_recreation(const bool pixel_size_changed,
                                             const std::uint32_t pixel_width,
                                             const std::uint32_t pixel_height) noexcept {
    return pixel_size_changed &&
           !presentation_surface_is_zero_sized(pixel_width, pixel_height);
}

bool PresentationRecreationController::begin_out_of_date_recreation(
    const std::string_view diagnostic_code) noexcept {
    if (!presentation_recreation_required(diagnostic_code) ||
        attempts_ >= max_presentation_recreation_attempts)
        return false;
    ++attempts_;
    return true;
}

void PresentationRecreationController::record_successful_frame() noexcept {
    attempts_ = 0U;
}

}  // namespace apex::app
