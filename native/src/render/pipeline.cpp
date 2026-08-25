#include "apex/render/pipeline.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace apex::render {
namespace {

struct DiagnosticSink {
    PipelineValidationResult& result;
    const PipelineLimits& limits;
    std::size_t bytes = 0;
    bool error_seen = false;

    void add(PipelineDiagnostic::Severity severity, std::string code, std::string message) {
        if (severity == PipelineDiagnostic::Severity::error) error_seen = true;
        const std::size_t cost = code.size() + message.size();
        if (result.diagnostics.size() >= limits.max_diagnostics ||
            cost > limits.max_diagnostic_bytes - std::min(bytes, limits.max_diagnostic_bytes))
            return;
        result.diagnostics.push_back({severity, std::move(code), std::move(message)});
        bytes += cost;
    }

    void error(std::string code, std::string message) {
        add(PipelineDiagnostic::Severity::error, std::move(code), std::move(message));
    }

    void warning(std::string code, std::string message) {
        add(PipelineDiagnostic::Severity::warning, std::move(code), std::move(message));
    }
};

[[nodiscard]] bool has_errors(const PipelineValidationResult& result) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const PipelineDiagnostic& diagnostic) {
        return diagnostic.severity == PipelineDiagnostic::Severity::error;
    });
}

[[nodiscard]] std::size_t attribute_bytes(PipelineVertexAttributeFormat format) noexcept {
    switch (format) {
    case PipelineVertexAttributeFormat::float32: return 4;
    case PipelineVertexAttributeFormat::float32x2: return 8;
    case PipelineVertexAttributeFormat::float32x3: return 12;
    case PipelineVertexAttributeFormat::float32x4: return 16;
    case PipelineVertexAttributeFormat::uint32x4: return 16;
    }
    return 0;
}

[[nodiscard]] bool valid_semantic(PipelineVertexSemantic semantic) noexcept {
    return static_cast<unsigned>(semantic) <= static_cast<unsigned>(PipelineVertexSemantic::bone_indices);
}

[[nodiscard]] bool valid_attribute_format(PipelineVertexAttributeFormat format) noexcept {
    return static_cast<unsigned>(format) <= static_cast<unsigned>(PipelineVertexAttributeFormat::uint32x4);
}

[[nodiscard]] bool valid_stage(PipelineShaderStage stage) noexcept {
    return stage == PipelineShaderStage::vertex || stage == PipelineShaderStage::fragment ||
           stage == PipelineShaderStage::geometry;
}

[[nodiscard]] bool valid_shader_format(PipelineShaderFormat format) noexcept {
    return format == PipelineShaderFormat::stock_container || format == PipelineShaderFormat::spirv ||
           format == PipelineShaderFormat::dxil;
}

[[nodiscard]] std::uint32_t u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool signature(std::span<const std::uint8_t> bytes, std::string_view value) noexcept {
    if (value.size() > bytes.size()) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (bytes[index] != static_cast<std::uint8_t>(value[index])) return false;
    }
    return true;
}

void validate_dxil(std::span<const std::uint8_t> bytes, DiagnosticSink& diagnostics) {
    // DXIL is commonly carried by a DXBC container. Checking the container's
    // bounded chunk table catches truncated/untrusted input without parsing
    // or executing the shader language.
    if (bytes.size() < 32 || !signature(bytes, "DXBC")) {
        diagnostics.error("shader_bytecode_truncated", "DXIL/DXBC shader module is truncated or has no DXBC signature");
        return;
    }
    const std::uint32_t chunk_count = u32_le(bytes, 28);
    if (chunk_count == 0U) {
        diagnostics.error("shader_bytecode_malformed", "DXBC container has no shader chunks");
        return;
    }
    if (chunk_count > (bytes.size() - 32U) / 4U) {
        diagnostics.error("shader_bytecode_bounds", "DXBC chunk table exceeds shader module bounds");
        return;
    }
    const std::size_t table_end = 32U + static_cast<std::size_t>(chunk_count) * 4U;
    const std::size_t declared_size = static_cast<std::size_t>(u32_le(bytes, 24));
    if (declared_size < table_end || declared_size > bytes.size()) {
        diagnostics.error("shader_bytecode_size", "DXBC declared size does not contain its table or exceeds supplied bytes");
        return;
    }
    for (std::uint32_t index = 0; index < chunk_count; ++index) {
        const std::size_t table_offset = 32U + static_cast<std::size_t>(index) * 4U;
        const std::size_t chunk_offset = u32_le(bytes, table_offset);
        if (chunk_offset < table_end || chunk_offset > declared_size || declared_size - chunk_offset < 8U) {
            diagnostics.error("shader_bytecode_bounds", "DXBC chunk offset exceeds shader module bounds");
            return;
        }
        const std::size_t chunk_bytes = u32_le(bytes, chunk_offset + 4U);
        if (chunk_bytes > declared_size - chunk_offset - 8U) {
            diagnostics.error("shader_bytecode_bounds", "DXBC chunk payload exceeds shader module bounds");
            return;
        }
    }
}

