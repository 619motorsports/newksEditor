#include "apex/render/stock_multimap_source.hpp"

#include "generated/stock_multimap_nm_detail_artifacts.hpp"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

using namespace generated;

constexpr std::string_view vertex_source_sha256 =
    "3f5345267a174b150471dad34b958052655327f51c4495acaf2d52d7091ed40c";
constexpr std::string_view fragment_source_sha256 =
    "e53f946111c776e93029299797675dba9a8d3ffa82c70a149634f76005279011";
constexpr std::string_view d3d12_source_sha256 =
    "366e41b6b710ff00bf92b8ccc1e341256518ab1bce7db84715f0ba15d80639f4";
constexpr std::string_view vertex_spirv_sha256 =
    "1f89e1213742d97a91a0aa6291ebc7c84d7035db6a12bf588e9c9f3a9f54b482";
constexpr std::string_view fragment_spirv_sha256 =
    "ec0b7bc1826a7f77f6218fb5c3b156499b55d1120fbf2ee18bb00cb3659d7239";
constexpr std::string_view vertex_dxbc_sha256 =
    "411515e9ee41778c5a0e79566db664346319d5baa2100cd8d27da59f1cc01dc9";
constexpr std::string_view pixel_dxbc_sha256 =
    "8ba9faec47f9a6b91af7d70941b079616cd6065c16cb6b6b2724273680788a34";
constexpr std::string_view evidence_document =
    "docs/evidence/stock-multimap-normal-detail-source.md";

bool valid_variant(StockMultiMapSourceVariant variant) noexcept {
    return variant == StockMultiMapSourceVariant::normal_detail ||
           variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail;
}

bool supported_backend(Backend backend) noexcept {
    return backend == Backend::Vulkan || backend == Backend::D3D12;
}

std::vector<std::uint8_t> bytes(std::span<const std::uint8_t> source) {
    return {source.begin(), source.end()};
}

std::vector<PipelineShaderModule> modules(Backend backend) {
    if (backend == Backend::Vulkan) {
        return {
            {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
             bytes(stock_multimap_nm_detail_vertex_spirv),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
             bytes(stock_multimap_nm_detail_fragment_spirv),
             PipelineShaderProvenance::source_equivalent},
        };
    }
    if (backend == Backend::D3D12) {
        return {
            {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
             bytes(stock_multimap_nm_detail_vertex_dxbc),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
             bytes(stock_multimap_nm_detail_pixel_dxbc),
             PipelineShaderProvenance::source_equivalent},
        };
    }
    return {};
}

