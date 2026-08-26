#include "apex/render/stock_ks_per_pixel_vulkan.hpp"

#include <array>
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

ValidatedStockKsPerPixelVulkanSourceProgram make_program(
    StockKsPerPixelVariant variant, PipelineRenderTargets targets) {
    StockKsPerPixelVulkanSourceProgramResult result =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(
            variant, std::move(targets));
    require(result.ok(), "target-specialized Vulkan source program creation");
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
    require(stock_ks_per_pixel_vulkan_source_shader_bytes(
                StockKsPerPixelVariant::base) ==
                base.pipeline().shaders[0].bytes.size() +
                    base.pipeline().shaders[1].bytes.size() &&
                stock_ks_per_pixel_vulkan_source_shader_bytes(
                    StockKsPerPixelVariant::alpha_to_coverage) ==
                    alpha.pipeline().shaders[0].bytes.size() +
                        alpha.pipeline().shaders[1].bytes.size(),
            "source package byte accounting matches immutable modules");
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
    pipeline.targets.colors[0].samples = 2U;
    require(validate_stock_ks_per_pixel_vulkan_source_program(
                pipeline, StockKsPerPixelVariant::base) ==
                StockKsPerPixelVulkanSourceStatus::target_contract_mismatch,
            "unsupported base sample-count drift is rejected");

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

void acceptsProductionTargetSpecializations() {
    constexpr std::array color_formats = {
        PipelineRenderTargetFormat::rgba8_unorm,
        PipelineRenderTargetFormat::rgba8_srgb,
        PipelineRenderTargetFormat::bgra8_unorm,
        PipelineRenderTargetFormat::bgra8_srgb,
    };
    for (PipelineRenderTargetFormat format : color_formats) {
        PipelineRenderTargets targets;
        targets.colors.push_back({format, 1U});
        auto base = make_program(StockKsPerPixelVariant::base,
                                 std::move(targets));
        require(base.pipeline().targets.colors[0].format == format &&
                    base.pipeline().targets.colors[0].samples == 1U,
                "base owner retains each supported single-sample format");
    }

    PipelineRenderTargets shared_msaa_targets;
    shared_msaa_targets.colors.push_back(
        {PipelineRenderTargetFormat::bgra8_srgb, 4U});
    shared_msaa_targets.has_depth = true;
    shared_msaa_targets.depth = {
        PipelineRenderTargetFormat::depth32_float, 4U};
    auto base = make_program(StockKsPerPixelVariant::base,
                             shared_msaa_targets);
    auto alpha = make_program(StockKsPerPixelVariant::alpha_to_coverage,
                              shared_msaa_targets);
    require(base.pipeline().targets.colors.size() == 1U &&
                alpha.pipeline().targets.colors.size() == 1U &&
                base.pipeline().targets.colors[0].format ==
                    alpha.pipeline().targets.colors[0].format &&
                base.pipeline().targets.colors[0].samples ==
                    alpha.pipeline().targets.colors[0].samples &&
                base.pipeline().targets.has_depth &&
                alpha.pipeline().targets.has_depth &&
                !base.pipeline().blend.alpha_to_coverage &&
                alpha.pipeline().blend.alpha_to_coverage,
            "base and AT owners share one production 4x color/depth pass");
}

void rejectsUnsupportedTargetSpecializations() {
    PipelineRenderTargets targets;
    targets.colors.push_back(
        {PipelineRenderTargetFormat::bgra8_srgb, 1U});
    auto result = create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant::alpha_to_coverage, targets);
    require(!result.ok() &&
                result.status ==
                    StockKsPerPixelVulkanSourceStatus::target_contract_mismatch,
            "AT owner rejects a single-sample target");

    targets.colors[0] = {PipelineRenderTargetFormat::rgba16_float, 4U};
    result = create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant::base, targets);
    require(!result.ok() &&
                result.status ==
                    StockKsPerPixelVulkanSourceStatus::target_contract_mismatch,
            "source owner rejects an unsupported color format");

    targets.colors[0] = {PipelineRenderTargetFormat::bgra8_unorm, 4U};
    targets.has_depth = true;
    targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    result = create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant::base, targets);
    require(!result.ok() &&
                result.status ==
                    StockKsPerPixelVulkanSourceStatus::target_contract_mismatch,
            "source owner rejects mismatched color/depth samples");

    targets.depth = {PipelineRenderTargetFormat::depth24_stencil8, 4U};
    result = create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant::base, std::move(targets));
    require(!result.ok() &&
                result.status ==
                    StockKsPerPixelVulkanSourceStatus::target_contract_mismatch,
            "source owner rejects an unsupported depth format");
}

