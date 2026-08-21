#include "apex/render/frame_plan.hpp"

#include <algorithm>
#include <utility>

namespace apex::render {
namespace {

[[nodiscard]] FrameGraphError graph_error(FrameGraphErrorCode code,
                                          const std::string& message) {
    return FrameGraphError(code, message);
}

[[nodiscard]] std::size_t resource_count(const FramePassGraph& graph) {
    std::vector<FrameResourceKind> resources;
    for (const FrameResourceDeclaration& declaration : graph.resources)
        resources.push_back(declaration.kind);
    for (const FramePass& pass : graph.passes) {
        resources.insert(resources.end(), pass.reads.begin(), pass.reads.end());
        resources.insert(resources.end(), pass.writes.begin(), pass.writes.end());
    }
    std::sort(resources.begin(), resources.end(), [](auto first, auto second) {
        return static_cast<unsigned>(first) < static_cast<unsigned>(second);
    });
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    return resources.size();
}

void validate_graph(const FramePassGraph& graph, const FramePlanLimits& limits) {
    if (graph.passes.size() > limits.max_passes) {
        throw graph_error(FrameGraphErrorCode::too_many_passes,
                          "frame pass count exceeds the configured limit");
    }
    if (resource_count(graph) > limits.max_resources) {
        throw graph_error(FrameGraphErrorCode::too_many_resources,
                          "frame resource count exceeds the configured limit");
    }

    std::vector<FrameResourceKind> declared_resources;
    declared_resources.reserve(graph.resources.size());
    for (const FrameResourceDeclaration& declaration : graph.resources) {
        if (std::find(declared_resources.begin(), declared_resources.end(), declaration.kind) !=
            declared_resources.end())
            throw graph_error(FrameGraphErrorCode::duplicate_resource,
                              "frame graph contains a duplicate resource declaration");
        declared_resources.push_back(declaration.kind);
    }

    std::vector<std::string> names;
    names.reserve(graph.passes.size());
    std::size_t dependency_count = 0;
    for (const FramePass& pass : graph.passes) {
        if (pass.name.empty() || pass.name.size() > limits.max_name_length) {
            throw graph_error(FrameGraphErrorCode::invalid_name,
                              "frame pass name is empty or exceeds the configured limit");
        }
        if (std::find(names.begin(), names.end(), pass.name) != names.end()) {
            throw graph_error(FrameGraphErrorCode::duplicate_pass,
                              "frame graph contains a duplicate pass name: " + pass.name);
        }
        names.push_back(pass.name);
        if (dependency_count > limits.max_dependencies ||
            pass.dependencies.size() > limits.max_dependencies - dependency_count) {
            throw graph_error(FrameGraphErrorCode::too_many_dependencies,
                              "frame pass dependency count exceeds the configured limit");
        }
        dependency_count += pass.dependencies.size();
        for (const std::string& dependency : pass.dependencies) {
            if (dependency.empty() || dependency.size() > limits.max_name_length) {
                throw graph_error(FrameGraphErrorCode::invalid_name,
                                  "frame dependency name is empty or exceeds the configured limit");
            }
            if (dependency == pass.name) {
                throw graph_error(FrameGraphErrorCode::cycle,
                                  "frame pass depends on itself: " + pass.name);
            }
            if (std::find(names.begin(), names.end(), dependency) == names.end() &&
                std::find_if(graph.passes.begin(), graph.passes.end(),
                             [&](const FramePass& candidate) { return candidate.name == dependency; }) ==
                    graph.passes.end()) {
                throw graph_error(FrameGraphErrorCode::missing_dependency,
                                  "frame pass depends on an unknown pass: " + dependency);
            }
        }
    }
}

void validate_resources(const FramePassGraph& graph) {
    std::vector<FrameResourceKind> produced;
    produced.reserve(graph.passes.size());
    const auto external = [&](FrameResourceKind kind) {
        return std::any_of(graph.resources.begin(), graph.resources.end(),
                           [&](const FrameResourceDeclaration& declaration) {
                               return declaration.kind == kind && declaration.external;
                           });
    };
    for (const FramePass& pass : graph.passes) {
        for (const FrameResourceKind resource : pass.reads) {
            if (std::find(produced.begin(), produced.end(), resource) == produced.end() &&
                !external(resource))
                throw graph_error(FrameGraphErrorCode::missing_resource,
                                  "frame pass reads a resource without an earlier producer or external declaration: " +
                                      std::string(frame_resource_kind_name(resource)));
        }
        for (const FrameResourceKind resource : pass.writes) {
            if (std::find(produced.begin(), produced.end(), resource) == produced.end())
                produced.push_back(resource);
        }
    }
}

}  // namespace

FrameGraphError::FrameGraphError(FrameGraphErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

const char* frame_pass_kind_name(FramePassKind kind) noexcept {
    switch (kind) {
        case FramePassKind::local_shadow: return "local_shadow";
        case FramePassKind::reflection_capture: return "reflection_capture";
        case FramePassKind::directional_shadow: return "directional_shadow";
        case FramePassKind::sky: return "sky";
        case FramePassKind::opaque: return "opaque";
        case FramePassKind::opaque_scene_resolve: return "opaque_scene_resolve";
        case FramePassKind::transparent: return "transparent";
        case FramePassKind::rain_overlay: return "rain_overlay";
        case FramePassKind::grid_overlay: return "grid_overlay";
        case FramePassKind::selection_overlay: return "selection_overlay";
        case FramePassKind::collider_overlay: return "collider_overlay";
        case FramePassKind::hdr_resolve: return "hdr_resolve";
        case FramePassKind::bloom: return "bloom";
        case FramePassKind::postprocess: return "postprocess";
    }
    return "unknown";
}

const char* frame_resource_kind_name(FrameResourceKind kind) noexcept {
    switch (kind) {
        case FrameResourceKind::local_shadow_atlas: return "local_shadow_atlas";
        case FrameResourceKind::reflection_cube: return "reflection_cube";
        case FrameResourceKind::reflection_probe_shadow_cascades: return "reflection_probe_shadow_cascades";
        case FrameResourceKind::directional_shadow_cascades: return "directional_shadow_cascades";
        case FrameResourceKind::depth_buffer: return "depth_buffer";
        case FrameResourceKind::hdr_ms_color: return "hdr_ms_color";
        case FrameResourceKind::opaque_scene_color: return "opaque_scene_color";
        case FrameResourceKind::hdr_resolved_color: return "hdr_resolved_color";
        case FrameResourceKind::bloom_chain: return "bloom_chain";
        case FrameResourceKind::backbuffer: return "backbuffer";
    }
    return "unknown";
}

FramePassGraph order_frame_pass_graph(const FramePassGraph& graph,
                                      const FramePlanLimits& limits) {
    validate_graph(graph, limits);
    const std::size_t count = graph.passes.size();
    std::vector<std::size_t> indegree(count, 0);
    std::vector<std::vector<std::size_t>> outgoing(count);
    for (std::size_t index = 0; index < count; ++index) {
        for (const std::string& dependency : graph.passes[index].dependencies) {
            const auto it = std::find_if(graph.passes.begin(), graph.passes.end(),
                                         [&](const FramePass& pass) { return pass.name == dependency; });
            const std::size_t dependency_index = static_cast<std::size_t>(it - graph.passes.begin());
            ++indegree[index];
            outgoing[dependency_index].push_back(index);
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < count; ++index) {
        if (indegree[index] == 0) ready.push_back(index);
    }
    FramePassGraph ordered;
    ordered.passes.reserve(count);
    ordered.resources = graph.resources;
    while (!ready.empty()) {
        // The source index is the tie breaker, not a hash or pointer address.
        const std::size_t index = ready.front();
        ready.erase(ready.begin());
        ordered.passes.push_back(graph.passes[index]);
        for (const std::size_t dependent : outgoing[index]) {
            if (--indegree[dependent] == 0) {
                const auto insertion = std::lower_bound(ready.begin(), ready.end(), dependent);
                ready.insert(insertion, dependent);
            }
        }
    }
    if (ordered.passes.size() != count) {
        throw graph_error(FrameGraphErrorCode::cycle,
                          "frame pass graph contains a dependency cycle");
    }
    validate_resources(ordered);
    return ordered;
}

FramePlan build_frame_plan(const apex::scene::SceneSnapshot& scene,
                           const FramePlanOptions& options) {
    FramePlan plan;
    plan.render = build_render_plan(scene, options.render);
    plan.diagnostics.push_back({
        "shader_execution_staged",
        "frame_composition",
        "Passes describe ordering and resource dependencies only; shader execution and pixel behavior are unsupported in this slice.",
    });

    FramePassGraph graph;
    auto add = [&](std::string name, FramePassKind kind,
                   std::vector<std::string> dependencies,
                   std::vector<FrameResourceKind> reads,
                   std::vector<FrameResourceKind> writes,
                   const char* effect) {
        graph.passes.push_back({std::move(name), kind, std::move(dependencies),
                                std::move(reads), std::move(writes), false});
        plan.diagnostics.push_back({"shader_execution_staged", effect,
                                    std::string(effect) + " is represented as a logical pass only; its shader/effect is not implemented."});
    };
    auto disabled = [&](const char* effect, const char* reason) {
        plan.diagnostics.push_back({"effect_disabled", effect, reason});
    };
    auto unavailable = [&](const char* effect) {
        plan.diagnostics.push_back({"capability_unavailable", effect,
                                    std::string(effect) + " was requested but the backend capability is unavailable."});
    };

    // draw() updates the CSP local atlas before reflection capture, then
    // captures the cubemap, and only then renders the main directional maps.
    std::string last_color;
    if (options.include_local_shadows && options.render.include_shadows) {
        if (options.capabilities.local_shadow_atlas) {
            add("local_shadow", FramePassKind::local_shadow, {}, {},
                {FrameResourceKind::local_shadow_atlas}, "local_shadows");
        } else {
            unavailable("local_shadows");
        }
    } else {
        disabled("local_shadows", "Local shadow atlas generation is disabled for this frame.");
    }

    const bool isolated = scene.isolated || options.render.isolated;
    const bool reflection_requested = options.include_reflection_capture &&
                                      options.render.include_reflections && !isolated;
    const bool reflection_has_geometry = !plan.render.reflection.items.empty();
    if (reflection_requested && reflection_has_geometry && options.capabilities.reflection_capture) {
        std::vector<std::string> dependencies;
        if (!graph.passes.empty() && graph.passes.back().name == "local_shadow") {
            dependencies.push_back("local_shadow");
        }
        const bool local_shadow_ready = std::find_if(
                                            graph.passes.begin(), graph.passes.end(),
                                            [](const FramePass& pass) { return pass.name == "local_shadow"; }) !=
                                        graph.passes.end();
        add("reflection_capture", FramePassKind::reflection_capture,
            std::move(dependencies),
            local_shadow_ready ? std::vector<FrameResourceKind>{FrameResourceKind::local_shadow_atlas}
                               : std::vector<FrameResourceKind>{},
            {FrameResourceKind::reflection_cube, FrameResourceKind::reflection_probe_shadow_cascades}, "reflection_capture");
    } else if (reflection_requested && !reflection_has_geometry) {
        plan.diagnostics.push_back({"capture_empty", "reflection_capture",
                                    "Reflection capture is enabled, but the selected capture set has no visible geometry."});
    } else if (!reflection_requested) {
        disabled("reflection_capture", isolated
                                           ? "Reflection capture is disabled for isolated rendering."
                                           : "Reflection capture is disabled for this frame.");
    } else {
        unavailable("reflection_capture");
    }

    if (options.include_directional_shadows && options.render.include_shadows) {
        if (options.capabilities.directional_shadow_cascades) {
            add("directional_shadow", FramePassKind::directional_shadow, {}, {},
                {FrameResourceKind::directional_shadow_cascades}, "directional_shadows");
        } else {
            unavailable("directional_shadows");
        }
    } else {
        disabled("directional_shadows", "Directional shadow cascades are disabled for this frame.");
    }

    if (options.include_sky) {
        add("sky", FramePassKind::sky, {}, {}, {FrameResourceKind::hdr_ms_color}, "sky_lighting");
        last_color = "sky";
    } else {
        disabled("sky_lighting", "Sky and weather lighting are disabled for this frame.");
    }

    std::vector<std::string> opaque_dependencies;
    if (!last_color.empty()) opaque_dependencies.push_back(last_color);
    for (const FramePass& pass : graph.passes) {
        if (pass.name == "local_shadow" || pass.name == "directional_shadow") {
            opaque_dependencies.push_back(pass.name);
        }
    }
    std::vector<FrameResourceKind> opaque_reads;
    if (std::find(opaque_dependencies.begin(), opaque_dependencies.end(), "local_shadow") != opaque_dependencies.end()) {
        opaque_reads.push_back(FrameResourceKind::local_shadow_atlas);
    }
    if (std::find(opaque_dependencies.begin(), opaque_dependencies.end(), "directional_shadow") != opaque_dependencies.end()) {
        opaque_reads.push_back(FrameResourceKind::directional_shadow_cascades);
    }
    if (reflection_requested && reflection_has_geometry && options.capabilities.reflection_capture) {
        opaque_dependencies.push_back("reflection_capture");
        opaque_reads.push_back(FrameResourceKind::reflection_cube);
    }
    add("opaque", FramePassKind::opaque, std::move(opaque_dependencies),
        std::move(opaque_reads), {FrameResourceKind::depth_buffer, FrameResourceKind::hdr_ms_color}, "opaque_lighting");
    last_color = "opaque";

    const bool has_transparent = !plan.render.transparent_items.empty();
    const bool can_resolve_scene = has_transparent && options.include_refraction;
    if (can_resolve_scene && options.capabilities.refraction) {
        add("opaque_scene_resolve", FramePassKind::opaque_scene_resolve, {"opaque"},
            {FrameResourceKind::hdr_ms_color}, {FrameResourceKind::opaque_scene_color}, "refraction_scene_resolve");
    } else if (has_transparent && !options.include_refraction) {
        disabled("refraction_scene_resolve", "Opaque scene color resolve is disabled; transparent materials remain scheduled without refraction input.");
    } else if (has_transparent) {
        unavailable("refraction_scene_resolve");
    }

    if (has_transparent) {
        std::vector<std::string> dependencies = {"opaque"};
        std::vector<FrameResourceKind> reads = {FrameResourceKind::depth_buffer};
        if (std::find_if(graph.passes.begin(), graph.passes.end(),
                         [](const FramePass& pass) { return pass.name == "local_shadow"; }) != graph.passes.end()) {
            reads.push_back(FrameResourceKind::local_shadow_atlas);
        }
        if (std::find_if(graph.passes.begin(), graph.passes.end(),
                         [](const FramePass& pass) { return pass.name == "directional_shadow"; }) != graph.passes.end()) {
            reads.push_back(FrameResourceKind::directional_shadow_cascades);
        }
        if (reflection_requested && reflection_has_geometry && options.capabilities.reflection_capture) {
            reads.push_back(FrameResourceKind::reflection_cube);
        }
        if (can_resolve_scene && options.capabilities.refraction) {
            dependencies.push_back("opaque_scene_resolve");
            reads.push_back(FrameResourceKind::opaque_scene_color);
        }
        add("transparent", FramePassKind::transparent, std::move(dependencies), std::move(reads),
            {FrameResourceKind::hdr_ms_color}, "transparent_lighting");
        last_color = "transparent";
    }

    const auto add_overlay = [&](bool requested, const char* name, FramePassKind kind,
                                 const char* effect, bool enabled_capability) {
        if (!requested) return;
        if (!enabled_capability) {
            unavailable(effect);
            return;
        }
        add(name, kind, {last_color}, {FrameResourceKind::hdr_ms_color},
            {FrameResourceKind::hdr_ms_color}, effect);
        last_color = name;
    };
    add_overlay(options.include_rain_overlay, "rain_overlay", FramePassKind::rain_overlay,
                "rain_overlay", options.capabilities.overlays);
    add_overlay(options.include_grid_overlay, "grid_overlay", FramePassKind::grid_overlay,
                "grid_overlay", options.capabilities.overlays);
    add_overlay(options.include_selection_overlay, "selection_overlay", FramePassKind::selection_overlay,
                "selection_overlay", options.capabilities.overlays);
    add_overlay(options.include_collider_overlay, "collider_overlay", FramePassKind::collider_overlay,
                "collider_overlay", options.capabilities.overlays);

    if (options.include_hdr_resolve && options.capabilities.hdr_composition) {
        add("hdr_resolve", FramePassKind::hdr_resolve, {last_color},
            {FrameResourceKind::hdr_ms_color}, {FrameResourceKind::hdr_resolved_color}, "hdr_resolve");
    } else if (!options.include_hdr_resolve) {
        disabled("hdr_resolve", "HDR resolve is disabled for this frame.");
    } else {
        unavailable("hdr_resolve");
    }
    const bool hdr_ready = !graph.passes.empty() && graph.passes.back().name == "hdr_resolve";
    if (options.include_bloom && hdr_ready && options.capabilities.bloom) {
        add("bloom", FramePassKind::bloom, {"hdr_resolve"},
            {FrameResourceKind::hdr_resolved_color}, {FrameResourceKind::bloom_chain}, "bloom");
    } else if (options.include_bloom && !hdr_ready) {
        plan.diagnostics.push_back({"dependency_unavailable", "bloom",
                                    "Bloom requires the HDR resolve pass and is therefore omitted."});
    } else if (!options.include_bloom) {
        disabled("bloom", "Bloom is disabled for this frame.");
    } else {
        unavailable("bloom");
    }
    if (options.include_postprocess && hdr_ready && options.capabilities.postprocess) {
        std::vector<FrameResourceKind> reads = {FrameResourceKind::hdr_resolved_color};
        std::vector<std::string> dependencies = {"hdr_resolve"};
        if (!graph.passes.empty() && graph.passes.back().name == "bloom") {
            dependencies.push_back("bloom");
            reads.push_back(FrameResourceKind::bloom_chain);
        }
        add("postprocess", FramePassKind::postprocess, std::move(dependencies), std::move(reads),
            {FrameResourceKind::backbuffer}, "postprocess");
    } else if (options.include_postprocess && !hdr_ready) {
        plan.diagnostics.push_back({"dependency_unavailable", "postprocess",
                                    "Postprocess requires the HDR resolve pass and is therefore omitted."});
    } else if (!options.include_postprocess) {
        disabled("postprocess", "Postprocess is disabled for this frame.");
    } else {
        unavailable("postprocess");
    }

    for (const FramePass& pass : graph.passes) {
        for (const FrameResourceKind resource : pass.writes) {
            const auto already_declared = std::any_of(
                graph.resources.begin(), graph.resources.end(),
                [&](const FrameResourceDeclaration& declaration) {
                    return declaration.kind == resource;
                });
            if (!already_declared) graph.resources.push_back({resource, false});
        }
    }
    plan.graph = order_frame_pass_graph(graph, options.limits);
    return plan;
}

}  // namespace apex::render
