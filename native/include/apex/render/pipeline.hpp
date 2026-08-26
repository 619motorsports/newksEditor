#pragma once

#include "apex/render/material_profile.hpp"
#include "apex/render/texture_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

// A logical program contract. These enums deliberately contain no Vulkan,
// D3D12, or other backend handles. Adapters translate this contract after it
// has passed validation.
enum class PipelineShaderStage : std::uint8_t { vertex, fragment, geometry };
enum class PipelineShaderFormat : std::uint8_t {
    unknown,
    stock_container,
    spirv,
    dxbc,
    dxil,
};

struct PipelineShaderModule {
    PipelineShaderStage stage = PipelineShaderStage::vertex;
    PipelineShaderFormat format = PipelineShaderFormat::unknown;
    std::vector<std::uint8_t> bytes;
};

enum class PipelineVertexSemantic : std::uint8_t {
    position,
    normal,
    texcoord0,
    texcoord1,
    tangent,
    color,
    bone_weights,
    bone_indices,
};

enum class PipelineVertexAttributeFormat : std::uint8_t {
    float32,
    float32x2,
    float32x3,
    float32x4,
    uint32x4,
};

struct PipelineVertexAttribute {
    PipelineVertexSemantic semantic = PipelineVertexSemantic::position;
    PipelineVertexAttributeFormat format = PipelineVertexAttributeFormat::float32x3;
    std::uint32_t location = 0;
    std::uint32_t offset = 0;
};

struct PipelineVertexLayout {
    std::uint32_t stride = 0;
    std::vector<PipelineVertexAttribute> attributes;
};

enum class PipelineRenderTargetFormat : std::uint8_t {
    unknown,
    rgba8_unorm,
    rgba8_srgb,
    bgra8_unorm,
    bgra8_srgb,
    rgba16_float,
    depth32_float,
    depth24_stencil8,
};

struct PipelineRenderTarget {
    PipelineRenderTargetFormat format = PipelineRenderTargetFormat::unknown;
    std::uint32_t samples = 1;
};

struct PipelineRenderTargets {
    std::vector<PipelineRenderTarget> colors;
    bool has_depth = false;
    PipelineRenderTarget depth;
};

enum class PipelineCullMode : std::uint8_t { none, front, back };
enum class PipelineFrontFace : std::uint8_t { counter_clockwise, clockwise };
enum class PipelineFillMode : std::uint8_t { solid, wireframe };

struct PipelineRasterState {
    PipelineCullMode cull = PipelineCullMode::back;
    PipelineFrontFace front_face = PipelineFrontFace::counter_clockwise;
    PipelineFillMode fill = PipelineFillMode::solid;
};

enum class PipelineBlendFactor : std::uint8_t {
    zero,
    one,
    source_alpha,
    one_minus_source_alpha,
    destination_color,
    destination_alpha,
    one_minus_destination_alpha,
};

enum class PipelineBlendOperation : std::uint8_t { add, subtract, reverse_subtract };

struct PipelineBlendState {
    bool enabled = false;
    bool alpha_to_coverage = false;
    PipelineBlendFactor source_color = PipelineBlendFactor::one;
    PipelineBlendFactor destination_color = PipelineBlendFactor::zero;
    PipelineBlendOperation color_operation = PipelineBlendOperation::add;
    PipelineBlendFactor source_alpha = PipelineBlendFactor::one;
    PipelineBlendFactor destination_alpha = PipelineBlendFactor::zero;
    PipelineBlendOperation alpha_operation = PipelineBlendOperation::add;
};

enum class PipelineCompareOperation : std::uint8_t {
    never,
    less,
    equal,
    less_or_equal,
    greater,
    not_equal,
    greater_or_equal,
    always,
};

struct PipelineDepthState {
    bool test_enabled = true;
    bool write_enabled = true;
    PipelineCompareOperation compare = PipelineCompareOperation::less_or_equal;
};

