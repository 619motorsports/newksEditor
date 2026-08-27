#pragma once

#include "apex/csp/material_evaluator.hpp"
#include "apex/render/kn5_scene_node_map.hpp"
#include "apex/render/render_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apex::render {

struct CspNodeStateLimits {
    std::size_t max_nodes = 1'000'000U;
    std::size_t max_overrides = 1'000'000U;
    std::size_t max_diagnostics = 100'000U;
    std::uint64_t max_output_bytes = 64ULL * 1024ULL * 1024ULL;
    Kn5SceneNodeMapLimits node_map{};
    apex::csp::MaterialEvaluationLimits evaluator{};
};

struct CspNodeStateDiagnostic {
    apex::csp::CspDiagnosticSeverity severity = apex::csp::CspDiagnosticSeverity::warning;
    std::string code;
    std::string message;
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::csp::CspSourceRef provenance;
};

struct CspNodeStateResult {
    std::vector<NodeRenderStateOverride> overrides;
    std::vector<CspNodeStateDiagnostic> diagnostics;
    bool supported = true;
    bool limit_exceeded = false;

    [[nodiscard]] bool ok() const noexcept { return supported && !limit_exceeded; }
};

// Resolve the five source-evidenced MESH_ADJUSTMENT fields into dense scene
// node IDs. The model and scene must retain the KN5 pre-order identity.
[[nodiscard]] CspNodeStateResult resolve_csp_node_render_states(const apex::csp::CspConfigModel& config,
                                                                const apex::formats::Kn5File& model,
                                                                const apex::scene::SceneSnapshot& scene,
                                                                const CspNodeStateLimits& limits = {});

}  // namespace apex::render
