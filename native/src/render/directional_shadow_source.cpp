#include "apex/render/directional_shadow_source.hpp"

#include "generated/directional_shadow_source_artifacts.hpp"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

using namespace generated;

constexpr std::string_view vertex_source_sha256 =
    "0e97d9331b5f3f84b07018c48919c88a3f53e7efede1e07830c649678f3f65c1";
constexpr std::string_view d3d12_source_sha256 =
    "c6b3ea63a4683f3f8b4d55cfacd5a397e89abb1a07113454621807e339bbc42a";
constexpr std::string_view vertex_spirv_sha256 =
    "89175a3e81e7d1d90ceabd7276d7d6e1c9a8e5500371ed70351216c2a8a36848";
constexpr std::string_view vertex_dxbc_sha256 =
    "230657fe40f61e24ed83a125c88942b92590a2e1799aff47830d00f0992d6ccf";
constexpr std::string_view evidence_document = "docs/evidence/directional-shadow-source.md";

bool valid_role(DirectionalShadowSourceRole role) noexcept {
    return role == DirectionalShadowSourceRole::opaque_static ||
           role == DirectionalShadowSourceRole::cpu_skinned;
}

bool supported_backend(Backend backend) noexcept {
    return backend == Backend::Vulkan || backend == Backend::D3D12;
}

std::vector<std::uint8_t> bytes(std::span<const std::uint8_t> source) {
    return {source.begin(), source.end()};
}

PipelineShaderModule module(Backend backend) {
    if (backend == Backend::Vulkan) {
        return {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
                bytes(directional_shadow_source_vertex_spirv),
                PipelineShaderProvenance::translated};
    }
    if (backend == Backend::D3D12) {
        return {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
                bytes(directional_shadow_source_vertex_dxbc), PipelineShaderProvenance::translated};
    }
    return {};
}

