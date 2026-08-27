#include "apex/render/stock_tyres_source.hpp"

#include "generated/stock_tyres_source_artifacts.hpp"

#include <span>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

using namespace generated;

constexpr std::string_view vertex_source_sha256 =
    "4383ad6d17b881bd2ec6460c11b786d44e841a5a59c1c2b3ef77cd1c8c951d1b";
constexpr std::string_view fragment_source_sha256 =
    "8446d9dc4f14178768281ba60a1aedf769254a3237e412399cd3ff7cbce2cc37";
constexpr std::string_view shadow_fragment_source_sha256 =
    "fb8d5382d3a1398611819849440d84d386d5ad3b1c844637dbab2d140ed59564";
constexpr std::string_view d3d12_source_sha256 =
    "3628d5ed8cb4bede3365de34c135fccd3a7326bef57ae25cd76efba824d43af8";
constexpr std::string_view vertex_spirv_sha256 =
    "2955d3d9a1b5a5baa93707caf97faff7d98c8c0ded0de62320ffb53c07f8a6b0";
constexpr std::string_view fragment_spirv_sha256 =
    "650a5926eef29727bcf0d5900222fd1075d3746d2bc08c6dd6b27d4ff0190824";
constexpr std::string_view shadow_fragment_spirv_sha256 =
    "ef4c054aa7b8c6d3e767d5e92cad416c8585685f5c0ac34f077dcee89a325204";
constexpr std::string_view vertex_dxbc_sha256 =
    "00103ced0eb7e676126b7dc2a294e7ee68fa6f3400ec2cbc5ed8c5cc7d4fcc65";
constexpr std::string_view fragment_dxbc_sha256 =
    "5314c980001a54fa861f663977521ee2134028dbf8121a1c965508f9a32020da";
constexpr std::string_view shadow_fragment_dxbc_sha256 =
    "be3c0ae3049a8e508da6c35e10ecd64ac9c3ce3d26a1ea73c730b337549a525b";
constexpr std::string_view installed_vertex_sha256 =
    "7938e6cc29b1a7da8575c99258a441e98929c00eb2b62a8f8ca99bdb40fa331f";
constexpr std::string_view installed_fragment_sha256 =
    "6f124f9d45e70035604894348f55f27c40ad969bfd37062f33e19194ebe3b609";
constexpr std::string_view provenance =
    "installed ksTyres DXBC disassembly; portable source-equivalent ABI";

bool valid_variant(StockTyresSourceVariant variant) noexcept {
    return variant == StockTyresSourceVariant::base ||
           variant == StockTyresSourceVariant::directional_shadow_receiver;
}

bool supported_backend(Backend backend) noexcept {
    return backend == Backend::Vulkan || backend == Backend::D3D12;
}

std::vector<std::uint8_t> bytes(std::span<const std::uint8_t> source) {
    return {source.begin(), source.end()};
}

std::vector<PipelineShaderModule> modules(Backend backend,
                                          StockTyresSourceVariant variant) {
    const bool shadow =
        variant == StockTyresSourceVariant::directional_shadow_receiver;
    if (backend == Backend::Vulkan)
        return {
            {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
             bytes(stock_tyres_vertex_spirv),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
             bytes(shadow ? std::span<const std::uint8_t>(
                                stock_tyres_shadow_fragment_spirv)
                          : std::span<const std::uint8_t>(
                                stock_tyres_fragment_spirv)),
             PipelineShaderProvenance::source_equivalent},
        };
    if (backend == Backend::D3D12)
        return {
            {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
             bytes(stock_tyres_vertex_dxbc),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
             bytes(shadow ? std::span<const std::uint8_t>(
                                stock_tyres_shadow_fragment_dxbc)
                          : std::span<const std::uint8_t>(
                                stock_tyres_fragment_dxbc)),
             PipelineShaderProvenance::source_equivalent},
        };
    return {};
}