void validate_spirv(std::span<const std::uint8_t> bytes, DiagnosticSink& diagnostics,
                    std::uint32_t max_version, std::uint32_t max_id_bound) {
    if (bytes.size() < 20U || bytes.size() % 4U != 0U || u32_le(bytes, 0) != 0x07230203U) {
        diagnostics.error("shader_bytecode_truncated", "SPIR-V module is truncated, unaligned, or has an invalid magic value");
        return;
    }
    const std::uint32_t version = u32_le(bytes, 4);
    if (version < 0x00010000U || version > max_version || (version & 0xffU) != 0U) {
        diagnostics.error("spirv_version", "SPIR-V header version is outside the supported bounded range");
    }
    if (u32_le(bytes, 16) != 0U)
        diagnostics.error("spirv_reserved_word", "SPIR-V header reserved word must be zero");
    const std::uint32_t id_bound = u32_le(bytes, 12);
    if (id_bound == 0U)
        diagnostics.error("spirv_id_bound", "SPIR-V header ID bound must be nonzero");
    else if (id_bound > max_id_bound)
        diagnostics.error("spirv_id_bound_limit", "SPIR-V header ID bound exceeds the configured limit");
}

[[nodiscard]] bool valid_target_format(PipelineRenderTargetFormat format, bool depth) noexcept {
    if (depth) {
        return format == PipelineRenderTargetFormat::depth32_float ||
               format == PipelineRenderTargetFormat::depth24_stencil8;
    }
    return format == PipelineRenderTargetFormat::rgba8_unorm || format == PipelineRenderTargetFormat::rgba8_srgb ||
           format == PipelineRenderTargetFormat::bgra8_unorm || format == PipelineRenderTargetFormat::bgra8_srgb ||
           format == PipelineRenderTargetFormat::rgba16_float;
}

[[nodiscard]] bool valid_cull(PipelineCullMode mode) noexcept {
    return mode == PipelineCullMode::none || mode == PipelineCullMode::front || mode == PipelineCullMode::back;
}

[[nodiscard]] bool valid_fill(PipelineFillMode mode) noexcept {
    return mode == PipelineFillMode::solid || mode == PipelineFillMode::wireframe;
}

[[nodiscard]] bool valid_front_face(PipelineFrontFace face) noexcept {
    return face == PipelineFrontFace::counter_clockwise || face == PipelineFrontFace::clockwise;
}

[[nodiscard]] bool valid_blend_factor(PipelineBlendFactor factor) noexcept {
    return static_cast<unsigned>(factor) <= static_cast<unsigned>(PipelineBlendFactor::one_minus_destination_alpha);
}

[[nodiscard]] bool valid_blend_operation(PipelineBlendOperation operation) noexcept {
    return operation == PipelineBlendOperation::add || operation == PipelineBlendOperation::subtract ||
           operation == PipelineBlendOperation::reverse_subtract;
}

[[nodiscard]] bool valid_compare(PipelineCompareOperation operation) noexcept {
    return static_cast<unsigned>(operation) <= static_cast<unsigned>(PipelineCompareOperation::always);
}

[[nodiscard]] bool valid_transform_contract(PipelineTransformContract contract) noexcept {
    return contract == PipelineTransformContract::none ||
           contract == PipelineTransformContract::draw_matrices ||
           contract == PipelineTransformContract::selected_mesh;
}

[[nodiscard]] bool valid_resource_kind(PipelineResourceKind kind) noexcept {
    return static_cast<unsigned>(kind) <= static_cast<unsigned>(PipelineResourceKind::storage_buffer);
}

