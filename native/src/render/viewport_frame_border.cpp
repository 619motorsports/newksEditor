#include "apex/render/viewport_frame_border.hpp"

#include "generated/viewport_frame_border_artifacts.hpp"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

using namespace generated;

constexpr std::string_view vertex_source_sha256 =
    "f61c144b285e9c0ee2c1390c87c2643b3cea3ab0d9b35ed8c0f3eb4533c1be43";
constexpr std::string_view fragment_source_sha256 =
    "6698c243b943df204358a9dde88d9584f4328a7f79ca3f99853fa7a5a6ff0ff2";
constexpr std::string_view d3d12_source_sha256 =
    "5a3bca3306daa6362e71b37e268a1b64e3e6fd50c0ee61599ce3c8274064d124";
constexpr std::string_view vertex_spirv_sha256 =
    "fca7d07484b327b1707294be4f0c01bd45c425418d4fe0eeb097754ab4942bed";
constexpr std::string_view fragment_spirv_sha256 =
    "4d4c1f3c32610219fd7aab87893aaf40fcf9928f3d7efc8d581786a37df15ec1";
constexpr std::string_view vertex_dxbc_sha256 =
    "757e7133ab13c6facd560e7a3713edf816f782a1a8089b27629ff1911f5dedee";
constexpr std::string_view pixel_dxbc_sha256 =
    "8f02c0d83ec5982c2c1b25cd5073085b6e273e44ce62c5b0bdbd36620464500f";
constexpr std::string_view evidence_document =
    "docs/evidence/recovered-viewport-frame-border.md";

bool supported_backend(Backend backend) noexcept {
    return backend == Backend::Vulkan || backend == Backend::D3D12;
}

std::vector<std::uint8_t> bytes(std::span<const std::uint8_t> source) {
    return {source.begin(), source.end()};
}

std::vector<PipelineShaderModule> modules(Backend backend) {
    if (backend == Backend::Vulkan) {
        return {
            {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
             bytes(viewport_frame_border_vertex_spirv),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
             bytes(viewport_frame_border_fragment_spirv),
             PipelineShaderProvenance::source_equivalent},
        };
    }
    if (backend == Backend::D3D12) {
        return {
            {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
             bytes(viewport_frame_border_vertex_dxbc),
             PipelineShaderProvenance::source_equivalent},
            {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
             bytes(viewport_frame_border_pixel_dxbc),
             PipelineShaderProvenance::source_equivalent},
        };
    }
    return {};
}

PipelineVertexLayout vertex_layout() {
    PipelineVertexLayout layout;
    layout.stride = sizeof(ViewportFrameBorderVertex);
    layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::color,
         PipelineVertexAttributeFormat::float32x4, 1U,
         static_cast<std::uint32_t>(3U * sizeof(float))},
    };
    return layout;
}

bool exact_vertex_layout(const PipelineVertexLayout& actual) {
    const PipelineVertexLayout expected = vertex_layout();
    if (actual.stride != expected.stride ||
        actual.attributes.size() != expected.attributes.size())
        return false;
    for (std::size_t index = 0U; index < expected.attributes.size(); ++index) {
        const PipelineVertexAttribute& left = actual.attributes[index];
        const PipelineVertexAttribute& right = expected.attributes[index];
        if (left.semantic != right.semantic || left.format != right.format ||
            left.location != right.location || left.offset != right.offset)
            return false;
    }
    return true;
}

bool supported_color_format(PipelineRenderTargetFormat format) noexcept {
    return format == PipelineRenderTargetFormat::rgba8_unorm ||
           format == PipelineRenderTargetFormat::rgba8_srgb ||
           format == PipelineRenderTargetFormat::bgra8_unorm ||
           format == PipelineRenderTargetFormat::bgra8_srgb ||
           format == PipelineRenderTargetFormat::rgba16_float;
}

bool supported_targets(const PipelineRenderTargets& targets) noexcept {
    if (targets.colors.size() != 1U ||
        !supported_color_format(targets.colors[0].format))
        return false;
    const std::uint32_t samples = targets.colors[0].samples;
    return (samples == 1U || samples == 4U) && targets.has_depth &&
           targets.depth.format == PipelineRenderTargetFormat::depth32_float &&
           targets.depth.samples == samples;
}

bool exact_state(const PipelineProgram& program) noexcept {
    return program.raster.fill == PipelineFillMode::solid &&
           program.raster.cull == PipelineCullMode::none &&
           !program.blend.enabled && !program.blend.alpha_to_coverage &&
           program.depth.test_enabled && program.depth.write_enabled &&
           program.depth.compare == PipelineCompareOperation::less;
}

