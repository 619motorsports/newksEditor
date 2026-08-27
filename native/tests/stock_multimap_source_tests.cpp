#include "apex/core/sha256.hpp"
#include "apex/render/stock_multimap_source.hpp"

#include "../src/render/generated/stock_multimap_nm_detail_artifacts.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef APEX_NATIVE_SOURCE_DIR
#error "APEX_NATIVE_SOURCE_DIR must identify the native source tree"
#endif

namespace {

using namespace apex::render;
using namespace apex::render::generated;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "MultiMap source file is readable");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void verify_program_contract(Backend backend, StockMultiMapSourceVariant variant) {
    const StockMultiMapSourceProgramResult result =
        create_builtin_stock_multimap_source_program(backend, variant);
    require(result.ok(), "built-in MultiMap source program is available");
    const PipelineProgram& program = *result.program;
    require(validate_stock_multimap_source_program(program, backend, variant) ==
                StockMultiMapSourceStatus::ready,
            "built-in MultiMap source program passes strict validation");
    require(program.shaders.size() == 2U && program.resources.size() == 12U &&
                program.vertex_layout.stride == 44U &&
                program.transform_contract == PipelineTransformContract::draw_matrices,
            "built-in MultiMap program retains the portable mesh ABI");
    const bool alpha_to_coverage =
        variant == StockMultiMapSourceVariant::alpha_to_coverage_normal_detail;
    require(program.blend.alpha_to_coverage == alpha_to_coverage &&
                program.targets.colors.front().samples == (alpha_to_coverage ? 4U : 1U),
            "built-in MultiMap variant retains its sample contract");
    require(program.shaders[0].provenance == PipelineShaderProvenance::source_equivalent &&
                program.shaders[1].provenance == PipelineShaderProvenance::source_equivalent,
            "built-in MultiMap artifacts remain labeled source-equivalent");

    PipelineProgram mutated = program;
    mutated.shaders[1].bytes.pop_back();
    require(validate_stock_multimap_source_program(mutated, backend, variant) ==
                StockMultiMapSourceStatus::shader_identity_mismatch,
            "truncated MultiMap artifacts fail closed");
    mutated = program;
    mutated.shaders[0].bytes.front() ^= 0x01U;
    require(validate_stock_multimap_source_program(mutated, backend, variant) ==
                StockMultiMapSourceStatus::shader_identity_mismatch,
            "mutated MultiMap artifacts fail closed");
    mutated = program;
    mutated.resources[11U].binding = 12U;
    require(validate_stock_multimap_source_program(mutated, backend, variant) ==
                StockMultiMapSourceStatus::resource_contract_mismatch,
            "changed detail bindings fail closed");
    mutated = program;
    mutated.shaders[0].provenance = PipelineShaderProvenance::installed_native;
    require(validate_stock_multimap_source_program(mutated, backend, variant) ==
                StockMultiMapSourceStatus::shader_provenance_mismatch,
            "portable artifacts cannot be relabeled as installed native");
}

void verify_target_contract() {
    PipelineRenderTargets one_sample;
    one_sample.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    const auto rejected = create_builtin_stock_multimap_source_program(
        Backend::Vulkan, StockMultiMapSourceVariant::alpha_to_coverage_normal_detail, one_sample);
    require(rejected.status == StockMultiMapSourceStatus::target_contract_mismatch &&
                !rejected.program.has_value(),
            "alpha-to-coverage MultiMap rejects a one-sample target");

    PipelineRenderTargets mismatched_depth;
    mismatched_depth.colors.push_back({PipelineRenderTargetFormat::rgba16_float, 4U});
    mismatched_depth.has_depth = true;
    mismatched_depth.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    const auto depth_rejected = create_builtin_stock_multimap_source_program(
        Backend::D3D12, StockMultiMapSourceVariant::normal_detail, mismatched_depth);
    require(depth_rejected.status == StockMultiMapSourceStatus::target_contract_mismatch,
            "MultiMap rejects mismatched color and depth samples");
}

void verify_source_and_artifact_identity() {
    const std::string root = APEX_NATIVE_SOURCE_DIR;
    const auto vertex_source =
        read_file(root + "/src/render/shaders/stock_multimap_nm_detail_source.vert");
    const auto fragment_source =
        read_file(root + "/src/render/shaders/stock_multimap_nm_detail_source.frag");
    const auto d3d12_source =
        read_file(root + "/src/render/shaders/d3d12_stock_multimap_nm_detail_source.hlsl");
    const auto vulkan = create_builtin_stock_multimap_source_program(
        Backend::Vulkan, StockMultiMapSourceVariant::normal_detail);
    const auto d3d12 = create_builtin_stock_multimap_source_program(
        Backend::D3D12, StockMultiMapSourceVariant::normal_detail);
    require(vulkan.ok() && d3d12.ok(), "both embedded backend packages expose evidence");
    require(apex::core::sha256Hex(vertex_source) == vulkan.evidence.vertex_source_sha256 &&
                apex::core::sha256Hex(fragment_source) == vulkan.evidence.fragment_source_sha256 &&
                apex::core::sha256Hex(d3d12_source) == vulkan.evidence.d3d12_source_sha256,
            "maintained source files have not drifted");
    require(apex::core::sha256Hex(vulkan.program->shaders[0].bytes) ==
                    vulkan.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(vulkan.program->shaders[1].bytes) ==
                    vulkan.evidence.fragment_artifact_sha256 &&
                apex::core::sha256Hex(d3d12.program->shaders[0].bytes) ==
                    d3d12.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(d3d12.program->shaders[1].bytes) ==
                    d3d12.evidence.fragment_artifact_sha256,
            "embedded backend artifacts have not drifted");
    require(detect_pipeline_shader_format(vulkan.program->shaders[0].bytes) ==
                    PipelineShaderFormat::spirv &&
                detect_pipeline_shader_format(d3d12.program->shaders[0].bytes) ==
                    PipelineShaderFormat::dxbc,
            "embedded artifacts retain their declared container formats");
    require(stock_multimap_source_shader_bytes(Backend::Vulkan,
                                               StockMultiMapSourceVariant::normal_detail) ==
                    stock_multimap_nm_detail_vertex_spirv_size +
                        stock_multimap_nm_detail_fragment_spirv_size &&
                stock_multimap_source_shader_bytes(Backend::D3D12,
                                                   StockMultiMapSourceVariant::normal_detail) ==
                    stock_multimap_nm_detail_vertex_dxbc_size +
                        stock_multimap_nm_detail_pixel_dxbc_size,
            "preflight byte accounting matches embedded artifact sizes");
}

} // namespace

int main() {
    try {
        for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
            verify_program_contract(backend, StockMultiMapSourceVariant::normal_detail);
            verify_program_contract(backend,
                                    StockMultiMapSourceVariant::alpha_to_coverage_normal_detail);
        }
        verify_target_contract();
        verify_source_and_artifact_identity();
        std::cout << "stock MultiMap source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stock MultiMap source tests failed: " << error.what() << '\n';
        return 1;
    }
}
