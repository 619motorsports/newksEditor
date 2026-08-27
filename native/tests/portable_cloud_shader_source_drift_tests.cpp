#include "apex/core/sha256.hpp"
#include "apex/render/pipeline.hpp"

#include "../src/render/dxbc_reader.hpp"
#include "../src/render/generated/d3d12_portable_clouds_dxbc.hpp"
#include "../src/render/generated/portable_clouds_spirv.hpp"

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
    "src/render/shaders/portable_clouds.vert", 1861U,
    "5391d385bc244e63cecdc8b43eebfeb8c58ffe510f7e09780fddd5d02552ccd6"};
constexpr SourceIdentity fragment_source{
    "src/render/shaders/portable_clouds.frag", 1452U,
    "b3f58cc181f3b7f4efcb0b1940bb69d6dfa92c74e291bcc57c5fec68661b0d01"};
constexpr SourceIdentity d3d12_source{
    "src/render/shaders/d3d12_portable_clouds.hlsl", 3059U,
    "75a4096499085038d145a610fba1fe42c7a012cafd18d3cd8062fea3e5fd361e"};

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_source(const SourceIdentity& identity) {
    std::ifstream input(std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
                        std::string(identity.path), std::ios::binary);
    require(input.good(), "portable cloud shader source is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

template <typename T, std::size_t Size>
std::span<const std::uint8_t> byte_span(const T (&bytes)[Size]) {
    return {reinterpret_cast<const std::uint8_t*>(bytes),
            Size * sizeof(T)};
}

void verify_source(const SourceIdentity& identity) {
    const auto bytes = read_source(identity);
    require(bytes.size() == identity.size,
            "portable cloud shader source size is recorded");
    require(apex::core::sha256Hex(bytes) == identity.sha256,
            "portable cloud shader source has drifted");
}

void verify_spirv(const std::span<const std::uint8_t> bytes,
                  const std::size_t expected_size,
                  const std::string_view expected_sha256) {
    require(bytes.size() == expected_size &&
                apex::core::sha256Hex(bytes) == expected_sha256,
            "generated portable cloud SPIR-V has drifted");
    require(detect_pipeline_shader_format(bytes) ==
                PipelineShaderFormat::spirv,
            "generated portable cloud SPIR-V has valid magic");
    require(detect_pipeline_shader_format(bytes.first(3U)) ==
                PipelineShaderFormat::unknown,
            "truncated portable cloud SPIR-V is rejected");
}

void verify_dxbc(const std::span<const std::uint8_t> bytes,
                 const std::size_t expected_size,
                 const std::string_view expected_sha256) {
    require(bytes.size() == expected_size &&
                apex::core::sha256Hex(bytes) == expected_sha256,
            "generated portable cloud DXBC has drifted");
    detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    require(detail::inspect_dxbc_container(
                detail::as_bytes(bytes), 4096U, inspection, error_offset) ==
                detail::DxbcReaderStatus::ready,
            "generated portable cloud DXBC container is valid");
    require(inspection.program_chunk_count == 1U &&
                inspection.program_format == detail::DxbcProgramFormat::legacy,
            "generated portable cloud DXBC contains one legacy program");
    require(detail::inspect_dxbc_container(
                detail::as_bytes(bytes.first(31U)), 4096U, inspection,
                error_offset) != detail::DxbcReaderStatus::ready,
            "truncated portable cloud DXBC is rejected");
}

} // namespace

int main() {
    try {
        verify_source(vertex_source);
        verify_source(fragment_source);
        verify_source(d3d12_source);
        verify_spirv(byte_span(portable_clouds_vertex_spirv), 2384U,
                     "beeb54af6d407a0f8f3fadcd69ca6dd5f318aa8d68a579228b28268d55ab5ff1");
        verify_spirv(byte_span(portable_clouds_fragment_spirv), 1908U,
                     "695ae7c2f70749bd5ed6d23c383e6d69dbcbb4d04b544e83f2f5b8efb67ce55d");
        verify_dxbc(byte_span(d3d12_portable_clouds_vertex_dxbc), 2264U,
                    "440cae04ab613bfaae60d847ecce784fdfe4d1c4c3af639145ac793fec2c14c7");
        verify_dxbc(byte_span(d3d12_portable_clouds_pixel_dxbc), 1684U,
                    "477324d3dc34cbe8d13039989efae226c84fd4abdcfe220f80970ad913b0fa32");
        std::cout << "portable cloud shader source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portable cloud shader source drift tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
