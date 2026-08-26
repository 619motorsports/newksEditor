#include "apex/render/stock_ks_per_pixel_vulkan.hpp"

#include "apex/render/stock_ks_per_pixel_vulkan_abi.hpp"

#include "generated/stock_ks_per_pixel_source_equivalent_at_fragment_spirv.hpp"
#include "generated/stock_ks_per_pixel_source_equivalent_base_fragment_spirv.hpp"
#include "generated/stock_ks_per_pixel_source_equivalent_vertex_spirv.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

constexpr std::string_view vertex_source_sha256 =
    "be94c0a01e37a32d744a1521c220a57508acec1c42c2cdcf7bd32c1fbcde84c0";
constexpr std::string_view base_fragment_source_sha256 =
    "e7f5e6e45b7a2241662e0dd1f34705f87b5c74318db4dea7893330e6464848d9";
constexpr std::string_view at_fragment_source_sha256 =
    "0c628068a21eca27ad54fb6416506484bd90664dc777ce0f475b192d2cf72a64";
constexpr std::string_view vertex_spirv_sha256 =
    "8d325332d2d8e3a38499845f108f701d23758e1d454d9e2e349aa2fdf4dacf58";
constexpr std::string_view base_fragment_spirv_sha256 =
    "c28c30946d76b1e856c2bfcb1dfe44e08d7645d9505913a5bb54b3fa300ff1b6";
constexpr std::string_view at_fragment_spirv_sha256 =
    "189c37c74539603bb0100e87afffe4bd5e31ae754ff6655692201f8e7678f989";
constexpr std::string_view evidence_document =
    "docs/evidence/stock-ks-per-pixel-native-abi.md";

template <std::size_t Size>
std::vector<std::uint8_t> spirv_bytes(const std::uint32_t (&words)[Size]) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(Size * sizeof(std::uint32_t));
    for (const std::uint32_t word : words) {
        bytes.push_back(static_cast<std::uint8_t>(word));
        bytes.push_back(static_cast<std::uint8_t>(word >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(word >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(word >> 24U));
    }
    return bytes;
}

std::vector<std::uint8_t> fragment_bytes(StockKsPerPixelVariant variant) {
    if (variant == StockKsPerPixelVariant::base)
        return spirv_bytes(
            stock_ks_per_pixel_source_equivalent_base_fragment_spirv);
    if (variant == StockKsPerPixelVariant::alpha_to_coverage)
        return spirv_bytes(
            stock_ks_per_pixel_source_equivalent_at_fragment_spirv);
    return {};
}

PipelineResourceKind pipeline_kind(StockShaderDescriptorKind kind) noexcept {
    switch (kind) {
    case StockShaderDescriptorKind::uniform_buffer:
        return PipelineResourceKind::uniform_buffer;
    case StockShaderDescriptorKind::sampled_image:
        return PipelineResourceKind::sampled_texture;
    case StockShaderDescriptorKind::sampler:
        return PipelineResourceKind::sampler;
    }
    return PipelineResourceKind::storage_buffer;
}

bool exact_vertex_layout(const PipelineVertexLayout& layout) noexcept {
    constexpr std::array<PipelineVertexAttribute, 4U> expected = {{
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0,
         PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent,
         PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    }};
    if (layout.stride != stock_ks_per_pixel_vertex_stride_bytes ||
        layout.attributes.size() != expected.size())
        return false;
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const PipelineVertexAttribute& lhs = layout.attributes[index];
        const PipelineVertexAttribute& rhs = expected[index];
        if (lhs.semantic != rhs.semantic || lhs.format != rhs.format ||
            lhs.location != rhs.location || lhs.offset != rhs.offset)
            return false;
    }
    return true;
}

