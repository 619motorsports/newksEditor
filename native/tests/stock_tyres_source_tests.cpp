#include "apex/core/sha256.hpp"
#include "apex/render/stock_tyres_source.hpp"

#include "../src/render/generated/stock_tyres_source_artifacts.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef APEX_NATIVE_SOURCE_DIR
#error "APEX_NATIVE_SOURCE_DIR must identify the native source tree"
#endif

using namespace apex::render;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "stock tyre source file is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void test_backend_variants() {
    for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
        for (const StockTyresSourceVariant variant : {
                 StockTyresSourceVariant::base,
                 StockTyresSourceVariant::directional_shadow_receiver}) {
            auto result = create_builtin_stock_tyres_source_program(
                backend, variant);
            if (!result.ok())
                throw std::runtime_error(
                    std::string("stock tyre package is available: ") +
                    stock_tyres_source_status_name(result.status));
            require(result.program->shaders.size() == 2U,
                    "stock tyre package has two stages");
            require(result.program->shaders[0].provenance ==
                        PipelineShaderProvenance::source_equivalent,
                    "stock tyre vertex provenance is explicit");
            require(pipeline_declares_stock_tyres(*result.program),
                    "stock tyre resources have a dedicated semantic layout");
            require(pipeline_declares_multimap_reflection(*result.program),
                    "stock tyre resources declare the portable cube extension");
            require(pipeline_declares_directional_shadow_receiver(
                        *result.program) ==
                        (variant == StockTyresSourceVariant::
                                        directional_shadow_receiver),
                    "stock tyre shadow variant is orthogonal");
            require(validate_stock_tyres_source_program(
                        *result.program, backend, variant) ==
                        StockTyresSourceStatus::ready,
                    "stock tyre package validates");
            require(stock_tyres_source_shader_bytes(backend, variant) > 0U,
                    "stock tyre package reports bounded shader bytes");
            require(result.evidence.installed_vertex_sha256.size() == 64U &&
                        result.evidence.installed_fragment_sha256.size() == 64U,
                    "installed shader identities remain evidence only");
        }
    }
}

void test_mutations_fail_closed() {
    auto result = create_builtin_stock_tyres_source_program(
        Backend::Vulkan, StockTyresSourceVariant::base);
    require(result.ok(), "baseline package is available");
    auto mutated = *result.program;
    mutated.resources[6].name = "txMaps";
    require(validate_stock_tyres_source_program(
                mutated, Backend::Vulkan, StockTyresSourceVariant::base) ==
                StockTyresSourceStatus::resource_contract_mismatch,
            "generic maps cannot masquerade as tyre dirt");
    mutated = *result.program;
    mutated.shaders[1].bytes.pop_back();
    require(validate_stock_tyres_source_program(
                mutated, Backend::Vulkan, StockTyresSourceVariant::base) ==
                StockTyresSourceStatus::shader_identity_mismatch,
            "truncated tyre artifact is rejected");
}

void test_source_and_artifact_identity() {
    using namespace apex::render::generated;

    const std::string root = APEX_NATIVE_SOURCE_DIR;
    const auto vertex_source =
        read_file(root + "/src/render/shaders/stock_tyres_source.vert");
    const auto fragment_source =
        read_file(root + "/src/render/shaders/stock_tyres_source.frag");
    const auto shadow_fragment_source =
        read_file(root + "/src/render/shaders/stock_tyres_shadow_source.frag");
    const auto d3d12_source =
        read_file(root + "/src/render/shaders/d3d12_stock_tyres_source.hlsl");

    const auto vulkan_base = create_builtin_stock_tyres_source_program(
        Backend::Vulkan, StockTyresSourceVariant::base);
    const auto vulkan_shadow = create_builtin_stock_tyres_source_program(
        Backend::Vulkan,
        StockTyresSourceVariant::directional_shadow_receiver);
    const auto d3d12_base = create_builtin_stock_tyres_source_program(
        Backend::D3D12, StockTyresSourceVariant::base);
    const auto d3d12_shadow = create_builtin_stock_tyres_source_program(
        Backend::D3D12,
        StockTyresSourceVariant::directional_shadow_receiver);
    require(vulkan_base.ok() && vulkan_shadow.ok() && d3d12_base.ok() &&
                d3d12_shadow.ok(),
            "all stock tyre packages expose evidence");

    require(apex::core::sha256Hex(vertex_source) ==
                    vulkan_base.evidence.vertex_source_sha256 &&
                apex::core::sha256Hex(fragment_source) ==
                    vulkan_base.evidence.fragment_source_sha256 &&
                apex::core::sha256Hex(shadow_fragment_source) ==
                    vulkan_shadow.evidence.fragment_source_sha256 &&
                apex::core::sha256Hex(d3d12_source) ==
                    d3d12_base.evidence.d3d12_source_sha256,
            "maintained stock tyre source files have not drifted");

    require(apex::core::sha256Hex(vulkan_base.program->shaders[0].bytes) ==
                    vulkan_base.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(vulkan_base.program->shaders[1].bytes) ==
                    vulkan_base.evidence.fragment_artifact_sha256 &&
                apex::core::sha256Hex(vulkan_shadow.program->shaders[1].bytes) ==
                    vulkan_shadow.evidence.fragment_artifact_sha256 &&
                apex::core::sha256Hex(d3d12_base.program->shaders[0].bytes) ==
                    d3d12_base.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(d3d12_base.program->shaders[1].bytes) ==
                    d3d12_base.evidence.fragment_artifact_sha256 &&
                apex::core::sha256Hex(d3d12_shadow.program->shaders[1].bytes) ==
                    d3d12_shadow.evidence.fragment_artifact_sha256,
            "embedded stock tyre artifacts have not drifted");

    require(stock_tyres_source_shader_bytes(
                Backend::Vulkan, StockTyresSourceVariant::base) ==
                    stock_tyres_vertex_spirv.size() +
                        stock_tyres_fragment_spirv.size() &&
                stock_tyres_source_shader_bytes(
                    Backend::Vulkan,
                    StockTyresSourceVariant::directional_shadow_receiver) ==
                    stock_tyres_vertex_spirv.size() +
                        stock_tyres_shadow_fragment_spirv.size() &&
                stock_tyres_source_shader_bytes(
                    Backend::D3D12, StockTyresSourceVariant::base) ==
                    stock_tyres_vertex_dxbc.size() +
                        stock_tyres_fragment_dxbc.size() &&
                stock_tyres_source_shader_bytes(
                    Backend::D3D12,
                    StockTyresSourceVariant::directional_shadow_receiver) ==
                    stock_tyres_vertex_dxbc.size() +
                        stock_tyres_shadow_fragment_dxbc.size(),
            "stock tyre preflight byte accounting matches the artifacts");
}

}  // namespace

int main() {
    try {
        test_backend_variants();
        test_mutations_fail_closed();
        test_source_and_artifact_identity();
        std::cout << "stock tyre source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
