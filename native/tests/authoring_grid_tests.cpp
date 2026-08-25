#include "apex/render/authoring_grid.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void reproduces_native_magenta_grid() {
    const auto vertices = apex::render::build_authoring_grid();
    require(vertices.size() == 44U &&
                apex::render::authoring_grid_line_count == 22U &&
                apex::render::authoring_grid_half_extent == 5.0F &&
                apex::render::authoring_grid_step == 1.0F,
            "native grid has 22 one-metre-spaced line segments");
    require(vertices[0].position ==
                    std::array<float, 3U>{5.0F, 0.0F, -5.0F} &&
                vertices[1].position ==
                    std::array<float, 3U>{5.0F, 0.0F, 5.0F} &&
                vertices[20].position ==
                    std::array<float, 3U>{-5.0F, 0.0F, -5.0F} &&
                vertices[21].position ==
                    std::array<float, 3U>{-5.0F, 0.0F, 5.0F},
            "first native loop emits the recovered X sequence");
    require(vertices[22].position ==
                    std::array<float, 3U>{-5.0F, 0.0F, 5.0F} &&
                vertices[23].position ==
                    std::array<float, 3U>{5.0F, 0.0F, 5.0F} &&
                vertices[42].position ==
                    std::array<float, 3U>{-5.0F, 0.0F, -5.0F} &&
                vertices[43].position ==
                    std::array<float, 3U>{5.0F, 0.0F, -5.0F},
            "second native loop emits the recovered Z sequence");
    for (const auto& vertex : vertices) {
        require(vertex.color == apex::render::authoring_grid_color,
                "every native grid vertex is magenta");
        require(vertex.position[1] == 0.0F,
                "every native grid vertex lies on the XZ plane");
    }
}

} // namespace

int main() {
    try {
        reproduces_native_magenta_grid();
        std::cout << "authoring grid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "authoring grid tests failed: " << error.what() << '\n';
        return 1;
    }
}