[[nodiscard]] PipelineVertexLayout stock_layout(std::string_view name) {
    PipelineVertexLayout layout;
    if (name == "mesh") {
        layout.stride = 44;
        layout.attributes = {{PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0, 0},
                             {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3, 1, 12},
                             {PipelineVertexSemantic::texcoord0, PipelineVertexAttributeFormat::float32x2, 2, 24},
                             {PipelineVertexSemantic::tangent, PipelineVertexAttributeFormat::float32x3, 3, 32}};
    } else if (name == "skinned") {
        layout = stock_layout("mesh");
        layout.stride = 76;
        layout.attributes.push_back({PipelineVertexSemantic::bone_weights, PipelineVertexAttributeFormat::float32x4, 4, 44});
        layout.attributes.push_back({PipelineVertexSemantic::bone_indices, PipelineVertexAttributeFormat::float32x4, 5, 60});
    }
    return layout;
}

void append_diagnostic(PipelineValidationResult& result, const PipelineLimits& limits,
                       PipelineDiagnostic::Severity severity, std::string code, std::string message) {
    if (severity == PipelineDiagnostic::Severity::error) result.valid = false;
    const std::size_t used = [&] {
        std::size_t total = 0;
        for (const auto& diagnostic : result.diagnostics) total += diagnostic.code.size() + diagnostic.message.size();
        return total;
    }();
    const std::size_t cost = code.size() + message.size();
    if (result.diagnostics.size() >= limits.max_diagnostics || cost > limits.max_diagnostic_bytes - std::min(used, limits.max_diagnostic_bytes))
        return;
    result.diagnostics.push_back({severity, std::move(code), std::move(message)});
}

} // namespace

const char* pipeline_shader_stage_name(PipelineShaderStage stage) noexcept {
    switch (stage) {
    case PipelineShaderStage::vertex: return "vertex";
    case PipelineShaderStage::fragment: return "fragment";
    case PipelineShaderStage::geometry: return "geometry";
    }
    return "unknown";
}

const char* pipeline_shader_format_name(PipelineShaderFormat format) noexcept {
    switch (format) {
    case PipelineShaderFormat::unknown: return "unknown";
    case PipelineShaderFormat::stock_container: return "stock-container";
    case PipelineShaderFormat::spirv: return "spirv";
    case PipelineShaderFormat::dxil: return "dxil";
    }
    return "unknown";
}

const char* pipeline_vertex_semantic_name(PipelineVertexSemantic semantic) noexcept {
    switch (semantic) {
    case PipelineVertexSemantic::position: return "position";
    case PipelineVertexSemantic::normal: return "normal";
    case PipelineVertexSemantic::texcoord0: return "texcoord0";
    case PipelineVertexSemantic::texcoord1: return "texcoord1";
    case PipelineVertexSemantic::tangent: return "tangent";
    case PipelineVertexSemantic::color: return "color";
    case PipelineVertexSemantic::bone_weights: return "bone_weights";
    case PipelineVertexSemantic::bone_indices: return "bone_indices";
    }
    return "unknown";
}

const char* pipeline_render_target_format_name(PipelineRenderTargetFormat format) noexcept {
    switch (format) {
    case PipelineRenderTargetFormat::unknown: return "unknown";
    case PipelineRenderTargetFormat::rgba8_unorm: return "rgba8_unorm";
    case PipelineRenderTargetFormat::rgba8_srgb: return "rgba8_srgb";
    case PipelineRenderTargetFormat::bgra8_unorm: return "bgra8_unorm";
    case PipelineRenderTargetFormat::bgra8_srgb: return "bgra8_srgb";
    case PipelineRenderTargetFormat::rgba16_float: return "rgba16_float";
    case PipelineRenderTargetFormat::depth32_float: return "depth32_float";
    case PipelineRenderTargetFormat::depth24_stencil8: return "depth24_stencil8";
    }
    return "unknown";
}

