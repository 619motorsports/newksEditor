#pragma once

#include "apex/render/device.hpp"

#include <optional>
#include <string>
#include <vector>

namespace apex::app {

struct WorkspaceShadowPipelineResult {
    render::Diagnostic diagnostic;
    std::optional<render::PipelineProgram> pipeline;

    [[nodiscard]] bool ok() const noexcept {
        return pipeline.has_value();
    }
};

// Build one portable depth-only caster role from explicit SPIR-V, DXBC, or DXIL.
// The function supplies no stock shader bytes and validates the complete role
// contract before returning the program.
[[nodiscard]] WorkspaceShadowPipelineResult buildWorkspaceShadowPipeline(
    std::string name, std::vector<render::PipelineShaderModule> shaders,
    render::DepthOnlyIndexedPipelineRole role);

} // namespace apex::app