PipelineVertexLayout vertex_layout() {
    return {44U,
            {{PipelineVertexSemantic::position,
              PipelineVertexAttributeFormat::float32x3, 0U, 0U},
             {PipelineVertexSemantic::normal,
              PipelineVertexAttributeFormat::float32x3, 1U, 12U},
             {PipelineVertexSemantic::texcoord0,
              PipelineVertexAttributeFormat::float32x2, 2U, 24U},
             {PipelineVertexSemantic::tangent,
              PipelineVertexAttributeFormat::float32x3, 3U, 32U}}};
}

std::vector<PipelineResourceBinding> resources(StockTyresSourceVariant variant) {
    std::vector<PipelineResourceBinding> result = {
        {PipelineResourceKind::sampled_texture, 0U, 0U, "txDiffuse"},
        {PipelineResourceKind::sampler, 0U, 1U, "samDiffuse"},
        {PipelineResourceKind::uniform_buffer, 0U, 2U, "TyreMaterial"},
        {PipelineResourceKind::uniform_buffer, 0U, 3U, "TyreFrame"},
        {PipelineResourceKind::sampled_texture, 0U, 4U, "txNormal"},
        {PipelineResourceKind::sampler, 0U, 5U, "samNormal"},
        {PipelineResourceKind::sampled_texture, 0U, 6U, "txDirty"},
        {PipelineResourceKind::sampler, 0U, 7U, "samDirty"},
        {PipelineResourceKind::sampled_texture, 0U, 8U, "txBlur"},
        {PipelineResourceKind::sampler, 0U, 9U, "samBlur"},
        {PipelineResourceKind::sampled_texture, 0U, 10U, "txNormalBlur"},
        {PipelineResourceKind::sampler, 0U, 11U, "samNormalBlur"},
    };
    if (variant == StockTyresSourceVariant::directional_shadow_receiver) {
        result.insert(result.end(), {
            {PipelineResourceKind::sampled_texture, 0U, 16U, "txShadow0"},
            {PipelineResourceKind::sampled_texture, 0U, 17U, "txShadow1"},
            {PipelineResourceKind::sampled_texture, 0U, 18U, "txShadow2"},
            {PipelineResourceKind::sampler, 0U, 19U, "shadowSampler"},
            {PipelineResourceKind::uniform_buffer, 0U, 20U, "shadowReceiver"},
        });
    }
    result.insert(result.end(), {
        {PipelineResourceKind::sampled_texture, 0U, 21U, "txCube"},
        {PipelineResourceKind::sampler, 0U, 22U, "txCubeSampler"},
        {PipelineResourceKind::uniform_buffer, 0U, 23U, "TyreControls"},
    });
    return result;
}

bool exact_vertex_layout(const PipelineVertexLayout& actual) {
    const PipelineVertexLayout expected = vertex_layout();
    if (actual.stride != expected.stride ||
        actual.attributes.size() != expected.attributes.size())
        return false;
    for (std::size_t index = 0U; index < expected.attributes.size(); ++index) {
        const auto& left = actual.attributes[index];
        const auto& right = expected.attributes[index];
        if (left.semantic != right.semantic || left.format != right.format ||
            left.location != right.location || left.offset != right.offset)
            return false;
    }
    return true;
}

bool exact_resources(const PipelineProgram& program,
                     StockTyresSourceVariant variant) {
    const auto expected = resources(variant);
    if (program.resources.size() != expected.size()) return false;
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const auto& left = program.resources[index];
        const auto& right = expected[index];
        if (left.kind != right.kind || left.set != right.set ||
            left.binding != right.binding || left.name != right.name)
            return false;
    }
    return true;
}

