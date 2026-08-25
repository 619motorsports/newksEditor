#include "apex/render/authoring_grid.hpp"

namespace apex::render {

std::array<OverlayLineVertex, authoring_grid_vertex_count>
build_authoring_grid() noexcept {
    std::array<OverlayLineVertex, authoring_grid_vertex_count> vertices{};
    std::size_t output = 0U;
    for (int coordinate = -authoring_grid_half_extent_steps;
         coordinate <= authoring_grid_half_extent_steps; ++coordinate) {
        const float x = -static_cast<float>(coordinate) * authoring_grid_step;
        vertices[output++] = {
            {x, 0.0F, -authoring_grid_half_extent}, authoring_grid_color};
        vertices[output++] = {
            {x, 0.0F, authoring_grid_half_extent}, authoring_grid_color};
    }
    for (int coordinate = -authoring_grid_half_extent_steps;
         coordinate <= authoring_grid_half_extent_steps; ++coordinate) {
        const float z = -static_cast<float>(coordinate) * authoring_grid_step;
        vertices[output++] = {
            {-authoring_grid_half_extent, 0.0F, z}, authoring_grid_color};
        vertices[output++] = {
            {authoring_grid_half_extent, 0.0F, z}, authoring_grid_color};
    }
    return vertices;
}

} // namespace apex::render