PipelineVertexLayout vertex_layout() {
    PipelineVertexLayout layout;
    layout.stride = 44U;
    layout.attributes = {
        {PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0, PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent, PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    return layout;
}

std::vector<PipelineResourceBinding> resources() {
    return {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "txDiffuse"},
        {PipelineResourceKind::sampler, 0U, 1U, "txDiffuseSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "ksPerPixelMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "ksPerPixelFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "txNormal"},
        {PipelineResourceKind::sampler, 0U, 5U, "txNormalSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 6U, "txMaps"},
        {PipelineResourceKind::sampler, 0U, 7U, "txMapsSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 8U, "txDetail"},
        {PipelineResourceKind::sampler, 0U, 9U, "txDetailSampler"},
        {PipelineResourceKind::sampled_texture, 0U, 10U, "txNormalDetail"},
        {PipelineResourceKind::sampler, 0U, 11U, "txNormalDetailSampler"},
    };
}

bool exact_vertex_layout(const PipelineVertexLayout& actual) {
    const PipelineVertexLayout expected = vertex_layout();
    if (actual.stride != expected.stride || actual.attributes.size() != expected.attributes.size())
        return false;
    for (std::size_t index = 0U; index < expected.attributes.size(); ++index) {
        const PipelineVertexAttribute& lhs = actual.attributes[index];
        const PipelineVertexAttribute& rhs = expected.attributes[index];
        if (lhs.semantic != rhs.semantic || lhs.format != rhs.format ||
            lhs.location != rhs.location || lhs.offset != rhs.offset)
            return false;
    }
    return true;
}

bool exact_resources(const PipelineProgram& program) {
    const auto expected = resources();
    if (program.resources.size() != expected.size())
        return false;
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const PipelineResourceBinding& lhs = program.resources[index];
        const PipelineResourceBinding& rhs = expected[index];
        if (lhs.kind != rhs.kind || lhs.set != rhs.set || lhs.binding != rhs.binding ||
            lhs.name != rhs.name)
            return false;
    }
    return true;
}

bool supported_color_format(PipelineRenderTargetFormat format) noexcept {
    return format == PipelineRenderTargetFormat::rgba8_unorm ||
           format == PipelineRenderTargetFormat::rgba8_srgb ||
           format == PipelineRenderTargetFormat::bgra8_unorm ||
           format == PipelineRenderTargetFormat::bgra8_srgb ||
           format == PipelineRenderTargetFormat::rgba16_float;
}

bool supported_targets(const PipelineRenderTargets& targets,
                       StockMultiMapSourceVariant variant) noexcept {
    if (!valid_variant(variant) || targets.colors.size() != 1U ||
        !supported_color_format(targets.colors[0].format))
        return false;
    const std::uint32_t samples = targets.colors[0].samples;
    if (variant == StockMultiMapSourceVariant::normal_detail) {
        if (samples != 1U && samples != 4U)
            return false;
    } else if (samples != 4U) {
        return false;
    }
    return !targets.has_depth ||
           (targets.depth.format == PipelineRenderTargetFormat::depth32_float &&
            targets.depth.samples == samples);
}

PipelineRenderTargets default_targets(StockMultiMapSourceVariant variant) {
    PipelineRenderTargets targets;
    targets.colors.push_back(
        {PipelineRenderTargetFormat::rgba8_unorm,
         variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail ? 4U : 1U});
    return targets;
}

StockMultiMapSourceEvidence source_evidence(Backend backend,
                                            StockMultiMapSourceVariant variant) noexcept {
    return {backend,
            variant,
            vertex_source_sha256,
            fragment_source_sha256,
            d3d12_source_sha256,
            backend == Backend::Vulkan ? vertex_spirv_sha256 : vertex_dxbc_sha256,
            backend == Backend::Vulkan ? fragment_spirv_sha256 : pixel_dxbc_sha256,
            evidence_document};
}

PipelineProgram built_in_pipeline(Backend backend, StockMultiMapSourceVariant variant,
                                  StockMultiMapSourcePipelineState state) {
    const bool alpha_to_coverage =
        variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail;
    PipelineProgram program;
    program.name = alpha_to_coverage ? "stock-multimap-at-nm-detail-source-equivalent"
                                     : "stock-multimap-nm-detail-source-equivalent";
    program.shaders = modules(backend);
    program.vertex_layout = vertex_layout();
    program.targets = std::move(state.targets);
    program.raster = state.raster;
    program.blend = state.blend;
    program.depth = state.depth;
    program.transform_contract = PipelineTransformContract::draw_matrices;
    program.resources = resources();
    return program;
}

} // namespace

const char* stock_multimap_source_status_name(StockMultiMapSourceStatus status) noexcept {
    switch (status) {
    case StockMultiMapSourceStatus::ready:
        return "ready";
    case StockMultiMapSourceStatus::invalid_backend:
        return "invalid_backend";
    case StockMultiMapSourceStatus::invalid_variant:
        return "invalid_variant";
    case StockMultiMapSourceStatus::shader_count_mismatch:
        return "shader_count_mismatch";
    case StockMultiMapSourceStatus::shader_stage_mismatch:
        return "shader_stage_mismatch";
    case StockMultiMapSourceStatus::shader_format_mismatch:
        return "shader_format_mismatch";
    case StockMultiMapSourceStatus::shader_provenance_mismatch:
        return "shader_provenance_mismatch";
    case StockMultiMapSourceStatus::shader_identity_mismatch:
        return "shader_identity_mismatch";
    case StockMultiMapSourceStatus::transform_contract_mismatch:
        return "transform_contract_mismatch";
    case StockMultiMapSourceStatus::resource_contract_mismatch:
        return "resource_contract_mismatch";
    case StockMultiMapSourceStatus::vertex_layout_mismatch:
        return "vertex_layout_mismatch";
    case StockMultiMapSourceStatus::target_contract_mismatch:
        return "target_contract_mismatch";
    case StockMultiMapSourceStatus::variant_state_mismatch:
        return "variant_state_mismatch";
    case StockMultiMapSourceStatus::pipeline_invalid:
        return "pipeline_invalid";
    }
    return "unknown";
}

std::size_t stock_multimap_source_shader_bytes(Backend backend,
                                               StockMultiMapSourceVariant variant) noexcept {
    if (!valid_variant(variant))
        return 0U;
    if (backend == Backend::Vulkan)
        return stock_multimap_nm_detail_vertex_spirv_size +
               stock_multimap_nm_detail_fragment_spirv_size;
    if (backend == Backend::D3D12)
        return stock_multimap_nm_detail_vertex_dxbc_size + stock_multimap_nm_detail_pixel_dxbc_size;
    return 0U;
}

StockMultiMapSourceStatus
validate_stock_multimap_source_program(const PipelineProgram& program, Backend backend,
                                       StockMultiMapSourceVariant variant) {
    if (!supported_backend(backend))
        return StockMultiMapSourceStatus::invalid_backend;
    if (!valid_variant(variant))
        return StockMultiMapSourceStatus::invalid_variant;
    if (program.shaders.size() != 2U)
        return StockMultiMapSourceStatus::shader_count_mismatch;
    if (program.shaders[0].stage != PipelineShaderStage::vertex ||
        program.shaders[1].stage != PipelineShaderStage::fragment)
        return StockMultiMapSourceStatus::shader_stage_mismatch;
    const PipelineShaderFormat expected_format =
        backend == Backend::Vulkan ? PipelineShaderFormat::spirv : PipelineShaderFormat::dxbc;
    for (const PipelineShaderModule& shader : program.shaders) {
        if (shader.format != expected_format)
            return StockMultiMapSourceStatus::shader_format_mismatch;
        if (shader.provenance != PipelineShaderProvenance::source_equivalent)
            return StockMultiMapSourceStatus::shader_provenance_mismatch;
    }
    const auto expected_modules = modules(backend);
    if (program.shaders[0].bytes != expected_modules[0].bytes ||
        program.shaders[1].bytes != expected_modules[1].bytes)
        return StockMultiMapSourceStatus::shader_identity_mismatch;
    if (program.transform_contract != PipelineTransformContract::draw_matrices)
        return StockMultiMapSourceStatus::transform_contract_mismatch;
    if (!exact_resources(program))
        return StockMultiMapSourceStatus::resource_contract_mismatch;
    if (!exact_vertex_layout(program.vertex_layout))
        return StockMultiMapSourceStatus::vertex_layout_mismatch;
    if (!supported_targets(program.targets, variant))
        return StockMultiMapSourceStatus::target_contract_mismatch;
    const bool alpha_to_coverage =
        variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail;
    if (program.blend.alpha_to_coverage != alpha_to_coverage)
        return StockMultiMapSourceStatus::variant_state_mismatch;
    if (!validate_pipeline(program).valid)
        return StockMultiMapSourceStatus::pipeline_invalid;
    return StockMultiMapSourceStatus::ready;
}

StockMultiMapSourceProgramResult
create_builtin_stock_multimap_source_program(Backend backend, StockMultiMapSourceVariant variant) {
    return create_builtin_stock_multimap_source_program(backend, variant, default_targets(variant));
}

StockMultiMapSourceProgramResult
create_builtin_stock_multimap_source_program(Backend backend, StockMultiMapSourceVariant variant,
                                             PipelineRenderTargets targets) {
    StockMultiMapSourcePipelineState state;
    state.targets = std::move(targets);
    state.raster.cull = PipelineCullMode::none;
    state.blend.alpha_to_coverage =
        variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail;
    state.depth.test_enabled = false;
    state.depth.write_enabled = false;
    state.depth.compare = PipelineCompareOperation::less;
    return create_builtin_stock_multimap_source_program(backend, variant, std::move(state));
}

StockMultiMapSourceProgramResult
create_builtin_stock_multimap_source_program(Backend backend, StockMultiMapSourceVariant variant,
                                             StockMultiMapSourcePipelineState state) {
    const StockMultiMapSourceEvidence evidence = source_evidence(backend, variant);
    if (!supported_backend(backend))
        return {StockMultiMapSourceStatus::invalid_backend, std::nullopt, evidence};
    if (!valid_variant(variant))
        return {StockMultiMapSourceStatus::invalid_variant, std::nullopt, evidence};
    if (!supported_targets(state.targets, variant))
        return {StockMultiMapSourceStatus::target_contract_mismatch, std::nullopt, evidence};
    const bool alpha_to_coverage =
        variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail;
    if (state.blend.alpha_to_coverage != alpha_to_coverage)
        return {StockMultiMapSourceStatus::variant_state_mismatch, std::nullopt, evidence};
    PipelineProgram program = built_in_pipeline(backend, variant, std::move(state));
    const StockMultiMapSourceStatus status =
        validate_stock_multimap_source_program(program, backend, variant);
    if (status != StockMultiMapSourceStatus::ready)
        return {status, std::nullopt, evidence};
    return {status, std::move(program), evidence};
}

} // namespace apex::render