bool supported_targets(const PipelineRenderTargets& targets) noexcept {
    if (targets.colors.size() != 1U) return false;
    const auto format = targets.colors[0].format;
    const bool color = format == PipelineRenderTargetFormat::rgba8_unorm ||
                       format == PipelineRenderTargetFormat::rgba8_srgb ||
                       format == PipelineRenderTargetFormat::bgra8_unorm ||
                       format == PipelineRenderTargetFormat::bgra8_srgb ||
                       format == PipelineRenderTargetFormat::rgba16_float;
    const std::uint32_t samples = targets.colors[0].samples;
    return color && (samples == 1U || samples == 4U) &&
           (!targets.has_depth ||
            (targets.depth.format ==
                 PipelineRenderTargetFormat::depth32_float &&
             targets.depth.samples == samples));
}

StockTyresSourceEvidence evidence(Backend backend,
                                  StockTyresSourceVariant variant) {
    const bool shadow =
        variant == StockTyresSourceVariant::directional_shadow_receiver;
    return {backend, variant, vertex_source_sha256,
            shadow ? shadow_fragment_source_sha256 : fragment_source_sha256,
            d3d12_source_sha256,
            backend == Backend::Vulkan ? vertex_spirv_sha256
                                       : vertex_dxbc_sha256,
            backend == Backend::Vulkan
                ? (shadow ? shadow_fragment_spirv_sha256
                          : fragment_spirv_sha256)
                : (shadow ? shadow_fragment_dxbc_sha256
                          : fragment_dxbc_sha256),
            installed_vertex_sha256, installed_fragment_sha256, provenance};
}

PipelineProgram built_in(Backend backend, StockTyresSourceVariant variant,
                         StockTyresSourcePipelineState state) {
    PipelineProgram program;
    program.name = variant == StockTyresSourceVariant::directional_shadow_receiver
                       ? "stock-tyres-shadow-source-equivalent"
                       : "stock-tyres-source-equivalent";
    program.shaders = modules(backend, variant);
    program.vertex_layout = vertex_layout();
    program.targets = std::move(state.targets);
    program.raster = state.raster;
    program.blend = state.blend;
    program.depth = state.depth;
    program.transform_contract = PipelineTransformContract::draw_matrices;
    program.resources = resources(variant);
    return program;
}

}  // namespace

const char* stock_tyres_source_status_name(
    StockTyresSourceStatus status) noexcept {
    switch (status) {
    case StockTyresSourceStatus::ready: return "ready";
    case StockTyresSourceStatus::invalid_backend: return "invalid_backend";
    case StockTyresSourceStatus::invalid_variant: return "invalid_variant";
    case StockTyresSourceStatus::shader_count_mismatch: return "shader_count_mismatch";
    case StockTyresSourceStatus::shader_stage_mismatch: return "shader_stage_mismatch";
    case StockTyresSourceStatus::shader_format_mismatch: return "shader_format_mismatch";
    case StockTyresSourceStatus::shader_identity_mismatch: return "shader_identity_mismatch";
    case StockTyresSourceStatus::transform_contract_mismatch: return "transform_contract_mismatch";
    case StockTyresSourceStatus::resource_contract_mismatch: return "resource_contract_mismatch";
    case StockTyresSourceStatus::vertex_layout_mismatch: return "vertex_layout_mismatch";
    case StockTyresSourceStatus::target_contract_mismatch: return "target_contract_mismatch";
    case StockTyresSourceStatus::variant_state_mismatch: return "variant_state_mismatch";
    case StockTyresSourceStatus::pipeline_invalid: return "pipeline_invalid";
    }
    return "unknown";
}

std::size_t stock_tyres_source_shader_bytes(
    Backend backend, StockTyresSourceVariant variant) noexcept {
    if (!valid_variant(variant)) return 0U;
    const bool shadow =
        variant == StockTyresSourceVariant::directional_shadow_receiver;
    if (backend == Backend::Vulkan)
        return stock_tyres_vertex_spirv.size() +
               (shadow ? stock_tyres_shadow_fragment_spirv.size()
                       : stock_tyres_fragment_spirv.size());
    if (backend == Backend::D3D12)
        return stock_tyres_vertex_dxbc.size() +
               (shadow ? stock_tyres_shadow_fragment_dxbc.size()
                       : stock_tyres_fragment_dxbc.size());
    return 0U;
}

