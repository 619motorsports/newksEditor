#include "apex/app/workspace_viewport.hpp"

#include "apex/core/parse_error.hpp"
#include "apex/render/authoring_grid.hpp"
#include "apex/render/selected_mesh.hpp"
#include "apex/render/view_axis.hpp"
#include "apex/workspace/workspace_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace apex::app {
namespace {

using render::PipelineRenderTarget;
using render::PipelineRenderTargetFormat;

[[nodiscard]] render::Diagnostic diagnostic(const char* code, const char* message) {
    return {code, message};
}

[[nodiscard]] std::optional<PipelineRenderTargetFormat> pipelineColorFormat(
    render::TextureFormat format) noexcept {
    switch (format) {
    case render::TextureFormat::rgba8_unorm:
        return PipelineRenderTargetFormat::rgba8_unorm;
    case render::TextureFormat::rgba8_srgb:
        return PipelineRenderTargetFormat::rgba8_srgb;
    case render::TextureFormat::bgra8_unorm:
        return PipelineRenderTargetFormat::bgra8_unorm;
    case render::TextureFormat::bgra8_srgb:
        return PipelineRenderTargetFormat::bgra8_srgb;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] render::CameraClipSpace
expectedClipSpace(render::Backend backend) noexcept {
    return backend == render::Backend::Vulkan ? render::CameraClipSpace::vulkan
                                              : render::CameraClipSpace::d3d12;
}

[[nodiscard]] bool finite_vector(const apex::scene::Vector3 &value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const float component) {
        return std::isfinite(component);
    });
}

