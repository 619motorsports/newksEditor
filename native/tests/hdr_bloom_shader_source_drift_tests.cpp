#include "apex/core/sha256.hpp"

#include "../src/render/generated/hdr_bloom_spirv.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <vector>

#ifndef APEX_NATIVE_SOURCE_DIR
#error "APEX_NATIVE_SOURCE_DIR must identify the native source tree"
#endif

namespace {

struct RecordedIdentity {
    std::string_view path;
    std::size_t size;
    std::string_view sha256;
};

constexpr RecordedIdentity bloom_identity{
    "src/render/shaders/hdr_bloom.frag", 2239U,
    "59bca7d059bf0017dd692211ad541b7b9d1cd2e3484fbc0cb07e60e07a34cb82"};
constexpr RecordedIdentity composite_identity{
    "src/render/shaders/hdr_tone_map_bloom.frag", 2550U,
    "229dd0371722b069f451ed924a585bb970ef7ce9566a2b5dabf399deeeda0925"};
constexpr RecordedIdentity d3d12_identity{
    "src/render/shaders/d3d12_hdr_bloom.hlsl", 4906U,
    "8fd6d49476c4f22c9ee8d4c7732abaf2300eacca98be6a70fcbef6848d6ab86f"};

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "HDR bloom shader source is readable");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void verify_source(const RecordedIdentity& identity) {
    const auto source = read_file(std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
                                  std::string(identity.path));
    require(source.size() == identity.size,
            "HDR bloom shader source size is recorded");
    require(apex::core::sha256Hex(source) == identity.sha256,
            "HDR bloom shader source has drifted from its recorded identity");
}

void verify_d3d12_runtime_source() {
    const auto source_bytes = read_file(
        std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
        std::string(d3d12_identity.path));
    const auto device_bytes = read_file(
        std::string(APEX_NATIVE_SOURCE_DIR) +
        "/src/render/d3d12_device.cpp");
    const std::string source(source_bytes.begin(), source_bytes.end());
    const std::string device(device_bytes.begin(), device_bytes.end());
    constexpr std::string_view begin_marker =
        "constexpr char kD3d12BloomShader[] = R\"HLSL(\n";
    constexpr std::string_view end_marker = ")HLSL\";";
    const std::size_t begin = device.find(begin_marker);
    require(begin != std::string::npos,
            "D3D12 HDR bloom runtime source marker is present");
    const std::size_t body_begin = begin + begin_marker.size();
    const std::size_t body_end = device.find(end_marker, body_begin);
    require(body_end != std::string::npos,
            "D3D12 HDR bloom runtime source terminator is present");
    require(device.substr(body_begin, body_end - body_begin) == source,
            "D3D12 HDR bloom runtime source matches its HLSL file");
}

void verify_spirv_artifacts() {
    using namespace apex::render::generated;
    const auto bytes = [](const auto& words) {
        return std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(words.data()),
            words.size() * sizeof(words[0]));
    };
    const auto bloom = bytes(std::span(hdr_bloom_frag_spirv));
    const auto composite =
        bytes(std::span(hdr_tone_map_bloom_frag_spirv));
    require(bloom.size() == 3432U && composite.size() == 4228U,
            "HDR bloom SPIR-V sizes are recorded");
    require(apex::core::sha256Hex(bloom) ==
                "cf1b886705f3fa9d423d2917f71ee6b2241c983c3ebb95ed4936a20631d18d86" &&
                apex::core::sha256Hex(composite) ==
                    "86826c596adf63e745d4e53575466aabd1c375a6411da0e795c4caa0f3559850",
            "HDR bloom SPIR-V matches the optimized source artifacts");
}

} // namespace

int main() {
    try {
        verify_source(bloom_identity);
        verify_source(composite_identity);
        verify_source(d3d12_identity);
        verify_d3d12_runtime_source();
        verify_spirv_artifacts();
        std::cout << "HDR bloom shader source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDR bloom shader source drift tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
