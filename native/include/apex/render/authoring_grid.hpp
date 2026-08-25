#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstddef>

namespace apex::render {

inline constexpr float authoring_grid_step = 1.0F;
inline constexpr int authoring_grid_half_extent_steps = 5;
inline constexpr float authoring_grid_half_extent =
    static_cast<float>(authoring_grid_half_extent_steps) * authoring_grid_step;
inline constexpr std::array<float, 3U> authoring_grid_color = {
    1.0F, 0.0F, 1.0F};
inline constexpr std::size_t authoring_grid_line_count =
    static_cast<std::size_t>(authoring_grid_half_extent_steps * 2 + 1) * 2U;
inline constexpr std::size_t authoring_grid_vertex_count =
    authoring_grid_line_count * 2U;

// Reproduce the fixed grid emitted by ksNet.ksGraphics.render: 11 lines in
// each direction on the XZ plane, from -5 m through +5 m, in magenta.
[[nodiscard]] std::array<OverlayLineVertex, authoring_grid_vertex_count>
build_authoring_grid() noexcept;

} // namespace apex::render
