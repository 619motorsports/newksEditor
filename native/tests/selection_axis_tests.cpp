#include "apex/render/selection_axis.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool near(float left, float right) {
    return std::abs(left - right) < 1.0e-6F;
}

void reproduces_normalized_rgb_axes() {
    auto world = apex::scene::identity_matrix;
    world[0] = 2.0F;
    world[5] = 3.0F;
    world[10] = 4.0F;
    world[12] = 10.0F;
    world[13] = 20.0F;
    world[14] = 30.0F;
    const auto result = apex::render::build_selection_axis(world);
    require(result.ok(), "finite nonuniform selected transform accepted");
    const auto& v = result.vertices;
    require(v[0].position == std::array<float, 3U>{10.0F, 20.0F, 30.0F} &&
                v[0].color == std::array<float, 3U>{1.0F, 0.0F, 0.0F} &&
                v[1].position == std::array<float, 3U>{11.0F, 20.0F, 30.0F},
            "red X segment is one world meter");
    require(v[2].color == std::array<float, 3U>{0.0F, 1.0F, 0.0F} &&
                v[3].position == std::array<float, 3U>{10.0F, 21.0F, 30.0F},
            "green Y segment is one world meter");
    require(v[4].color == std::array<float, 3U>{0.0F, 0.0F, 1.0F} &&
                v[5].position == std::array<float, 3U>{10.0F, 20.0F, 29.0F},
            "blue segment follows normalized negative Z");
}

void preserves_rotated_basis_direction() {
    auto world = apex::scene::identity_matrix;
    world[0] = 0.0F;
    world[1] = 2.0F;
    world[4] = -3.0F;
    world[5] = 0.0F;
    const auto result = apex::render::build_selection_axis(world);
    require(result.ok() && near(result.vertices[1].position[0], 0.0F) &&
                near(result.vertices[1].position[1], 1.0F) &&
                near(result.vertices[3].position[0], -1.0F) &&
                near(result.vertices[3].position[1], 0.0F),
            "rotated bases retain direction after normalization");
}

void rejects_malformed_transforms() {
    auto non_finite = apex::scene::identity_matrix;
    non_finite[7] = std::numeric_limits<float>::quiet_NaN();
    auto result = apex::render::build_selection_axis(non_finite);
    require(!result.ok() &&
                result.diagnostic.code == "selection_axis_transform_non_finite",
            "non-finite transform rejected");

    auto zero_basis = apex::scene::identity_matrix;
    zero_basis[0] = zero_basis[1] = zero_basis[2] = 0.0F;
    result = apex::render::build_selection_axis(zero_basis);
    require(!result.ok() &&
                result.diagnostic.code == "selection_axis_basis_invalid",
            "zero-length axis basis rejected");

}

} // namespace

int main() {
    try {
        reproduces_normalized_rgb_axes();
        preserves_rotated_basis_direction();
        rejects_malformed_transforms();
        std::cout << "selection axis tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "selection axis tests failed: " << error.what() << '\n';
        return 1;
    }
}
