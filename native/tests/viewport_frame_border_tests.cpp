#include "apex/core/sha256.hpp"
#include "apex/render/viewport_frame_border.hpp"

#include "../src/render/generated/viewport_frame_border_artifacts.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool near(float left, float right) {
    return std::abs(left - right) < 0.00001F;
}

class FakeBuffer final : public Buffer {
public:
    FakeBuffer(Backend backend, BufferDescription description)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    BufferInfo info_;
};

class FakeTexture final : public Texture {
public:
    FakeTexture(Backend backend, TextureDescription description)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    TextureInfo info_;
};

class FakeDepthAttachment final : public DepthAttachment {
public:
    FakeDepthAttachment(Backend backend, DepthAttachmentDescription description)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    DepthAttachmentInfo info_;
};

PipelineRenderTargets targets(std::uint32_t samples = 1U) {
    PipelineRenderTargets value;
    value.colors = {{PipelineRenderTargetFormat::rgba8_unorm, samples}};
    value.has_depth = true;
    value.depth = {PipelineRenderTargetFormat::depth32_float, samples};
    return value;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "viewport frame source file is readable");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void verify_geometry(Backend backend) {
    const auto active = build_viewport_frame_border(800U, 600U, true, backend);
    require(active.ok(), "active viewport frame geometry builds");
    require(active.vertices.size() == 16U && active.indices.size() == 24U,
            "viewport frame uses four independent indexed quads");
    const auto& vertices = active.vertices;
    require(vertices[0U].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                vertices[1U].position == std::array<float, 3U>{0.0F, 600.0F, 0.0F} &&
                vertices[2U].position == std::array<float, 3U>{2.0F, 600.0F, 0.0F} &&
                vertices[3U].position == std::array<float, 3U>{2.0F, 0.0F, 0.0F},
            "left quad keeps the recovered GLRenderer vertex order");
    require(vertices[8U].position[0U] == 798.0F &&
                vertices[12U].position[1U] == 598.0F,
            "right and bottom quads start two pixels from each edge");
    for (const auto& vertex : vertices)
        require(vertex.color == std::array<float, 4U>{0.0F, 0.7F, 0.0F, 1.0F},
                "active viewport frame uses recovered green");
    const std::array<std::uint16_t, 6U> first_indices = {0U, 1U, 2U, 0U, 2U, 3U};
    require(std::equal(first_indices.begin(), first_indices.end(), active.indices.begin()),
            "each recovered quad translates to two stable triangles");

    const auto inactive = build_viewport_frame_border(800U, 600U, false, backend);
    require(inactive.ok() &&
                inactive.vertices[0U].color ==
                    std::array<float, 4U>{0.3F, 0.3F, 0.3F, 1.0F},
            "inactive viewport frame uses recovered gray");
    const auto tiny = build_viewport_frame_border(1U, 1U, false, backend);
    require(tiny.ok() && tiny.vertices[8U].position[0U] == -1.0F &&
                tiny.vertices[12U].position[1U] == -1.0F,
            "sub-two-pixel frames preserve native negative edge positions");
    require(build_viewport_frame_border(0U, 1U, false, backend).status ==
                ViewportFrameBorderStatus::invalid_dimensions,
            "zero-sized geometry fails closed");

    const auto& matrix = active.matrices.view_projection;
    require(near(matrix[0U] * 0.0F + matrix[12U], -1.0F) &&
                near(matrix[0U] * 800.0F + matrix[12U], 1.0F) &&
                near(matrix[10U] * 0.0F + matrix[14U], 0.5F),
            "screen matrix maps horizontal edges and native zero Z exactly");
    const float expected_top = backend == Backend::Vulkan ? -1.0F : 1.0F;
    const float expected_bottom = -expected_top;
    require(near(matrix[5U] * 0.0F + matrix[13U], expected_top) &&
                near(matrix[5U] * 600.0F + matrix[13U], expected_bottom),
            "screen matrix preserves top-left pixels in each backend clip convention");
}

void verify_program(Backend backend) {
    const auto result = create_viewport_frame_border_program(backend, targets());
    require(result.ok(), "viewport frame program exists for both production backends");
    const auto& program = *result.program;
    require(validate_viewport_frame_border_program(program, backend) ==
                ViewportFrameBorderStatus::ready &&
                program.raster.fill == PipelineFillMode::solid &&
                program.raster.cull == PipelineCullMode::none &&
                !program.blend.enabled && !program.blend.alpha_to_coverage &&
                program.depth.test_enabled && program.depth.write_enabled &&
                program.depth.compare == PipelineCompareOperation::less &&
                program.resources.empty(),
            "viewport frame preserves recovered fixed-function state");
    PipelineProgram changed = program;
    changed.shaders[0U].bytes.pop_back();
    require(validate_viewport_frame_border_program(changed, backend) ==
                ViewportFrameBorderStatus::shader_identity_mismatch,
            "truncated viewport frame shader artifacts fail closed");
    changed = program;
    changed.depth.compare = PipelineCompareOperation::less_or_equal;
    require(validate_viewport_frame_border_program(changed, backend) ==
                ViewportFrameBorderStatus::pipeline_state_mismatch,
            "less-or-equal cannot replace recovered strict less depth");
    changed = program;
    changed.raster.fill = PipelineFillMode::wireframe;
    require(validate_viewport_frame_border_program(changed, backend) ==
                ViewportFrameBorderStatus::pipeline_state_mismatch,
            "line rendering cannot replace recovered solid quads");
}

