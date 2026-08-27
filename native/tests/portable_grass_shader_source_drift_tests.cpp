#include "apex/core/sha256.hpp"
#include "apex/render/pipeline.hpp"

#include "../src/render/dxbc_reader.hpp"
#include "../src/render/generated/d3d12_portable_grass_dxbc.hpp"
#include "../src/render/generated/portable_grass_spirv.hpp"

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

struct SourceIdentity {
    std::string_view path;
    std::size_t size;
    std::string_view sha256;
};

constexpr SourceIdentity vertex_source{
    "src/render/shaders/portable_grass.vert", 1904U,
    "1bd9c238bdf6183b645055a45541b3c6d092dbaa70369474e2d8f7029a93c72f"};
constexpr SourceIdentity fragment_source{
    "src/render/shaders/portable_grass.frag", 2141U,
    "b7a6b49191032711ae0366c18f1e517e8b82c7809139c0dfb45839993496e341"};
constexpr SourceIdentity d3d12_source{
    "src/render/shaders/d3d12_portable_grass.hlsl", 3377U,
    "45e10b78448f90874ed06daedd3fb1c7cd2345d4f47c2c36d5580f47a4bf7939"};

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_source(const SourceIdentity& identity) {
    std::ifstream input(std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
                        std::string(identity.path), std::ios::binary);
    require(input.good(), "portable grass shader source is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

template <typename T, std::size_t Size>
std::span<const std::uint8_t> byte_span(const T (&bytes)[Size]) {
    return {reinterpret_cast<const std::uint8_t*>(bytes), Size * sizeof(T)};
}

void verify_source(const SourceIdentity& identity) {
    const auto bytes = read_source(identity);
    require(bytes.size() == identity.size,
            "portable grass shader source size is recorded");
    require(apex::core::sha256Hex(bytes) == identity.sha256,
            "portable grass shader source has drifted");
}

void verify_spirv(const std::span<const std::uint8_t> bytes,
                  const std::size_t size, const std::string_view sha256) {
    require(bytes.size() == size && apex::core::sha256Hex(bytes) == sha256,
            "generated portable grass SPIR-V has drifted");
    require(detect_pipeline_shader_format(bytes) == PipelineShaderFormat::spirv,
            "generated portable grass SPIR-V has valid magic");
    require(detect_pipeline_shader_format(bytes.first(3U)) ==
                PipelineShaderFormat::unknown,
            "truncated portable grass SPIR-V is rejected");
}

void verify_dxbc(const std::span<const std::uint8_t> bytes,
                 const std::size_t size, const std::string_view sha256) {
    require(bytes.size() == size && apex::core::sha256Hex(bytes) == sha256,
            "generated portable grass DXBC has drifted");
    detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    require(detail::inspect_dxbc_container(
                detail::as_bytes(bytes), 4096U, inspection, error_offset) ==
                detail::DxbcReaderStatus::ready,
            "generated portable grass DXBC container is valid");
    require(inspection.program_chunk_count == 1U &&
                inspection.program_format == detail::DxbcProgramFormat::legacy,
            "generated portable grass DXBC contains one legacy program");
    require(detail::inspect_dxbc_container(
                detail::as_bytes(bytes.first(31U)), 4096U, inspection,
                error_offset) != detail::DxbcReaderStatus::ready,
            "truncated portable grass DXBC is rejected");
}

} // namespace

int main() {
    try {
        verify_source(vertex_source);
        verify_source(fragment_source);
        verify_source(d3d12_source);
        verify_spirv(byte_span(portable_grass_vertex_spirv), 2380U,
                     "de4e039ff7dc026cbe905e25d1d7f58058e6d138f37417a06eee7b35ef06f3c3");
        verify_spirv(byte_span(portable_grass_fragment_spirv), 2644U,
                     "6247c95c94e1d5bfd59a62661059fcdcde20a81873a1c6b9a279f9ecb63806f8");
        verify_dxbc(byte_span(d3d12_portable_grass_vertex_dxbc), 2168U,
                    "39f456d315167d3048979623e1868af7a426632e0308461549282a6dd3f410ce");
        verify_dxbc(byte_span(d3d12_portable_grass_pixel_dxbc), 2424U,
                    "a4125ff93f038858bc531f81ae04d25abd37c1da7aab5dd7ac293e6d27d17660");
        std::cout << "portable grass shader source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portable grass shader source drift tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