bool exact_resources(const PipelineProgram& program) noexcept {
    if (program.resources.size() != stock_ks_per_pixel_backend_contract.size())
        return false;
    for (std::size_t index = 0U;
         index < stock_ks_per_pixel_backend_contract.size(); ++index) {
        const StockKsPerPixelBackendBinding& expected =
            stock_ks_per_pixel_backend_contract[index];
        const PipelineResourceBinding& actual = program.resources[index];
        if (actual.kind != pipeline_kind(expected.descriptor_kind) ||
            actual.set != expected.vulkan_set ||
            actual.binding != expected.vulkan_binding ||
            actual.name != expected.name)
            return false;
    }
    return true;
}

PipelineProgram built_in_pipeline(StockKsPerPixelVariant variant) {
    const bool alpha_to_coverage =
        variant == StockKsPerPixelVariant::alpha_to_coverage;
    PipelineProgram program;
    program.name = alpha_to_coverage
                       ? "stock-ks-per-pixel-at-vulkan-source-equivalent"
                       : "stock-ks-per-pixel-vulkan-source-equivalent";
    program.shaders = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
         spirv_bytes(stock_ks_per_pixel_source_equivalent_vertex_spirv),
         PipelineShaderProvenance::source_equivalent},
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
         fragment_bytes(variant),
         PipelineShaderProvenance::source_equivalent},
    };
    program.vertex_layout.stride = stock_ks_per_pixel_vertex_stride_bytes;
    program.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0,
         PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent,
         PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    program.targets.colors.push_back(
        {PipelineRenderTargetFormat::rgba8_unorm,
         alpha_to_coverage ? 4U : 1U});
    program.raster.cull = PipelineCullMode::none;
    program.blend.alpha_to_coverage = alpha_to_coverage;
    program.depth.test_enabled = false;
    program.depth.write_enabled = false;
    program.depth.compare = PipelineCompareOperation::less;
    program.transform_contract = PipelineTransformContract::none;
    for (const StockKsPerPixelBackendBinding& binding :
         stock_ks_per_pixel_backend_contract) {
        program.resources.push_back(
            {pipeline_kind(binding.descriptor_kind), binding.vulkan_set,
             binding.vulkan_binding, std::string(binding.name)});
    }
    return program;
}

StockKsPerPixelVulkanSourceEvidence source_evidence(
    StockKsPerPixelVariant variant) noexcept {
    return {variant,
            vertex_source_sha256,
            variant == StockKsPerPixelVariant::alpha_to_coverage
                ? at_fragment_source_sha256
                : base_fragment_source_sha256,
            vertex_spirv_sha256,
            variant == StockKsPerPixelVariant::alpha_to_coverage
                ? at_fragment_spirv_sha256
                : base_fragment_spirv_sha256,
            evidence_document};
}

} // namespace

const char* stock_ks_per_pixel_vulkan_source_status_name(
    StockKsPerPixelVulkanSourceStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelVulkanSourceStatus::ready: return "ready";
    case StockKsPerPixelVulkanSourceStatus::invalid_variant:
        return "invalid_variant";
    case StockKsPerPixelVulkanSourceStatus::shader_count_mismatch:
        return "shader_count_mismatch";
    case StockKsPerPixelVulkanSourceStatus::shader_stage_mismatch:
        return "shader_stage_mismatch";
    case StockKsPerPixelVulkanSourceStatus::shader_format_mismatch:
        return "shader_format_mismatch";
    case StockKsPerPixelVulkanSourceStatus::shader_provenance_mismatch:
        return "shader_provenance_mismatch";
    case StockKsPerPixelVulkanSourceStatus::shader_identity_mismatch:
        return "shader_identity_mismatch";
    case StockKsPerPixelVulkanSourceStatus::transform_contract_mismatch:
        return "transform_contract_mismatch";
    case StockKsPerPixelVulkanSourceStatus::resource_contract_mismatch:
        return "resource_contract_mismatch";
    case StockKsPerPixelVulkanSourceStatus::vertex_layout_mismatch:
        return "vertex_layout_mismatch";
    case StockKsPerPixelVulkanSourceStatus::target_contract_mismatch:
        return "target_contract_mismatch";
    case StockKsPerPixelVulkanSourceStatus::variant_state_mismatch:
        return "variant_state_mismatch";
    case StockKsPerPixelVulkanSourceStatus::pipeline_invalid:
        return "pipeline_invalid";
    }
    return "unknown";
}

