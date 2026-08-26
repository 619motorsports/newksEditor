#include "apex/core/sha256.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan.hpp"

#include "../src/render/generated/stock_ks_per_pixel_source_equivalent_at_fragment_spirv.hpp"
#include "../src/render/generated/stock_ks_per_pixel_source_equivalent_base_fragment_spirv.hpp"
#include "../src/render/generated/stock_ks_per_pixel_source_equivalent_vertex_spirv.hpp"

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

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "source-equivalent shader source is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

template <std::size_t Size>
std::vector<std::uint8_t> little_endian_bytes(
    const std::uint32_t (&words)[Size]) {
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

void verify_source_and_package_identity() {
    const std::string root = APEX_NATIVE_SOURCE_DIR;
    const auto vertex_source = read_file(
        root + "/tests/shaders/stock_ks_per_pixel_source_equivalent.vert");
    const auto base_source = read_file(
        root + "/tests/shaders/stock_ks_per_pixel_source_equivalent_base.frag");
    const auto at_source = read_file(
        root + "/tests/shaders/stock_ks_per_pixel_source_equivalent_at.frag");
    const auto vertex_spirv = little_endian_bytes(
        stock_ks_per_pixel_source_equivalent_vertex_spirv);
    const auto base_spirv = little_endian_bytes(
        stock_ks_per_pixel_source_equivalent_base_fragment_spirv);
    const auto at_spirv = little_endian_bytes(
        stock_ks_per_pixel_source_equivalent_at_fragment_spirv);

    const auto vertex_source_hash = apex::core::sha256Hex(vertex_source);
    const auto base_source_hash = apex::core::sha256Hex(base_source);
    const auto at_source_hash = apex::core::sha256Hex(at_source);
    const auto vertex_spirv_hash = apex::core::sha256Hex(vertex_spirv);
    const auto base_spirv_hash = apex::core::sha256Hex(base_spirv);
    const auto at_spirv_hash = apex::core::sha256Hex(at_spirv);

    const auto base_result =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(
            StockKsPerPixelVariant::base);
    const auto at_result =
        create_builtin_stock_ks_per_pixel_vulkan_source_program(
            StockKsPerPixelVariant::alpha_to_coverage);
    require(base_result.ok() && at_result.ok(),
            "built-in source owners expose their recorded identities");

    const auto& base_evidence = base_result.program->evidence();
    const auto& at_evidence = at_result.program->evidence();
    require(vertex_source_hash == base_evidence.vertex_source_sha256 &&
                vertex_source_hash == at_evidence.vertex_source_sha256,
            "shared vertex source has not drifted from recorded identity");
    require(base_source_hash == base_evidence.fragment_source_sha256,
            "base fragment source has not drifted from recorded identity");
    require(at_source_hash == at_evidence.fragment_source_sha256,
            "AT fragment source has not drifted from recorded identity");
    require(vertex_spirv_hash == base_evidence.vertex_spirv_sha256 &&
                vertex_spirv_hash == at_evidence.vertex_spirv_sha256,
            "embedded vertex SPIR-V has not drifted from recorded identity");
    require(base_spirv_hash == base_evidence.fragment_spirv_sha256,
            "embedded base SPIR-V has not drifted from recorded identity");
    require(at_spirv_hash == at_evidence.fragment_spirv_sha256,
            "embedded AT SPIR-V has not drifted from recorded identity");

    require(base_result.program->pipeline().shaders[0].bytes == vertex_spirv &&
                base_result.program->pipeline().shaders[1].bytes == base_spirv &&
                at_result.program->pipeline().shaders[0].bytes == vertex_spirv &&
                at_result.program->pipeline().shaders[1].bytes == at_spirv,
            "validated owners use the exact embedded SPIR-V package");
}

}  // namespace

int main() {
    try {
        verify_source_and_package_identity();
        std::cout << "stock ksPerPixel Vulkan source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stock ksPerPixel Vulkan source drift tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
