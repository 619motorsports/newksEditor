#include "apex/render/pipeline.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool has_code(const PipelineValidationResult& result, std::string_view code) {
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

PipelineVertexLayout mesh_layout() {
    return {44,
            {{PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0, 0},
             {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3, 1, 12},
             {PipelineVertexSemantic::texcoord0, PipelineVertexAttributeFormat::float32x2, 2, 24},
             {PipelineVertexSemantic::tangent, PipelineVertexAttributeFormat::float32x3, 3, 32}}};
}

PipelineShaderModule spirv(PipelineShaderStage stage) {
    return {stage, PipelineShaderFormat::spirv,
            {0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}};
}

std::vector<std::uint8_t> dxbc_container(std::uint32_t declared_size = 44U,
                                          std::uint32_t chunk_count = 1U,
                                          std::uint32_t chunk_offset = 36U) {
    std::vector<std::uint8_t> bytes(44U, 0);
    bytes[0] = 'D'; bytes[1] = 'X'; bytes[2] = 'B'; bytes[3] = 'C';
    for (std::size_t index = 0; index < 4; ++index) bytes[24U + index] = static_cast<std::uint8_t>(declared_size >> (index * 8U));
    for (std::size_t index = 0; index < 4; ++index) bytes[28U + index] = static_cast<std::uint8_t>(chunk_count >> (index * 8U));
    for (std::size_t index = 0; index < 4; ++index) bytes[32U + index] = static_cast<std::uint8_t>(chunk_offset >> (index * 8U));
    if (chunk_offset + 8U <= bytes.size()) {
        for (std::size_t index = 0; index < 4; ++index) bytes[chunk_offset + 4U + index] = 0;
    }
    return bytes;
}

PipelineProgram valid_program() {
    PipelineProgram program;
    program.name = "stock-mesh";
    program.shaders = {spirv(PipelineShaderStage::vertex), spirv(PipelineShaderStage::fragment)};
    program.vertex_layout = mesh_layout();
    program.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_srgb, 1});
    program.targets.has_depth = true;
    program.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1};
    program.resources.push_back({PipelineResourceKind::sampled_texture, 0, 0, "txDiffuse"});
    return program;
}

std::vector<std::uint8_t> stock_container() {
    // v2, mesh layout, 4-byte DXBC vertex and pixel payloads, no geometry.
    return {2, 0, 0, 0, 0, 0, 4, 0, 0, 0, 'D', 'X', 'B', 'C', 4, 0, 0, 0, 'D', 'X', 'B', 'C', 0, 0, 0, 0};
}

void acceptsBoundedBackendNeutralContract() {
    const PipelineValidationResult result = validate_pipeline(valid_program());
    require(result.valid, "valid pipeline contract accepted");
    require(!result.shader_execution_supported && result.shader_bytes == 40, "shader execution remains staged");
}

void rejectsMalformedShaderBytes() {
    PipelineProgram truncated = valid_program();
    truncated.shaders[0].bytes = {0x03, 0x02, 0x23, 0x07};
    const PipelineValidationResult spirv_result = validate_pipeline(truncated);
    require(!spirv_result.valid && has_code(spirv_result, "shader_bytecode_truncated"), "truncated SPIR-V rejected");

    PipelineProgram stock = valid_program();
    stock.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::stock_container, {2, 0}}};
    const PipelineValidationResult stock_result = validate_pipeline(stock);
    require(!stock_result.valid && has_code(stock_result, "shader_bytecode_malformed"), "truncated stock container rejected");

    PipelineProgram reserved = valid_program();
    reserved.shaders[0].bytes[16] = 1;
    const PipelineValidationResult reserved_result = validate_pipeline(reserved);
    require(!reserved_result.valid && has_code(reserved_result, "spirv_reserved_word"), "SPIR-V reserved header word rejected");

    PipelineProgram no_ids = valid_program();
    no_ids.shaders[0].bytes[12] = 0;
    const PipelineValidationResult no_ids_result = validate_pipeline(no_ids);
    require(!no_ids_result.valid && has_code(no_ids_result, "spirv_id_bound"), "zero SPIR-V ID bound rejected");

    PipelineProgram unsupported_version = valid_program();
    unsupported_version.shaders[0].bytes[6] = 2;
    const PipelineValidationResult version_result = validate_pipeline(unsupported_version);
    require(!version_result.valid && has_code(version_result, "spirv_version"), "unsupported SPIR-V version rejected");

    PipelineProgram bounded_ids = valid_program();
    PipelineLimits id_limits;
    id_limits.max_spirv_id_bound = 0;
    const PipelineValidationResult id_limit_result = validate_pipeline(bounded_ids, id_limits);
    require(!id_limit_result.valid && has_code(id_limit_result, "spirv_id_bound_limit"), "SPIR-V ID bound limit enforced");

    PipelineProgram bad_size = valid_program();
    bad_size.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::dxil, dxbc_container(32U)}};
    const PipelineValidationResult bad_size_result = validate_pipeline(bad_size);
    require(!bad_size_result.valid && has_code(bad_size_result, "shader_bytecode_size"), "undersized DXBC declaration rejected");

    PipelineProgram oversized = valid_program();
    oversized.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::dxil, dxbc_container(48U)}};
    const PipelineValidationResult oversized_result = validate_pipeline(oversized);
    require(!oversized_result.valid && has_code(oversized_result, "shader_bytecode_size"), "oversized DXBC declaration rejected");

    PipelineProgram oversized_table = valid_program();
    oversized_table.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::dxil, dxbc_container(44U, 2U)}};
    const PipelineValidationResult oversized_table_result = validate_pipeline(oversized_table);
    require(!oversized_table_result.valid && has_code(oversized_table_result, "shader_bytecode_bounds"), "oversized DXBC table rejected");
}

