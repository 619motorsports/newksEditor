#pragma once

#include <array>
#include <cstdint>

namespace apex::render {

inline constexpr std::array<float, 3U> selected_mesh_rgb = {
    1.0F, 0.0F, 1.0F};
inline constexpr float selected_mesh_initial_alpha = 0.5F;
inline constexpr float selected_mesh_fade_per_millisecond = 0.0005F;
inline constexpr std::uint32_t selected_mesh_fade_milliseconds = 2000U;

struct SelectedMeshHighlight {
    bool visible = false;
    std::array<float, 4U> color{};
};

// Reproduce SelectedMesh::render's unsigned GetTickCount delta contract. The
// original draw remains scheduled at exactly 2000 ms with zero alpha and is
// suppressed only when the fade value becomes greater than one.
[[nodiscard]] SelectedMeshHighlight evaluate_selected_mesh_highlight(
    std::uint32_t elapsed_milliseconds) noexcept;

} // namespace apex::render
