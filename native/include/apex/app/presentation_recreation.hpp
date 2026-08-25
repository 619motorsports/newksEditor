#pragma once

#include <cstdint>
#include <string_view>

namespace apex::app {

// A transient presentation status may require rebuilding the target. Keep the
// retry policy in the app layer so backends expose diagnostics, not app types.
inline constexpr std::uint32_t max_presentation_recreation_attempts = 8U;

[[nodiscard]] bool presentation_recreation_required(
    std::string_view diagnostic_code) noexcept;

[[nodiscard]] bool presentation_surface_is_zero_sized(
    std::uint32_t pixel_width, std::uint32_t pixel_height) noexcept;

[[nodiscard]] bool presentation_resize_requires_recreation(
    bool pixel_size_changed, std::uint32_t pixel_width,
    std::uint32_t pixel_height) noexcept;

/** Tracks bounded recovery attempts for transient presentation failures. */
class PresentationRecreationController final {
public:
    [[nodiscard]] bool begin_out_of_date_recreation(
        std::string_view diagnostic_code) noexcept;

    // A successfully presented frame starts a fresh recovery budget.
    void record_successful_frame() noexcept;

    [[nodiscard]] std::uint32_t attempts() const noexcept { return attempts_; }

private:
    std::uint32_t attempts_ = 0U;
};

}  // namespace apex::app
