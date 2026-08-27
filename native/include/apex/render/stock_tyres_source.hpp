#pragma once

#include "apex/render/device.hpp"
#include "apex/render/pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace apex::render {

// Portable source-equivalent translation of the recovered ksTyres shader.
// It preserves the evidenced texture, normal, Fresnel, reflection-mip, and
// first-projected-cascade behavior. It does not claim bytecode identity with
// the installed DXBC or its native register ABI.
enum class StockTyresSourceVariant : std::uint8_t {
    base,
    directional_shadow_receiver,
};

enum class StockTyresSourceStatus : std::uint8_t {
    ready,
    invalid_backend,
    invalid_variant,
    shader_count_mismatch,
    shader_stage_mismatch,
    shader_format_mismatch,
    shader_identity_mismatch,
    transform_contract_mismatch,
    resource_contract_mismatch,
    vertex_layout_mismatch,
    target_contract_mismatch,
    variant_state_mismatch,
    pipeline_invalid,
};

[[nodiscard]] const char* stock_tyres_source_status_name(
    StockTyresSourceStatus status) noexcept;

struct StockTyresSourceEvidence {
    Backend backend = Backend::Vulkan;
    StockTyresSourceVariant variant = StockTyresSourceVariant::base;
    std::string_view vertex_source_sha256;
    std::string_view fragment_source_sha256;
    std::string_view d3d12_source_sha256;
    std::string_view vertex_artifact_sha256;
    std::string_view fragment_artifact_sha256;
    std::string_view installed_vertex_sha256;
    std::string_view installed_fragment_sha256;
    std::string_view provenance;
};

struct StockTyresSourcePipelineState {
    PipelineRenderTargets targets;
    PipelineRasterState raster;
    PipelineBlendState blend;
    PipelineDepthState depth;
};

struct StockTyresSourceProgramResult {
    StockTyresSourceStatus status = StockTyresSourceStatus::invalid_variant;
    std::optional<PipelineProgram> program;
    StockTyresSourceEvidence evidence;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockTyresSourceStatus::ready && program.has_value();
    }
};

[[nodiscard]] std::size_t stock_tyres_source_shader_bytes(
    Backend backend, StockTyresSourceVariant variant) noexcept;

[[nodiscard]] StockTyresSourceStatus validate_stock_tyres_source_program(
    const PipelineProgram& program, Backend backend,
    StockTyresSourceVariant variant);

[[nodiscard]] StockTyresSourceProgramResult
create_builtin_stock_tyres_source_program(Backend backend,
                                           StockTyresSourceVariant variant);

[[nodiscard]] StockTyresSourceProgramResult
create_builtin_stock_tyres_source_program(Backend backend,
                                           StockTyresSourceVariant variant,
                                           StockTyresSourcePipelineState state);

}  // namespace apex::render
