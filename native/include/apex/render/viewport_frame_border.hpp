#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace apex::render {

inline constexpr float viewport_frame_border_thickness_pixels = 2.0F;
inline constexpr std::size_t viewport_frame_border_quad_count = 4U;
inline constexpr std::size_t viewport_frame_border_vertex_count = 16U;
inline constexpr std::size_t viewport_frame_border_index_count = 24U;

struct ViewportFrameBorderVertex {
    std::array<float, 3U> position{};
    std::array<float, 4U> color{};
};

static_assert(sizeof(ViewportFrameBorderVertex) == 7U * sizeof(float));
static_assert(std::is_trivially_copyable_v<ViewportFrameBorderVertex>);

enum class ViewportFrameBorderStatus : std::uint8_t {
    ready,
    invalid_dimensions,
    invalid_backend,
    shader_count_mismatch,
    shader_stage_mismatch,
    shader_format_mismatch,
    shader_provenance_mismatch,
    shader_identity_mismatch,
    transform_contract_mismatch,
    resource_contract_mismatch,
    vertex_layout_mismatch,
    target_contract_mismatch,
    pipeline_state_mismatch,
    pipeline_invalid,
};

struct ViewportFrameBorderGeometryResult {
    ViewportFrameBorderStatus status =
        ViewportFrameBorderStatus::invalid_dimensions;
    Diagnostic diagnostic;
    std::array<ViewportFrameBorderVertex,
               viewport_frame_border_vertex_count> vertices{};
    std::array<std::uint16_t,
               viewport_frame_border_index_count> indices{};
    DrawMatrices matrices{};

    [[nodiscard]] bool ok() const noexcept {
        return status == ViewportFrameBorderStatus::ready;
    }
};

struct ViewportFrameBorderEvidence {
    Backend backend = Backend::Vulkan;
    std::string_view vertex_source_sha256;
    std::string_view fragment_source_sha256;
    std::string_view d3d12_source_sha256;
    std::string_view vertex_artifact_sha256;
    std::string_view fragment_artifact_sha256;
    std::string_view evidence_document;
};

struct ViewportFrameBorderProgramResult {
    ViewportFrameBorderStatus status =
        ViewportFrameBorderStatus::invalid_backend;
    std::optional<PipelineProgram> program;
    ViewportFrameBorderEvidence evidence;

    [[nodiscard]] bool ok() const noexcept {
        return status == ViewportFrameBorderStatus::ready &&
               program.has_value();
    }
};

[[nodiscard]] const char* viewport_frame_border_status_name(
    ViewportFrameBorderStatus status) noexcept;

// Reproduces ksGraphics::renderFrame(bool): left/top/right/bottom quad order,
// 2-pixel thickness, and active/inactive colors. Coordinates
// remain unclipped, including the native negative edge positions below 2 px.
[[nodiscard]] ViewportFrameBorderGeometryResult
build_viewport_frame_border(std::uint32_t width, std::uint32_t height,
                            bool controls_active, Backend backend) noexcept;

[[nodiscard]] std::size_t viewport_frame_border_shader_bytes(
    Backend backend) noexcept;

[[nodiscard]] ViewportFrameBorderStatus
validate_viewport_frame_border_program(const PipelineProgram& program,
                                       Backend backend) noexcept;

[[nodiscard]] ViewportFrameBorderProgramResult
create_viewport_frame_border_program(Backend backend,
                                     PipelineRenderTargets targets);

}  // namespace apex::render
