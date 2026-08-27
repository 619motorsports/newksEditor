#include "apex/core/sha256.hpp"
#include "apex/render/pipeline.hpp"

#include "../src/render/dxbc_reader.hpp"
#include "../src/render/generated/d3d12_fxaa_dxbc.hpp"
#include "../src/render/generated/fxaa_spirv.hpp"

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

constexpr SourceIdentity fragment_source{
    "src/render/shaders/fxaa.frag", 6927U,
    "ff624dc722eef33dbbf11ba09144ce5013efe3b164b99181cf60cdba30730dda"};
constexpr SourceIdentity d3d12_source{
    "src/render/shaders/d3d12_fxaa.hlsl", 8067U,
    "7028fb4fa978b21cca51ea2e85be73a046807ce8a09899038634f42d33283058"};

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_source(const SourceIdentity& identity) {
    std::ifstream input(std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
                        std::string(identity.path), std::ios::binary);
    require(input.good(), "FXAA shader source is readable");
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
            "FXAA shader source size is recorded");
    require(apex::core::sha256Hex(bytes) == identity.sha256,
            "FXAA shader source has drifted from its recorded identity");
}

void verify_spirv() {
    const auto bytes = byte_span(fxaa_fragment_spirv);
    require(bytes.size() == 9652U &&
                apex::core::sha256Hex(bytes) ==
                    "1b199a87148f094ea8afcccef1c301f5a897ba640986bfaf8725ec79e0368559",
            "generated FXAA SPIR-V has drifted");
    require(detect_pipeline_shader_format(bytes) ==
                PipelineShaderFormat::spirv,
            "generated FXAA SPIR-V has a valid magic value");
}

void verify_dxbc(const std::span<const std::uint8_t> bytes,
                 const std::size_t expected_size,
                 const std::string_view expected_sha256) {
    require(bytes.size() == expected_size &&
                apex::core::sha256Hex(bytes) == expected_sha256,
            "generated FXAA DXBC has drifted");
    detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    require(detail::inspect_dxbc_container(
                detail::as_bytes(bytes), 4096U, inspection, error_offset) ==
                detail::DxbcReaderStatus::ready,
            "generated FXAA DXBC container is valid");
    require(inspection.program_chunk_count == 1U &&
                inspection.program_format == detail::DxbcProgramFormat::legacy,
            "generated FXAA DXBC contains one legacy shader program");
}

}  // namespace

int main() {
    try {
        verify_source(fragment_source);
        verify_source(d3d12_source);
        verify_spirv();
        verify_dxbc(byte_span(d3d12_fxaa_vertex_dxbc), 884U,
                    "1226553b9f78efddaa9f9ab82cdb50b158893d78582e88d0a47c1a6e7660cb75");
        verify_dxbc(byte_span(d3d12_fxaa_pixel_dxbc), 9360U,
                    "f5250d3707f0bbe5f3f6faddaec4a3994d6cd47f8cd0b7c7c62dd1eed9466bb7");
        std::cout << "FXAA shader source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FXAA shader source drift tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