// A backend-neutral ABI for small per-draw transform constants. Vulkan maps
// this contract to vertex push constants. D3D12 maps it to root constants at
// b0, space0. Descriptor-backed frame and material resources remain separate.
enum class PipelineTransformContract : std::uint8_t {
    none,
    draw_matrices,
    // Draw matrices use the vertex constant path. The fragment color uses a
    // separate uniform/constant-buffer binding so the two values never share
    // storage on backends with a 128-byte push-constant minimum.
    selected_mesh,
};

enum class PipelineResourceKind : std::uint8_t { uniform_buffer, sampled_texture, sampler, storage_buffer };

struct PipelineResourceBinding {
    PipelineResourceKind kind = PipelineResourceKind::sampled_texture;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::string name;
};

struct PipelineProgram {
    std::string name;
    std::vector<PipelineShaderModule> shaders;
    PipelineVertexLayout vertex_layout;
    PipelineRenderTargets targets;
    PipelineRasterState raster;
    PipelineBlendState blend;
    PipelineDepthState depth;
    PipelineTransformContract transform_contract = PipelineTransformContract::none;
    std::vector<PipelineResourceBinding> resources;
};

struct PipelineLimits {
    std::size_t max_name_bytes = 256;
    std::size_t max_shader_stages = 3;
    std::size_t max_shader_module_bytes = 16U * 1024U * 1024U;
    std::size_t max_total_shader_bytes = 32U * 1024U * 1024U;
    std::size_t max_vertex_attributes = 16;
    std::size_t max_vertex_stride = 4096;
    // Numeric slots are bounded before a backend maps them to API locations.
    std::uint32_t max_vertex_location = 31;
    std::size_t max_color_targets = 8;
    std::size_t max_resources = 64;
    std::size_t max_resource_name_bytes = 256;
    std::uint32_t max_resource_set = 7;
    std::uint32_t max_resource_binding = 4095;
    // Header-only SPIR-V checks are intentional. Instruction and interface
    // validation belongs to the eventual backend shader compiler.
    std::uint32_t max_spirv_version = 0x00010600U;
    std::uint32_t max_spirv_id_bound = 1U << 24U;
    std::uint32_t max_dxbc_chunks = 4096U;
    std::size_t max_diagnostics = 128;
    std::size_t max_diagnostic_bytes = 64U * 1024U;
};

struct PipelineDiagnostic {
    enum class Severity : std::uint8_t { warning, error };
    Severity severity = Severity::error;
    std::string code;
    std::string message;
};

struct PipelineValidationResult {
    bool valid = false;
    bool shader_execution_supported = false;
    std::size_t shader_bytes = 0;
    std::vector<PipelineDiagnostic> diagnostics;
};

// Request used by the stock mapping helper. The material profile resolver is
// the source-evidenced authority for blend/A2C/transparency/depth/cull flags.
// Bytecode linking and executable shader creation remain staged.
struct StockPipelineRequest {
    MaterialInput material;
    NodeMaterialInput node;
    const MaterialOverride* override_values = nullptr;
    std::vector<PipelineShaderModule> shaders;
    PipelineRenderTargets targets;
    std::vector<PipelineResourceBinding> resources;
    bool wireframe = false;
};

struct StockPipelineResult {
    PipelineProgram program;
    MaterialRenderProfile profile;
    PipelineValidationResult validation;
};

[[nodiscard]] const char* pipeline_shader_stage_name(PipelineShaderStage stage) noexcept;
[[nodiscard]] const char* pipeline_shader_format_name(PipelineShaderFormat format) noexcept;
[[nodiscard]] PipelineShaderFormat detect_pipeline_shader_format(
    std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] const char* pipeline_vertex_semantic_name(PipelineVertexSemantic semantic) noexcept;
[[nodiscard]] const char* pipeline_render_target_format_name(PipelineRenderTargetFormat format) noexcept;

[[nodiscard]] PipelineValidationResult validate_pipeline(
    const PipelineProgram& program, const PipelineLimits& limits = {});

[[nodiscard]] StockPipelineResult build_stock_pipeline(
    const StockPipelineRequest& request, const PipelineLimits& limits = {});

} // namespace apex::render
