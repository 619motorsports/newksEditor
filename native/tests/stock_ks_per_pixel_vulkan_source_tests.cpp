#include "apex/render/stock_ks_per_pixel_vulkan.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using namespace apex::render;

static_assert(!std::is_copy_constructible_v<
              ValidatedStockKsPerPixelVulkanSourceProgram>);
static_assert(!std::is_copy_assignable_v<
              ValidatedStockKsPerPixelVulkanSourceProgram>);
static_assert(std::is_move_constructible_v<
              ValidatedStockKsPerPixelVulkanSourceProgram>);
static_assert(std::is_move_assignable_v<
              ValidatedStockKsPerPixelVulkanSourceProgram>);

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

ValidatedStockKsPerPixelVulkanSourceProgram make_program(
    StockKsPerPixelVariant variant) {
    StockKsPerPixelVulkanSourceProgramResult result =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(variant);
    require(result.ok(), "built-in Vulkan source program creation");
    return std::move(*result.program);
}

void acceptsBothImmutableBuiltIns() {
    auto base = make_program(StockKsPerPixelVariant::base);
    auto alpha = make_program(StockKsPerPixelVariant::alpha_to_coverage);
    require(base.validation_status() ==
                StockKsPerPixelVulkanSourceStatus::ready,
            "base owner remains validated");
    require(alpha.validation_status() ==
                StockKsPerPixelVulkanSourceStatus::ready,
            "AT owner remains validated");
    require(base.pipeline().shaders[0].bytes ==
                alpha.pipeline().shaders[0].bytes,
            "both variants use the recovered shared vertex source");
    require(base.pipeline().shaders[1].bytes !=
                alpha.pipeline().shaders[1].bytes,
            "base and AT retain distinct fragment behavior");
    require(base.pipeline().targets.colors[0].samples == 1U &&
                !base.pipeline().blend.alpha_to_coverage,
            "base owns single-sample non-A2C state");
    require(alpha.pipeline().targets.colors[0].samples == 4U &&
                alpha.pipeline().blend.alpha_to_coverage,
            "AT owns 4x A2C state");
    require(base.evidence().vertex_source_sha256.size() == 64U &&
                base.evidence().fragment_source_sha256.size() == 64U &&
                base.evidence().vertex_spirv_sha256.size() == 64U &&
                base.evidence().fragment_spirv_sha256.size() == 64U &&
                !base.evidence().evidence_document.empty(),
            "built-in owner exposes complete source evidence identity");
}

void rejectsRelabelingAndModuleDrift() {
    auto base = make_program(StockKsPerPixelVariant::base);
    PipelineProgram pipeline = base.pipeline();
    pipeline.shaders[0].provenance =
        PipelineShaderProvenance::native_abi_probe;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::shader_provenance_mismatch,
            "ABI probe provenance cannot enter source-equivalent path");

    pipeline = base.pipeline();
    pipeline.shaders[1].bytes.back() ^= 0x01U;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::shader_identity_mismatch,
            "altered source-equivalent SPIR-V is rejected");

    pipeline = base.pipeline();
    pipeline.shaders[0].bytes.resize(20U);
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::shader_identity_mismatch,
            "truncated source-equivalent SPIR-V is rejected");
}

void rejectsAbiAndVariantDrift() {
    auto base = make_program(StockKsPerPixelVariant::base);
    PipelineProgram pipeline = base.pipeline();
    pipeline.resources[5].set = 0U;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::resource_contract_mismatch,
            "recovered descriptor namespace drift is rejected");

    pipeline = base.pipeline();
    pipeline.vertex_layout.stride = 32U;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::vertex_layout_mismatch,
            "recovered mesh layout drift is rejected");

    pipeline = base.pipeline();
    pipeline.targets.colors[0].samples = 4U;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::target_contract_mismatch,
            "base sample-count drift is rejected");

    pipeline = base.pipeline();
    pipeline.blend.alpha_to_coverage = true;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::variant_state_mismatch,
            "base cannot be relabeled as A2C state");

    require(validate_stock_ks_per_pixel_vulkan_source_program(
                base.pipeline(),
                StockKsPerPixelVariant::alpha_to_coverage) ==
                StockKsPerPixelVulkanSourceStatus::shader_identity_mismatch,
            "base modules cannot enter the AT variant");
}

void rejectsInvalidVariant() {
    constexpr auto invalid = static_cast<StockKsPerPixelVariant>(0xffU);
    const auto result =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(invalid);
    require(!result.ok() &&
                result.status ==
                    StockKsPerPixelVulkanSourceStatus::invalid_variant &&
                !result.program.has_value(),
            "unknown variant has no built-in module owner");
}

} // namespace

int main() {
    try {
        acceptsBothImmutableBuiltIns();
        rejectsRelabelingAndModuleDrift();
        rejectsAbiAndVariantDrift();
        rejectsInvalidVariant();
        std::cout << "stock ksPerPixel Vulkan source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stock ksPerPixel Vulkan source tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
