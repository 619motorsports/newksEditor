#pragma once

#include "apex/render/device.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace apex::render {

// This is a bounded backend translation for the portable retained-geometry
// shadow path. The skinned role consumes vertices already skinned on the CPU;
// it does not claim the recovered native ksShadowGenSKIN b13 bone-buffer ABI.
enum class DirectionalShadowSourceRole : std::uint8_t {
    opaque_static,
    alpha_tested_static,
    cpu_skinned,
};

enum class BuiltinDirectionalShadowSourceSelector : std::uint8_t {
    disabled,
    all_supported,
};

enum class DirectionalShadowSourceStatus : std::uint8_t {
    ready,
    invalid_backend,
    invalid_role,
    shader_count_mismatch,
    shader_stage_mismatch,
    shader_format_mismatch,
    shader_provenance_mismatch,
    shader_identity_mismatch,
    transform_contract_mismatch,
    resource_contract_mismatch,
    vertex_layout_mismatch,
    target_contract_mismatch,
    raster_contract_mismatch,
    blend_contract_mismatch,
    depth_contract_mismatch,
    pipeline_invalid,
};

[[nodiscard]] const char*
directional_shadow_source_status_name(DirectionalShadowSourceStatus status) noexcept;

struct DirectionalShadowSourceEvidence {
    Backend backend = Backend::Vulkan;
    DirectionalShadowSourceRole role = DirectionalShadowSourceRole::opaque_static;
    std::string_view vertex_source_sha256;
    std::string_view fragment_source_sha256;
    std::string_view d3d12_source_sha256;
    std::string_view vertex_artifact_sha256;
    std::string_view fragment_artifact_sha256;
    std::string_view evidence_document;
};

struct DirectionalShadowSourceProgramResult {
    DirectionalShadowSourceStatus status = DirectionalShadowSourceStatus::invalid_role;
    std::optional<PipelineProgram> program;
    DirectionalShadowSourceEvidence evidence;

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectionalShadowSourceStatus::ready && program.has_value();
    }
};

// Returns the total copied shader size for one role. The opaque and CPU-skinned
// roles retain independent byte vectors, even though they share one artifact.
// The alpha-tested role includes both its vertex and fragment artifacts.
[[nodiscard]] std::size_t
directional_shadow_source_shader_bytes(Backend backend, DirectionalShadowSourceRole role) noexcept;

[[nodiscard]] DirectionalShadowSourceStatus
validate_directional_shadow_source_program(const PipelineProgram& program, Backend backend,
                                           DirectionalShadowSourceRole role);

[[nodiscard]] DirectionalShadowSourceProgramResult
create_builtin_directional_shadow_source_program(Backend backend, DirectionalShadowSourceRole role);

} // namespace apex::render