ViewportFrameBorderEvidence source_evidence(Backend backend) noexcept {
    return {backend,
            vertex_source_sha256,
            fragment_source_sha256,
            d3d12_source_sha256,
            backend == Backend::Vulkan ? vertex_spirv_sha256 : vertex_dxbc_sha256,
            backend == Backend::Vulkan ? fragment_spirv_sha256 : pixel_dxbc_sha256,
            evidence_document};
}

void write_quad(std::array<ViewportFrameBorderVertex,
                           viewport_frame_border_vertex_count>& vertices,
                std::array<std::uint16_t,
                           viewport_frame_border_index_count>& indices,
                std::size_t quad, float x, float y, float width, float height,
                const std::array<float, 4U>& color) noexcept {
    const std::size_t vertex = quad * 4U;
    vertices[vertex + 0U] = {{x, y, 0.0F}, color};
    vertices[vertex + 1U] = {{x, y + height, 0.0F}, color};
    vertices[vertex + 2U] = {{x + width, y + height, 0.0F}, color};
    vertices[vertex + 3U] = {{x + width, y, 0.0F}, color};
    const std::size_t index = quad * 6U;
    const auto base = static_cast<std::uint16_t>(vertex);
    indices[index + 0U] = base;
    indices[index + 1U] = static_cast<std::uint16_t>(base + 1U);
    indices[index + 2U] = static_cast<std::uint16_t>(base + 2U);
    indices[index + 3U] = base;
    indices[index + 4U] = static_cast<std::uint16_t>(base + 2U);
    indices[index + 5U] = static_cast<std::uint16_t>(base + 3U);
}

} // namespace

const char* viewport_frame_border_status_name(
    ViewportFrameBorderStatus status) noexcept {
    switch (status) {
    case ViewportFrameBorderStatus::ready: return "ready";
    case ViewportFrameBorderStatus::invalid_dimensions: return "invalid_dimensions";
    case ViewportFrameBorderStatus::invalid_backend: return "invalid_backend";
    case ViewportFrameBorderStatus::shader_count_mismatch: return "shader_count_mismatch";
    case ViewportFrameBorderStatus::shader_stage_mismatch: return "shader_stage_mismatch";
    case ViewportFrameBorderStatus::shader_format_mismatch: return "shader_format_mismatch";
    case ViewportFrameBorderStatus::shader_provenance_mismatch: return "shader_provenance_mismatch";
    case ViewportFrameBorderStatus::shader_identity_mismatch: return "shader_identity_mismatch";
    case ViewportFrameBorderStatus::transform_contract_mismatch: return "transform_contract_mismatch";
    case ViewportFrameBorderStatus::resource_contract_mismatch: return "resource_contract_mismatch";
    case ViewportFrameBorderStatus::vertex_layout_mismatch: return "vertex_layout_mismatch";
    case ViewportFrameBorderStatus::target_contract_mismatch: return "target_contract_mismatch";
    case ViewportFrameBorderStatus::pipeline_state_mismatch: return "pipeline_state_mismatch";
    case ViewportFrameBorderStatus::pipeline_invalid: return "pipeline_invalid";
    }
    return "unknown";
}

ViewportFrameBorderGeometryResult build_viewport_frame_border(
    std::uint32_t width, std::uint32_t height, bool controls_active,
    Backend backend) noexcept {
    if (width == 0U || height == 0U)
        return {ViewportFrameBorderStatus::invalid_dimensions,
                {"viewport_frame_border_dimensions_invalid",
                 "Viewport frame dimensions must be non-zero"}};
    if (!supported_backend(backend))
        return {ViewportFrameBorderStatus::invalid_backend,
                {"viewport_frame_border_backend_invalid",
                 "Viewport frame requires Vulkan or D3D12"}};

    ViewportFrameBorderGeometryResult result;
    result.status = ViewportFrameBorderStatus::ready;
    const float frame_width = static_cast<float>(width);
    const float frame_height = static_cast<float>(height);
    const std::array<float, 4U> color = controls_active
        ? std::array<float, 4U>{0.0F, 0.7F, 0.0F, 1.0F}
        : std::array<float, 4U>{0.3F, 0.3F, 0.3F, 1.0F};
    write_quad(result.vertices, result.indices, 0U, 0.0F, 0.0F,
               viewport_frame_border_thickness_pixels, frame_height, color);
    write_quad(result.vertices, result.indices, 1U, 0.0F, 0.0F,
               frame_width, viewport_frame_border_thickness_pixels, color);
    write_quad(result.vertices, result.indices, 2U,
               frame_width - viewport_frame_border_thickness_pixels, 0.0F,
               viewport_frame_border_thickness_pixels, frame_height, color);
    write_quad(result.vertices, result.indices, 3U, 0.0F,
               frame_height - viewport_frame_border_thickness_pixels,
               frame_width, viewport_frame_border_thickness_pixels, color);

    const float x_scale = 2.0F / frame_width;
    const float y_scale = 2.0F / frame_height;
    result.matrices.world = apex::scene::identity_matrix;
    result.matrices.view_projection = {
        x_scale, 0.0F, 0.0F, 0.0F,
        0.0F, backend == Backend::Vulkan ? y_scale : -y_scale, 0.0F, 0.0F,
        0.0F, 0.0F, 0.5F, 0.0F,
        -1.0F, backend == Backend::Vulkan ? -1.0F : 1.0F, 0.5F, 1.0F,
    };
    result.diagnostic = {};
    return result;
}

