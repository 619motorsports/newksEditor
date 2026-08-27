#include "apex/core/sha256.hpp"

#include "../src/render/dxbc_reader.hpp"
#include "../src/render/generated/d3d12_texture_mip_dxbc.hpp"

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

using namespace apex::render::generated;

constexpr std::string_view source_sha256 =
    "6246f6f18f24dbbb9b364e9ec93f85eaea40eb0467393105751cabd859f8d661";
constexpr std::string_view vertex_dxbc_sha256 =
    "a174a158a51b830af3819634e9639ecfa2610cf79305b48d96c097b3d6e00ee2";
constexpr std::string_view pixel_dxbc_sha256 =
    "48ff977c6bf009a0ae5c1703ba77d32198ceed7ee63e48b1bc20c84d8561ea28";

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "D3D12 mip shader source is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void verify_container(const std::span<const std::uint8_t> bytes,
                      const std::string_view message) {
    apex::render::detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    require(apex::render::detail::inspect_dxbc_container(
                apex::render::detail::as_bytes(bytes), 4096U, inspection,
                error_offset) == apex::render::detail::DxbcReaderStatus::ready,
            message);
    require(inspection.program_format ==
                apex::render::detail::DxbcProgramFormat::legacy,
            "generated D3D12 mip artifact is legacy DXBC");
    require(inspection.program_chunk_count == 1U,
            "generated D3D12 mip artifact contains one program chunk");
}

void verifies_source_and_bytecode_identity() {
    const std::string root = APEX_NATIVE_SOURCE_DIR;
    const std::vector<std::uint8_t> source = read_file(
        root + "/src/render/shaders/d3d12_texture_mip.hlsl");
    const std::span<const std::uint8_t> vertex(d3d12_texture_mip_vertex_dxbc,
                                               d3d12_texture_mip_vertex_dxbc_size);
    const std::span<const std::uint8_t> pixel(d3d12_texture_mip_pixel_dxbc,
                                              d3d12_texture_mip_pixel_dxbc_size);

    require(apex::core::sha256Hex(source) == source_sha256,
            "D3D12 mip HLSL source has drifted from its generated artifacts");
    require(apex::core::sha256Hex(vertex) == vertex_dxbc_sha256,
            "generated D3D12 mip vertex DXBC has drifted");
    require(apex::core::sha256Hex(pixel) == pixel_dxbc_sha256,
            "generated D3D12 mip pixel DXBC has drifted");
    require(d3d12_texture_mip_vertex_dxbc_size == 736U &&
                d3d12_texture_mip_pixel_dxbc_size == 724U,
            "generated D3D12 mip bytecode sizes are recorded");
    verify_container(vertex, "generated D3D12 mip vertex DXBC is valid");
    verify_container(pixel, "generated D3D12 mip pixel DXBC is valid");
}

} // namespace

int main() {
    try {
        verifies_source_and_bytecode_identity();
        std::cout << "D3D12 texture mip shader source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 texture mip shader source drift tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
