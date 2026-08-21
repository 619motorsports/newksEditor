#pragma once

#include "apex/render/render_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::render {

// These are logical stages only. They deliberately do not expose a graphics
// API handle, shader object, framebuffer, or pixel-fidelity promise.
enum class FramePassKind : std::uint8_t {
    local_shadow,
    reflection_capture,
    directional_shadow,
    sky,
    opaque,
    opaque_scene_resolve,
    transparent,
    rain_overlay,
    grid_overlay,
    selection_overlay,
    collider_overlay,
    hdr_resolve,
    bloom,
    postprocess,
};

enum class FrameResourceKind : std::uint8_t {
    local_shadow_atlas,
    reflection_cube,
    reflection_probe_shadow_cascades,
    directional_shadow_cascades,
    depth_buffer,
    hdr_ms_color,
    opaque_scene_color,
    hdr_resolved_color,
    bloom_chain,
    backbuffer,
};

struct FrameResourceDeclaration {
    FrameResourceKind kind = FrameResourceKind::hdr_ms_color;
    // External resources are supplied by the backend before this graph. A
    // non-external resource must be written by an earlier pass before reads.
    bool external = false;
};

using FrameResource = FrameResourceDeclaration;

struct FrameBackendCapabilities {
    bool local_shadow_atlas = true;
    bool reflection_capture = true;
    bool directional_shadow_cascades = true;
    bool hdr_composition = true;
    bool refraction = true;
    bool bloom = true;
    bool postprocess = true;
    bool overlays = true;
};

struct FramePlanLimits {
    std::size_t max_passes = 64;
    std::size_t max_resources = 32;
    std::size_t max_dependencies = 256;
    std::size_t max_name_length = 96;
};

struct FramePass {
    std::string name;
    FramePassKind kind = FramePassKind::sky;
    std::vector<std::string> dependencies;
    std::vector<FrameResourceKind> reads;
    std::vector<FrameResourceKind> writes;
    // False is intentional: this slice describes composition and ordering,
    // while shader execution remains an explicit later backend stage.
    bool shader_execution_supported = false;
};

struct FramePassGraph {
    std::vector<FramePass> passes;
    std::vector<FrameResourceDeclaration> resources;
};

enum class FrameGraphErrorCode : std::uint8_t {
    too_many_passes,
    too_many_resources,
    too_many_dependencies,
    invalid_name,
    duplicate_pass,
    duplicate_resource,
    missing_dependency,
    missing_resource,
    cycle,
};

class FrameGraphError final : public std::runtime_error {
public:
    FrameGraphError(FrameGraphErrorCode code, std::string message);

    [[nodiscard]] FrameGraphErrorCode code() const noexcept { return code_; }

private:
    FrameGraphErrorCode code_;
};

[[nodiscard]] const char* frame_pass_kind_name(FramePassKind kind) noexcept;
[[nodiscard]] const char* frame_resource_kind_name(FrameResourceKind kind) noexcept;

// Validates a logical graph and returns a stable topological order. Nodes
// with equal dependency readiness retain their source order. This is also the
// public validation seam for adapters that construct their own pass graph.
[[nodiscard]] FramePassGraph order_frame_pass_graph(
    const FramePassGraph& graph, const FramePlanLimits& limits = {});

struct FramePlanDiagnostic {
    std::string code;
    std::string effect;
    std::string description;
};

struct FramePlanOptions {
    RenderPlanOptions render;
    bool include_local_shadows = true;
    bool include_reflection_capture = true;
    bool include_directional_shadows = true;
    bool include_sky = true;
    bool include_refraction = true;
    bool include_rain_overlay = false;
    bool include_grid_overlay = false;
    bool include_selection_overlay = false;
    bool include_collider_overlay = false;
    bool include_hdr_resolve = true;
    bool include_bloom = true;
    bool include_postprocess = true;
    FrameBackendCapabilities capabilities;
    FramePlanLimits limits;
};

struct FramePlan {
    RenderPlan render;
    FramePassGraph graph;
    std::vector<FramePlanDiagnostic> diagnostics;
    // Every pass in this plan is a scheduling description. A backend must not
    // interpret this object as evidence that any shader or effect is present.
    bool shader_execution_supported = false;
};

[[nodiscard]] FramePlan build_frame_plan(
    const apex::scene::SceneSnapshot& scene,
    const FramePlanOptions& options = {});

}  // namespace apex::render