StockKsPerPixelVulkanSourceStatus
validate_stock_ks_per_pixel_vulkan_source_program(
    const PipelineProgram& program,
    StockKsPerPixelVariant variant) noexcept {
    if (variant != StockKsPerPixelVariant::base &&
        variant != StockKsPerPixelVariant::alpha_to_coverage)
        return StockKsPerPixelVulkanSourceStatus::invalid_variant;
    if (program.shaders.size() != 2U)
        return StockKsPerPixelVulkanSourceStatus::shader_count_mismatch;
    if (program.shaders[0].stage != PipelineShaderStage::vertex ||
        program.shaders[1].stage != PipelineShaderStage::fragment)
        return StockKsPerPixelVulkanSourceStatus::shader_stage_mismatch;
    for (const PipelineShaderModule& shader : program.shaders) {
        if (shader.format != PipelineShaderFormat::spirv)
            return StockKsPerPixelVulkanSourceStatus::shader_format_mismatch;
        if (shader.provenance != PipelineShaderProvenance::source_equivalent)
            return StockKsPerPixelVulkanSourceStatus::shader_provenance_mismatch;
    }
    const std::vector<std::uint8_t> expected_vertex =
        spirv_bytes(stock_ks_per_pixel_source_equivalent_vertex_spirv);
    const std::vector<std::uint8_t> expected_fragment = fragment_bytes(variant);
    if (program.shaders[0].bytes != expected_vertex ||
        program.shaders[1].bytes != expected_fragment)
        return StockKsPerPixelVulkanSourceStatus::shader_identity_mismatch;
    if (program.transform_contract != PipelineTransformContract::none)
        return StockKsPerPixelVulkanSourceStatus::transform_contract_mismatch;
    if (!exact_resources(program))
        return StockKsPerPixelVulkanSourceStatus::resource_contract_mismatch;
    if (!exact_vertex_layout(program.vertex_layout))
        return StockKsPerPixelVulkanSourceStatus::vertex_layout_mismatch;
    const bool alpha_to_coverage =
        variant == StockKsPerPixelVariant::alpha_to_coverage;
    if (program.targets.colors.size() != 1U || program.targets.has_depth ||
        program.targets.colors[0].format !=
            PipelineRenderTargetFormat::rgba8_unorm ||
        program.targets.colors[0].samples !=
            (alpha_to_coverage ? 4U : 1U))
        return StockKsPerPixelVulkanSourceStatus::target_contract_mismatch;
    if (program.blend.alpha_to_coverage != alpha_to_coverage)
        return StockKsPerPixelVulkanSourceStatus::variant_state_mismatch;
    if (!validate_pipeline(program).valid)
        return StockKsPerPixelVulkanSourceStatus::pipeline_invalid;
    return StockKsPerPixelVulkanSourceStatus::ready;
}

StockKsPerPixelVulkanSourceProgramResult
create_builtin_stock_ks_per_pixel_vulkan_source_program(
    StockKsPerPixelVariant variant) {
    if (variant != StockKsPerPixelVariant::base &&
        variant != StockKsPerPixelVariant::alpha_to_coverage)
        return {StockKsPerPixelVulkanSourceStatus::invalid_variant,
                std::nullopt};
    PipelineProgram pipeline = built_in_pipeline(variant);
    const StockKsPerPixelVulkanSourceStatus status =
        validate_stock_ks_per_pixel_vulkan_source_program(pipeline, variant);
    if (status != StockKsPerPixelVulkanSourceStatus::ready)
        return {status, std::nullopt};
    return {status,
            ValidatedStockKsPerPixelVulkanSourceProgram(
                std::move(pipeline), variant, source_evidence(variant))};
}

} // namespace apex::render
