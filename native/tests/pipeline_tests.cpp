#include "apex/render/pipeline.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
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

std::vector<std::uint8_t> dxbc_container(std::uint32_t declared_size = 48U,
                                          std::uint32_t chunk_count = 1U,
                                          std::uint32_t chunk_offset = 36U) {
    std::vector<std::uint8_t> bytes(48U, 0);
    bytes[0] = 'D'; bytes[1] = 'X'; bytes[2] = 'B'; bytes[3] = 'C';
    bytes[20U] = 1U;
    for (std::size_t index = 0; index < 4; ++index) bytes[24U + index] = static_cast<std::uint8_t>(declared_size >> (index * 8U));
    for (std::size_t index = 0; index < 4; ++index) bytes[28U + index] = static_cast<std::uint8_t>(chunk_count >> (index * 8U));
    for (std::size_t index = 0; index < 4; ++index) bytes[32U + index] = static_cast<std::uint8_t>(chunk_offset >> (index * 8U));
    if (chunk_offset + 8U <= bytes.size()) {
        bytes[chunk_offset] = 'S';
        bytes[chunk_offset + 1U] = 'H';
        bytes[chunk_offset + 2U] = 'D';
        bytes[chunk_offset + 3U] = 'R';
        bytes[chunk_offset + 4U] = 4U;
        bytes[chunk_offset + 8U] = 0x40U;
        bytes[chunk_offset + 10U] = 1U;
    }
    return bytes;
}

std::vector<std::uint8_t> duplicate_dxbc_program_container() {
    std::vector<std::uint8_t> bytes(64U, 0U);
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0U; index < 4U; ++index)
            bytes[offset + index] =
                static_cast<std::uint8_t>(value >> (index * 8U));
    };
    put(0U, 0x43425844U);
    put(20U, 1U);
    put(24U, static_cast<std::uint32_t>(bytes.size()));
    put(28U, 2U);
    put(32U, 40U);
    put(36U, 52U);
    for (const std::size_t offset : {40U, 52U}) {
        put(offset, 0x58454853U);
        put(offset + 4U, 4U);
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
    const auto legacy = dxbc_container();
    require(detect_pipeline_shader_format(legacy) == PipelineShaderFormat::dxbc,
            "legacy Direct3D bytecode is labeled DXBC");
    auto llvm = legacy;
    llvm[36U] = 'D';
    llvm[37U] = 'X';
    llvm[38U] = 'I';
    llvm[39U] = 'L';
    require(detect_pipeline_shader_format(llvm) == PipelineShaderFormat::dxil,
            "LLVM Direct3D bytecode is labeled DXIL");
}