void verify_draw_contract(Backend backend) {
    const auto program = create_viewport_frame_border_program(backend, targets());
    require(program.ok(), "draw-contract program exists");
    TextureDescription texture_description;
    texture_description.width = 800U;
    texture_description.height = 600U;
    texture_description.format = TextureFormat::rgba8_unorm;
    texture_description.usage = TextureUsage::color_attachment |
                                TextureUsage::transfer_source;
    FakeTexture texture(backend, texture_description);
    BufferDescription vertex_description{
        viewport_frame_border_vertex_count * sizeof(ViewportFrameBorderVertex),
        BufferUsage::vertex, BufferMemory::device_local,
        BufferMutability::immutable};
    BufferDescription index_description{
        viewport_frame_border_index_count * sizeof(std::uint16_t),
        BufferUsage::index, BufferMemory::device_local,
        BufferMutability::immutable};
    FakeBuffer vertices(backend, vertex_description);
    FakeBuffer indices(backend, index_description);
    const auto geometry = build_viewport_frame_border(800U, 600U, true, backend);
    ViewportFrameBorderDrawRequest draw{
        &*program.program, &vertices, &indices, geometry.matrices};
    Diagnostic diagnostic;
    require(validate_viewport_frame_border_draw_request(texture, draw, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "fixed viewport frame draw validates");

    FakeDepthAttachment depth(backend, {800U, 600U, 1U});
    IndexedStaticMeshBatchDescription batch;
    batch.depth_attachment = &depth;
    batch.viewport_frame_border = draw;
    require(validate_indexed_static_mesh_batch_description(texture, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "clear-only scene can append the recovered viewport frame");
    batch.depth_attachment = nullptr;
    require(validate_indexed_static_mesh_batch_description(texture, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "viewport_frame_border_depth_attachment_missing",
            "viewport frame rejects a missing persistent depth attachment");

    vertex_description.size_bytes -= 1U;
    FakeBuffer truncated_vertices(backend, vertex_description);
    draw.vertex_buffer = &truncated_vertices;
    require(validate_viewport_frame_border_draw_request(texture, draw, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "truncated viewport frame geometry fails before backend execution");
    draw.vertex_buffer = &vertices;
    draw.matrices.view_projection[0U] =
        std::numeric_limits<float>::quiet_NaN();
    require(validate_viewport_frame_border_draw_request(texture, draw, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "non-finite viewport frame matrices fail closed");
}

void verify_source_identity() {
    const std::string root = APEX_NATIVE_SOURCE_DIR;
    const auto vertex = read_file(root + "/src/render/shaders/viewport_frame_border.vert");
    const auto fragment = read_file(root + "/src/render/shaders/viewport_frame_border.frag");
    const auto d3d12 = read_file(root + "/src/render/shaders/d3d12_viewport_frame_border.hlsl");
    const auto vk_program = create_viewport_frame_border_program(Backend::Vulkan, targets());
    const auto dx_program = create_viewport_frame_border_program(Backend::D3D12, targets());
    require(vk_program.ok() && dx_program.ok(), "both viewport frame packages expose evidence");
    require(apex::core::sha256Hex(vertex) == vk_program.evidence.vertex_source_sha256 &&
                apex::core::sha256Hex(fragment) == vk_program.evidence.fragment_source_sha256 &&
                apex::core::sha256Hex(d3d12) == dx_program.evidence.d3d12_source_sha256,
            "maintained viewport frame shader sources have not drifted");
    require(apex::core::sha256Hex(vk_program.program->shaders[0U].bytes) ==
                    vk_program.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(vk_program.program->shaders[1U].bytes) ==
                    vk_program.evidence.fragment_artifact_sha256 &&
                apex::core::sha256Hex(dx_program.program->shaders[0U].bytes) ==
                    dx_program.evidence.vertex_artifact_sha256 &&
                apex::core::sha256Hex(dx_program.program->shaders[1U].bytes) ==
                    dx_program.evidence.fragment_artifact_sha256,
            "embedded viewport frame backend artifacts have not drifted");
    require(viewport_frame_border_shader_bytes(Backend::Vulkan) ==
                    viewport_frame_border_vertex_spirv_size +
                        viewport_frame_border_fragment_spirv_size &&
                viewport_frame_border_shader_bytes(Backend::D3D12) ==
                    viewport_frame_border_vertex_dxbc_size +
                        viewport_frame_border_pixel_dxbc_size,
            "viewport frame artifact byte accounting is exact");
}

} // namespace

int main() {
    try {
        for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
            verify_geometry(backend);
            verify_program(backend);
            verify_draw_contract(backend);
        }
        verify_source_identity();
        std::cout << "viewport frame border tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "viewport frame border tests failed: " << error.what() << '\n';
        return 1;
    }
}
