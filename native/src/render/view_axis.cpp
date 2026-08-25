#include "apex/render/view_axis.hpp"

namespace apex::render {

std::array<OverlayLineVertex, view_axis_vertex_count>
build_view_axis() noexcept {
    return {{
        {{0.0F, 0.0F, 0.0F}, view_axis_colors[0]},
        {{view_axis_length, 0.0F, 0.0F}, view_axis_colors[0]},
        {{0.0F, 0.0F, 0.0F}, view_axis_colors[1]},
        {{0.0F, view_axis_length, 0.0F}, view_axis_colors[1]},
        {{0.0F, 0.0F, 0.0F}, view_axis_colors[2]},
        {{0.0F, 0.0F, view_axis_length}, view_axis_colors[2]},
    }};
}

} // namespace apex::render