[[nodiscard]] bool
validateAiSplineGeometry(const WorkspaceAiSplineGeometry &geometry,
                         const WorkspaceAiSplinePassKind kind,
                         const WorkspaceAiSplineGeometry *primary,
                         render::Diagnostic &output_diagnostic) {
    std::size_t expected_first = 0U;
    for (const WorkspaceAiSplineChunk &chunk : geometry.chunks) {
        if (expected_first > geometry.vertices.size() ||
            chunk.first_vertex != expected_first || chunk.vertex_count < 2U ||
            chunk.vertex_count % 2U != 0U ||
            chunk.vertex_count > render::max_overlay_line_vertices ||
            static_cast<std::size_t>(chunk.vertex_count) >
                geometry.vertices.size() - expected_first) {
            output_diagnostic =
                diagnostic("workspace_viewport_ai_spline_chunk_invalid",
                           "AI spline chunks must cover complete bounded line "
                           "pairs in order");
            return false;
        }
        expected_first += chunk.vertex_count;
    }

    std::optional<std::size_t> expected_vertices;
    const std::size_t sample_count = geometry.sample_point_count;
    if (geometry.topology == WorkspaceAiSplineTopology::independent_lines) {
        if (sample_count <= std::numeric_limits<std::size_t>::max() / 2U)
            expected_vertices = sample_count * 2U;
    } else if (geometry.topology == WorkspaceAiSplineTopology::polyline) {
        if (sample_count <= 2U) {
            expected_vertices = 0U;
        } else if (sample_count - 1U <=
                   std::numeric_limits<std::size_t>::max() / 2U) {
            expected_vertices = (sample_count - 1U) * 2U;
        }
    }
    const bool selection_state_empty =
        geometry.selected_point_count == 0U &&
        !geometry.last_selected_index.has_value();
    const bool temporary_state_empty =
        geometry.temporary_point_count == 0U;
    const bool primary_metadata_valid =
        kind == WorkspaceAiSplinePassKind::primary &&
        geometry.pass == WorkspaceAiSplinePassKind::primary &&
        geometry.topology == WorkspaceAiSplineTopology::polyline &&
        selection_state_empty && temporary_state_empty &&
        (geometry.mode == WorkspaceAiSplineDisplayMode::raw
             ? geometry.sample_point_count == geometry.source_point_count
             : geometry.mode == WorkspaceAiSplineDisplayMode::interpolated &&
                   geometry.source_point_count >= 4U &&
                   geometry.sample_point_count ==
                       workspace_ai_spline_interpolated_sample_count);
    const bool interval_metadata_valid =
        kind == WorkspaceAiSplinePassKind::interval &&
        geometry.pass == WorkspaceAiSplinePassKind::interval &&
        geometry.topology == WorkspaceAiSplineTopology::polyline &&
        geometry.mode == WorkspaceAiSplineDisplayMode::interpolated &&
        selection_state_empty && temporary_state_empty &&
        geometry.source_point_count >= 4U &&
        geometry.sample_point_count >= 1U &&
        geometry.sample_point_count <=
            workspace_ai_spline_interpolated_sample_count;
    const bool side_metadata_valid =
        (kind == WorkspaceAiSplinePassKind::left_side ||
         kind == WorkspaceAiSplinePassKind::right_side) &&
        geometry.pass == kind &&
        geometry.topology == WorkspaceAiSplineTopology::polyline &&
        geometry.mode == WorkspaceAiSplineDisplayMode::raw &&
        selection_state_empty && temporary_state_empty &&
        geometry.sample_point_count <= geometry.source_point_count &&
        geometry.source_point_count <=
            render::max_overlay_line_total_vertices / 2U + 1U &&
        primary != nullptr &&
        geometry.source_point_count == primary->source_point_count;
    const bool selection_metadata_valid =
        kind == WorkspaceAiSplinePassKind::selection &&
        geometry.pass == WorkspaceAiSplinePassKind::selection &&
        geometry.topology == WorkspaceAiSplineTopology::independent_lines &&
        geometry.mode == WorkspaceAiSplineDisplayMode::raw &&
        temporary_state_empty &&
        geometry.selected_point_count >= 1U &&
        geometry.selected_point_count <= geometry.source_point_count &&
        geometry.last_selected_index.has_value() &&
        *geometry.last_selected_index < geometry.source_point_count &&
        geometry.sample_point_count >= geometry.selected_point_count &&
        static_cast<std::size_t>(geometry.sample_point_count) <=
            static_cast<std::size_t>(geometry.selected_point_count) * 3U &&
        geometry.source_point_count <=
            workspace_ai_spline_max_interpolation_control_points &&
        primary != nullptr &&
        geometry.source_point_count == primary->source_point_count;
    const bool temporary_interpolation_metadata_valid =
        kind == WorkspaceAiSplinePassKind::temporary_interpolation &&
        geometry.pass == WorkspaceAiSplinePassKind::temporary_interpolation &&
        geometry.topology == WorkspaceAiSplineTopology::polyline &&
        geometry.mode == WorkspaceAiSplineDisplayMode::interpolated &&
        selection_state_empty && geometry.temporary_point_count >= 5U &&
        geometry.temporary_point_count <=
            workspace_ai_spline_max_temporary_edit_points &&
        geometry.sample_point_count ==
            workspace_ai_spline_interpolated_sample_count &&
        primary != nullptr &&
        geometry.source_point_count == primary->source_point_count;
    const bool temporary_marker_metadata_valid =
        kind == WorkspaceAiSplinePassKind::temporary_markers &&
        geometry.pass == WorkspaceAiSplinePassKind::temporary_markers &&
        geometry.topology == WorkspaceAiSplineTopology::independent_lines &&
        geometry.mode == WorkspaceAiSplineDisplayMode::raw &&
        selection_state_empty && geometry.temporary_point_count >= 1U &&
        geometry.temporary_point_count <=
            workspace_ai_spline_max_temporary_edit_points &&
        (geometry.sample_point_count == geometry.temporary_point_count ||
         geometry.sample_point_count ==
             geometry.temporary_point_count + 2U) &&
        primary != nullptr &&
        geometry.source_point_count == primary->source_point_count;
    const bool camber_metadata_valid =
        kind == WorkspaceAiSplinePassKind::camber &&
        geometry.pass == WorkspaceAiSplinePassKind::camber &&
        geometry.topology == WorkspaceAiSplineTopology::independent_lines &&
        geometry.mode == WorkspaceAiSplineDisplayMode::raw &&
        selection_state_empty && temporary_state_empty &&
        geometry.sample_point_count == geometry.source_point_count &&
        geometry.source_point_count <=
            render::max_overlay_line_total_vertices / 2U &&
        primary != nullptr &&
        geometry.source_point_count == primary->source_point_count;
    if (!expected_vertices.has_value() ||
        expected_first != geometry.vertices.size() ||
        (geometry.vertices.empty() != geometry.chunks.empty()) ||
        geometry.vertices.size() != *expected_vertices ||
        !(primary_metadata_valid || interval_metadata_valid ||
          side_metadata_valid || selection_metadata_valid ||
          temporary_interpolation_metadata_valid ||
          temporary_marker_metadata_valid || camber_metadata_valid)) {
        output_diagnostic = diagnostic(
            "workspace_viewport_ai_spline_geometry_invalid",
            "AI spline geometry does not match its display mode, sample "
            "count, and chunk metadata");
        return false;
    }

    const auto &expected_color = kind == WorkspaceAiSplinePassKind::primary
                                     ? workspace_ai_spline_raw_color
                                 : kind == WorkspaceAiSplinePassKind::interval
                                     ? workspace_ai_spline_interval_color
                                     : workspace_ai_spline_side_color;
    for (const render::OverlayLineVertex &vertex : geometry.vertices) {
        const bool color_valid =
            kind == WorkspaceAiSplinePassKind::camber
                ? vertex.color == workspace_ai_spline_camber_positive_color ||
                      vertex.color ==
                          workspace_ai_spline_camber_nonpositive_color
            : kind == WorkspaceAiSplinePassKind::selection
                ? vertex.color == workspace_ai_spline_selection_color ||
                      vertex.color == workspace_ai_spline_selection_side_color
            : kind == WorkspaceAiSplinePassKind::temporary_interpolation ||
                      kind == WorkspaceAiSplinePassKind::temporary_markers
                ? vertex.color == workspace_ai_spline_temporary_color ||
                      (kind == WorkspaceAiSplinePassKind::temporary_markers &&
                       vertex.color ==
                           workspace_ai_spline_temporary_forward_color)
                : vertex.color == expected_color;
        if (!finite_vector(vertex.position) || !color_valid) {
            output_diagnostic = diagnostic(
                "workspace_viewport_ai_spline_vertex_invalid",
                "AI spline vertices require finite positions and the "
                "recovered pass color");
            return false;
        }
    }

    if (kind == WorkspaceAiSplinePassKind::selection) {
        std::size_t center_count = 0U;
        std::size_t side_count = 0U;
        for (std::size_t vertex = 0U; vertex < geometry.vertices.size();
             vertex += 2U) {
            const auto &begin = geometry.vertices[vertex];
            const auto &end = geometry.vertices[vertex + 1U];
            const bool center =
                begin.color == workspace_ai_spline_selection_color;
            if (center) {
                if (side_count != 0U && side_count != 2U) {
                    output_diagnostic = diagnostic(
                        "workspace_viewport_ai_spline_selection_line_invalid",
                        "Each AI spline selection marker requires zero or two "
                        "side lines");
                    return false;
                }
                ++center_count;
                side_count = 0U;
            } else {
                ++side_count;
            }
            const auto &line_color =
                center ? workspace_ai_spline_selection_color
                       : workspace_ai_spline_selection_side_color;
            if (center_count == 0U || side_count > 2U ||
                begin.color != line_color || end.color != line_color ||
                begin.position[0] != end.position[0] ||
                begin.position[2] != end.position[2] ||
                end.position[1] !=
                    begin.position[1] + workspace_ai_spline_selection_height) {
                output_diagnostic = diagnostic(
                    "workspace_viewport_ai_spline_selection_line_invalid",
                    "AI spline selection lines must be vertical and use the "
                    "recovered height and colors");
                return false;
            }
        }
        if ((side_count != 0U && side_count != 2U) ||
            center_count != geometry.selected_point_count) {
            output_diagnostic = diagnostic(
                "workspace_viewport_ai_spline_selection_line_invalid",
                "AI spline selection markers do not match the recovered line "
                "groups");
            return false;
        }
    }
    if (kind == WorkspaceAiSplinePassKind::camber) {
        for (std::size_t vertex = 0U; vertex < geometry.vertices.size();
             vertex += 2U) {
            const auto &begin = geometry.vertices[vertex];
            const auto &end = geometry.vertices[vertex + 1U];
            if (begin.color != end.color ||
                begin.position[0] != end.position[0] ||
                begin.position[2] != end.position[2] ||
                end.position[1] < begin.position[1]) {
                output_diagnostic = diagnostic(
                    "workspace_viewport_ai_spline_camber_line_invalid",
                    "AI spline camber lines must be vertical, upward, and one "
                    "color");
                return false;
            }
        }
    }
    if (kind == WorkspaceAiSplinePassKind::temporary_markers) {
        const std::size_t marker_vertex_count =
            static_cast<std::size_t>(geometry.temporary_point_count) * 2U;
        for (std::size_t vertex = 0U; vertex < marker_vertex_count;
             vertex += 2U) {
            const auto& begin = geometry.vertices[vertex];
            const auto& end = geometry.vertices[vertex + 1U];
            if (begin.position[0] != end.position[0] ||
                begin.position[2] != end.position[2] ||
                end.position[1] != begin.position[1] +
                                       workspace_ai_spline_temporary_marker_height) {
                output_diagnostic = diagnostic(
                    "workspace_viewport_ai_spline_temporary_marker_invalid",
                    "Temporary AI spline markers must match the portable vertical-line contract");
                return false;
            }
        }
        if (geometry.vertices.size() > marker_vertex_count) {
            const auto& forwardBegin = geometry.vertices[marker_vertex_count];
            const auto& forwardEnd =
                geometry.vertices[marker_vertex_count + 1U];
            const auto& sideBegin =
                geometry.vertices[marker_vertex_count + 2U];
            const auto& sideEnd =
                geometry.vertices[marker_vertex_count + 3U];
            const std::array<float, 3U> forwardDelta = {
                forwardEnd.position[0U] - forwardBegin.position[0U],
                forwardEnd.position[1U] - forwardBegin.position[1U],
                forwardEnd.position[2U] - forwardBegin.position[2U]};
            const std::array<float, 3U> sideDelta = {
                sideEnd.position[0U] - sideBegin.position[0U],
                sideEnd.position[1U] - sideBegin.position[1U],
                sideEnd.position[2U] - sideBegin.position[2U]};
            const float forwardLengthSquared =
                forwardDelta[0U] * forwardDelta[0U] +
                forwardDelta[1U] * forwardDelta[1U] +
                forwardDelta[2U] * forwardDelta[2U];
            const float sideLengthSquared =
                sideDelta[0U] * sideDelta[0U] +
                sideDelta[1U] * sideDelta[1U] +
                sideDelta[2U] * sideDelta[2U];
            const float axisDot =
                forwardDelta[0U] * sideDelta[0U] +
                forwardDelta[1U] * sideDelta[1U] +
                forwardDelta[2U] * sideDelta[2U];
            const bool recoveredLength =
                (std::abs(forwardLengthSquared - 9.0F) <= 0.0001F &&
                 std::abs(sideLengthSquared - 9.0F) <= 0.0001F) ||
                (forwardLengthSquared == 0.0F &&
                 sideLengthSquared == 0.0F);
            if (forwardBegin.color !=
                    workspace_ai_spline_temporary_forward_color ||
                forwardEnd.color !=
                    workspace_ai_spline_temporary_forward_color ||
                sideBegin.color != workspace_ai_spline_temporary_color ||
                sideEnd.color != workspace_ai_spline_temporary_color ||
                forwardBegin.position != sideBegin.position ||
                forwardDelta[1U] != 0.0F || sideDelta[1U] != 0.0F ||
                !recoveredLength ||
                std::abs(axisDot) > 0.0001F) {
                output_diagnostic = diagnostic(
                    "workspace_viewport_ai_spline_temporary_axis_invalid",
                    "Movable temporary AI spline axes must use the recovered colors, origin, length, and perpendicular directions");
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool
validateAiSplineDepth(const render::PipelineProgram &pipeline,
                      const WorkspaceAiSplinePassKind kind,
                      render::Diagnostic &output_diagnostic) {
    const bool valid =
        kind != WorkspaceAiSplinePassKind::interval
            ? pipeline.depth.test_enabled && pipeline.depth.write_enabled &&
                  pipeline.depth.compare ==
                      render::PipelineCompareOperation::less_or_equal
            : !pipeline.depth.test_enabled && !pipeline.depth.write_enabled;
    if (!valid) {
        output_diagnostic =
            diagnostic("workspace_viewport_ai_spline_depth_invalid",
                       "AI spline pass depth state does not match "
                       "the recovered spline-pass behavior");
    }
    return valid;
}

[[nodiscard]] bool finite_input_delta(const float value) noexcept {
    // SDL normally supplies small deltas, but keep the application seam
    // bounded when a caller supplies synthetic or hostile event data.
    return std::isfinite(value) && std::abs(value) <= 100'000.0F;
}

[[nodiscard]] apex::scene::Vector3
orbit_position(const apex::scene::Vector3 &target, float yaw, float pitch,
               float distance) noexcept {
    const float horizontal = distance * std::cos(pitch);
    return {
        target[0] + horizontal * std::sin(yaw),
        target[1] + distance * std::sin(pitch),
        target[2] + horizontal * std::cos(yaw),
    };
}

[[nodiscard]] WorkspaceViewportStatus
preparationStatus(render::StaticSceneResourceStatus status) noexcept {
    switch (status) {
    case render::StaticSceneResourceStatus::ready:
        return WorkspaceViewportStatus::ready;
    case render::StaticSceneResourceStatus::unsupported:
        return WorkspaceViewportStatus::unsupported;
    case render::StaticSceneResourceStatus::allocation_failed:
        return WorkspaceViewportStatus::allocation_failed;
    case render::StaticSceneResourceStatus::invalid_request:
    case render::StaticSceneResourceStatus::upload_failed:
        return WorkspaceViewportStatus::invalid;
    }
    return WorkspaceViewportStatus::invalid;
}

[[nodiscard]] WorkspaceViewportStatus externalTextureStatus(
    render::ExternalTextureAuthorityStatus status) noexcept {
    switch (status) {
    case render::ExternalTextureAuthorityStatus::ready:
        return WorkspaceViewportStatus::ready;
    case render::ExternalTextureAuthorityStatus::unsupported:
        return WorkspaceViewportStatus::unsupported;
    case render::ExternalTextureAuthorityStatus::resource_limit:
        return WorkspaceViewportStatus::allocation_failed;
    case render::ExternalTextureAuthorityStatus::invalid_request:
    case render::ExternalTextureAuthorityStatus::rejected:
    case render::ExternalTextureAuthorityStatus::missing:
    case render::ExternalTextureAuthorityStatus::ambiguous:
    case render::ExternalTextureAuthorityStatus::read_failed:
        return WorkspaceViewportStatus::invalid;
    }
    return WorkspaceViewportStatus::invalid;
}

[[nodiscard]] WorkspaceViewportFrameStatus frameStatus(
    render::PresentationFrameStatus status) noexcept {
    switch (status) {
    case render::PresentationFrameStatus::ready:
        return WorkspaceViewportFrameStatus::ready;
    case render::PresentationFrameStatus::invalid_request:
        return WorkspaceViewportFrameStatus::invalid;
    case render::PresentationFrameStatus::unsupported:
        return WorkspaceViewportFrameStatus::unsupported;
    case render::PresentationFrameStatus::execution_failed:
        return WorkspaceViewportFrameStatus::execution_failed;
    }
    return WorkspaceViewportFrameStatus::execution_failed;
}

[[nodiscard]] WorkspaceViewportFrameStatus drawStatus(
    render::IndexedStaticMeshBatchStatus status) noexcept {
    switch (status) {
    case render::IndexedStaticMeshBatchStatus::ready:
        return WorkspaceViewportFrameStatus::ready;
    case render::IndexedStaticMeshBatchStatus::invalid_request:
        return WorkspaceViewportFrameStatus::invalid;
    case render::IndexedStaticMeshBatchStatus::unsupported:
        return WorkspaceViewportFrameStatus::unsupported;
    case render::IndexedStaticMeshBatchStatus::execution_failed:
        return WorkspaceViewportFrameStatus::execution_failed;
    }
    return WorkspaceViewportFrameStatus::execution_failed;
}

[[nodiscard]] WorkspaceViewportStatus shadowPreparationStatus(
    render::DirectionalShadowMapStatus status) noexcept {
    switch (status) {
    case render::DirectionalShadowMapStatus::ready:
        return WorkspaceViewportStatus::ready;
    case render::DirectionalShadowMapStatus::invalid_request:
        return WorkspaceViewportStatus::invalid;
    case render::DirectionalShadowMapStatus::unsupported:
        return WorkspaceViewportStatus::unsupported;
    case render::DirectionalShadowMapStatus::allocation_failed:
        return WorkspaceViewportStatus::allocation_failed;
    }
    return WorkspaceViewportStatus::invalid;
}

[[nodiscard]] WorkspaceViewportFrameStatus shadowFrameStatus(
    render::DirectionalShadowMapStatus status) noexcept {
    switch (status) {
    case render::DirectionalShadowMapStatus::ready:
        return WorkspaceViewportFrameStatus::ready;
    case render::DirectionalShadowMapStatus::invalid_request:
        return WorkspaceViewportFrameStatus::invalid;
    case render::DirectionalShadowMapStatus::unsupported:
        return WorkspaceViewportFrameStatus::unsupported;
    case render::DirectionalShadowMapStatus::allocation_failed:
        return WorkspaceViewportFrameStatus::execution_failed;
    }
    return WorkspaceViewportFrameStatus::execution_failed;
}

[[nodiscard]] WorkspaceViewportFrameStatus shadowDrawStatus(
    render::StaticSceneDirectionalShadowStatus status) noexcept {
    switch (status) {
    case render::StaticSceneDirectionalShadowStatus::ready:
    case render::StaticSceneDirectionalShadowStatus::partial:
        return WorkspaceViewportFrameStatus::ready;
    case render::StaticSceneDirectionalShadowStatus::invalid_request:
        return WorkspaceViewportFrameStatus::invalid;
    case render::StaticSceneDirectionalShadowStatus::unsupported:
        return WorkspaceViewportFrameStatus::unsupported;
    case render::StaticSceneDirectionalShadowStatus::execution_failed:
        return WorkspaceViewportFrameStatus::execution_failed;
    }
    return WorkspaceViewportFrameStatus::execution_failed;
}

[[nodiscard]] bool validateShadowPrograms(
    const WorkspaceViewportPrepareRequest& request,
    render::Backend backend,
    render::Diagnostic& output_diagnostic) {
    if (!request.directional_shadows.has_value()) return true;
    const auto layout = request.directional_shadows->constants_layout;
    if (layout != render::DirectionalShadowReceiverConstantsLayout::portable &&
        layout != render::DirectionalShadowReceiverConstantsLayout::stock_ks_shadow_maps) {
        output_diagnostic = diagnostic(
            "workspace_viewport_shadow_constants_layout_invalid",
            "Directional shadow constants require an explicit supported layout");
        return false;
    }

    std::uint64_t total_shader_bytes = 0U;
    const auto add_modules = [&](std::span<const render::PipelineShaderModule> modules) {
        for (const auto& module : modules) {
            const std::uint64_t size = module.bytes.size();
            if (size > request.limits.material.scene.max_total_shader_bytes ||
                total_shader_bytes >
                    request.limits.material.scene.max_total_shader_bytes - size)
                return false;
            total_shader_bytes += size;
        }
        return true;
    };
    for (const auto& set : request.shader_modules) {
        if (!add_modules(set.modules)) {
            output_diagnostic = diagnostic(
                "workspace_viewport_shadow_shader_budget",
                "Material and shadow programs exceed the shared shader byte budget");
            return false;
        }
    }
    struct ProgramRole {
        const std::optional<render::PipelineProgram>* program = nullptr;
        render::DepthOnlyIndexedPipelineRole role =
            render::DepthOnlyIndexedPipelineRole::opaque_static;
    };
    const std::array<ProgramRole, 3U> programs = {{
        {&request.directional_shadows->opaque_pipeline,
         render::DepthOnlyIndexedPipelineRole::opaque_static},
        {&request.directional_shadows->alpha_static_pipeline,
         render::DepthOnlyIndexedPipelineRole::stock_alpha_tested_static},
        {&request.directional_shadows->skinned_pipeline,
         render::DepthOnlyIndexedPipelineRole::skinned},
    }};
    const render::PipelineShaderFormat expected_format =
        backend == render::Backend::Vulkan
            ? render::PipelineShaderFormat::spirv
            : render::PipelineShaderFormat::dxil;
    for (const ProgramRole& entry : programs) {
        if (!entry.program->has_value()) continue;
        const render::PipelineProgram& program = **entry.program;
        const auto validation = render::validate_pipeline(
            program, request.limits.material.scene.pipeline);
        if (!validation.valid) {
            if (validation.diagnostics.empty()) {
                output_diagnostic = diagnostic(
                    "workspace_viewport_shadow_pipeline_invalid",
                    "Directional shadow pipeline validation failed");
            } else {
                output_diagnostic = {
                    "workspace_viewport_shadow_pipeline_" +
                        validation.diagnostics.front().code,
                    validation.diagnostics.front().message};
            }
            return false;
        }
        render::Diagnostic role_diagnostic;
        if (!render::validate_depth_only_indexed_pipeline_contract(
                program, entry.role, role_diagnostic)) {
            output_diagnostic = {
                "workspace_viewport_shadow_" + role_diagnostic.code,
                role_diagnostic.message};
            return false;
        }
        if (!std::all_of(
                program.shaders.begin(), program.shaders.end(),
                [&](const render::PipelineShaderModule& shader) {
                    return shader.format == expected_format;
                })) {
            output_diagnostic = diagnostic(
                "workspace_viewport_shadow_shader_format_mismatch",
                "Directional shadow shader formats must match the backend");
            return false;
        }
        if (!add_modules(program.shaders)) {
            output_diagnostic = diagnostic(
                "workspace_viewport_shadow_shader_budget",
                "Material and shadow programs exceed the shared shader byte budget");
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool finiteFrameLighting(
    const render::KsPerPixelFrameConstants& constants) noexcept {
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(),
                           [](float value) { return std::isfinite(value); });
    };
    return finite(constants.sun_direction) && finite(constants.sun_color) &&
           finite(constants.ambient_color) && finite(constants.camera_position) &&
           finite(constants.horizon_color) && finite(constants.sky_color) &&
           finite(constants.fog_color) && finite(constants.fog);
}

[[nodiscard]] bool nonzeroFrameSun(
    const render::KsPerPixelFrameConstants& constants) noexcept {
    const double length_squared =
        static_cast<double>(constants.sun_direction[0]) *
            constants.sun_direction[0] +
        static_cast<double>(constants.sun_direction[1]) *
            constants.sun_direction[1] +
        static_cast<double>(constants.sun_direction[2]) *
            constants.sun_direction[2];
    return length_squared > 1.0e-12;
}

}  // namespace

WorkspaceViewportLightingResult evaluateWorkspaceViewportLighting(
    const WorkspaceViewportLightingRequest& request) try {
    WorkspaceViewportLightingResult result;
    if (request.weather_id.size() > 128U ||
        !std::isfinite(request.sun_heading_degrees) ||
        !std::isfinite(request.sun_height_degrees) ||
        request.sun_heading_degrees < 0.0F ||
        request.sun_heading_degrees > 360.0F ||
        request.sun_height_degrees < 0.0F ||
        request.sun_height_degrees > 90.0F) {
        result.diagnostic = diagnostic(
            "workspace_viewport_lighting_input_invalid",
            "Weather ID and sun angles must stay inside their finite bounds");
        return result;
    }

    const auto& presets = render::stockWeatherPresets();
    const render::WeatherPreset* preset = &render::defaultWeatherPreset();
    if (!request.weather_id.empty()) {
        const auto found = std::find_if(
            presets.begin(), presets.end(), [&](const auto& candidate) {
                return candidate.id == request.weather_id;
            });
        if (found == presets.end()) {
            result.diagnostic = diagnostic(
                "workspace_viewport_weather_unknown",
                "Weather ID must name one of the bounded stock presets");
            return result;
        }
        preset = &*found;
    }

    const auto sun_direction = render::sunDirectionFromAngles(
        request.sun_heading_degrees, request.sun_height_degrees);
    result.evaluated = render::evaluateKsLighting(*preset, sun_direction);
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(),
                           [](float value) { return std::isfinite(value); });
    };
    if (!finite(result.evaluated.sun_direction) ||
        !finite(result.evaluated.sun_color) ||
        !finite(result.evaluated.ambient_color) ||
        !finite(result.evaluated.horizon_color) ||
        !finite(result.evaluated.sky_color) ||
        !finite(result.evaluated.fog_color) ||
        !std::isfinite(result.evaluated.fog_distance) ||
        !std::isfinite(result.evaluated.fog_blend)) {
        result.diagnostic = diagnostic(
            "workspace_viewport_lighting_result_invalid",
            "Evaluated weather lighting must contain only finite values");
        return result;
    }
    result.frame_constants.sun_direction = {
        result.evaluated.sun_direction[0], result.evaluated.sun_direction[1],
        result.evaluated.sun_direction[2], 0.0F};
    result.frame_constants.sun_color = {
        result.evaluated.sun_color[0], result.evaluated.sun_color[1],
        result.evaluated.sun_color[2], 0.0F};
    result.frame_constants.ambient_color = {
        result.evaluated.ambient_color[0], result.evaluated.ambient_color[1],
        result.evaluated.ambient_color[2], 0.0F};
    result.frame_constants.camera_position = {};
    result.frame_constants.horizon_color = {
        result.evaluated.horizon_color[0], result.evaluated.horizon_color[1],
        result.evaluated.horizon_color[2], 0.0F};
    result.frame_constants.sky_color = {
        result.evaluated.sky_color[0], result.evaluated.sky_color[1],
        result.evaluated.sky_color[2], 0.0F};
    result.frame_constants.fog_color = {
        result.evaluated.fog_color[0], result.evaluated.fog_color[1],
        result.evaluated.fog_color[2], 0.0F};
    result.frame_constants.fog = {
        result.evaluated.fog_distance, result.evaluated.fog_blend, 1.0F, 0.0F};
    result.status = WorkspaceViewportLightingStatus::ready;
    return result;
} catch (const std::bad_alloc&) {
    WorkspaceViewportLightingResult result;
    result.status = WorkspaceViewportLightingStatus::allocation_failed;
    result.diagnostic = diagnostic(
        "workspace_viewport_lighting_allocation_failed",
        "Weather lighting evaluation has insufficient memory");
    return result;
}

bool WorkspaceViewportCameraController::apply(
    const WorkspaceViewportCameraInput& input) noexcept {
    switch (input.gesture) {
    case WorkspaceViewportCameraGesture::begin_orbit:
        dragging_ = true;
        panning_ = false;
        return true;
    case WorkspaceViewportCameraGesture::begin_pan:
        dragging_ = true;
        panning_ = true;
        return true;
    case WorkspaceViewportCameraGesture::end_drag:
        dragging_ = false;
        panning_ = false;
        return true;
    case WorkspaceViewportCameraGesture::drag:
        if (!dragging_ || !finite_input_delta(input.x_delta) ||
            !finite_input_delta(input.y_delta) || !finite_vector(target) ||
            !std::isfinite(yaw) || !std::isfinite(pitch) ||
            !std::isfinite(distance) || !(distance >= 0.02F && distance <= 1.0e7F))
            return false;
        if (panning_) {
            const float scale = distance * 0.0015F;
            auto next_target = target;
            next_target[0] -= input.x_delta * scale * std::cos(yaw);
            next_target[2] += input.x_delta * scale * std::sin(yaw);
            next_target[1] += input.y_delta * scale;
            if (!finite_vector(next_target)) return false;
            target = next_target;
        } else {
            const float next_yaw = yaw - input.x_delta * 0.006F;
            const float next_pitch = std::clamp(
                pitch - input.y_delta * 0.006F, -1.5F, 1.5F);
            if (!std::isfinite(next_yaw) || !std::isfinite(next_pitch)) return false;
            yaw = next_yaw;
            pitch = next_pitch;
        }
        return true;
    case WorkspaceViewportCameraGesture::wheel:
        if (!finite_input_delta(input.y_delta) || !finite_vector(target) ||
            !std::isfinite(yaw) || !std::isfinite(pitch) ||
            !std::isfinite(distance) || !(distance >= 0.02F && distance <= 1.0e7F))
            return false;
        {
            const float next_distance = std::clamp(
                distance * std::exp(input.y_delta * 0.001F), 0.02F, 1.0e7F);
            if (!std::isfinite(next_distance)) return false;
            distance = next_distance;
        }
        return true;
    }
    return false;
}

render::CameraFrameResult WorkspaceViewportCameraController::frame(
    const float aspect, const render::CameraClipSpace clip_space) const {
    if (!finite_vector(target) || !std::isfinite(yaw) || !std::isfinite(pitch) ||
        !std::isfinite(distance) || !(distance >= 0.02F && distance <= 1.0e7F)) {
        return {std::nullopt, "workspace_viewport_camera_invalid",
                "workspace viewport camera state is outside its finite bounds"};
    }
    render::CameraFrameRequest request;
    request.eye = orbit_position(target, yaw, pitch, distance);
    request.target = target;
    request.up = {0.0F, 1.0F, 0.0F};
    request.aspect = aspect;
    request.near_plane = std::max(0.01F, distance / 10'000.0F);
    request.far_plane = std::max(100.0F, distance * 10.0F);
    request.clip_space = clip_space;
    return render::build_camera_frame(request);
}

bool WorkspaceViewportCameraController::move(
    const WorkspaceViewportCameraMove direction, const float step) noexcept {
    if (!finite_input_delta(step) || !finite_vector(target) ||
        !std::isfinite(yaw) || !std::isfinite(pitch) ||
        !std::isfinite(this->distance) ||
        !(this->distance >= 0.02F && this->distance <= 1.0e7F))
        return false;

    const float forward_x = -std::cos(pitch) * std::sin(yaw);
    const float forward_y = -std::sin(pitch);
    const float forward_z = -std::cos(pitch) * std::cos(yaw);
    const float right_x = std::cos(yaw);
    const float right_z = -std::sin(yaw);
    apex::scene::Vector3 delta{};
    switch (direction) {
    case WorkspaceViewportCameraMove::forward:
        delta = {forward_x * step, forward_y * step,
                 forward_z * step};
        break;
    case WorkspaceViewportCameraMove::backward:
        delta = {-forward_x * step, -forward_y * step,
                 -forward_z * step};
        break;
    case WorkspaceViewportCameraMove::left:
        delta = {-right_x * step, 0.0F, -right_z * step};
        break;
    case WorkspaceViewportCameraMove::right:
        delta = {right_x * step, 0.0F, right_z * step};
        break;
    case WorkspaceViewportCameraMove::up:
        delta = {0.0F, step, 0.0F};
        break;
    case WorkspaceViewportCameraMove::down:
        delta = {0.0F, -step, 0.0F};
        break;
    }
    const apex::scene::Vector3 next_target = {
        target[0] + delta[0], target[1] + delta[1], target[2] + delta[2]};
    if (!finite_vector(next_target)) return false;
    target = next_target;
    return true;
}

const char *
workspace_viewport_status_name(WorkspaceViewportStatus status) noexcept {
    switch (status) {
    case WorkspaceViewportStatus::ready:
        return "ready";
    case WorkspaceViewportStatus::invalid:
        return "invalid";
    case WorkspaceViewportStatus::unsupported:
        return "unsupported";
    case WorkspaceViewportStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unsupported";
}

const char *workspace_viewport_frame_status_name(
    WorkspaceViewportFrameStatus status) noexcept {
    switch (status) {
    case WorkspaceViewportFrameStatus::ready:
        return "ready";
    case WorkspaceViewportFrameStatus::invalid:
        return "invalid";
    case WorkspaceViewportFrameStatus::unsupported:
        return "unsupported";
    case WorkspaceViewportFrameStatus::execution_failed:
        return "execution_failed";
    }
    return "execution_failed";
}

const char *workspace_viewport_ai_spline_update_status_name(
    WorkspaceViewportAiSplineUpdateStatus status) noexcept {
    switch (status) {
    case WorkspaceViewportAiSplineUpdateStatus::ready: return "ready";
    case WorkspaceViewportAiSplineUpdateStatus::invalid: return "invalid";
    case WorkspaceViewportAiSplineUpdateStatus::unsupported:
        return "unsupported";
    case WorkspaceViewportAiSplineUpdateStatus::allocation_failed:
        return "allocation_failed";
    case WorkspaceViewportAiSplineUpdateStatus::upload_failed:
        return "upload_failed";
    }
    return "invalid";
}

WorkspaceViewport::WorkspaceViewport(
    render::Device *device, render::Backend backend,
    render::PresentationTargetDescription presentation,
    std::unique_ptr<render::Texture> color,
    std::unique_ptr<render::Texture> resolved_color,
    std::unique_ptr<render::DepthAttachment> depth,
    std::unique_ptr<render::StockSceneExecutionResult> execution,
    std::optional<render::PipelineProgram> authoring_overlay_pipeline,
    std::array<AiSplinePassResources, workspace_ai_spline_pass_count>
        ai_spline_passes,
    std::optional<WorkspaceViewportAiSplineGeneration> ai_spline_generation,
    std::unique_ptr<render::Buffer> authoring_grid_buffer, bool grid_visible,
    std::unique_ptr<render::Buffer> view_axis_buffer, bool view_axis_visible,
    std::unique_ptr<render::Buffer> selection_axis_buffer,
    std::optional<apex::scene::Matrix4> selection_axis_world,
    std::optional<render::PipelineProgram> selected_mesh_pipeline,
    std::unique_ptr<render::Buffer> selected_mesh_color_buffer,
    std::unique_ptr<render::DirectionalShadowMapResources> shadow_maps,
    std::optional<WorkspaceViewportDirectionalShadowOptions>
        directional_shadows,
    std::optional<LodCatalog> lod_catalog)
    : device_(device), backend_(backend), presentation_(presentation),
      color_(std::move(color)), resolved_color_(std::move(resolved_color)),
      depth_(std::move(depth)), execution_(std::move(execution)),
      authoring_overlay_pipeline_(std::move(authoring_overlay_pipeline)),
      ai_spline_passes_(std::move(ai_spline_passes)),
      ai_spline_generation_(ai_spline_generation),
      authoring_grid_buffer_(std::move(authoring_grid_buffer)),
      grid_visible_(grid_visible),
      view_axis_buffer_(std::move(view_axis_buffer)),
      view_axis_visible_(view_axis_visible),
      selection_axis_buffer_(std::move(selection_axis_buffer)),
      selection_axis_world_(std::move(selection_axis_world)),
      selected_mesh_pipeline_(std::move(selected_mesh_pipeline)),
      selected_mesh_color_buffer_(std::move(selected_mesh_color_buffer)),
      shadow_maps_(std::move(shadow_maps)),
      directional_shadows_(std::move(directional_shadows)),
      lod_catalog_(std::move(lod_catalog)) {}

WorkspaceViewport::~WorkspaceViewport() = default;

WorkspaceViewportAiSplineUpdateResult
WorkspaceViewport::replaceAiSplineOverlaysBorrowed(
    render::Device &device, const AiSplineUpdateRequest &request,
    std::optional<WorkspaceViewportAiSplineGenerationTransition> generation) {
    WorkspaceViewportAiSplineUpdateResult result;
    if (&device != device_ || device.info().backend != backend_) {
        result.diagnostic =
            diagnostic("workspace_viewport_ai_spline_update_device_mismatch",
                       "AI spline overlays must use the device that prepared "
                       "the viewport");
        return result;
    }
    if (ai_spline_generation_.has_value() != generation.has_value()) {
        result.diagnostic = diagnostic(
            "workspace_viewport_ai_spline_generation_required",
            "AI spline replacement must preserve controller generation "
            "tracking");
        return result;
    }
    if (generation.has_value()) {
        const bool nextModelRevision =
            generation->expected.revision !=
                std::numeric_limits<std::uint64_t>::max() &&
            generation->replacement.revision ==
                generation->expected.revision + 1U;
        const bool validModelRevision =
            generation->replacement.revision ==
                generation->expected.revision ||
            nextModelRevision;
        const bool nextPublication =
            generation->expected.publication !=
                std::numeric_limits<std::uint64_t>::max() &&
            generation->replacement.publication ==
                generation->expected.publication + 1U;
        if (!generation->expected.valid() ||
            !generation->replacement.valid() ||
            !generation->expected.sameOwner(generation->replacement) ||
            !validModelRevision || !nextPublication) {
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_generation_transition_invalid",
                "AI spline generation replacement must retain its owner and "
                "use the next publication with the current or next model "
                "revision");
            return result;
        }
        if (generation->expected != *ai_spline_generation_) {
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_generation_stale",
                "AI spline replacement expected a different visible "
                "generation");
            return result;
        }
    }

    const std::array<const WorkspaceAiSplineGeometry *,
                     workspace_ai_spline_pass_count>
        geometries = {request.primary,
                      request.interval,
                      request.left,
                      request.right,
                      request.selection,
                      request.temporaryInterpolation,
                      request.temporaryMarkers,
                      request.camber};
    constexpr std::array<WorkspaceAiSplinePassKind,
                         workspace_ai_spline_pass_count>
        kinds = {WorkspaceAiSplinePassKind::primary,
                 WorkspaceAiSplinePassKind::interval,
                 WorkspaceAiSplinePassKind::left_side,
                 WorkspaceAiSplinePassKind::right_side,
                 WorkspaceAiSplinePassKind::selection,
                 WorkspaceAiSplinePassKind::temporary_interpolation,
                 WorkspaceAiSplinePassKind::temporary_markers,
                 WorkspaceAiSplinePassKind::camber};

    for (std::size_t index = 0U; index < geometries.size(); ++index) {
        const bool dynamicPassMayBeEmpty =
            (kinds[index] == WorkspaceAiSplinePassKind::left_side ||
             kinds[index] == WorkspaceAiSplinePassKind::right_side ||
             kinds[index] == WorkspaceAiSplinePassKind::selection ||
             kinds[index] ==
                 WorkspaceAiSplinePassKind::temporary_interpolation ||
             kinds[index] == WorkspaceAiSplinePassKind::temporary_markers) &&
            geometries[index] == nullptr &&
            ai_spline_passes_[index].pipeline.has_value();
        if ((geometries[index] != nullptr &&
             !ai_spline_passes_[index].pipeline.has_value()) ||
            (geometries[index] == nullptr &&
             ai_spline_passes_[index].pipeline.has_value() &&
             !dynamicPassMayBeEmpty)) {
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_update_configuration_invalid",
                "AI spline replacement must preserve prepared pass presence");
            return result;
        }
    }
    if ((request.interval != nullptr || request.left != nullptr ||
         request.right != nullptr || request.selection != nullptr ||
         request.temporaryInterpolation != nullptr ||
         request.temporaryMarkers != nullptr || request.camber != nullptr) &&
        request.primary == nullptr) {
        result.diagnostic =
            diagnostic("workspace_viewport_ai_spline_overlay_primary_missing",
                       "AI spline overlays require the primary spline pass");
        return result;
    }

    std::size_t ai_draw_count = 0U;
    std::size_t ai_vertex_count = 0U;
    for (const WorkspaceAiSplineGeometry *geometry : geometries) {
        if (geometry == nullptr) continue;
        if (geometry->chunks.size() >
                render::max_overlay_line_draws - ai_draw_count ||
            geometry->vertices.size() >
                render::max_overlay_line_total_vertices - ai_vertex_count) {
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_limit",
                "AI spline geometry exceeds the bounded line-render limits");
            return result;
        }
        ai_draw_count += geometry->chunks.size();
        ai_vertex_count += geometry->vertices.size();
    }
    const std::size_t authoring_draw_count =
        static_cast<std::size_t>(authoring_grid_buffer_ != nullptr) +
        static_cast<std::size_t>(view_axis_buffer_ != nullptr) +
        static_cast<std::size_t>(selection_axis_buffer_ != nullptr);
    const std::size_t authoring_vertex_count =
        (authoring_grid_buffer_ != nullptr ? render::authoring_grid_vertex_count
                                           : 0U) +
        (view_axis_buffer_ != nullptr ? render::view_axis_vertex_count : 0U) +
        (selection_axis_buffer_ != nullptr ? 6U : 0U);
    if (ai_draw_count > render::max_overlay_line_draws - authoring_draw_count ||
        ai_vertex_count >
            render::max_overlay_line_total_vertices - authoring_vertex_count) {
        result.diagnostic = diagnostic(
            "workspace_viewport_overlay_budget_exceeded",
            "AI spline and authoring overlays exceed the shared render budget");
        return result;
    }

    for (std::size_t index = 0U; index < geometries.size(); ++index) {
        if (geometries[index] == nullptr) continue;
        if (!validateAiSplineGeometry(*geometries[index], kinds[index],
                                      request.primary, result.diagnostic) ||
            !validateAiSplineDepth(*ai_spline_passes_[index].pipeline,
                                   kinds[index], result.diagnostic))
            return result;
    }

    try {
        std::array<AiSplinePassResources, workspace_ai_spline_pass_count>
            candidate;
        for (std::size_t index = 0U; index < geometries.size(); ++index) {
            const WorkspaceAiSplineGeometry *geometry = geometries[index];
            candidate[index].pipeline = ai_spline_passes_[index].pipeline;
            if (geometry == nullptr) continue;
            candidate[index].chunks = geometry->chunks;
            if (geometry->vertices.empty()) continue;

            render::BufferDescription description;
            description.size_bytes =
                geometry->vertices.size() * sizeof(render::OverlayLineVertex);
            description.usage = render::BufferUsage::vertex;
            description.memory = render::BufferMemory::host_visible;
            description.mutability = render::BufferMutability::immutable;
            auto buffer = device.create_buffer(
                description, std::as_bytes(std::span(geometry->vertices)));
            if (!buffer.ok()) {
                result.status =
                    buffer.status == render::BufferStatus::unsupported
                        ? WorkspaceViewportAiSplineUpdateStatus::unsupported
                    : buffer.status == render::BufferStatus::allocation_failed
                        ? WorkspaceViewportAiSplineUpdateStatus::
                              allocation_failed
                    : buffer.status == render::BufferStatus::upload_failed
                        ? WorkspaceViewportAiSplineUpdateStatus::upload_failed
                        : WorkspaceViewportAiSplineUpdateStatus::invalid;
                result.diagnostic = std::move(buffer.diagnostic);
                return result;
            }

            std::array<render::OverlayLineDrawRequest,
                       render::max_overlay_line_draws>
                draws{};
            for (std::size_t chunk = 0U; chunk < geometry->chunks.size();
                 ++chunk) {
                draws[chunk].pipeline = &*candidate[index].pipeline;
                draws[chunk].vertex_buffer = buffer.buffer.get();
                draws[chunk].vertex_offset_bytes =
                    static_cast<std::uint64_t>(
                        geometry->chunks[chunk].first_vertex) *
                    sizeof(render::OverlayLineVertex);
                draws[chunk].vertex_count =
                    geometry->chunks[chunk].vertex_count;
            }
            render::IndexedStaticMeshBatchDescription batch;
            batch.depth_attachment = depth_.get();
            batch.overlay_draws =
                std::span<const render::OverlayLineDrawRequest>(draws).first(
                    geometry->chunks.size());
            render::Diagnostic overlay_diagnostic;
            const auto validation =
                render::validate_indexed_static_mesh_batch_description(
                    *color_, batch, overlay_diagnostic);
            if (validation != render::IndexedStaticMeshBatchStatus::ready) {
                result.status =
                    validation ==
                            render::IndexedStaticMeshBatchStatus::unsupported
                        ? WorkspaceViewportAiSplineUpdateStatus::unsupported
                        : WorkspaceViewportAiSplineUpdateStatus::invalid;
                result.diagnostic = std::move(overlay_diagnostic);
                return result;
            }
            candidate[index].buffer = std::move(buffer.buffer);
        }
        const std::size_t replacedPassCount =
            static_cast<std::size_t>(std::count_if(
                candidate.begin(), candidate.end(), [](const auto& pass) {
                    return pass.pipeline.has_value();
                }));
        ai_spline_passes_.swap(candidate);
        if (generation.has_value())
            ai_spline_generation_ = generation->replacement;
        result.replaced_pass_count = replacedPassCount;
        result.status = WorkspaceViewportAiSplineUpdateStatus::ready;
        return result;
    } catch (const std::bad_alloc &) {
        result.status =
            WorkspaceViewportAiSplineUpdateStatus::allocation_failed;
        result.diagnostic =
            diagnostic("workspace_viewport_ai_spline_update_allocation_failed",
                       "AI spline overlay replacement exceeded available "
                       "allocation capacity");
        return result;
    } catch (const std::exception &error) {
        result.diagnostic = {"workspace_viewport_ai_spline_update_failed",
                             error.what()};
        return result;
    }
}

