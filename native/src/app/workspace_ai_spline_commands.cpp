#include "apex/app/workspace_ai_spline_commands.hpp"

namespace apex::app {

std::optional<WorkspaceAiSplineSideVisibilityCommand>
workspaceAiSplineSideVisibilityCommand(
    const platform::WindowEvent& event) noexcept {
    if (event.type != platform::WindowEventType::key_down || event.repeat ||
        !platform::window_modifier_active(
            event.modifiers, platform::WindowModifier::control))
        return std::nullopt;

    switch (event.semantic_key) {
    case platform::WindowKey::l:
        return WorkspaceAiSplineSideVisibilityCommand::toggle_left;
    case platform::WindowKey::r:
        return WorkspaceAiSplineSideVisibilityCommand::toggle_right;
    default:
        return std::nullopt;
    }
}

} // namespace apex::app
