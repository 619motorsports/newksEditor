#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstddef>

namespace apex::render {

inline constexpr float view_axis_length = 1.0F;
inline constexpr std::array<std::array<float, 3U>, 3U> view_axis_colors = {{
    {3.0F, 0.0F, 0.0F},
    {0.0F, 3.0F, 0.0F},
    {0.0F, 0.0F, 3.0F},
}};
inline constexpr std::size_t view_axis_line_count = 3U;
inline constexpr std::size_t view_axis_vertex_count = view_axis_line_count * 2U;

// Reproduce the recovered world-origin marker: one-meter +X, +Y, and +Z
// segments with the native immediate colors scaled to three.
[[nodiscard]] std::array<OverlayLineVertex, view_axis_vertex_count>
build_view_axis() noexcept;

} // namespace apex::render
