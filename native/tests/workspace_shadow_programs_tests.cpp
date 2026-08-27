#include "apex/app/workspace_shadow_programs.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> shader_fixture() {
    constexpr std::array<std::uint32_t, 29U> words = {
        0x07230203U, 0x00010000U, 0x00070000U, 0x00000005U, 0x00000000U,
        0x00020011U, 0x00000001U, 0x0003000eU, 0x00000000U, 0x00000001U,
        0x0005000fU, 0x00000000U, 0x00000001U, 0x6e69616dU, 0x00000000U,
        0x00020013U, 0x00000002U, 0x00030021U, 0x00000003U, 0x00000002U,
        0x00050036U, 0x00000002U, 0x00000001U, 0x00000000U, 0x00000003U,
        0x000200f8U, 0x00000004U, 0x000100fdU, 0x00010038U,
    };
    std::vector<std::uint8_t> bytes(words.size() * sizeof(std::uint32_t));
    for (std::size_t index = 0U; index < words.size(); ++index) {
        const std::uint32_t word = words[index];
        for (std::size_t byte = 0U; byte < sizeof(word); ++byte)
            bytes[index * sizeof(word) + byte] =
                static_cast<std::uint8_t>(word >> (byte * 8U));
    }
    return bytes;
}

PipelineShaderModule module(PipelineShaderStage stage) {
    return {stage, PipelineShaderFormat::spirv, shader_fixture()};
}

void builds_all_supported_shadow_roles() {
    auto opaque = apex::app::buildWorkspaceShadowPipeline(
        "opaque", {module(PipelineShaderStage::vertex)},
        DepthOnlyIndexedPipelineRole::opaque_static);
    require(opaque.ok() && opaque.pipeline->vertex_layout.stride == 44U &&
                opaque.pipeline->shaders.size() == 1U &&
                opaque.pipeline->resources.empty(),
            "opaque role builds the static vertex-only depth contract");

    auto alpha = apex::app::buildWorkspaceShadowPipeline(
        "alpha",
        {module(PipelineShaderStage::vertex),
         module(PipelineShaderStage::fragment)},
        DepthOnlyIndexedPipelineRole::stock_alpha_tested_static);
    require(alpha.ok() && alpha.pipeline->vertex_layout.stride == 44U &&
                alpha.pipeline->shaders.size() == 2U &&
                alpha.pipeline->resources.size() == 3U &&
                alpha.pipeline->resources[0].binding == 0U &&
                alpha.pipeline->resources[1].binding == 3U &&
                alpha.pipeline->resources[2].binding == 4U,
            "alpha role builds the exact static t0, s3, and b4 contract");

    auto skinned = apex::app::buildWorkspaceShadowPipeline(
        "skinned", {module(PipelineShaderStage::vertex)},
        DepthOnlyIndexedPipelineRole::skinned);
    require(skinned.ok() && skinned.pipeline->vertex_layout.stride == 76U &&
                skinned.pipeline->vertex_layout.attributes.size() == 6U &&
                skinned.pipeline->vertex_layout.attributes[4].semantic ==
                    PipelineVertexSemantic::bone_weights &&
                skinned.pipeline->vertex_layout.attributes[5].semantic ==
                    PipelineVertexSemantic::bone_indices &&
                skinned.pipeline->resources.empty(),
            "skinned role keeps the retained 19-float CPU-skinned stream");
}

void rejects_malformed_role_modules_before_backend_work() {
    auto incomplete_alpha = apex::app::buildWorkspaceShadowPipeline(
        "alpha", {module(PipelineShaderStage::vertex)},
        DepthOnlyIndexedPipelineRole::stock_alpha_tested_static);
    require(!incomplete_alpha.ok() &&
                incomplete_alpha.diagnostic.code ==
                    "depth_only_indexed_shader_pair_invalid",
            "alpha role rejects a missing fragment module");

    PipelineShaderModule empty = module(PipelineShaderStage::vertex);
    empty.bytes.clear();
    auto truncated = apex::app::buildWorkspaceShadowPipeline(
        "opaque", {std::move(empty)},
        DepthOnlyIndexedPipelineRole::opaque_static);
    require(!truncated.ok() &&
                truncated.diagnostic.code ==
                    "depth_only_indexed_pipeline_shader_bytecode_limit",
            "empty shader input is rejected before a backend receives it");
}

} // namespace

int main() {
    try {
        builds_all_supported_shadow_roles();
        rejects_malformed_role_modules_before_backend_work();
        std::cout << "workspace shadow program tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace shadow program tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
