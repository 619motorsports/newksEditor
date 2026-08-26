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
    StockKsPerPixelVariant variant) noexcept;

struct StockKsPerPixelVulkanSourceProgramResult;

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
        const noexcept {
        return validate_stock_ks_per_pixel_vulkan_source_program(
            pipeline_, variant_);
    }

private:
    friend StockKsPerPixelVulkanSourceProgramResult
    create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant variant);

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
// caller byte-span overload.
[[nodiscard]] StockKsPerPixelVulkanSourceProgramResult
create_builtin_stock_ks_per_pixel_vulkan_source_program(
    StockKsPerPixelVariant variant);

} // namespace apex::render