PipelineValidationResult validate_pipeline(const PipelineProgram& program, const PipelineLimits& limits) {
    PipelineValidationResult result;
    DiagnosticSink diagnostics{result, limits};
    if (program.name.empty()) diagnostics.error("invalid_name", "pipeline name must not be empty");
    if (program.name.size() > limits.max_name_bytes) diagnostics.error("name_limit", "pipeline name exceeds the configured limit");

    if (program.shaders.empty() || program.shaders.size() > limits.max_shader_stages)
        diagnostics.error("shader_stage_limit", "pipeline shader stage count is outside the configured limit");
    std::set<PipelineShaderStage> stages;
    bool stock_package = false;
    bool stock_geometry = false;
    const std::size_t shader_count = std::min(program.shaders.size(), limits.max_shader_stages);
    for (std::size_t shader_index = 0; shader_index < shader_count; ++shader_index) {
        const PipelineShaderModule& module = program.shaders[shader_index];
        if (!valid_stage(module.stage)) {
            diagnostics.error("invalid_shader_stage", "pipeline contains an unknown shader stage");
            continue;
        }
        if (!stages.insert(module.stage).second) diagnostics.error("duplicate_shader_stage", "pipeline contains a duplicate shader stage");
        if (!valid_shader_format(module.format)) {
            diagnostics.error("invalid_shader_format", "pipeline contains an unknown shader bytecode format");
            continue;
        }
        if (module.bytes.empty() || module.bytes.size() > limits.max_shader_module_bytes) {
            diagnostics.error("shader_bytecode_limit", "shader module is empty or exceeds the configured byte limit");
            continue;
        }
        if (result.shader_bytes > limits.max_total_shader_bytes - std::min(result.shader_bytes, limits.max_total_shader_bytes) ||
            module.bytes.size() > limits.max_total_shader_bytes - result.shader_bytes) {
            diagnostics.error("shader_total_limit", "total shader bytecode exceeds the configured limit");
            continue;
        }
        result.shader_bytes += module.bytes.size();
        const std::span<const std::uint8_t> bytes(module.bytes.data(), module.bytes.size());
        if (module.format == PipelineShaderFormat::spirv)
            validate_spirv(bytes, diagnostics, limits.max_spirv_version, limits.max_spirv_id_bound);
        else if (module.format == PipelineShaderFormat::dxil) validate_dxil(bytes, diagnostics);
        else {
            try {
                const StockShaderContainerHeader header = parse_stock_shader_container_header(bytes);
                stock_package = true;
                stock_geometry = header.geometry_bytes != 0;
                if (module.stage != PipelineShaderStage::vertex)
                    diagnostics.error("stock_stage", "a stock shader container must be supplied as the vertex program module");
                if (header.vertex_layout.empty()) diagnostics.error("stock_layout", "stock shader container has no vertex layout");
            } catch (const MaterialProfileError& error) {
                diagnostics.error("shader_bytecode_malformed", error.what());
            }
        }
    }
    if (!stages.contains(PipelineShaderStage::vertex) && !stock_package)
        diagnostics.error("missing_vertex_stage", "pipeline requires a vertex shader stage");
    const bool depth_only_stage_contract = program.targets.colors.empty() &&
                                           program.targets.has_depth;
    if (!stages.contains(PipelineShaderStage::fragment) && !stock_package &&
        (!depth_only_stage_contract || !stages.contains(PipelineShaderStage::vertex) ||
         stages.size() != 1U))
        diagnostics.error("missing_fragment_stage", "pipeline requires a fragment shader stage");
    if (stock_package && stock_geometry && !stages.contains(PipelineShaderStage::geometry))
        diagnostics.warning("stock_geometry_embedded", "stock container carries a geometry stage; backend extraction remains staged");

    if (program.vertex_layout.stride == 0 || program.vertex_layout.stride > limits.max_vertex_stride)
        diagnostics.error("vertex_stride", "vertex layout stride is zero or exceeds the configured limit");
    if (program.vertex_layout.attributes.empty() || program.vertex_layout.attributes.size() > limits.max_vertex_attributes)
        diagnostics.error("vertex_attribute_limit", "vertex attribute count is outside the configured limit");
    std::set<std::uint32_t> locations;
    std::set<PipelineVertexSemantic> semantics;
    const std::size_t attribute_count = std::min(program.vertex_layout.attributes.size(), limits.max_vertex_attributes);
    for (std::size_t attribute_index = 0; attribute_index < attribute_count; ++attribute_index) {
        const auto& attribute = program.vertex_layout.attributes[attribute_index];
        if (!valid_semantic(attribute.semantic) || !valid_attribute_format(attribute.format)) {
            diagnostics.error("invalid_vertex_attribute", "vertex attribute uses an unknown semantic or format");
            continue;
        }
        const std::size_t bytes = attribute_bytes(attribute.format);
        if (bytes == 0 || attribute.offset % 4U != 0U ||
            static_cast<std::size_t>(attribute.offset) > program.vertex_layout.stride ||
            bytes > static_cast<std::size_t>(program.vertex_layout.stride) - attribute.offset)
            diagnostics.error("vertex_attribute_bounds", "vertex attribute exceeds the declared vertex stride or alignment");
        if (attribute.location > limits.max_vertex_location)
            diagnostics.error("vertex_location_limit", "vertex attribute location exceeds the configured limit");
        if (!locations.insert(attribute.location).second) diagnostics.error("duplicate_vertex_location", "vertex attribute locations must be unique");
        if (!semantics.insert(attribute.semantic).second) diagnostics.error("duplicate_vertex_semantic", "vertex attribute semantics must be unique");
    }

    if (program.targets.colors.size() > limits.max_color_targets)
        diagnostics.error("render_target_limit", "color render target count exceeds the configured limit");
    const std::size_t color_count = std::min(program.targets.colors.size(), limits.max_color_targets);
    for (std::size_t color_index = 0; color_index < color_count; ++color_index) {
        const auto& target = program.targets.colors[color_index];
        if (!valid_target_format(target.format, false)) diagnostics.error("invalid_color_format", "color render target format is unknown or depth-only");
        if (target.samples == 0 || target.samples > 16U || (target.samples & (target.samples - 1U)) != 0U)
            diagnostics.error("invalid_sample_count", "render target sample count must be a power of two from 1 through 16");
    }
    if (!program.targets.colors.empty() && program.targets.colors.front().samples != 0U) {
        for (std::size_t color_index = 0; color_index < color_count; ++color_index)
            if (program.targets.colors[color_index].samples != program.targets.colors.front().samples)
                diagnostics.error("sample_count_mismatch", "all color targets must use the same sample count");
    }
    if (program.targets.has_depth) {
        if (!valid_target_format(program.targets.depth.format, true)) diagnostics.error("invalid_depth_format", "depth target format is not a depth format");
        if (program.targets.depth.samples == 0 || program.targets.depth.samples > 16U ||
            (program.targets.depth.samples & (program.targets.depth.samples - 1U)) != 0U)
            diagnostics.error("invalid_sample_count", "depth target sample count must be a power of two from 1 through 16");
        if (!program.targets.colors.empty() && program.targets.depth.samples != program.targets.colors.front().samples)
            diagnostics.error("sample_count_mismatch", "depth and color targets must use the same sample count");
    } else if (program.depth.test_enabled || program.depth.write_enabled) {
        diagnostics.error("depth_target_missing", "depth state requires a depth render target");
    }
    if (program.targets.colors.empty() && !program.targets.has_depth)
        diagnostics.error("render_target_missing", "pipeline must declare a color or depth render target");

    if (!valid_cull(program.raster.cull) || !valid_fill(program.raster.fill)) diagnostics.error("invalid_raster_state", "raster state contains an unknown cull or fill mode");
    if (!valid_front_face(program.raster.front_face)) diagnostics.error("invalid_front_face", "raster state contains an unknown front-face winding");
    if (program.blend.enabled || program.blend.alpha_to_coverage) {
        if (program.targets.colors.empty()) diagnostics.error("blend_target_missing", "blend state requires a color render target");
        if (program.blend.alpha_to_coverage) {
            const std::uint32_t samples = program.targets.colors.empty() ? 1U : program.targets.colors.front().samples;
            if (samples <= 1U) diagnostics.error("a2c_sample_count", "alpha-to-coverage requires multisample color output");
        }
    }
    if (!valid_blend_factor(program.blend.source_color) || !valid_blend_factor(program.blend.destination_color) ||
        !valid_blend_factor(program.blend.source_alpha) || !valid_blend_factor(program.blend.destination_alpha) ||
        !valid_blend_operation(program.blend.color_operation) || !valid_blend_operation(program.blend.alpha_operation))
        diagnostics.error("invalid_blend_state", "blend state contains an unknown factor or operation");
    if (!valid_compare(program.depth.compare)) diagnostics.error("invalid_depth_state", "depth state contains an unknown comparison operation");
    if (!valid_transform_contract(program.transform_contract))
        diagnostics.error("invalid_transform_contract", "pipeline transform contract is unknown");

    if (program.resources.size() > limits.max_resources) diagnostics.error("resource_limit", "pipeline resource count exceeds the configured limit");
    std::set<std::pair<std::uint32_t, std::uint32_t>> bindings;
    std::set<std::string> resource_names;
    const std::size_t resource_count = std::min(program.resources.size(), limits.max_resources);
    for (std::size_t resource_index = 0; resource_index < resource_count; ++resource_index) {
        const auto& resource = program.resources[resource_index];
        if (!valid_resource_kind(resource.kind)) diagnostics.error("invalid_resource_kind", "pipeline resource uses an unknown kind");
        if (resource.set > limits.max_resource_set)
            diagnostics.error("resource_set_limit", "pipeline resource set exceeds the configured limit");
        if (resource.binding > limits.max_resource_binding)
            diagnostics.error("resource_binding_limit", "pipeline resource binding exceeds the configured limit");
        const bool valid_name = !resource.name.empty() && resource.name.size() <= limits.max_resource_name_bytes;
        if (!valid_name)
            diagnostics.error("resource_name", "pipeline resource name is empty or exceeds the configured limit");
        if (!bindings.insert({resource.set, resource.binding}).second) diagnostics.error("duplicate_resource_binding", "pipeline resource set/binding pairs must be unique");
        if (valid_name && !resource_names.insert(resource.name).second) diagnostics.error("duplicate_resource_name", "pipeline resource names must be unique");
    }
    result.valid = !diagnostics.error_seen;
    // The contract validates module bytes and state only. Creating executable
    // backend shader objects remains an explicit, source-agnostic later stage.
    result.shader_execution_supported = false;
    return result;
}

