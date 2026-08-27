#pragma once

#include "apex/platform/window.hpp"

#include <cstdint>
#include <optional>

namespace apex::app {

// The installed editor exposes independent left-side and right-side checkbox
// actions. These keyboard commands are a portable native input policy for the
// same actions; they are not recovered original shortcuts.
enum class WorkspaceAiSplineSideVisibilityCommand : std::uint8_t {
    toggle_left,
    toggle_right,
};

[[nodiscard]] std::optional<WorkspaceAiSplineSideVisibilityCommand>
workspaceAiSplineSideVisibilityCommand(
    const platform::WindowEvent& event) noexcept;

} // namespace apex::app