WorkspaceViewportAiSplineUpdateResult
WorkspaceViewport::replaceAiSplineOverlays(
    render::Device &device, const WorkspaceAiSplineOverlaySet &overlays,
    std::optional<WorkspaceViewportAiSplineGenerationTransition> generation) {
    AiSplineUpdateRequest request;
    request.primary = &overlays.primary;
    request.interval =
        overlays.interval.has_value() ? &*overlays.interval : nullptr;
    request.left = overlays.left.has_value() ? &*overlays.left : nullptr;
    request.right = overlays.right.has_value() ? &*overlays.right : nullptr;
    request.selection =
        overlays.selection.has_value() ? &*overlays.selection : nullptr;
    request.temporaryInterpolation =
        overlays.temporaryInterpolation.has_value()
            ? &*overlays.temporaryInterpolation
            : nullptr;
    request.temporaryMarkers = overlays.temporaryMarkers.has_value()
                                   ? &*overlays.temporaryMarkers
                                   : nullptr;
    request.camber = overlays.camber.has_value() ? &*overlays.camber : nullptr;
    return replaceAiSplineOverlaysBorrowed(device, request, generation);
}

WorkspaceViewportFrameStatus
WorkspaceViewport::drawAndPresent(render::Device &device,
                                  render::PresentationTarget &target,
                                  const WorkspaceViewportFrameRequest &request,
                                  render::Diagnostic &output_diagnostic) {
    output_diagnostic = {};
    if (&device != device_) {
        output_diagnostic = diagnostic(
            "workspace_viewport_device_mismatch",
            "workspace viewport must use the device that prepared it");
        return WorkspaceViewportFrameStatus::invalid;
    }
    if (device.info().backend != backend_ || target.backend() != backend_) {
        output_diagnostic =
            diagnostic("workspace_viewport_backend_mismatch",
                       "workspace viewport, device, and "
                       "presentation target must use one backend");
        return WorkspaceViewportFrameStatus::invalid;
    }
    const auto &description = target.info().description;
    if (description.width != presentation_.width ||
        description.height != presentation_.height ||
        description.format != presentation_.format) {
        output_diagnostic =
            diagnostic("workspace_viewport_target_mismatch",
                       "presentation target dimensions and format "
                       "must match viewport preparation");
        return WorkspaceViewportFrameStatus::invalid;
    }
    if (request.camera.clip_space != expectedClipSpace(backend_)) {
        output_diagnostic =
            diagnostic("workspace_viewport_camera_clip_space",
                       "camera clip space does not match the prepared backend");
        return WorkspaceViewportFrameStatus::invalid;
    }
    const bool grid_visible = request.grid_visible.value_or(grid_visible_);
    if (grid_visible && (authoring_overlay_pipeline_ == std::nullopt ||
                         authoring_grid_buffer_ == nullptr)) {
        output_diagnostic =
            diagnostic("workspace_viewport_grid_unprepared",
                       "A frame cannot show a grid that was not prepared");
        return WorkspaceViewportFrameStatus::invalid;
    }
    const bool view_axis_visible =
        request.view_axis_visible.value_or(view_axis_visible_);
    if (view_axis_visible && (authoring_overlay_pipeline_ == std::nullopt ||
                              view_axis_buffer_ == nullptr)) {
        output_diagnostic =
            diagnostic("workspace_viewport_view_axis_unprepared",
                       "A frame cannot show a view axis that was not prepared");
        return WorkspaceViewportFrameStatus::invalid;
    }
    if (request.selection_axis_world.has_value() &&
        (authoring_overlay_pipeline_ == std::nullopt ||
         selection_axis_buffer_ == nullptr ||
         selection_axis_world_ == std::nullopt)) {
        output_diagnostic = diagnostic(
            "workspace_viewport_selection_axis_unprepared",
            "A frame cannot override a selection axis that was not prepared");
        return WorkspaceViewportFrameStatus::invalid;
    }

    std::span<const std::uint8_t> packet_visibility = request.packet_visibility;
    if (request.packet_visibility.empty() && lod_catalog_.has_value()) {
        auto &catalog = *lod_catalog_;
        if (!finite_vector(request.camera.position)) {
            output_diagnostic =
                diagnostic("workspace_viewport_lod_camera_invalid",
                           "Workspace LOD camera position must be finite");
            return WorkspaceViewportFrameStatus::invalid;
        }
        const double dx = static_cast<double>(request.camera.position[0]) -
                          static_cast<double>(catalog.bounds_center[0]);
        const double dy = static_cast<double>(request.camera.position[1]) -
                          static_cast<double>(catalog.bounds_center[1]);
        const double dz = static_cast<double>(request.camera.position[2]) -
                          static_cast<double>(catalog.bounds_center[2]);
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) ||
            distance > static_cast<double>(std::numeric_limits<float>::max())) {
            output_diagnostic =
                diagnostic("workspace_viewport_lod_camera_invalid",
                           "Workspace LOD camera distance is outside the "
                           "finite float range");
            return WorkspaceViewportFrameStatus::invalid;
        }
        const float effective_distance = workspace::carLodDistance(
            static_cast<float>(distance), catalog.fov_degrees,
            catalog.distance_divisor, catalog.track_camera);
        for (std::size_t index = 0U; index < catalog.file_for_packet.size();
             ++index) {
            const std::size_t file_index = catalog.file_for_packet[index];
            const auto &lod = catalog.file_lods[file_index];
            const bool workspace_visible = workspace::carLodVisible(
                lod.has_value() ? &*lod : nullptr, effective_distance,
                catalog.selected_index);
            catalog.frame_visibility[index] = workspace_visible ? 1U : 0U;
        }
        packet_visibility = catalog.frame_visibility;
    }

    render::StaticSceneFrameDescription frame;
    frame.camera = request.camera;
    frame.depth_attachment = depth_.get();
    frame.load_color = request.load_color;
    frame.clear_color = request.clear_color;
    frame.clear_depth = request.clear_depth;
    frame.depth_clear_value = request.depth_clear_value;
    frame.resolve_target = resolved_color_.get();
    frame.capture_rgba8 = false;
    frame.refreshed_packets = request.refreshed_packets;
    frame.packet_visibility = packet_visibility;
    frame.apply_skinning = request.apply_skinning;
    frame.frame_constants = request.frame_constants;
    if (request.selected_mesh_elapsed_ms.has_value() &&
        !selected_mesh_pipeline_.has_value()) {
        output_diagnostic =
            diagnostic("workspace_viewport_selected_mesh_unprepared",
                       "A selected-mesh elapsed time requires prepared "
                       "highlight resources");
        return WorkspaceViewportFrameStatus::invalid;
    }
    if (selected_mesh_pipeline_.has_value()) {
        if (selected_mesh_color_buffer_ == nullptr) {
            output_diagnostic = diagnostic(
                "workspace_viewport_selected_mesh_resource_missing",
                "The prepared selected-mesh pass has no color buffer");
            return WorkspaceViewportFrameStatus::invalid;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_count =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - selected_mesh_touch_time_)
                .count();
        const std::uint32_t elapsed =
            request.selected_mesh_elapsed_ms.has_value()
                ? *request.selected_mesh_elapsed_ms
            : elapsed_count > render::selected_mesh_fade_milliseconds
                ? render::selected_mesh_fade_milliseconds + 1U
                : static_cast<std::uint32_t>(
                      std::max<std::int64_t>(0, elapsed_count));
        const render::SelectedMeshHighlight highlight =
            render::evaluate_selected_mesh_highlight(elapsed);
        if (highlight.visible) {
            const auto color_bytes =
                std::as_bytes(std::span(&highlight.color, 1U));
            const auto updated = device.update_buffer(
                *selected_mesh_color_buffer_, 0U, color_bytes);
            if (!updated.ok()) {
                output_diagnostic = updated.diagnostic;
                switch (updated.status) {
                case render::BufferStatus::invalid_description:
                    return WorkspaceViewportFrameStatus::invalid;
                case render::BufferStatus::unsupported:
                    return WorkspaceViewportFrameStatus::unsupported;
                case render::BufferStatus::allocation_failed:
                case render::BufferStatus::upload_failed:
                    return WorkspaceViewportFrameStatus::execution_failed;
                case render::BufferStatus::ready:
                    break;
                }
                return WorkspaceViewportFrameStatus::execution_failed;
            }
            frame.selected_mesh_pipeline = &*selected_mesh_pipeline_;
            frame.selected_mesh_color_buffer =
                selected_mesh_color_buffer_.get();
        }
    }

    std::array<render::OverlayLineDrawRequest, render::max_overlay_line_draws>
        scene_finished_draws{};
    std::size_t scene_finished_draw_count = 0U;
    for (const AiSplinePassResources &pass : ai_spline_passes_) {
        if (pass.buffer != nullptr) {
            for (const WorkspaceAiSplineChunk &chunk : pass.chunks) {
                auto &draw = scene_finished_draws[scene_finished_draw_count++];
                draw.pipeline = &*pass.pipeline;
                draw.vertex_buffer = pass.buffer.get();
                draw.vertex_offset_bytes =
                    static_cast<std::uint64_t>(chunk.first_vertex) *
                    sizeof(render::OverlayLineVertex);
                draw.vertex_count = chunk.vertex_count;
                draw.matrices.world = apex::scene::identity_matrix;
                draw.matrices.view_projection = request.camera.view_projection;
            }
        }
    }
    frame.scene_finished_overlay_draws =
        std::span<const render::OverlayLineDrawRequest>(scene_finished_draws)
            .first(scene_finished_draw_count);

    std::array<render::OverlayLineDrawRequest, 1U> view_axis_draws{};
    std::size_t view_axis_draw_count = 0U;
    if (view_axis_visible) {
        auto &axis = view_axis_draws[view_axis_draw_count++];
        axis.pipeline = &*authoring_overlay_pipeline_;
        axis.vertex_buffer = view_axis_buffer_.get();
        axis.vertex_count =
            static_cast<std::uint32_t>(render::view_axis_vertex_count);
        axis.matrices.world = apex::scene::identity_matrix;
        axis.matrices.view_projection = request.camera.view_projection;
    }
    frame.view_axis_draws =
        std::span<const render::OverlayLineDrawRequest>(view_axis_draws)
            .first(view_axis_draw_count);

    std::array<render::OverlayLineDrawRequest, 2U> overlay_draws{};
    std::size_t overlay_count = 0U;
    if (grid_visible) {
        auto &grid = overlay_draws[overlay_count++];
        grid.pipeline = &*authoring_overlay_pipeline_;
        grid.vertex_buffer = authoring_grid_buffer_.get();
        grid.vertex_count =
            static_cast<std::uint32_t>(render::authoring_grid_vertex_count);
        grid.matrices.world = apex::scene::identity_matrix;
        grid.matrices.view_projection = request.camera.view_projection;
    }
    if (authoring_overlay_pipeline_.has_value() &&
        selection_axis_buffer_ != nullptr &&
        selection_axis_world_.has_value()) {
        const auto axis = render::build_selection_axis(
            request.selection_axis_world.value_or(*selection_axis_world_));
        if (!axis.ok()) {
            output_diagnostic = axis.diagnostic;
            return WorkspaceViewportFrameStatus::invalid;
        }
        const auto bytes = std::as_bytes(std::span(axis.vertices));
        const auto updated =
            device.update_buffer(*selection_axis_buffer_, 0U, bytes);
        if (!updated.ok()) {
            output_diagnostic = updated.diagnostic;
            switch (updated.status) {
            case render::BufferStatus::invalid_description:
                return WorkspaceViewportFrameStatus::invalid;
            case render::BufferStatus::unsupported:
                return WorkspaceViewportFrameStatus::unsupported;
            case render::BufferStatus::allocation_failed:
            case render::BufferStatus::upload_failed:
                return WorkspaceViewportFrameStatus::execution_failed;
            case render::BufferStatus::ready:
                break;
            }
            return WorkspaceViewportFrameStatus::execution_failed;
        }
        auto &selection = overlay_draws[overlay_count++];
        selection.pipeline = &*authoring_overlay_pipeline_;
        selection.vertex_buffer = selection_axis_buffer_.get();
        selection.vertex_count =
            static_cast<std::uint32_t>(axis.vertices.size());
        selection.matrices.world = apex::scene::identity_matrix;
        selection.matrices.view_projection = request.camera.view_projection;
    }
    frame.overlay_draws =
        std::span<const render::OverlayLineDrawRequest>(overlay_draws)
            .first(overlay_count);

    render::Diagnostic shadow_diagnostic;
    if (directional_shadows_.has_value()) {
        if (shadow_maps_ == nullptr) {
            output_diagnostic = diagnostic(
                "workspace_viewport_shadow_maps_missing",
                "Prepared directional shadows require retained map resources");
            return WorkspaceViewportFrameStatus::invalid;
        }
        if (execution_->resources->owns_frame_constants() &&
            !request.frame_constants.has_value()) {
            output_diagnostic =
                diagnostic("static_scene_frame_constants_missing",
                           "A prepared pipeline requires per-frame constants");
            return WorkspaceViewportFrameStatus::invalid;
        }
        if (request.frame_constants.has_value() &&
            !finiteFrameLighting(*request.frame_constants)) {
            output_diagnostic = diagnostic(
                "static_scene_frame_constants_non_finite",
                "Per-frame constants must contain only finite values");
            return WorkspaceViewportFrameStatus::invalid;
        }
        if (request.frame_constants.has_value() &&
            !nonzeroFrameSun(*request.frame_constants)) {
            output_diagnostic = diagnostic(
                "static_scene_frame_sun_direction_invalid",
                "Per-frame lighting requires a nonzero sun direction");
            return WorkspaceViewportFrameStatus::invalid;
        }
        auto lighting = directional_shadows_->maps.lighting;
        lighting.eye = request.camera.position;
        lighting.target = {
            request.camera.position[0] + request.camera.forward[0],
            request.camera.position[1] + request.camera.forward[1],
            request.camera.position[2] + request.camera.forward[2],
        };
        lighting.up = request.camera.up;
        lighting.fov_radians = request.camera.fov_radians;
        lighting.aspect = request.camera.aspect;
        lighting.near_plane = request.camera.near_plane;
        lighting.far_plane = request.camera.far_plane;
        if (request.frame_constants.has_value()) {
            lighting.sun_direction = {
                request.frame_constants->sun_direction[0],
                request.frame_constants->sun_direction[1],
                request.frame_constants->sun_direction[2],
            };
        }
        const auto refreshed =
            render::refresh_directional_shadow_maps(*shadow_maps_, lighting);
        if (!refreshed.ok()) {
            output_diagnostic = refreshed.diagnostic;
            return shadowFrameStatus(refreshed.status);
        }

        render::StaticSceneDirectionalShadowFrameDescription shadow_frame;
        shadow_frame.maps = shadow_maps_.get();
        shadow_frame.opaque_pipeline =
            directional_shadows_->opaque_pipeline.has_value()
                ? &*directional_shadows_->opaque_pipeline
                : nullptr;
        shadow_frame.alpha_static_pipeline =
            directional_shadows_->alpha_static_pipeline.has_value()
                ? &*directional_shadows_->alpha_static_pipeline
                : nullptr;
        shadow_frame.skinned_pipeline =
            directional_shadows_->skinned_pipeline.has_value()
                ? &*directional_shadows_->skinned_pipeline
                : nullptr;
        shadow_frame.refreshed_packets = request.refreshed_packets;
        shadow_frame.packet_visibility = packet_visibility;
        const auto shadowed =
            execution_->resources->draw_opaque_directional_shadows(
                device, shadow_frame);
        if (!shadowed.ok()) {
            output_diagnostic = shadowed.diagnostic;
            return shadowDrawStatus(shadowed.status);
        }
        shadow_diagnostic = shadowed.diagnostic;
        frame.directional_shadow_maps = shadow_maps_.get();
        frame.directional_shadow_constants_layout =
            directional_shadows_->constants_layout;
    }

    const auto drawn =
        execution_->resources->draw_and_readback(device, *color_, frame);
    if (!drawn.ok()) {
        output_diagnostic = drawn.diagnostic;
        return drawStatus(drawn.status);
    }
    render::Texture &presentation_color =
        resolved_color_ != nullptr ? *resolved_color_ : *color_;
    const auto presented = device.present_texture(target, presentation_color);
    output_diagnostic = presented.ok() && !shadow_diagnostic.code.empty()
                            ? std::move(shadow_diagnostic)
                            : presented.diagnostic;
    return frameStatus(presented.status);
}

