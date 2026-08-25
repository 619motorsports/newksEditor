#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <string>

namespace apex::render {

enum class SelectionAxisStatus : std::uint8_t {
    ready,
    invalid_transform,
};

struct SelectionAxisResult {
    SelectionAxisStatus status = SelectionAxisStatus::invalid_transform;
    Diagnostic diagnostic;
    std::array<OverlayLineVertex, 6U> vertices{};

    [[nodiscard]] bool ok() const noexcept {
        return status == SelectionAxisStatus::ready;
    }
};

// Reproduce the selected-node marker emitted by ksNet.ksGraphics.render:
// one-meter normalized world X/Y/-Z line segments with RGB vertex colors.
[[nodiscard]] SelectionAxisResult build_selection_axis(
    const apex::scene::Matrix4& world) noexcept;

} // namespace apex::render
