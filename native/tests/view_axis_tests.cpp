#include "apex/render/view_axis.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void reproduces_native_world_origin_axis() {
    const auto vertices = apex::render::build_view_axis();
    require(vertices.size() == 6U &&
                apex::render::view_axis_line_count == 3U &&
                apex::render::view_axis_vertex_count == 6U &&
                apex::render::view_axis_length == 1.0F,
            "native view axis has three one-meter line segments");
    require(vertices[0].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                vertices[1].position == std::array<float, 3U>{1.0F, 0.0F, 0.0F} &&
                vertices[2].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                vertices[3].position == std::array<float, 3U>{0.0F, 1.0F, 0.0F} &&
                vertices[4].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                vertices[5].position == std::array<float, 3U>{0.0F, 0.0F, 1.0F},
            "native view axis emits +X, +Y, and +Z from the origin");
    require(apex::render::view_axis_colors ==
                std::array<std::array<float, 3U>, 3U>{{
                    {3.0F, 0.0F, 0.0F},
                    {0.0F, 3.0F, 0.0F},
                    {0.0F, 0.0F, 3.0F},
                }},
            "native view axis keeps the recovered RGB intensity of three");
    for (std::size_t axis = 0U; axis < apex::render::view_axis_line_count;
         ++axis) {
        require(vertices[axis * 2U].color == apex::render::view_axis_colors[axis] &&
                    vertices[axis * 2U + 1U].color ==
                        apex::render::view_axis_colors[axis],
                "each view-axis segment keeps its native RGB color");
    }
}

} // namespace

int main() {
    try {
        reproduces_native_world_origin_axis();
        std::cout << "view axis tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "view axis tests failed: " << error.what() << '\n';
        return 1;
    }
}