void preservesValidatedRenderPlanState() {
    StockKsPerPixelVulkanSourcePipelineState state;
    state.targets.colors.push_back(
        {PipelineRenderTargetFormat::bgra8_srgb, 4U});
    state.targets.has_depth = true;
    state.targets.depth = {
        PipelineRenderTargetFormat::depth32_float, 4U};
    state.raster.cull = PipelineCullMode::front;
    state.raster.front_face = PipelineFrontFace::clockwise;
    state.raster.fill = PipelineFillMode::wireframe;
    state.blend.enabled = true;
    state.blend.source_color = PipelineBlendFactor::source_alpha;
    state.blend.destination_color =
        PipelineBlendFactor::one_minus_source_alpha;
    state.blend.source_alpha = PipelineBlendFactor::one;
    state.blend.destination_alpha =
        PipelineBlendFactor::one_minus_source_alpha;
    state.depth.test_enabled = true;
    state.depth.write_enabled = false;
    state.depth.compare = PipelineCompareOperation::less_or_equal;
    auto program = make_program(StockKsPerPixelVariant::base, state.targets);
    StockKsPerPixelVulkanSourceProgramResult specialized =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(
            StockKsPerPixelVariant::base, state);
    require(specialized.ok(),
            "validated render-plan state specializes a source owner");
    const PipelineProgram& pipeline = specialized.program->pipeline();
    require(pipeline.raster.cull == state.raster.cull &&
                pipeline.raster.front_face == state.raster.front_face &&
                pipeline.raster.fill == state.raster.fill &&
                pipeline.blend.enabled == state.blend.enabled &&
                pipeline.blend.source_color == state.blend.source_color &&
                pipeline.blend.destination_color ==
                    state.blend.destination_color &&
                pipeline.depth.test_enabled == state.depth.test_enabled &&
                pipeline.depth.write_enabled == state.depth.write_enabled &&
                pipeline.depth.compare == state.depth.compare,
            "immutable source owner retains raster, blend, and depth state");
    require(program.pipeline().shaders.size() == pipeline.shaders.size() &&
                program.pipeline().shaders[0].bytes ==
                    pipeline.shaders[0].bytes &&
                program.pipeline().shaders[1].bytes ==
                    pipeline.shaders[1].bytes &&
                program.pipeline().shaders[0].provenance ==
                    pipeline.shaders[0].provenance &&
                program.pipeline().shaders[1].provenance ==
                    pipeline.shaders[1].provenance,
            "render-plan specialization does not alter built-in shader modules");

    state.blend.alpha_to_coverage = true;
    specialized = create_builtin_stock_ks_per_pixel_vulkan_source_program(
        StockKsPerPixelVariant::base, std::move(state));
    require(!specialized.ok() &&
                specialized.status ==
                    StockKsPerPixelVulkanSourceStatus::variant_state_mismatch,
            "render-plan A2C state cannot disagree with the shader variant");
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
    require(stock_ks_per_pixel_vulkan_source_shader_bytes(invalid) == 0U,
            "unknown variant has no source package byte budget");
}

} // namespace

int main() {
    try {
        acceptsBothImmutableBuiltIns();
        acceptsProductionTargetSpecializations();
        preservesValidatedRenderPlanState();
        rejectsRelabelingAndModuleDrift();
        rejectsAbiAndVariantDrift();
        rejectsUnsupportedTargetSpecializations();
        rejectsInvalidVariant();
        std::cout << "stock ksPerPixel Vulkan source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stock ksPerPixel Vulkan source tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