PipelineVertexLayout vertex_layout(DirectionalShadowSourceRole role) {
    const bool skinned = role == DirectionalShadowSourceRole::cpu_skinned;
    PipelineVertexLayout layout;
    layout.stride = skinned ? 76U : 44U;
    layout.attributes = {
        {PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0, PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent, PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    if (skinned) {
        layout.attributes.push_back({PipelineVertexSemantic::bone_weights,
                                     PipelineVertexAttributeFormat::float32x4, 4U, 44U});
        layout.attributes.push_back({PipelineVertexSemantic::bone_indices,
                                     PipelineVertexAttributeFormat::float32x4, 5U, 60U});
    }
    return layout;
}

bool exact_vertex_layout(const PipelineVertexLayout& actual, DirectionalShadowSourceRole role) {
    const PipelineVertexLayout expected = vertex_layout(role);
    if (actual.stride != expected.stride || actual.attributes.size() != expected.attributes.size())
        return false;
    for (std::size_t index = 0U; index < expected.attributes.size(); ++index) {
        const PipelineVertexAttribute& left = actual.attributes[index];
        const PipelineVertexAttribute& right = expected.attributes[index];
        if (left.semantic != right.semantic || left.format != right.format ||
            left.location != right.location || left.offset != right.offset)
            return false;
    }
    return true;
}

DirectionalShadowSourceEvidence source_evidence(Backend backend,
                                                DirectionalShadowSourceRole role) noexcept {
    return {backend,
            role,
            vertex_source_sha256,
            d3d12_source_sha256,
            backend == Backend::Vulkan ? vertex_spirv_sha256 : vertex_dxbc_sha256,
            evidence_document};
}

DepthOnlyIndexedPipelineRole depth_role(DirectionalShadowSourceRole role) noexcept {
    return role == DirectionalShadowSourceRole::cpu_skinned
               ? DepthOnlyIndexedPipelineRole::skinned
               : DepthOnlyIndexedPipelineRole::opaque_static;
}

PipelineProgram built_in_pipeline(Backend backend, DirectionalShadowSourceRole role) {
    PipelineProgram program;
    program.name = role == DirectionalShadowSourceRole::cpu_skinned
                       ? "directional-shadow-cpu-skinned-translated"
                       : "directional-shadow-opaque-translated";
    program.shaders.push_back(module(backend));
    program.vertex_layout = vertex_layout(role);
    program.targets.has_depth = true;
    program.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    program.raster.cull = PipelineCullMode::back;
    program.raster.front_face = PipelineFrontFace::counter_clockwise;
    program.raster.fill = PipelineFillMode::solid;
    program.depth.test_enabled = true;
    program.depth.write_enabled = true;
    program.depth.compare = PipelineCompareOperation::less;
    program.transform_contract = PipelineTransformContract::draw_matrices;
    return program;
}

} // namespace

const char* directional_shadow_source_status_name(DirectionalShadowSourceStatus status) noexcept {
    switch (status) {
    case DirectionalShadowSourceStatus::ready:
        return "ready";
    case DirectionalShadowSourceStatus::invalid_backend:
        return "invalid_backend";
    case DirectionalShadowSourceStatus::invalid_role:
        return "invalid_role";
    case DirectionalShadowSourceStatus::shader_count_mismatch:
        return "shader_count_mismatch";
    case DirectionalShadowSourceStatus::shader_stage_mismatch:
        return "shader_stage_mismatch";
    case DirectionalShadowSourceStatus::shader_format_mismatch:
        return "shader_format_mismatch";
    case DirectionalShadowSourceStatus::shader_provenance_mismatch:
        return "shader_provenance_mismatch";
    case DirectionalShadowSourceStatus::shader_identity_mismatch:
        return "shader_identity_mismatch";
    case DirectionalShadowSourceStatus::transform_contract_mismatch:
        return "transform_contract_mismatch";
    case DirectionalShadowSourceStatus::resource_contract_mismatch:
        return "resource_contract_mismatch";
    case DirectionalShadowSourceStatus::vertex_layout_mismatch:
        return "vertex_layout_mismatch";
    case DirectionalShadowSourceStatus::target_contract_mismatch:
        return "target_contract_mismatch";
    case DirectionalShadowSourceStatus::raster_contract_mismatch:
        return "raster_contract_mismatch";
    case DirectionalShadowSourceStatus::blend_contract_mismatch:
        return "blend_contract_mismatch";
    case DirectionalShadowSourceStatus::depth_contract_mismatch:
        return "depth_contract_mismatch";
    case DirectionalShadowSourceStatus::pipeline_invalid:
        return "pipeline_invalid";
    }
    return "unknown";
}

std::size_t directional_shadow_source_shader_bytes(Backend backend,
                                                   DirectionalShadowSourceRole role) noexcept {
    if (!valid_role(role))
        return 0U;
    if (backend == Backend::Vulkan)
        return directional_shadow_source_vertex_spirv_size;
    if (backend == Backend::D3D12)
        return directional_shadow_source_vertex_dxbc_size;
    return 0U;
}

DirectionalShadowSourceStatus
validate_directional_shadow_source_program(const PipelineProgram& program, Backend backend,
                                           DirectionalShadowSourceRole role) {
    if (!supported_backend(backend))
        return DirectionalShadowSourceStatus::invalid_backend;
    if (!valid_role(role))
        return DirectionalShadowSourceStatus::invalid_role;
    if (program.shaders.size() != 1U)
        return DirectionalShadowSourceStatus::shader_count_mismatch;
    const PipelineShaderModule& shader = program.shaders.front();
    if (shader.stage != PipelineShaderStage::vertex)
        return DirectionalShadowSourceStatus::shader_stage_mismatch;
    const PipelineShaderFormat expected_format =
        backend == Backend::Vulkan ? PipelineShaderFormat::spirv : PipelineShaderFormat::dxbc;
    if (shader.format != expected_format)
        return DirectionalShadowSourceStatus::shader_format_mismatch;
    if (shader.provenance != PipelineShaderProvenance::translated)
        return DirectionalShadowSourceStatus::shader_provenance_mismatch;
    if (shader.bytes != module(backend).bytes)
        return DirectionalShadowSourceStatus::shader_identity_mismatch;
    if (program.transform_contract != PipelineTransformContract::draw_matrices)
        return DirectionalShadowSourceStatus::transform_contract_mismatch;
    if (!program.resources.empty())
        return DirectionalShadowSourceStatus::resource_contract_mismatch;
    if (!exact_vertex_layout(program.vertex_layout, role))
        return DirectionalShadowSourceStatus::vertex_layout_mismatch;
    if (!program.targets.colors.empty() || !program.targets.has_depth ||
        program.targets.depth.format != PipelineRenderTargetFormat::depth32_float ||
        program.targets.depth.samples != 1U)
        return DirectionalShadowSourceStatus::target_contract_mismatch;
    if (program.raster.cull != PipelineCullMode::back ||
        program.raster.front_face != PipelineFrontFace::counter_clockwise ||
        program.raster.fill != PipelineFillMode::solid)
        return DirectionalShadowSourceStatus::raster_contract_mismatch;
    if (program.blend.enabled || program.blend.alpha_to_coverage)
        return DirectionalShadowSourceStatus::blend_contract_mismatch;
    if (!program.depth.test_enabled || !program.depth.write_enabled ||
        program.depth.compare != PipelineCompareOperation::less)
        return DirectionalShadowSourceStatus::depth_contract_mismatch;
    if (!validate_pipeline(program).valid)
        return DirectionalShadowSourceStatus::pipeline_invalid;
    Diagnostic diagnostic;
    if (!validate_depth_only_indexed_pipeline_contract(program, depth_role(role), diagnostic))
        return DirectionalShadowSourceStatus::pipeline_invalid;
    return DirectionalShadowSourceStatus::ready;
}

DirectionalShadowSourceProgramResult
create_builtin_directional_shadow_source_program(Backend backend,
                                                 DirectionalShadowSourceRole role) {
    const DirectionalShadowSourceEvidence evidence = source_evidence(backend, role);
    if (!supported_backend(backend))
        return {DirectionalShadowSourceStatus::invalid_backend, std::nullopt, evidence};
    if (!valid_role(role))
        return {DirectionalShadowSourceStatus::invalid_role, std::nullopt, evidence};
    PipelineProgram program = built_in_pipeline(backend, role);
    const DirectionalShadowSourceStatus status =
        validate_directional_shadow_source_program(program, backend, role);
    if (status != DirectionalShadowSourceStatus::ready)
        return {status, std::nullopt, evidence};
    return {status, std::move(program), evidence};
}

} // namespace apex::render