std::size_t viewport_frame_border_shader_bytes(Backend backend) noexcept {
    if (backend == Backend::Vulkan)
        return viewport_frame_border_vertex_spirv_size +
               viewport_frame_border_fragment_spirv_size;
    if (backend == Backend::D3D12)
        return viewport_frame_border_vertex_dxbc_size +
               viewport_frame_border_pixel_dxbc_size;
    return 0U;
}

ViewportFrameBorderStatus validate_viewport_frame_border_program(
    const PipelineProgram& program, Backend backend) noexcept {
    if (!supported_backend(backend))
        return ViewportFrameBorderStatus::invalid_backend;
    if (program.shaders.size() != 2U)
        return ViewportFrameBorderStatus::shader_count_mismatch;
    if (program.shaders[0].stage != PipelineShaderStage::vertex ||
        program.shaders[1].stage != PipelineShaderStage::fragment)
        return ViewportFrameBorderStatus::shader_stage_mismatch;
    const PipelineShaderFormat format = backend == Backend::Vulkan
        ? PipelineShaderFormat::spirv : PipelineShaderFormat::dxbc;
    for (const PipelineShaderModule& shader : program.shaders) {
        if (shader.format != format)
            return ViewportFrameBorderStatus::shader_format_mismatch;
        if (shader.provenance != PipelineShaderProvenance::source_equivalent)
            return ViewportFrameBorderStatus::shader_provenance_mismatch;
    }
    const auto expected = modules(backend);
    if (program.shaders[0].bytes != expected[0].bytes ||
        program.shaders[1].bytes != expected[1].bytes)
        return ViewportFrameBorderStatus::shader_identity_mismatch;
    if (program.transform_contract != PipelineTransformContract::draw_matrices)
        return ViewportFrameBorderStatus::transform_contract_mismatch;
    if (!program.resources.empty())
        return ViewportFrameBorderStatus::resource_contract_mismatch;
    if (!exact_vertex_layout(program.vertex_layout))
        return ViewportFrameBorderStatus::vertex_layout_mismatch;
    if (!supported_targets(program.targets))
        return ViewportFrameBorderStatus::target_contract_mismatch;
    if (!exact_state(program))
        return ViewportFrameBorderStatus::pipeline_state_mismatch;
    if (!validate_pipeline(program).valid)
        return ViewportFrameBorderStatus::pipeline_invalid;
    return ViewportFrameBorderStatus::ready;
}

ViewportFrameBorderProgramResult create_viewport_frame_border_program(
    Backend backend, PipelineRenderTargets targets) {
    const ViewportFrameBorderEvidence evidence = source_evidence(backend);
    if (!supported_backend(backend))
        return {ViewportFrameBorderStatus::invalid_backend, std::nullopt, evidence};
    if (!supported_targets(targets))
        return {ViewportFrameBorderStatus::target_contract_mismatch,
                std::nullopt, evidence};
    PipelineProgram program;
    program.name = "recovered-viewport-frame-border-source-equivalent";
    program.shaders = modules(backend);
    program.vertex_layout = vertex_layout();
    program.targets = std::move(targets);
    program.raster.cull = PipelineCullMode::none;
    program.raster.fill = PipelineFillMode::solid;
    program.blend.enabled = false;
    program.blend.alpha_to_coverage = false;
    program.depth.test_enabled = true;
    program.depth.write_enabled = true;
    program.depth.compare = PipelineCompareOperation::less;
    program.transform_contract = PipelineTransformContract::draw_matrices;
    const ViewportFrameBorderStatus status =
        validate_viewport_frame_border_program(program, backend);
    if (status != ViewportFrameBorderStatus::ready)
        return {status, std::nullopt, evidence};
    return {status, std::move(program), evidence};
}

} // namespace apex::render