void rejectsInvalidLayoutAndState() {
    PipelineProgram invalid = valid_program();
    invalid.vertex_layout.attributes[1].location = 0;
    invalid.vertex_layout.attributes[2].offset = 43;
    invalid.targets.has_depth = false;
    invalid.blend.enabled = true;
    const PipelineValidationResult result = validate_pipeline(invalid);
    require(!result.valid && has_code(result, "duplicate_vertex_location") && has_code(result, "vertex_attribute_bounds") &&
                has_code(result, "depth_target_missing"),
            "invalid layout and depth state rejected");

    PipelineProgram bad_winding = valid_program();
    bad_winding.raster.front_face = static_cast<PipelineFrontFace>(99);
    const PipelineValidationResult winding_result = validate_pipeline(bad_winding);
    require(!winding_result.valid && has_code(winding_result, "invalid_front_face"), "invalid front-face winding rejected");

    PipelineProgram numeric_limits = valid_program();
    numeric_limits.vertex_layout.attributes[0].location = 4;
    numeric_limits.resources[0].set = 2;
    numeric_limits.resources[0].binding = 3;
    PipelineLimits limits;
    limits.max_vertex_location = 3;
    limits.max_resource_set = 1;
    limits.max_resource_binding = 2;
    const PipelineValidationResult numeric_result = validate_pipeline(numeric_limits, limits);
    require(!numeric_result.valid && has_code(numeric_result, "vertex_location_limit") &&
                has_code(numeric_result, "resource_set_limit") && has_code(numeric_result, "resource_binding_limit"),
            "numeric layout and resource limits enforced");

    PipelineProgram a2c = valid_program();
    a2c.blend.alpha_to_coverage = true;
    const PipelineValidationResult a2c_result = validate_pipeline(a2c);
    require(!a2c_result.valid && has_code(a2c_result, "a2c_sample_count"), "A2C requires multisample output");
}

void enforcesResourceAndByteLimits() {
    PipelineProgram limited = valid_program();
    limited.resources.push_back({PipelineResourceKind::sampler, 0, 0, "sampler"});
    PipelineLimits limits;
    limits.max_resources = 2;
    const PipelineValidationResult resource_result = validate_pipeline(limited, limits);
    require(!resource_result.valid && has_code(resource_result, "duplicate_resource_binding"),
            "duplicate resource bindings are deterministic");
    limits.max_resources = 1;
    const StockPipelineRequest limited_request = [] {
        StockPipelineRequest request;
        request.material.shader = "ksPerPixel";
        request.resources = {{PipelineResourceKind::sampled_texture, 0, 0, "tx"},
                             {PipelineResourceKind::sampler, 0, 1, "sampler"}};
        return request;
    }();
    const StockPipelineResult resource_limit = build_stock_pipeline(limited_request, limits);
    require(!resource_limit.validation.valid && has_code(resource_limit.validation, "resource_limit"),
            "resource count limit enforced before copy");

    limits = {};
    limits.max_shader_module_bytes = 8;
    const PipelineValidationResult byte_result = validate_pipeline(valid_program(), limits);
    require(!byte_result.valid && has_code(byte_result, "shader_bytecode_limit"), "shader module byte limit enforced");
}

