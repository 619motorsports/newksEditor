#include "apex/render/selection_axis.hpp"

#include <cmath>
#include <limits>

namespace apex::render {
namespace {

[[nodiscard]] bool normalized_axis(double x, double y, double z,
                                   std::array<float, 3U>& output) noexcept {
    const double length = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(length) || length <= 1.0e-12) return false;
    x /= length;
    y /= length;
    z /= length;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    output = {static_cast<float>(x), static_cast<float>(y),
              static_cast<float>(z)};
    return true;
}

} // namespace

SelectionAxisResult build_selection_axis(
    const apex::scene::Matrix4& world) noexcept {
    for (float value : world) {
        if (!std::isfinite(value)) {
            return {SelectionAxisStatus::invalid_transform,
                    {"selection_axis_transform_non_finite",
                     "Selected-node world transform must contain only finite values"},
                    {}};
        }
    }

    const std::array<float, 3U> origin = {world[12], world[13], world[14]};
    std::array<std::array<float, 3U>, 3U> directions{};
    if (!normalized_axis(world[0], world[1], world[2], directions[0]) ||
        !normalized_axis(world[4], world[5], world[6], directions[1]) ||
        !normalized_axis(-static_cast<double>(world[8]),
                         -static_cast<double>(world[9]),
                         -static_cast<double>(world[10]), directions[2])) {
        return {SelectionAxisStatus::invalid_transform,
                {"selection_axis_basis_invalid",
                 "Selected-node world transform contains a zero-length or invalid basis"},
                {}};
    }

    constexpr std::array<std::array<float, 3U>, 3U> colors = {{
        {1.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, 1.0F},
    }};
    SelectionAxisResult result;
    result.status = SelectionAxisStatus::ready;
    for (std::size_t axis = 0U; axis < directions.size(); ++axis) {
        result.vertices[axis * 2U] = {origin, colors[axis]};
        std::array<float, 3U> endpoint{};
        for (std::size_t component = 0U; component < endpoint.size(); ++component) {
            const double value = static_cast<double>(origin[component]) +
                                 static_cast<double>(directions[axis][component]);
            if (!std::isfinite(value) ||
                value > std::numeric_limits<float>::max() ||
                value < -std::numeric_limits<float>::max()) {
                return {SelectionAxisStatus::invalid_transform,
                        {"selection_axis_endpoint_invalid",
                         "Selected-node axis endpoint exceeds the finite float range"},
                        {}};
            }
            endpoint[component] = static_cast<float>(value);
        }
        result.vertices[axis * 2U + 1U] = {endpoint, colors[axis]};
    }
    return result;
}

} // namespace apex::render
