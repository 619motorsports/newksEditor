#pragma once

#include "apex/render/pipeline.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace apex::render {

// Identity of the checked-in GLSL source and the glslang-produced SPIR-V
// used by the Vulkan source-equivalent path. The installed DXBC remains the
// authority for the equations; these hashes do not claim bytecode identity.
struct StockKsPerPixelVulkanSourceEvidence {
    StockKsPerPixelVariant variant = StockKsPerPixelVariant::base;
    std::string_view vertex_source_sha256;
    std::string_view fragment_source_sha256;
    std::string_view vertex_spirv_sha256;
    std::string_view fragment_spirv_sha256;
    std::string_view evidence_document;
};

enum class StockKsPerPixelVulkanSourceStatus : std::uint8_t {
    ready,
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

[[nodiscard]] const char* stock_ks_per_pixel_vulkan_source_status_name(
    StockKsPerPixelVulkanSourceStatus status) noexcept;

// Validate a complete declaration against the immutable built-in modules.
// This is deliberately stricter than PipelineShaderProvenance: arbitrary
// SPIR-V cannot enter this path by carrying a source_equivalent label.
[[nodiscard]] StockKsPerPixelVulkanSourceStatus
validate_stock_ks_per_pixel_vulkan_source_program(
    const PipelineProgram& program,
    StockKsPerPixelVariant variant);

struct StockKsPerPixelVulkanSourceProgramResult;

// Pipeline state remains render-plan authority rather than shader authority.
// This value specializes an immutable source owner without permitting caller
// shader modules, resource layouts, vertex layouts, or transform contracts.
struct StockKsPerPixelVulkanSourcePipelineState {
    PipelineRenderTargets targets;
    PipelineRasterState raster;
    PipelineBlendState blend;
    PipelineDepthState depth;
};

class ValidatedStockKsPerPixelVulkanSourceProgram {
public:
    ValidatedStockKsPerPixelVulkanSourceProgram(
        ValidatedStockKsPerPixelVulkanSourceProgram&&) noexcept = default;
    ValidatedStockKsPerPixelVulkanSourceProgram& operator=(
        ValidatedStockKsPerPixelVulkanSourceProgram&&) noexcept = default;
    ValidatedStockKsPerPixelVulkanSourceProgram(
        const ValidatedStockKsPerPixelVulkanSourceProgram&) = delete;
    ValidatedStockKsPerPixelVulkanSourceProgram& operator=(
        const ValidatedStockKsPerPixelVulkanSourceProgram&) = delete;

    [[nodiscard]] StockKsPerPixelVariant variant() const noexcept {
        return variant_;
    }
    [[nodiscard]] const PipelineProgram& pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] const StockKsPerPixelVulkanSourceEvidence& evidence()
        const noexcept {
        return evidence_;
    }
    [[nodiscard]] StockKsPerPixelVulkanSourceStatus validation_status()
        const {
        return validate_stock_ks_per_pixel_vulkan_source_program(
            pipeline_, variant_);
    }

private:
    friend StockKsPerPixelVulkanSourceProgramResult
    create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant variant);
    friend StockKsPerPixelVulkanSourceProgramResult
    create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant variant, PipelineRenderTargets targets);
    friend StockKsPerPixelVulkanSourceProgramResult
    create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant variant,
        StockKsPerPixelVulkanSourcePipelineState state);

    ValidatedStockKsPerPixelVulkanSourceProgram(
        PipelineProgram pipeline, StockKsPerPixelVariant variant,
        StockKsPerPixelVulkanSourceEvidence evidence) noexcept
        : pipeline_(std::move(pipeline)), variant_(variant), evidence_(evidence) {}

    PipelineProgram pipeline_;
    StockKsPerPixelVariant variant_ = StockKsPerPixelVariant::base;
    StockKsPerPixelVulkanSourceEvidence evidence_;
};

struct StockKsPerPixelVulkanSourceProgramResult {
    StockKsPerPixelVulkanSourceStatus status =
        StockKsPerPixelVulkanSourceStatus::invalid_variant;
    std::optional<ValidatedStockKsPerPixelVulkanSourceProgram> program;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockKsPerPixelVulkanSourceStatus::ready &&
               program.has_value();
    }
};

// Selects private checked-in modules by variant. There is intentionally no
// caller byte-span overload. The compatibility overload retains the original
// RGBA8-unorm contract: base uses 1x and alpha-to-coverage uses 4x.
[[nodiscard]] StockKsPerPixelVulkanSourceProgramResult
create_builtin_stock_ks_per_pixel_vulkan_source_program(
    StockKsPerPixelVariant variant);

// Specializes the immutable owner for one production render pass. Supported
// color targets are RGBA8/BGRA8, linear or sRGB. Base accepts 1x or 4x;
// alpha-to-coverage requires 4x. An optional depth target must be D32 with the
// same sample count. Unsupported contracts return target_contract_mismatch.
[[nodiscard]] StockKsPerPixelVulkanSourceProgramResult
create_builtin_stock_ks_per_pixel_vulkan_source_program(
    StockKsPerPixelVariant variant, PipelineRenderTargets targets);

// Specializes the same immutable shader/ABI package with caller-selected
// render-plan state. Alpha-to-coverage must still match the selected shader
// variant, and the complete resulting pipeline must pass normal validation.
[[nodiscard]] StockKsPerPixelVulkanSourceProgramResult
create_builtin_stock_ks_per_pixel_vulkan_source_program(
    StockKsPerPixelVariant variant,
    StockKsPerPixelVulkanSourcePipelineState state);

} // namespace apex::render
