#include "apex/core/sha256.hpp"
#include "apex/render/directional_shadow_source.hpp"

#include "../src/render/generated/directional_shadow_source_artifacts.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
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
    require(input.good(), "directional-shadow source file is readable");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void verify_program_contract(Backend backend, DirectionalShadowSourceRole role) {
    const auto result = create_builtin_directional_shadow_source_program(backend, role);
    require(result.ok(), "built-in directional-shadow program is available");
    const PipelineProgram& program = *result.program;
    require(validate_directional_shadow_source_program(program, backend, role) ==
                DirectionalShadowSourceStatus::ready,
            "built-in directional-shadow program passes strict validation");
    const bool skinned = role == DirectionalShadowSourceRole::cpu_skinned;
    require(program.shaders.size() == 1U && program.resources.empty() &&
                program.vertex_layout.stride == (skinned ? 76U : 44U) &&
                program.targets.colors.empty() && program.targets.has_depth &&
                program.targets.depth.format == PipelineRenderTargetFormat::depth32_float &&
                program.transform_contract == PipelineTransformContract::draw_matrices &&
                program.shaders.front().provenance == PipelineShaderProvenance::translated,
            "built-in directional-shadow role retains its translated depth ABI");

    PipelineProgram mutated = program;
    mutated.shaders.front().bytes.pop_back();
    require(validate_directional_shadow_source_program(mutated, backend, role) ==
                DirectionalShadowSourceStatus::shader_identity_mismatch,
            "truncated directional-shadow artifact fails closed");
    mutated = program;
    mutated.shaders.front().bytes.front() ^= 0x01U;
    require(validate_directional_shadow_source_program(mutated, backend, role) ==
                DirectionalShadowSourceStatus::shader_identity_mismatch,
            "mutated directional-shadow artifact fails closed");
    mutated = program;
    mutated.shaders.front().provenance = PipelineShaderProvenance::installed_native;
    require(validate_directional_shadow_source_program(mutated, backend, role) ==
                DirectionalShadowSourceStatus::shader_provenance_mismatch,
            "translated artifact cannot be relabeled installed native");
    mutated = program;
    mutated.vertex_layout.stride = skinned ? 44U : 76U;
    require(validate_directional_shadow_source_program(mutated, backend, role) ==
                DirectionalShadowSourceStatus::vertex_layout_mismatch,
            "directional-shadow roles reject the other retained stream");
}

void verify_source_and_artifact_identity() {
    const std::string root = APEX_NATIVE_SOURCE_DIR;
    const auto vertex_source =
        read_file(root + "/src/render/shaders/directional_shadow_source.vert");
    const auto d3d12_source =
        read_file(root + "/src/render/shaders/d3d12_directional_shadow_source.hlsl");
    const auto vulkan = create_builtin_directional_shadow_source_program(
        Backend::Vulkan, DirectionalShadowSourceRole::cpu_skinned);
    const auto d3d12 = create_builtin_directional_shadow_source_program(
        Backend::D3D12, DirectionalShadowSourceRole::cpu_skinned);
    require(vulkan.ok() && d3d12.ok(), "both directional-shadow backend artifacts expose evidence");
    require(apex::core::sha256Hex(vertex_source) == vulkan.evidence.vertex_source_sha256 &&
                apex::core::sha256Hex(d3d12_source) == vulkan.evidence.d3d12_source_sha256 &&
                apex::core::sha256Hex(vulkan.program->shaders.front().bytes) ==
                    vulkan.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(d3d12.program->shaders.front().bytes) ==
                    d3d12.evidence.vertex_artifact_sha256,
            "directional-shadow source and embedded artifacts have not drifted");
    require(detect_pipeline_shader_format(vulkan.program->shaders.front().bytes) ==
                    PipelineShaderFormat::spirv &&
                detect_pipeline_shader_format(d3d12.program->shaders.front().bytes) ==
                    PipelineShaderFormat::dxbc,
            "directional-shadow artifacts retain their container formats");
    require(directional_shadow_source_shader_bytes(Backend::Vulkan,
                                                   DirectionalShadowSourceRole::opaque_static) ==
                    directional_shadow_source_vertex_spirv_size &&
                directional_shadow_source_shader_bytes(Backend::D3D12,
                                                       DirectionalShadowSourceRole::cpu_skinned) ==
                    directional_shadow_source_vertex_dxbc_size,
            "preflight accounting matches directional-shadow artifact sizes");
}

void rejects_invalid_enum_values() {
    const auto invalid_role = static_cast<DirectionalShadowSourceRole>(255U);
    require(!create_builtin_directional_shadow_source_program(Backend::Vulkan, invalid_role).ok() &&
                directional_shadow_source_shader_bytes(Backend::Vulkan, invalid_role) == 0U,
            "invalid directional-shadow role fails without copying artifacts");
    require(!create_builtin_directional_shadow_source_program(
                 static_cast<Backend>(255U), DirectionalShadowSourceRole::opaque_static)
                 .ok(),
            "invalid directional-shadow backend fails closed");
}

} // namespace

int main() {
    try {
        for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
            verify_program_contract(backend, DirectionalShadowSourceRole::opaque_static);
            verify_program_contract(backend, DirectionalShadowSourceRole::cpu_skinned);
        }
        verify_source_and_artifact_identity();
        rejects_invalid_enum_values();
        std::cout << "directional shadow source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "directional shadow source tests failed: " << error.what() << '\n';
        return 1;
    }
}
