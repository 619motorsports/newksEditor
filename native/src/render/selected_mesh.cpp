#include "apex/render/selected_mesh.hpp"

namespace apex::render {

SelectedMeshHighlight evaluate_selected_mesh_highlight(
    std::uint32_t elapsed_milliseconds) noexcept {
    const float fade = static_cast<float>(elapsed_milliseconds) *
                       selected_mesh_fade_per_millisecond;
    if (fade > 1.0F) return {};
    return {true,
            {selected_mesh_rgb[0], selected_mesh_rgb[1],
             selected_mesh_rgb[2],
             (1.0F - fade) * selected_mesh_initial_alpha}};
}

} // namespace apex::render