StockPipelineResult build_stock_pipeline(const StockPipelineRequest& request, const PipelineLimits& limits) {
    StockPipelineResult output;
    const char* preflight_code = nullptr;
    const char* preflight_message = nullptr;
    const auto override_string_too_large = [&](const std::optional<std::string>& value) {
        return value.has_value() && value->size() > limits.max_name_bytes;
    };
    if (request.material.shader.size() > limits.max_name_bytes ||
        (request.override_values != nullptr &&
         (override_string_too_large(request.override_values->shader) ||
          override_string_too_large(request.override_values->blend_mode) ||
          override_string_too_large(request.override_values->depth_mode) ||
          override_string_too_large(request.override_values->cull_mode)))) {
        preflight_code = "name_limit";
        preflight_message = "stock pipeline shader name exceeds the configured limit";
    } else if (request.shaders.size() > limits.max_shader_stages) {
        preflight_code = "shader_stage_limit";
        preflight_message = "stock pipeline shader stage count exceeds the configured limit";
    } else if (request.targets.colors.size() > limits.max_color_targets) {
        preflight_code = "render_target_limit";
        preflight_message = "stock pipeline color target count exceeds the configured limit";
    } else if (request.resources.size() > limits.max_resources) {
        preflight_code = "resource_limit";
        preflight_message = "stock pipeline resource count exceeds the configured limit";
    } else {
        std::size_t shader_bytes = 0;
        for (const auto& shader : request.shaders) {
            if (shader.bytes.size() > limits.max_shader_module_bytes) {
                preflight_code = "shader_bytecode_limit";
                preflight_message = "stock pipeline shader module exceeds the configured byte limit";
                break;
            }
            if (shader.bytes.size() > limits.max_total_shader_bytes - std::min(shader_bytes, limits.max_total_shader_bytes) ||
                shader_bytes > limits.max_total_shader_bytes - shader.bytes.size()) {
                preflight_code = "shader_total_limit";
                preflight_message = "stock pipeline shader bytecode exceeds the configured total limit";
                break;
            }
            shader_bytes += shader.bytes.size();
        }
        if (preflight_code == nullptr) {
            for (const auto& resource : request.resources) {
                if (resource.name.empty() || resource.name.size() > limits.max_resource_name_bytes) {
                    preflight_code = "resource_name";
                    preflight_message = "stock pipeline resource name is empty or exceeds the configured limit";
                    break;
                }
            }
        }
    }
    if (preflight_code != nullptr) {
        // Do not validate a synthetic empty program here. Its generic missing
        // stage/layout diagnostics could consume a one-entry budget and hide
        // the untrusted input limit that caused this preflight rejection.
        output.validation.valid = false;
        output.validation.shader_execution_supported = false;
        append_diagnostic(output.validation, limits, PipelineDiagnostic::Severity::error, preflight_code, preflight_message);
        output.validation.valid = false;
        return output;
    }
    output.profile = resolve_material_render_profile(request.material, request.node, request.override_values);
    output.program.name = output.profile.shader;
    output.program.shaders = request.shaders;
    output.program.targets = request.targets;
    output.program.resources = request.resources;
    output.program.raster.fill = request.wireframe ? PipelineFillMode::wireframe : PipelineFillMode::solid;
    output.program.raster.cull = output.profile.cull == MaterialCullMode::none
                                     ? PipelineCullMode::none
                                     : output.profile.cull == MaterialCullMode::front ? PipelineCullMode::front : PipelineCullMode::back;
    output.program.blend.enabled = output.profile.blend_enabled;
    output.program.blend.alpha_to_coverage = output.profile.alpha_to_coverage;
    if (output.program.blend.enabled) {
        // public/app.js applyItemRenderState is the source authority. WebGL
        // glBlendFunc applies the same named factors to color and alpha. The
        // alpha factors below express the equivalent component behavior.
        if (output.profile.blend == "multiply") {
            output.program.blend.source_color = PipelineBlendFactor::destination_color;
            output.program.blend.destination_color = PipelineBlendFactor::zero;
            output.program.blend.source_alpha = PipelineBlendFactor::destination_alpha;
            output.program.blend.destination_alpha = PipelineBlendFactor::zero;
        } else if (output.profile.blend == "transparent-as-black") {
            output.program.blend.source_color = PipelineBlendFactor::one;
            output.program.blend.destination_color = PipelineBlendFactor::one_minus_source_alpha;
            output.program.blend.source_alpha = PipelineBlendFactor::one;
            output.program.blend.destination_alpha = PipelineBlendFactor::one_minus_source_alpha;
        } else {
            output.program.blend.source_color = PipelineBlendFactor::source_alpha;
            output.program.blend.destination_color = PipelineBlendFactor::one_minus_source_alpha;
            output.program.blend.source_alpha = PipelineBlendFactor::source_alpha;
            output.program.blend.destination_alpha = PipelineBlendFactor::one_minus_source_alpha;
        }
    }
    output.program.depth.test_enabled = output.profile.depth_test;
    output.program.depth.write_enabled = output.profile.depth_write;

    if (output.profile.stock != nullptr) {
        output.program.vertex_layout = stock_layout(output.profile.stock->vertex_layout);
    }

    output.validation = validate_pipeline(output.program, limits);
    if (output.profile.stock == nullptr) {
        append_diagnostic(output.validation, limits, PipelineDiagnostic::Severity::error, "unknown_shader", "stock material shader is not in the known shader catalog");
    } else {
        if (output.program.vertex_layout.attributes.empty())
            append_diagnostic(output.validation, limits, PipelineDiagnostic::Severity::warning, "stock_layout_staged", "stock shader vertex layout is known by name but not declaratively mapped yet");
        for (const auto& module : output.program.shaders) {
            if (module.format != PipelineShaderFormat::stock_container) continue;
            try {
                const StockShaderContainerHeader header = parse_stock_shader_container_header(
                    std::span<const std::uint8_t>(module.bytes.data(), module.bytes.size()));
                if (header.vertex_layout != output.profile.stock->vertex_layout)
                    append_diagnostic(output.validation, limits, PipelineDiagnostic::Severity::error, "stock_layout_mismatch", "stock shader package layout does not match its catalog profile");
            } catch (const MaterialProfileError&) {
                // validate_pipeline already supplied the bounded malformed
                // bytecode diagnostic; do not duplicate it here.
            }
        }
    }
    append_diagnostic(output.validation, limits, PipelineDiagnostic::Severity::warning, "depth_compare_staged", "stock profile proves depth test/write flags; the depth comparison operation is backend policy");
    append_diagnostic(output.validation, limits, PipelineDiagnostic::Severity::warning, "shader_execution_staged", "shader linking, resource reflection, and execution are not implemented by this backend-neutral contract");
    output.validation.valid = output.validation.valid && !has_errors(output.validation);
    return output;
}

} // namespace apex::render
