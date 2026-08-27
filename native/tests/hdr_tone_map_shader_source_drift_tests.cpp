#include "apex/core/sha256.hpp"
#include "apex/render/pipeline.hpp"

#include "../src/render/dxbc_reader.hpp"
#include "../src/render/generated/d3d12_hdr_tone_map_dxbc.hpp"
#include "../src/render/generated/hdr_tone_map_spirv.hpp"

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

struct RecordedIdentity {
    std::string_view source_path;
    std::size_t source_size;
    std::string_view source_sha256;
    std::size_t artifact_size;
    std::string_view artifact_sha256;
};

constexpr RecordedIdentity vertex_identity{
    "src/render/shaders/hdr_tone_map.vert", 345U,
    "8c8e630c21ae83aa8abbc424c65f2aa5f0b3751fcb0ff0ea12cb37ce3ca4d89d",
    916U,
    "32c3014d1403a79fe237cb72b6fe3dac91a6adf9538e09c96e099f7602fca64b"};
constexpr RecordedIdentity fragment_identity{
    "src/render/shaders/hdr_tone_map.frag", 1999U,
    "c40f6f88b76793626c091e7e6ad83fdcb92031e02e055cf577d8e1040c406e65",
    3380U,
    "4447479c47f68fe8beda413e7760804550b7ec4e1a1226db1d3f40a876923a52"};
constexpr RecordedIdentity d3d12_identity{
    "src/render/shaders/d3d12_hdr_tone_map.hlsl", 2380U,
    "51ee9e4d791536b298eeeccc3daf4e450f706fa048d1a54d36237c9826b3ac37",
    0U, ""};

constexpr std::string_view d3d12_vertex_sha256 =
    "1226553b9f78efddaa9f9ab82cdb50b158893d78582e88d0a47c1a6e7660cb75";
constexpr std::string_view d3d12_pixel_sha256 =
    "21896499e82b658a1f7a3b5b89c5a77c15c1f2eae1353f2bfbf9c7e4466e4436";

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "HDR tone-map shader source is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

template <typename Byte, std::size_t Size>
std::span<const std::uint8_t> byte_span(const Byte (&bytes)[Size]) {
    static_assert(sizeof(Byte) == sizeof(std::uint8_t));
    return {reinterpret_cast<const std::uint8_t*>(bytes), Size};
}

void verify_source(const RecordedIdentity& identity) {
    const auto source = read_file(std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
                                  std::string(identity.source_path));
    require(source.size() == identity.source_size,
            "HDR tone-map shader source size is recorded");
    require(apex::core::sha256Hex(source) == identity.source_sha256,
            "HDR tone-map shader source has drifted from its recorded identity");
}

void verify_spirv(const std::span<const std::uint8_t> vertex_bytes,
                 const std::span<const std::uint8_t> fragment_bytes) {
    require(vertex_bytes.size() % sizeof(std::uint32_t) == 0U &&
                fragment_bytes.size() % sizeof(std::uint32_t) == 0U,
            "HDR tone-map SPIR-V is word aligned");
    require(apex::render::detect_pipeline_shader_format(vertex_bytes) ==
                PipelineShaderFormat::spirv &&
                apex::render::detect_pipeline_shader_format(fragment_bytes) ==
                PipelineShaderFormat::spirv,
            "HDR tone-map SPIR-V has a valid magic value");

    PipelineProgram program;
    program.name = "hdr_tone_map_drift";
    program.shaders.push_back(
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
         std::vector<std::uint8_t>(vertex_bytes.begin(), vertex_bytes.end()),
         PipelineShaderProvenance::portable});
    program.shaders.push_back(
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
         std::vector<std::uint8_t>(fragment_bytes.begin(), fragment_bytes.end()),
         PipelineShaderProvenance::portable});
    program.vertex_layout.stride = 8U;
    program.vertex_layout.attributes.push_back(
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x2, 0U, 0U});
    program.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    program.depth.test_enabled = false;
    program.depth.write_enabled = false;
    const PipelineValidationResult result = validate_pipeline(program);
    require(result.valid,
            "generated HDR tone-map SPIR-V failed validation");
    // validate_pipeline dispatches each SPIR-V module through the bounded
    // validate_spirv path used by backend-neutral pipeline preflight.
}

void verify_dxbc(const std::span<const std::uint8_t> bytes,
                const std::string_view hash,
                const std::size_t expected_size,
                const std::string_view message) {
    require(bytes.size() == expected_size,
            "generated HDR tone-map DXBC size is recorded");
    require(apex::core::sha256Hex(bytes) == hash,
            "generated HDR tone-map DXBC has drifted");

    apex::render::detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    require(apex::render::detail::inspect_dxbc_container(
                apex::render::detail::as_bytes(bytes), 4096U, inspection,
                error_offset) == apex::render::detail::DxbcReaderStatus::ready,
            message);
    require(inspection.chunk_count == 5U &&
                inspection.program_chunk_count == 1U &&
                inspection.program_format ==
                    apex::render::detail::DxbcProgramFormat::legacy,
            "generated HDR tone-map DXBC has one legacy program container");

    bool has_rdef = false;
    bool has_isgn = false;
    bool has_osgn = false;
    bool has_shdr = false;
    bool has_stat = false;
    for (std::uint32_t index = 0U; index < inspection.chunk_count; ++index) {
        apex::render::detail::DxbcChunkView chunk;
        require(apex::render::detail::read_dxbc_chunk(
                    apex::render::detail::as_bytes(bytes), index, chunk),
                "generated HDR tone-map DXBC chunk is readable");
        has_rdef = has_rdef || chunk.tag == apex::render::detail::dxbc_tag_rdef;
        has_isgn = has_isgn || chunk.tag == apex::render::detail::dxbc_tag_isgn;
        has_osgn = has_osgn || chunk.tag == apex::render::detail::dxbc_tag_osgn;
        has_shdr = has_shdr || chunk.tag == apex::render::detail::dxbc_tag_shdr;
        has_stat = has_stat || chunk.tag == 0x54415453U; // STAT
    }
    require(has_rdef && has_isgn && has_osgn && has_shdr && has_stat,
            "generated HDR tone-map DXBC has the expected container chunks");
}

void verify_source_and_artifact_identity() {
    verify_source(vertex_identity);
    verify_source(fragment_identity);
    verify_source(d3d12_identity);

    const auto vertex_spirv = byte_span(hdr_tone_map_vertex_spirv);
    const auto fragment_spirv = byte_span(hdr_tone_map_fragment_spirv);
    require(vertex_spirv.size() == vertex_identity.artifact_size &&
                fragment_spirv.size() == fragment_identity.artifact_size,
            "generated HDR tone-map SPIR-V sizes are recorded");
    require(apex::core::sha256Hex(vertex_spirv) == vertex_identity.artifact_sha256 &&
                apex::core::sha256Hex(fragment_spirv) ==
                    fragment_identity.artifact_sha256,
            "generated HDR tone-map SPIR-V has drifted");
    verify_spirv(vertex_spirv, fragment_spirv);

    verify_dxbc(byte_span(d3d12_hdr_tone_map_vertex_dxbc), d3d12_vertex_sha256,
                884U, "generated HDR tone-map vertex DXBC container is invalid");
    verify_dxbc(byte_span(d3d12_hdr_tone_map_pixel_dxbc), d3d12_pixel_sha256,
                3732U, "generated HDR tone-map pixel DXBC container is invalid");
}

} // namespace

int main() {
    try {
        verify_source_and_artifact_identity();
        std::cout << "HDR tone-map shader source drift tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDR tone-map shader source drift tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