StockTyresSourceStatus validate_stock_tyres_source_program(
    const PipelineProgram& program, Backend backend,
    StockTyresSourceVariant variant) {
    if (!supported_backend(backend)) return StockTyresSourceStatus::invalid_backend;
    if (!valid_variant(variant)) return StockTyresSourceStatus::invalid_variant;
    if (program.shaders.size() != 2U)
        return StockTyresSourceStatus::shader_count_mismatch;
    if (program.shaders[0].stage != PipelineShaderStage::vertex ||
        program.shaders[1].stage != PipelineShaderStage::fragment)
        return StockTyresSourceStatus::shader_stage_mismatch;
    const auto format = backend == Backend::Vulkan
                            ? PipelineShaderFormat::spirv
                            : PipelineShaderFormat::dxbc;
    for (const auto& shader : program.shaders)
        if (shader.format != format ||
            shader.provenance != PipelineShaderProvenance::source_equivalent)
            return StockTyresSourceStatus::shader_format_mismatch;
    const auto expected = modules(backend, variant);
    if (program.shaders[0].bytes != expected[0].bytes ||
        program.shaders[1].bytes != expected[1].bytes)
        return StockTyresSourceStatus::shader_identity_mismatch;
    if (program.transform_contract != PipelineTransformContract::draw_matrices)
        return StockTyresSourceStatus::transform_contract_mismatch;
    if (!exact_resources(program, variant))
        return StockTyresSourceStatus::resource_contract_mismatch;
    if (!exact_vertex_layout(program.vertex_layout))
        return StockTyresSourceStatus::vertex_layout_mismatch;
    if (!supported_targets(program.targets))
        return StockTyresSourceStatus::target_contract_mismatch;
    if (program.blend.enabled || program.blend.alpha_to_coverage)
        return StockTyresSourceStatus::variant_state_mismatch;
    if (!validate_pipeline(program).valid)
        return StockTyresSourceStatus::pipeline_invalid;
    return StockTyresSourceStatus::ready;
}

StockTyresSourceProgramResult create_builtin_stock_tyres_source_program(
    Backend backend, StockTyresSourceVariant variant) {
    StockTyresSourcePipelineState state;
    state.targets.colors = {
        {PipelineRenderTargetFormat::rgba8_unorm, 1U}};
    state.raster.cull = PipelineCullMode::none;
    state.depth.test_enabled = false;
    state.depth.write_enabled = false;
    state.depth.compare = PipelineCompareOperation::less;
    return create_builtin_stock_tyres_source_program(backend, variant,
                                                      std::move(state));
}

StockTyresSourceProgramResult create_builtin_stock_tyres_source_program(
    Backend backend, StockTyresSourceVariant variant,
    StockTyresSourcePipelineState state) {
    const StockTyresSourceEvidence source_evidence = evidence(backend, variant);
    if (!supported_backend(backend))
        return {StockTyresSourceStatus::invalid_backend, std::nullopt,
                source_evidence};
    if (!valid_variant(variant))
        return {StockTyresSourceStatus::invalid_variant, std::nullopt,
                source_evidence};
    if (!supported_targets(state.targets))
        return {StockTyresSourceStatus::target_contract_mismatch,
                std::nullopt, source_evidence};
    if (state.blend.enabled || state.blend.alpha_to_coverage)
        return {StockTyresSourceStatus::variant_state_mismatch,
                std::nullopt, source_evidence};
    PipelineProgram program = built_in(backend, variant, std::move(state));
    const StockTyresSourceStatus status =
        validate_stock_tyres_source_program(program, backend, variant);
    if (status != StockTyresSourceStatus::ready)
        return {status, std::nullopt, source_evidence};
    return {status, std::move(program), source_evidence};
}

}  // namespace apex::render