void preservesPreflightReasonWithOneDiagnostic() {
    PipelineLimits one_diagnostic;
    one_diagnostic.max_diagnostics = 1;
    one_diagnostic.max_name_bytes = 3;
    StockPipelineRequest name_request;
    name_request.material.shader = "longShaderName";
    const StockPipelineResult name_result = build_stock_pipeline(name_request, one_diagnostic);
    require(name_result.validation.diagnostics.size() == 1U &&
                name_result.validation.diagnostics.front().code == "name_limit",
            "name preflight reason survives a one-diagnostic budget");

    PipelineLimits shader_limits;
    shader_limits.max_diagnostics = 1;
    shader_limits.max_shader_module_bytes = 4;
    StockPipelineRequest shader_request;
    shader_request.material.shader = "ksPerPixel";
    shader_request.shaders = {spirv(PipelineShaderStage::vertex)};
    const StockPipelineResult shader_result = build_stock_pipeline(shader_request, shader_limits);
    require(shader_result.validation.diagnostics.size() == 1U &&
                shader_result.validation.diagnostics.front().code == "shader_bytecode_limit",
            "shader preflight reason survives a one-diagnostic budget");

    PipelineLimits resource_limits;
    resource_limits.max_diagnostics = 1;
    resource_limits.max_resources = 1;
    StockPipelineRequest resource_request;
    resource_request.material.shader = "ksPerPixel";
    resource_request.resources = {{PipelineResourceKind::sampled_texture, 0, 0, "tx0"},
                                  {PipelineResourceKind::sampler, 0, 1, "sampler"}};
    const StockPipelineResult resource_result = build_stock_pipeline(resource_request, resource_limits);
    require(resource_result.validation.diagnostics.size() == 1U &&
                resource_result.validation.diagnostics.front().code == "resource_limit",
            "resource-count preflight reason survives a one-diagnostic budget");

    PipelineLimits resource_name_limits;
    resource_name_limits.max_diagnostics = 1;
    StockPipelineRequest resource_name_request;
    resource_name_request.material.shader = "ksPerPixel";
    resource_name_request.resources = {{PipelineResourceKind::sampled_texture, 0, 0, ""}};
    const StockPipelineResult resource_name_result = build_stock_pipeline(resource_name_request, resource_name_limits);
    require(resource_name_result.validation.diagnostics.size() == 1U &&
                resource_name_result.validation.diagnostics.front().code == "resource_name",
            "resource-name preflight reason survives a one-diagnostic budget");
}

void mapsStockProfileWithExplicitStaging() {
    StockPipelineRequest request;
    request.material.shader = "ksPerPixel";
    request.material.serialized_blend_mode = 1;
    request.node.transparent = true;
    request.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::stock_container, stock_container()}};
    request.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_srgb, 1});
    request.targets.has_depth = true;
    request.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1};
    const StockPipelineResult result = build_stock_pipeline(request);
    require(result.profile.stock != nullptr && result.profile.transparent && result.profile.blend_enabled,
            "stock profile controls transparent blend state");
    require(result.program.raster.cull == PipelineCullMode::back && result.program.vertex_layout.stride == 44,
            "stock mesh layout and cull mapping");
    require(!result.validation.shader_execution_supported && has_code(result.validation, "shader_execution_staged") &&
                has_code(result.validation, "blend_factors_staged"),
            "staged stock execution and unspecified blend factors are explicit");

    request.material.shader = "extensionShader";
    const StockPipelineResult unknown = build_stock_pipeline(request);
    require(!unknown.validation.valid && has_code(unknown.validation, "unknown_shader"), "unknown stock shader diagnosed");
}

} // namespace

int main() {
    try {
        acceptsBoundedBackendNeutralContract();
        rejectsMalformedShaderBytes();
        rejectsInvalidLayoutAndState();
        enforcesResourceAndByteLimits();
        preservesPreflightReasonWithOneDiagnostic();
        mapsStockProfileWithExplicitStaging();
        std::cout << "pipeline tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipeline tests failed: " << error.what() << '\n';
        return 1;
    }
}
