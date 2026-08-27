#pragma once

#include "apex/render/device.hpp"
#include "apex/render/pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace apex::render {

// This is a bounded portable implementation of the production WebGL detail
// stack. It is not installed Kunos bytecode and does not claim the native
// lighting, reflection, shadow, weather, damage, or overlay branches.
enum class StockMultiMapSourceVariant : std::uint8_t {
    normal_detail,
    alpha_to_coverage_normal_detail,
};

enum class StockMultiMapSourceStatus : std::uint8_t {
    ready,
    invalid_backend,
    invalid_variant,
    shader_count_mismatch,
    shader_stage_mismatch,
    shader_format_mismatch,
    shader_provenance_mismatch,
    shader_identity_mismatch,
    transform_contract_mismatch,
    resource_contract_mismatch,
    vertex_layout_mismatch,
    target_contract_mismatch,
    variant_state_mismatch,
    pipeline_invalid,
};

[[nodiscard]] const char*
stock_multimap_source_status_name(StockMultiMapSourceStatus status) noexcept;

struct StockMultiMapSourceEvidence {
    Backend backend = Backend::Vulkan;
    StockMultiMapSourceVariant variant = StockMultiMapSourceVariant::normal_detail;
    std::string_view vertex_source_sha256;
    std::string_view fragment_source_sha256;
    std::string_view d3d12_source_sha256;
    std::string_view vertex_artifact_sha256;
    std::string_view fragment_artifact_sha256;
    std::string_view evidence_document;
};

struct StockMultiMapSourcePipelineState {
    PipelineRenderTargets targets;
    PipelineRasterState raster;
    PipelineBlendState blend;
    PipelineDepthState depth;
};

struct StockMultiMapSourceProgramResult {
    StockMultiMapSourceStatus status = StockMultiMapSourceStatus::invalid_variant;
    std::optional<PipelineProgram> program;
    StockMultiMapSourceEvidence evidence;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockMultiMapSourceStatus::ready && program.has_value();
    }
};

// Returns the exact combined artifact size used by one program. Returns zero
// for an invalid backend or variant so callers can preflight retained copies.
[[nodiscard]] std::size_t
stock_multimap_source_shader_bytes(Backend backend, StockMultiMapSourceVariant variant) noexcept;

[[nodiscard]] StockMultiMapSourceStatus
validate_stock_multimap_source_program(const PipelineProgram& program, Backend backend,
                                       StockMultiMapSourceVariant variant);

[[nodiscard]] StockMultiMapSourceProgramResult
create_builtin_stock_multimap_source_program(Backend backend, StockMultiMapSourceVariant variant);

[[nodiscard]] StockMultiMapSourceProgramResult
create_builtin_stock_multimap_source_program(Backend backend, StockMultiMapSourceVariant variant,
                                             PipelineRenderTargets targets);

[[nodiscard]] StockMultiMapSourceProgramResult
create_builtin_stock_multimap_source_program(Backend backend, StockMultiMapSourceVariant variant,
                                             StockMultiMapSourcePipelineState state);

} // namespace apex::render