void enforcesShaderProvenanceFormatBoundary() {
    PipelineProgram source_equivalent = valid_program();
    for (auto& shader : source_equivalent.shaders)
        shader.provenance = PipelineShaderProvenance::source_equivalent;
    const PipelineValidationResult source_result = validate_pipeline(source_equivalent);
    require(source_result.valid &&
                pipeline_shader_provenance_name(
                    PipelineShaderProvenance::source_equivalent) ==
                    std::string_view("source_equivalent"),
            "source-equivalent SPIR-V provenance is explicit and accepted");

    PipelineProgram mislabeled_source = source_equivalent;
    mislabeled_source.shaders[0].format = PipelineShaderFormat::dxbc;
    const PipelineValidationResult source_mismatch =
        validate_pipeline(mislabeled_source);
    require(!source_mismatch.valid &&
                has_code(source_mismatch, "shader_provenance_format_mismatch"),
            "source-equivalent provenance cannot label DXBC");

    PipelineProgram mislabeled_native = valid_program();
    mislabeled_native.shaders[0].provenance =
        PipelineShaderProvenance::installed_native;
    const PipelineValidationResult native_mismatch =
        validate_pipeline(mislabeled_native);
    require(!native_mismatch.valid &&
                has_code(native_mismatch, "shader_provenance_format_mismatch"),
            "installed-native provenance cannot label portable SPIR-V");
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
    bad_size.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::dxbc, dxbc_container(32U)}};
    const PipelineValidationResult bad_size_result = validate_pipeline(bad_size);
    require(!bad_size_result.valid && has_code(bad_size_result, "shader_bytecode_size"), "undersized DXBC declaration rejected");

    PipelineProgram oversized = valid_program();
    oversized.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::dxbc, dxbc_container(52U)}};
    const PipelineValidationResult oversized_result = validate_pipeline(oversized);
    require(!oversized_result.valid && has_code(oversized_result, "shader_bytecode_size"), "oversized DXBC declaration rejected");

    PipelineProgram oversized_table = valid_program();
    oversized_table.shaders = {{PipelineShaderStage::vertex, PipelineShaderFormat::dxbc, dxbc_container(48U, 2U)}};
    const PipelineValidationResult oversized_table_result = validate_pipeline(oversized_table);
    require(!oversized_table_result.valid && has_code(oversized_table_result, "shader_bytecode_bounds"), "oversized DXBC table rejected");

    PipelineProgram table_chunk = valid_program();
    auto table_chunk_bytes = dxbc_container();
    table_chunk_bytes[32U] = 32U;
    table_chunk_bytes[33U] = 0U;
    table_chunk_bytes[34U] = 0U;
    table_chunk_bytes[35U] = 0U;
    table_chunk.shaders = {{PipelineShaderStage::vertex,
                            PipelineShaderFormat::dxbc,
                            std::move(table_chunk_bytes)}};
    const PipelineValidationResult table_chunk_result =
        validate_pipeline(table_chunk);
    require(!table_chunk_result.valid &&
                has_code(table_chunk_result, "shader_bytecode_bounds"),
            "DXBC chunk inside its offset table rejected");

    PipelineProgram oversized_payload = valid_program();
    auto oversized_payload_bytes = dxbc_container();
    for (std::size_t byte = 0U; byte < 4U; ++byte)
        oversized_payload_bytes[40U + byte] = 0xffU;
    oversized_payload.shaders = {{PipelineShaderStage::vertex,
                                  PipelineShaderFormat::dxbc,
                                  std::move(oversized_payload_bytes)}};
    const PipelineValidationResult oversized_payload_result =
        validate_pipeline(oversized_payload);
    require(!oversized_payload_result.valid &&
                has_code(oversized_payload_result,
                         "shader_bytecode_bounds"),
            "overflow-sized DXBC chunk payload rejected");

    PipelineProgram mislabeled = valid_program();
    auto dxil = dxbc_container();
    dxil[36U] = 'D';
    dxil[37U] = 'X';
    dxil[38U] = 'I';
    dxil[39U] = 'L';
    mislabeled.shaders = {{PipelineShaderStage::vertex,
                           PipelineShaderFormat::dxbc, std::move(dxil)}};
    const PipelineValidationResult mislabeled_result = validate_pipeline(mislabeled);
    require(!mislabeled_result.valid &&
                has_code(mislabeled_result,
                         "shader_bytecode_format_mismatch"),
            "DXIL payload with a DXBC label is rejected");

    PipelineProgram duplicate_program = valid_program();
    duplicate_program.shaders = {{PipelineShaderStage::vertex,
                                  PipelineShaderFormat::dxbc,
                                  duplicate_dxbc_program_container()}};
    const PipelineValidationResult duplicate_program_result =
        validate_pipeline(duplicate_program);
    require(!duplicate_program_result.valid &&
                has_code(duplicate_program_result,
                         "shader_bytecode_program"),
            "duplicate DXBC program chunks are rejected");
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

    PipelineProgram bad_transform = valid_program();
    bad_transform.transform_contract = static_cast<PipelineTransformContract>(99);
    const PipelineValidationResult transform_result = validate_pipeline(bad_transform);
    require(!transform_result.valid && has_code(transform_result, "invalid_transform_contract"),
            "invalid transform contract rejected");

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

