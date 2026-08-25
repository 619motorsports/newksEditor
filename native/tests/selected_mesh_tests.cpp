#include "apex/render/selected_mesh.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void require_alpha(std::uint32_t elapsed, float expected) {
    const auto highlight =
        apex::render::evaluate_selected_mesh_highlight(elapsed);
    require(highlight.visible, "selected mesh remains scheduled during its fade");
    require(highlight.color[0] == 1.0F && highlight.color[1] == 0.0F &&
                highlight.color[2] == 1.0F,
            "selected mesh uses recovered magenta RGB");
    require(std::abs(highlight.color[3] - expected) <= 0.000001F,
            "selected mesh alpha follows the recovered linear curve");
}

void reproduces_native_fade_boundaries() {
    require_alpha(0U, 0.5F);
    require_alpha(1000U, 0.25F);
    require_alpha(1999U, 0.00025F);
    require_alpha(apex::render::selected_mesh_fade_milliseconds, 0.0F);

    require(!apex::render::evaluate_selected_mesh_highlight(2001U).visible,
            "selected mesh stops after the recovered two-second boundary");
    require(!apex::render::evaluate_selected_mesh_highlight(
                 std::numeric_limits<std::uint32_t>::max()).visible,
            "large unsigned tick deltas cannot revive the selected mesh");
}

} // namespace

int main() {
    try {
        reproduces_native_fade_boundaries();
        std::cout << "selected mesh tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "selected mesh tests failed: " << error.what() << '\n';
        return 1;
    }
}