WorkspaceViewportPrepareResult prepareWorkspaceViewport(
    render::Device& device, const WorkspaceSessionDocument& document,
    const WorkspaceViewportPrepareRequest& request) {
    WorkspaceViewportPrepareResult result;
    try {
        if (request.color_samples != 1U && request.color_samples != 4U) {
            result.status = WorkspaceViewportStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_viewport_multisample_unsupported",
                "workspace presentation supports one-sample or four-sample color rendering");
            return result;
        }
        if (request.directional_shadow_receiver !=
            request.directional_shadows.has_value()) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_directional_shadow_configuration_invalid",
                "The receiver selector and directional shadow configuration must be enabled together");
            return result;
        }
        render::Diagnostic shadow_program_diagnostic;
        if (!validateShadowPrograms(
                request, device.info().backend, shadow_program_diagnostic)) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = std::move(shadow_program_diagnostic);
            return result;
        }
        render::Diagnostic target_diagnostic;
        if (render::validate_presentation_target_description(
                request.presentation, target_diagnostic) !=
            render::PresentationTargetStatus::ready) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = std::move(target_diagnostic);
            return result;
        }
        const auto color_format = pipelineColorFormat(request.presentation.format);
        if (!color_format.has_value()) {
            result.status = WorkspaceViewportStatus::unsupported;
            result.diagnostic = diagnostic(
                "workspace_viewport_color_format_unsupported",
                "presentation format has no bounded stock-scene color contract");
            return result;
        }
        if (document.assembly.model.root.kind.empty() ||
            document.scene.snapshot.root == apex::scene::invalid_node_id) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_document_invalid",
                "workspace document has no valid model and scene roots");
            return result;
        }

        render::RenderPlanOptions render_options = request.render;
        if (render_options.workspace_kind.empty())
            render_options.workspace_kind = document.scene.snapshot.workspace_kind;
        if (render_options.bounds_radius <= 0.0F)
            render_options.bounds_radius = document.scene.snapshot.bounds_radius;

        std::vector<apex::scene::NodeId> excluded_roots(
            render_options.excluded_subtree_roots.begin(),
            render_options.excluded_subtree_roots.end());
        std::vector<apex::scene::NodeId> suppressed_roots(
            render_options.suppressed_subtree_roots.begin(),
            render_options.suppressed_subtree_roots.end());
        std::vector<apex::scene::NodeActivityOverride> activity_overrides(
            render_options.activity_overrides.begin(),
            render_options.activity_overrides.end());

        std::optional<workspace::WorkspaceLodResolution> lod_resolution;
        if (request.workspace.lod_bounds_center.has_value()) {
            workspace::WorkspaceLodResolutionRequest lod_request;
            lod_request.workspace = &document.assembly.workspace;
            lod_request.scene = &document.scene.snapshot;
            lod_request.file_root_nodes = document.sceneBinding.file_root_nodes;
            lod_request.bounds_center = *request.workspace.lod_bounds_center;
            lod_request.camera_position = render_options.camera_position;
            lod_request.selected_index = request.workspace.lod_index;
            lod_request.lod_fov_degrees = request.workspace.lod_fov_degrees;
            lod_request.lod_distance_divisor = request.workspace.lod_distance_divisor;
            lod_request.track_camera = request.workspace.lod_track_camera;
            lod_resolution = workspace::resolveWorkspaceLod(
                lod_request, request.workspace_scene_limits);
        }

        if (request.workspace.cockpit_high_visible.has_value() ||
            request.workspace.blurred_rims_visible.has_value() ||
            request.workspace.driver_cockpit ||
            !request.workspace.driver_hidden_names.empty()) {
            workspace::WorkspacePreviewResolutionRequest preview_request;
            preview_request.scene = &document.scene.snapshot;
            preview_request.cockpit_high_visible = request.workspace.cockpit_high_visible;
            preview_request.blurred_rims_visible = request.workspace.blurred_rims_visible;
            preview_request.driver_cockpit = request.workspace.driver_cockpit;
            preview_request.driver_hidden_names = request.workspace.driver_hidden_names;
            const auto preview = workspace::resolveWorkspacePreview(
                preview_request, request.workspace_scene_limits);
            activity_overrides.insert(activity_overrides.end(),
                                      preview.activity_overrides.begin(),
                                      preview.activity_overrides.end());
            suppressed_roots.insert(suppressed_roots.end(),
                                    preview.suppressed_root_nodes.begin(),
                                    preview.suppressed_root_nodes.end());
        }
        render_options.excluded_subtree_roots = excluded_roots;
        render_options.suppressed_subtree_roots = suppressed_roots;
        render_options.activity_overrides = activity_overrides;

        std::unique_ptr<render::ExternalTextureEffectiveStockSceneInput>
            effective_scene;
        const formats::Kn5File* scene_model = &document.assembly.model;
        std::span<const render::MaterialBindingOverrides> material_overrides =
            request.overrides_by_material;
        if (request.external_textures.has_value()) {
            if (request.external_textures->requests.empty()) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_external_texture_request_empty",
                    "External texture viewport preparation requires at least one request");
                return result;
            }
            auto prepared_effective = render::prepare_effective_stock_scene_input(
                document.assembly.model, request.overrides_by_material,
                request.external_textures->grants,
                request.external_textures->requests,
                request.external_textures->limits);
            if (!prepared_effective.ok()) {
                result.status = externalTextureStatus(prepared_effective.status);
                if (prepared_effective.diagnostics.empty()) {
                    result.diagnostic = diagnostic(
                        "workspace_viewport_external_texture_failed",
                        "External texture viewport preparation failed without a diagnostic");
                } else {
                    result.diagnostic = {
                        prepared_effective.diagnostics.front().code,
                        prepared_effective.diagnostics.front().message};
                }
                return result;
            }
            effective_scene = std::move(prepared_effective.input);
            scene_model = &effective_scene->model;
            material_overrides = effective_scene->overrides_by_material;
        }

        render::TextureDescription color_description;
        color_description.width = request.presentation.width;
        color_description.height = request.presentation.height;
        color_description.format = request.presentation.format;
        color_description.usage = render::TextureUsage::color_attachment |
                                   render::TextureUsage::transfer_source;
        color_description.mutability = render::TextureMutability::mutable_data;
        color_description.samples = request.color_samples;
        auto color = device.create_texture(color_description);
        if (!color.ok()) {
            result.status = color.status == render::TextureStatus::allocation_failed
                                ? WorkspaceViewportStatus::allocation_failed
                                : WorkspaceViewportStatus::unsupported;
            result.diagnostic = color.diagnostic;
            return result;
        }

        std::unique_ptr<render::Texture> resolved_color;
        if (request.color_samples == 4U) {
            render::TextureDescription resolve_description = color_description;
            resolve_description.samples = 1U;
            auto resolved = device.create_texture(resolve_description);
            if (!resolved.ok()) {
                result.status = resolved.status == render::TextureStatus::allocation_failed
                                    ? WorkspaceViewportStatus::allocation_failed
                                    : WorkspaceViewportStatus::unsupported;
                result.diagnostic = resolved.diagnostic;
                return result;
            }
            resolved_color = std::move(resolved.texture);
        }

        render::DepthAttachmentDescription depth_description;
        depth_description.width = request.presentation.width;
        depth_description.height = request.presentation.height;
        depth_description.samples = request.color_samples;
        auto depth = device.create_depth_attachment(depth_description);
        if (!depth.ok()) {
            result.status = depth.status == render::DepthAttachmentStatus::allocation_failed
                                ? WorkspaceViewportStatus::allocation_failed
                                : WorkspaceViewportStatus::unsupported;
            result.diagnostic = depth.diagnostic;
            return result;
        }

        std::unique_ptr<render::DirectionalShadowMapResources> shadow_maps;
        if (request.directional_shadows.has_value()) {
            auto prepared_maps = render::prepare_directional_shadow_maps(
                device, request.directional_shadows->maps);
            if (!prepared_maps.ok()) {
                result.status = shadowPreparationStatus(prepared_maps.status);
                result.diagnostic = std::move(prepared_maps.diagnostic);
                return result;
            }
            shadow_maps = std::move(prepared_maps.resources);
        }

        render::StockSceneExecutionRequest scene_request;
        scene_request.model = scene_model;
        scene_request.scene = &document.scene.snapshot;
        scene_request.render = render_options;
        scene_request.packets = request.packets;
        scene_request.shader_modules = request.shader_modules;
        scene_request.overrides_by_material = material_overrides;
        scene_request.evaluate_damage_preview = request.evaluate_damage_preview;
        scene_request.damage_broken_visible = request.damage_broken_visible;
        scene_request.targets.colors = {
            PipelineRenderTarget{*color_format, request.color_samples}};
        scene_request.targets.has_depth = true;
        scene_request.targets.depth = {
            PipelineRenderTargetFormat::depth32_float, request.color_samples};
        scene_request.wireframe = request.wireframe;
        scene_request.directional_shadow_receiver = request.directional_shadow_receiver;
        scene_request.texture_authority = render::StaticSceneTextureAuthority::embedded_kn5;
        scene_request.limits = request.limits;
        auto execution = std::make_unique<render::StockSceneExecutionResult>(
            render::prepare_stock_scene_execution(device, scene_request));
        if (!execution->ok()) {
            result.status = preparationStatus(execution->status);
            result.diagnostic = execution->diagnostic;
            return result;
        }

        std::optional<WorkspaceViewport::LodCatalog> lod_catalog;
        if (lod_resolution.has_value() && !render_options.isolated) {
            WorkspaceViewport::LodCatalog catalog;
            catalog.bounds_center = *request.workspace.lod_bounds_center;
            catalog.selected_index = request.workspace.lod_index;
            catalog.fov_degrees = request.workspace.lod_fov_degrees;
            catalog.distance_divisor = request.workspace.lod_distance_divisor;
            catalog.track_camera = request.workspace.lod_track_camera;
            catalog.file_lods.reserve(document.assembly.workspace.files.size());
            for (const auto& file : document.assembly.workspace.files)
                catalog.file_lods.push_back(file.lod);

            const auto& scene = document.scene.snapshot;
            constexpr std::size_t invalid_file =
                std::numeric_limits<std::size_t>::max();
            std::vector<std::size_t> file_for_root(scene.nodes.size(), invalid_file);
            for (std::size_t index = 0U;
                 index < document.sceneBinding.file_root_nodes.size(); ++index) {
                const auto root = document.sceneBinding.file_root_nodes[index];
                if (root == apex::scene::invalid_node_id ||
                    static_cast<std::size_t>(root) >= file_for_root.size() ||
                    file_for_root[static_cast<std::size_t>(root)] != invalid_file) {
                    result.status = WorkspaceViewportStatus::invalid;
                    result.diagnostic = diagnostic(
                        "workspace_viewport_lod_root_mapping_invalid",
                        "Workspace LOD root mapping is not valid and unique");
                    return result;
                }
                file_for_root[static_cast<std::size_t>(root)] = index;
            }
            const auto packets = execution->resources->prepared_packets();
            catalog.file_for_packet.reserve(packets.size());
            for (const auto& packet : packets) {
                auto node = packet.node;
                std::size_t file_index = invalid_file;
                for (std::size_t ancestry_depth = 0U;
                     ancestry_depth < scene.nodes.size(); ++ancestry_depth) {
                    if (node == apex::scene::invalid_node_id ||
                        static_cast<std::size_t>(node) >= scene.nodes.size())
                        break;
                    file_index = file_for_root[static_cast<std::size_t>(node)];
                    if (file_index != invalid_file) break;
                    node = scene.nodes[static_cast<std::size_t>(node)].parent;
                }
                if (file_index == invalid_file || file_index >= catalog.file_lods.size()) {
                    result.status = WorkspaceViewportStatus::invalid;
                    result.diagnostic = diagnostic(
                        "workspace_viewport_lod_packet_mapping_invalid",
                        "A prepared packet is not inside a workspace LOD file root");
                    return result;
                }
                catalog.file_for_packet.push_back(file_index);
            }
            catalog.frame_visibility.resize(packets.size(), 1U);
            lod_catalog = std::move(catalog);
        }

        std::optional<render::PipelineProgram> selected_mesh_pipeline;
        std::unique_ptr<render::Buffer> selected_mesh_color_buffer;
        if (request.selected_mesh_pipeline.has_value()) {
            const auto packets = execution->resources->prepared_packets();
            const render::DrawPacket* selected_packet = nullptr;
            for (const render::DrawPacket& packet : packets) {
                if (!packet.flags.selected) continue;
                if (selected_packet != nullptr) {
                    result.status = WorkspaceViewportStatus::invalid;
                    result.diagnostic = diagnostic(
                        "workspace_viewport_selected_mesh_count_invalid",
                        "A selected-mesh pipeline requires exactly one selected packet");
                    return result;
                }
                selected_packet = &packet;
            }
            if (selected_packet == nullptr) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_selected_mesh_missing",
                    "A selected-mesh pipeline requires one selected packet");
                return result;
            }
            if (selected_packet->primitive !=
                    render::DrawPrimitiveKind::static_mesh ||
                selected_packet->vertex_stride_floats != 11U) {
                result.status = WorkspaceViewportStatus::unsupported;
                result.diagnostic = diagnostic(
                    "workspace_viewport_selected_mesh_unsupported",
                    "The recovered selected-mesh pass supports only static 44-byte geometry");
                return result;
            }
            const render::PipelineProgram& pipeline =
                *request.selected_mesh_pipeline;
            const auto pipeline_validation = render::validate_pipeline(pipeline);
            const render::PipelineVertexAttribute expected_position{
                render::PipelineVertexSemantic::position,
                render::PipelineVertexAttributeFormat::float32x3, 0U, 0U};
            const bool shader_format_matches = std::all_of(
                pipeline.shaders.begin(), pipeline.shaders.end(),
                [&](const render::PipelineShaderModule& shader) {
                    return shader.format ==
                           (device.info().backend == render::Backend::Vulkan
                                ? render::PipelineShaderFormat::spirv
                                : render::PipelineShaderFormat::dxil);
                });
            const auto shader_stage_count = [&](render::PipelineShaderStage stage) {
                return std::count_if(
                    pipeline.shaders.begin(), pipeline.shaders.end(),
                    [&](const render::PipelineShaderModule& shader) {
                        return shader.stage == stage;
                    });
            };
            if (!pipeline_validation.valid || pipeline.shaders.size() != 2U ||
                shader_stage_count(render::PipelineShaderStage::vertex) != 1 ||
                shader_stage_count(render::PipelineShaderStage::fragment) != 1 ||
                !shader_format_matches ||
                pipeline.transform_contract !=
                    render::PipelineTransformContract::selected_mesh ||
                pipeline.vertex_layout.stride != 11U * sizeof(float) ||
                pipeline.vertex_layout.attributes.size() != 1U ||
                pipeline.vertex_layout.attributes[0].semantic !=
                    expected_position.semantic ||
                pipeline.vertex_layout.attributes[0].format !=
                    expected_position.format ||
                pipeline.vertex_layout.attributes[0].location != 0U ||
                pipeline.vertex_layout.attributes[0].offset != 0U ||
                pipeline.targets.colors.size() != 1U ||
                pipeline.targets.colors[0].format != *color_format ||
                pipeline.targets.colors[0].samples != request.color_samples ||
                !pipeline.targets.has_depth ||
                pipeline.targets.depth.format !=
                    render::PipelineRenderTargetFormat::depth32_float ||
                pipeline.targets.depth.samples != request.color_samples ||
                pipeline.raster.fill != render::PipelineFillMode::solid ||
                pipeline.raster.cull != render::PipelineCullMode::front ||
                pipeline.depth.test_enabled || pipeline.depth.write_enabled ||
                pipeline.blend.enabled || pipeline.blend.alpha_to_coverage ||
                !pipeline.resources.empty()) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_selected_mesh_pipeline_invalid",
                    "The selected-mesh pipeline does not match the recovered pass contract");
                return result;
            }
            const render::SelectedMeshHighlight initial =
                render::evaluate_selected_mesh_highlight(0U);
            std::array<std::byte, render::selected_mesh_color_view_bytes> bytes{};
            std::memcpy(bytes.data(), &initial.color, sizeof(initial.color));
            render::BufferDescription color_buffer_description;
            color_buffer_description.size_bytes = bytes.size();
            color_buffer_description.usage = render::BufferUsage::uniform;
            color_buffer_description.memory = render::BufferMemory::host_visible;
            color_buffer_description.mutability =
                render::BufferMutability::mutable_data;
            auto color_buffer = device.create_buffer(color_buffer_description, bytes);
            if (!color_buffer.ok()) {
                result.status =
                    color_buffer.status == render::BufferStatus::allocation_failed
                        ? WorkspaceViewportStatus::allocation_failed
                    : color_buffer.status == render::BufferStatus::unsupported
                        ? WorkspaceViewportStatus::unsupported
                        : WorkspaceViewportStatus::invalid;
                result.diagnostic = std::move(color_buffer.diagnostic);
                return result;
            }
            selected_mesh_pipeline = pipeline;
            selected_mesh_color_buffer = std::move(color_buffer.buffer);
        }

        std::optional<render::PipelineProgram> authoring_overlay_pipeline;
        std::array<WorkspaceViewport::AiSplinePassResources,
                   workspace_ai_spline_pass_count>
            ai_spline_passes;
        std::unique_ptr<render::Buffer> authoring_grid_buffer;
        std::unique_ptr<render::Buffer> view_axis_buffer;
        std::unique_ptr<render::Buffer> selection_axis_buffer;
        std::optional<apex::scene::Matrix4> selection_axis_world;
        const bool selection_axis_requested =
            request.packets.selected_node != apex::scene::invalid_node_id;
        struct AiSplinePassInput {
            const WorkspaceAiSplineGeometry *geometry = nullptr;
            const std::optional<render::PipelineProgram> *pipeline = nullptr;
            WorkspaceAiSplinePassKind kind = WorkspaceAiSplinePassKind::primary;
        };
        const std::array<AiSplinePassInput, workspace_ai_spline_pass_count>
            ai_spline_inputs = {{
                {request.ai_spline_geometry, &request.ai_spline_pipeline,
                 WorkspaceAiSplinePassKind::primary},
                {request.ai_spline_interval_geometry,
                 &request.ai_spline_interval_pipeline,
                 WorkspaceAiSplinePassKind::interval},
                {request.ai_spline_left_geometry,
                 &request.ai_spline_left_pipeline,
                 WorkspaceAiSplinePassKind::left_side},
                {request.ai_spline_right_geometry,
                 &request.ai_spline_right_pipeline,
                 WorkspaceAiSplinePassKind::right_side},
                {request.ai_spline_selection_geometry,
                 &request.ai_spline_selection_pipeline,
                 WorkspaceAiSplinePassKind::selection},
                {request.ai_spline_temporary_interpolation_geometry,
                 &request.ai_spline_temporary_interpolation_pipeline,
                 WorkspaceAiSplinePassKind::temporary_interpolation},
                {request.ai_spline_temporary_marker_geometry,
                 &request.ai_spline_temporary_marker_pipeline,
                 WorkspaceAiSplinePassKind::temporary_markers},
                {request.ai_spline_camber_geometry,
                 &request.ai_spline_camber_pipeline,
                 WorkspaceAiSplinePassKind::camber},
            }};
        for (const AiSplinePassInput &input : ai_spline_inputs) {
            const bool latent_dynamic_pass =
                (input.kind == WorkspaceAiSplinePassKind::left_side ||
                 input.kind == WorkspaceAiSplinePassKind::right_side ||
                 input.kind == WorkspaceAiSplinePassKind::selection ||
                 input.kind ==
                     WorkspaceAiSplinePassKind::temporary_interpolation ||
                 input.kind == WorkspaceAiSplinePassKind::temporary_markers) &&
                input.geometry == nullptr && input.pipeline->has_value();
            if ((input.geometry != nullptr && !input.pipeline->has_value()) ||
                (input.geometry == nullptr && input.pipeline->has_value() &&
                 !latent_dynamic_pass)) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_ai_spline_configuration_invalid",
                    "Each AI spline geometry requires a matching pipeline");
                return result;
            }
        }
        if (request.ai_spline_generation.has_value() &&
            request.ai_spline_geometry == nullptr) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_generation_without_geometry",
                "AI spline generation tracking requires a primary pass");
            return result;
        }
        if (request.ai_spline_generation.has_value() &&
            !request.ai_spline_generation->valid()) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_generation_invalid",
                "AI spline generation tracking requires a valid owner");
            return result;
        }
        if ((request.ai_spline_interval_geometry != nullptr ||
             request.ai_spline_left_geometry != nullptr ||
             request.ai_spline_right_geometry != nullptr ||
             request.ai_spline_selection_geometry != nullptr ||
             request.ai_spline_temporary_interpolation_geometry != nullptr ||
             request.ai_spline_temporary_marker_geometry != nullptr ||
             request.ai_spline_camber_geometry != nullptr) &&
            request.ai_spline_geometry == nullptr) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_ai_spline_overlay_primary_missing",
                "AI spline overlays require the primary spline pass");
            return result;
        }
        const std::size_t authoring_draw_count =
            static_cast<std::size_t>(request.grid_visible) +
            static_cast<std::size_t>(request.view_axis_visible) +
            static_cast<std::size_t>(selection_axis_requested);
        const std::size_t authoring_vertex_count =
            (request.grid_visible ? render::authoring_grid_vertex_count : 0U) +
            (request.view_axis_visible ? render::view_axis_vertex_count : 0U) +
            (selection_axis_requested ? 6U : 0U);
        std::size_t ai_draw_count = 0U;
        std::size_t ai_vertex_count = 0U;
        for (const AiSplinePassInput &input : ai_spline_inputs) {
            if (input.geometry == nullptr) continue;
            if (input.geometry->chunks.size() >
                    render::max_overlay_line_draws - ai_draw_count ||
                input.geometry->vertices.size() >
                    render::max_overlay_line_total_vertices -
                        ai_vertex_count) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_ai_spline_limit",
                    "AI spline geometry exceeds the bounded line-render "
                    "limits");
                return result;
            }
            ai_draw_count += input.geometry->chunks.size();
            ai_vertex_count += input.geometry->vertices.size();
        }
        if (ai_draw_count >
                render::max_overlay_line_draws - authoring_draw_count ||
            ai_vertex_count > render::max_overlay_line_total_vertices -
                                  authoring_vertex_count) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic =
                diagnostic("workspace_viewport_overlay_budget_exceeded",
                           "AI spline and authoring overlays exceed the shared "
                           "render budget");
            return result;
        }
        for (std::size_t pass_index = 0U; pass_index < ai_spline_inputs.size();
             ++pass_index) {
            const AiSplinePassInput &input = ai_spline_inputs[pass_index];
            if (input.geometry == nullptr) {
                if (input.pipeline->has_value()) {
                    if (!validateAiSplineDepth(**input.pipeline, input.kind,
                                               result.diagnostic)) {
                        result.status = WorkspaceViewportStatus::invalid;
                        return result;
                    }
                    ai_spline_passes[pass_index].pipeline = **input.pipeline;
                }
                continue;
            }
            const WorkspaceAiSplineGeometry &geometry = *input.geometry;
            if (!validateAiSplineGeometry(geometry, input.kind,
                                          request.ai_spline_geometry,
                                          result.diagnostic)) {
                result.status = WorkspaceViewportStatus::invalid;
                return result;
            }
            const render::PipelineProgram &pipeline = **input.pipeline;
            if (!validateAiSplineDepth(pipeline, input.kind,
                                       result.diagnostic)) {
                result.status = WorkspaceViewportStatus::invalid;
                return result;
            }
            if (!geometry.vertices.empty()) {
                render::BufferDescription description;
                description.size_bytes = geometry.vertices.size() *
                                         sizeof(render::OverlayLineVertex);
                description.usage = render::BufferUsage::vertex;
                description.memory = render::BufferMemory::host_visible;
                description.mutability = render::BufferMutability::immutable;
                auto buffer = device.create_buffer(
                    description, std::as_bytes(std::span(geometry.vertices)));
                if (!buffer.ok()) {
                    result.status =
                        buffer.status == render::BufferStatus::unsupported
                            ? WorkspaceViewportStatus::unsupported
                        : buffer.status ==
                                render::BufferStatus::allocation_failed
                            ? WorkspaceViewportStatus::allocation_failed
                            : WorkspaceViewportStatus::invalid;
                    result.diagnostic = std::move(buffer.diagnostic);
                    return result;
                }
                std::array<render::OverlayLineDrawRequest,
                           render::max_overlay_line_draws>
                    draws{};
                for (std::size_t index = 0U; index < geometry.chunks.size();
                     ++index) {
                    draws[index].pipeline = &pipeline;
                    draws[index].vertex_buffer = buffer.buffer.get();
                    draws[index].vertex_offset_bytes =
                        static_cast<std::uint64_t>(
                            geometry.chunks[index].first_vertex) *
                        sizeof(render::OverlayLineVertex);
                    draws[index].vertex_count =
                        geometry.chunks[index].vertex_count;
                    draws[index].scene_position = 0U;
                }
                render::IndexedStaticMeshBatchDescription batch;
                batch.depth_attachment = depth.attachment.get();
                batch.overlay_draws =
                    std::span<const render::OverlayLineDrawRequest>(draws)
                        .first(geometry.chunks.size());
                render::Diagnostic overlay_diagnostic;
                const auto validation =
                    render::validate_indexed_static_mesh_batch_description(
                        *color.texture, batch, overlay_diagnostic);
                if (validation != render::IndexedStaticMeshBatchStatus::ready) {
                    result.status =
                        validation == render::IndexedStaticMeshBatchStatus::
                                          unsupported
                            ? WorkspaceViewportStatus::unsupported
                            : WorkspaceViewportStatus::invalid;
                    result.diagnostic = std::move(overlay_diagnostic);
                    return result;
                }
                ai_spline_passes[pass_index].buffer = std::move(buffer.buffer);
            }
            ai_spline_passes[pass_index].pipeline = pipeline;
            ai_spline_passes[pass_index].chunks = geometry.chunks;
        }
        if (request.grid_visible &&
            !request.authoring_overlay_pipeline.has_value()) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_grid_pipeline_missing",
                "A visible authoring grid requires an overlay pipeline");
            return result;
        }
        if (request.view_axis_visible &&
            !request.authoring_overlay_pipeline.has_value()) {
            result.status = WorkspaceViewportStatus::invalid;
            result.diagnostic = diagnostic(
                "workspace_viewport_view_axis_pipeline_missing",
                "A visible view axis requires an overlay pipeline");
            return result;
        }
        if (request.authoring_overlay_pipeline.has_value()) {
            const auto selected = request.packets.selected_node;
            if (!request.grid_visible && !request.view_axis_visible &&
                !selection_axis_requested) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_selection_axis_node_invalid",
                    "An authoring-overlay pipeline requires a view axis, grid, or selected node");
                return result;
            }
            if (selection_axis_requested &&
                static_cast<std::size_t>(selected) >=
                    document.scene.snapshot.nodes.size()) {
                result.status = WorkspaceViewportStatus::invalid;
                result.diagnostic = diagnostic(
                    "workspace_viewport_selection_axis_node_invalid",
                    "An authoring-overlay pipeline received an invalid selected node");
                return result;
            }

            const auto create_overlay_buffer = [&device, &result](
                std::span<const std::byte> bytes,
                render::BufferMutability mutability,
                std::unique_ptr<render::Buffer>& output) {
                render::BufferDescription description;
                description.size_bytes = bytes.size();
                description.usage = render::BufferUsage::vertex;
                description.memory = render::BufferMemory::host_visible;
                description.mutability = mutability;
                auto buffer = device.create_buffer(description, bytes);
                if (!buffer.ok()) {
                    result.status =
                        buffer.status == render::BufferStatus::unsupported
                            ? WorkspaceViewportStatus::unsupported
                        : buffer.status == render::BufferStatus::allocation_failed
                            ? WorkspaceViewportStatus::allocation_failed
                            : WorkspaceViewportStatus::invalid;
                    result.diagnostic = std::move(buffer.diagnostic);
                    return false;
                }
                output = std::move(buffer.buffer);
                return true;
            };

            const auto grid = render::build_authoring_grid();
            if (request.grid_visible &&
                !create_overlay_buffer(
                    std::as_bytes(std::span(grid)),
                    render::BufferMutability::immutable,
                    authoring_grid_buffer))
                return result;

            const auto view_axis = render::build_view_axis();
            if (request.view_axis_visible &&
                !create_overlay_buffer(
                    std::as_bytes(std::span(view_axis)),
                    render::BufferMutability::immutable,
                    view_axis_buffer))
                return result;

            std::array<render::OverlayLineVertex, 6U> axis_vertices{};
            if (selection_axis_requested) {
                selection_axis_world = document.scene.snapshot.nodes[
                    static_cast<std::size_t>(selected)].transform;
                const auto axis =
                    render::build_selection_axis(*selection_axis_world);
                if (!axis.ok()) {
                    result.status = WorkspaceViewportStatus::invalid;
                    result.diagnostic = axis.diagnostic;
                    return result;
                }
                axis_vertices = axis.vertices;
                if (!create_overlay_buffer(
                        std::as_bytes(std::span(axis_vertices)),
                        render::BufferMutability::mutable_data,
                        selection_axis_buffer))
                    return result;
            }

            std::array<render::OverlayLineDrawRequest, 3U> overlay_draws{};
            std::size_t overlay_count = 0U;
            if (view_axis_buffer != nullptr) {
                auto& draw = overlay_draws[overlay_count++];
                draw.pipeline = &*request.authoring_overlay_pipeline;
                draw.vertex_buffer = view_axis_buffer.get();
                draw.vertex_count =
                    static_cast<std::uint32_t>(view_axis.size());
                draw.scene_position = 0U;
            }
            if (authoring_grid_buffer != nullptr) {
                auto& draw = overlay_draws[overlay_count++];
                draw.pipeline = &*request.authoring_overlay_pipeline;
                draw.vertex_buffer = authoring_grid_buffer.get();
                draw.vertex_count = static_cast<std::uint32_t>(grid.size());
            }
            if (selection_axis_buffer != nullptr) {
                auto& draw = overlay_draws[overlay_count++];
                draw.pipeline = &*request.authoring_overlay_pipeline;
                draw.vertex_buffer = selection_axis_buffer.get();
                draw.vertex_count =
                    static_cast<std::uint32_t>(axis_vertices.size());
            }
            render::IndexedStaticMeshBatchDescription overlay_batch;
            overlay_batch.depth_attachment = depth.attachment.get();
            overlay_batch.overlay_draws =
                std::span<const render::OverlayLineDrawRequest>(overlay_draws)
                    .first(overlay_count);
            render::Diagnostic overlay_diagnostic;
            const auto overlay_validation =
                render::validate_indexed_static_mesh_batch_description(
                    *color.texture, overlay_batch, overlay_diagnostic);
            if (overlay_validation !=
                render::IndexedStaticMeshBatchStatus::ready) {
                result.status =
                    overlay_validation ==
                            render::IndexedStaticMeshBatchStatus::unsupported
                        ? WorkspaceViewportStatus::unsupported
                        : WorkspaceViewportStatus::invalid;
                result.diagnostic = std::move(overlay_diagnostic);
                return result;
            }
            authoring_overlay_pipeline =
                *request.authoring_overlay_pipeline;
        }

        result.viewport = std::unique_ptr<WorkspaceViewport>(new WorkspaceViewport(
            &device, device.info().backend, request.presentation,
            std::move(color.texture),
            std::move(resolved_color), std::move(depth.attachment),
            std::move(execution),
            std::move(authoring_overlay_pipeline),
            std::move(ai_spline_passes),
            request.ai_spline_generation,
            std::move(authoring_grid_buffer), request.grid_visible,
            std::move(view_axis_buffer), request.view_axis_visible,
            std::move(selection_axis_buffer),
            std::move(selection_axis_world),
            std::move(selected_mesh_pipeline),
            std::move(selected_mesh_color_buffer),
            std::move(shadow_maps), request.directional_shadows,
            std::move(lod_catalog)));
        result.status = WorkspaceViewportStatus::ready;
        return result;
    } catch (const workspace::WorkspaceError& error) {
        result.status = WorkspaceViewportStatus::invalid;
        result.diagnostic = {error.code(), error.what()};
        return result;
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceViewportStatus::allocation_failed;
        result.diagnostic = diagnostic(
            "workspace_viewport_allocation_failed",
            "workspace viewport preparation exceeded available allocation capacity");
        return result;
    } catch (const std::exception& error) {
        result.status = WorkspaceViewportStatus::invalid;
        result.diagnostic = {"workspace_viewport_prepare_failed", error.what()};
        return result;
    }
}

}  // namespace apex::app
