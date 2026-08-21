#include "apex/render/csp_node_state.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string_view>

namespace apex::render {
namespace {

bool charge(std::uint64_t amount, std::uint64_t& used, std::uint64_t limit) noexcept {
    if (amount > limit - std::min(used, limit)) return false;
    used += amount;
    return true;
}

void fail(CspNodeStateResult& result, std::string code, std::string message, bool limit_exceeded = false) {
    result.overrides.clear();
    result.supported = false;
    result.limit_exceeded = limit_exceeded;
    result.diagnostics.clear();
    result.diagnostics.push_back({apex::csp::CspDiagnosticSeverity::error,
                                  std::move(code),
                                  std::move(message),
                                  apex::scene::invalid_node_id,
                                  {}});
}

bool has_field(const apex::csp::MaterialEvaluationResult& evaluation, std::string_view field) {
    return std::any_of(evaluation.state_provenance.begin(),
                       evaluation.state_provenance.end(),
                       [field](const apex::csp::MaterialStateProvenance& item) { return item.field == field; });
}

}  // namespace

CspNodeStateResult resolve_csp_node_render_states(const apex::csp::CspConfigModel& config,
                                                  const apex::formats::Kn5File& model,
                                                  const apex::scene::SceneSnapshot& scene,
                                                  const CspNodeStateLimits& limits) try {
    CspNodeStateResult result;
    if (limits.max_nodes == 0U || limits.max_overrides == 0U || limits.max_diagnostics == 0U ||
        limits.max_output_bytes == 0U) {
        fail(result, "CSP_NODE_STATE_LIMIT", "CSP node-state limits must be nonzero", true);
        return result;
    }
    if (scene.nodes.size() > limits.max_nodes || model.materials.size() != scene.materials.size()) {
        fail(result, "CSP_NODE_STATE_IDENTITY", "CSP node-state model and scene tables do not match");
        return result;
    }

    Kn5SceneNodeMapLimits map_limits = limits.node_map;
    map_limits.max_nodes = std::min(map_limits.max_nodes, limits.max_nodes);
    const Kn5SceneNodeMapResult mapped = map_kn5_scene_nodes(model.root, scene, map_limits);
    if (!mapped.ok()) {
        fail(result,
             mapped.diagnostic.code.empty() ? "CSP_NODE_STATE_IDENTITY" : mapped.diagnostic.code,
             mapped.diagnostic.message.empty() ? "CSP node-state model and scene identities do not match"
                                               : mapped.diagnostic.message,
             mapped.diagnostic.limit_exceeded);
        return result;
    }

    std::size_t geometry_count = 0U;
    for (const apex::formats::Kn5Node* node : mapped.source_nodes) {
        if (node != nullptr && (node->type == 2U || node->type == 3U)) ++geometry_count;
    }
    const std::size_t reserved_overrides = std::min(geometry_count, limits.max_overrides);
    if (reserved_overrides > std::numeric_limits<std::uint64_t>::max() / sizeof(NodeRenderStateOverride)) {
        fail(result, "CSP_NODE_STATE_OUTPUT_LIMIT", "CSP node-state output size overflows", true);
        return result;
    }
    std::uint64_t output_bytes = 0U;
    if (!charge(static_cast<std::uint64_t>(reserved_overrides) * sizeof(NodeRenderStateOverride),
                output_bytes,
                limits.max_output_bytes)) {
        fail(result, "CSP_NODE_STATE_OUTPUT_LIMIT", "CSP node-state output exceeds its byte limit", true);
        return result;
    }
    result.overrides.reserve(reserved_overrides);

    for (std::size_t index = 0U; index < mapped.source_nodes.size(); ++index) {
        const apex::formats::Kn5Node& source = *mapped.source_nodes[index];
        if (source.type != 2U && source.type != 3U) continue;
        if (source.materialId >= model.materials.size() || scene.nodes[index].material != source.materialId) {
            fail(result, "CSP_NODE_STATE_MATERIAL", "A CSP target references an unknown or mismatched material");
            return result;
        }
        const apex::csp::MaterialEvaluationResult evaluation =
            apex::csp::evaluate_csp_material(config, source, model.materials[source.materialId], limits.evaluator);
        if (evaluation.limit_exceeded) {
            fail(result,
                 "CSP_NODE_STATE_EVALUATION_LIMIT",
                 "CSP mesh-state evaluation exceeded its configured limit",
                 true);
            return result;
        }

        NodeRenderStateOverride override_value;
        override_value.node = static_cast<apex::scene::NodeId>(index);
        if (has_field(evaluation, "is_transparent")) override_value.is_transparent = evaluation.state.is_transparent;
        if (has_field(evaluation, "layer")) override_value.layer = evaluation.state.layer;
        if (has_field(evaluation, "lod_in")) override_value.lod_in = evaluation.state.lod_in;
        if (has_field(evaluation, "lod_out")) override_value.lod_out = evaluation.state.lod_out;
        if (has_field(evaluation, "cast_shadows")) override_value.cast_shadows = evaluation.state.cast_shadows;
        if (override_value.is_transparent.has_value() || override_value.layer.has_value() ||
            override_value.lod_in.has_value() || override_value.lod_out.has_value() ||
            override_value.cast_shadows.has_value()) {
            if (result.overrides.size() >= limits.max_overrides) {
                fail(
                    result, "CSP_NODE_STATE_OVERRIDE_LIMIT", "CSP node-state overrides exceed their count limit", true);
                return result;
            }
            result.overrides.push_back(std::move(override_value));
        }

        for (const apex::csp::MaterialEvaluationDiagnostic& diagnostic : evaluation.diagnostics) {
            if (diagnostic.code != "INVALID_MESH_OVERRIDE") continue;
            if (result.diagnostics.size() >= limits.max_diagnostics) {
                fail(result,
                     "CSP_NODE_STATE_DIAGNOSTIC_LIMIT",
                     "CSP node-state diagnostics exceed their count limit",
                     true);
                return result;
            }
            const std::uint64_t bytes = static_cast<std::uint64_t>(diagnostic.code.size()) +
                                        static_cast<std::uint64_t>(diagnostic.message.size()) +
                                        static_cast<std::uint64_t>(diagnostic.provenance.source.size()) +
                                        sizeof(CspNodeStateDiagnostic);
            if (!charge(bytes, output_bytes, limits.max_output_bytes)) {
                fail(result, "CSP_NODE_STATE_OUTPUT_LIMIT", "CSP node-state diagnostics exceed their byte limit", true);
                return result;
            }
            result.diagnostics.push_back({diagnostic.severity,
                                          diagnostic.code,
                                          diagnostic.message,
                                          static_cast<apex::scene::NodeId>(index),
                                          diagnostic.provenance});
        }
    }
    return result;
} catch (const std::bad_alloc&) {
    CspNodeStateResult result;
    fail(result, "CSP_NODE_STATE_ALLOCATION", "CSP node-state resolution could not allocate bounded output");
    return result;
}

}  // namespace apex::render