void acceptsDepthOnlyVertexStageOnly() {
    PipelineProgram depth_only = valid_program();
    depth_only.targets.colors.clear();
    depth_only.targets.has_depth = true;
    depth_only.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    depth_only.resources.clear();
    depth_only.depth.test_enabled = true;
    depth_only.depth.write_enabled = true;
    depth_only.depth.compare = PipelineCompareOperation::less;
    depth_only.transform_contract = PipelineTransformContract::draw_matrices;
    depth_only.shaders.pop_back();
    const PipelineValidationResult accepted = validate_pipeline(depth_only);
    require(accepted.valid, "vertex-only depth pipeline accepted");

    PipelineProgram color_vertex_only = depth_only;
    color_vertex_only.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    const PipelineValidationResult color_result = validate_pipeline(color_vertex_only);
    require(!color_result.valid && has_code(color_result, "missing_fragment_stage"),
            "color pipeline still requires a fragment stage");

    PipelineProgram no_target = depth_only;
    no_target.targets.has_depth = false;
    const PipelineValidationResult no_target_result = validate_pipeline(no_target);
    require(!no_target_result.valid && has_code(no_target_result, "missing_fragment_stage") &&
                has_code(no_target_result, "render_target_missing"),
            "vertex-only pipeline without depth target rejected");
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
    require(result.program.raster.cull == PipelineCullMode::back &&
                result.program.vertex_layout.stride ==
                    stock_ks_per_pixel_vertex_stride_bytes &&
                result.program.vertex_layout.attributes.size() == 4U &&
                result.program.vertex_layout.attributes[0U].semantic ==
                    PipelineVertexSemantic::position &&
                result.program.vertex_layout.attributes[0U].offset == 0U &&
                result.program.vertex_layout.attributes[1U].semantic ==
                    PipelineVertexSemantic::normal &&
                result.program.vertex_layout.attributes[1U].offset == 12U &&
                result.program.vertex_layout.attributes[2U].semantic ==
                    PipelineVertexSemantic::texcoord0 &&
                result.program.vertex_layout.attributes[2U].offset == 24U &&
                result.program.vertex_layout.attributes[3U].semantic ==
                    PipelineVertexSemantic::tangent &&
                result.program.vertex_layout.attributes[3U].offset == 32U,
            "stock mesh layout and cull mapping");
    require(result.program.blend.source_color == PipelineBlendFactor::source_alpha &&
                result.program.blend.destination_color ==
                    PipelineBlendFactor::one_minus_source_alpha &&
                result.program.blend.source_alpha == PipelineBlendFactor::source_alpha &&
                result.program.blend.destination_alpha ==
                    PipelineBlendFactor::one_minus_source_alpha,
            "stock alpha blend uses the production WebGL factors");
    require(!result.validation.shader_execution_supported && has_code(result.validation, "shader_execution_staged") &&
                !has_code(result.validation, "blend_factors_staged"),
            "shader execution remains staged while source-evidenced blend factors are exact");

    MaterialOverride multiply;
    multiply.blend_mode = "MULTIPLY";
    request.material.serialized_blend_mode = 0;
    request.override_values = &multiply;
    const StockPipelineResult multiplied = build_stock_pipeline(request);
    require(multiplied.program.blend.enabled &&
                multiplied.program.blend.source_color ==
                    PipelineBlendFactor::destination_color &&
                multiplied.program.blend.destination_color == PipelineBlendFactor::zero &&
                multiplied.program.blend.source_alpha ==
                    PipelineBlendFactor::destination_alpha &&
                multiplied.program.blend.destination_alpha == PipelineBlendFactor::zero,
            "stock multiply blend matches WebGL destination-color multiplication");

    MaterialOverride transparent_black;
    transparent_black.blend_mode = "TRANSPARENT_AS_BLACK";
    request.override_values = &transparent_black;
    const StockPipelineResult transparent = build_stock_pipeline(request);
    require(transparent.program.blend.enabled &&
                transparent.program.blend.source_color == PipelineBlendFactor::one &&
                transparent.program.blend.destination_color ==
                    PipelineBlendFactor::one_minus_source_alpha &&
                transparent.program.blend.source_alpha == PipelineBlendFactor::one &&
                transparent.program.blend.destination_alpha ==
                    PipelineBlendFactor::one_minus_source_alpha,
            "transparent-as-black blend matches the production WebGL factors");

    request.override_values = nullptr;
    request.material.shader = "extensionShader";
    const StockPipelineResult unknown = build_stock_pipeline(request);
    require(!unknown.validation.valid && has_code(unknown.validation, "unknown_shader"), "unknown stock shader diagnosed");
}

} // namespace

int main() {
    try {
        acceptsBoundedBackendNeutralContract();
        enforcesShaderProvenanceFormatBoundary();
        rejectsMalformedShaderBytes();
        rejectsInvalidLayoutAndState();
        acceptsDepthOnlyVertexStageOnly();
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
