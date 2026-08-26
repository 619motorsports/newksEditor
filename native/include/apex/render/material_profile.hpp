#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

// These values intentionally match the values stored in a KN5 material.  A
// value outside this set is retained in serialized_blend_mode, but is not
// treated as alpha blend or alpha-to-coverage by the state resolver.
enum class MaterialBlendMode : std::uint8_t {
    opaque = 0,
    alpha = 1,
    alpha_to_coverage = 2,
};

enum class MaterialCullMode : std::uint8_t {
    back,
    front,
    none,
};

enum class MaterialGlassMode : std::uint8_t {
    none,
    windscreen,
    reflection,
    refractive,
    broken_glass_csp_refraction,
};

struct StockShaderProfile {
    std::string_view name;
    bool alpha_tested = false;
    std::string_view vertex_layout = "mesh";
};

// The returned pointer refers to immutable process-lifetime catalog storage.
// Shader names are matched case-insensitively after surrounding whitespace is
// removed, just as the reference editor does. Unknown names return nullptr.
[[nodiscard]] const StockShaderProfile* stock_shader_profile(std::string_view name) noexcept;
[[nodiscard]] std::span<const StockShaderProfile> stock_shader_profiles() noexcept;

struct StockShaderContainerHeader {
    std::uint8_t version = 0;
    bool alpha_tested = false;
    std::string_view vertex_layout = "mesh";
    std::uint32_t vertex_bytes = 0;
    std::uint32_t pixel_bytes = 0;
    std::uint32_t geometry_bytes = 0;
};

struct StockShaderContainerLimits {
    std::size_t max_container_bytes = 16U * 1024U * 1024U;
    std::size_t max_stage_bytes = 16U * 1024U * 1024U;
    std::uint32_t max_dxbc_chunks = 4096U;
};

struct StockShaderStageMetadata {
    std::uint8_t shader_model_major = 0U;
    std::uint8_t shader_model_minor = 0U;
    std::uint32_t chunk_count = 0U;
};

// Own the extracted stage programs so a later D3D12 handoff cannot retain
// spans into a temporary file buffer. Vulkan still requires explicit SPIR-V;
// this type does not translate DXBC or claim cross-backend shader parity.
struct StockShaderContainer {
    StockShaderContainerHeader header;
    StockShaderStageMetadata vertex_metadata;
    StockShaderStageMetadata pixel_metadata;
    std::optional<StockShaderStageMetadata> geometry_metadata;
    std::vector<std::uint8_t> vertex_shader;
    std::vector<std::uint8_t> pixel_shader;
    std::vector<std::uint8_t> geometry_shader;
};

enum class StockShaderContainerFileStatus : std::uint8_t {
    ready,
    invalid_limits,
    invalid_path,
    not_regular_file,
    file_size_unavailable,
    file_too_large,
    open_failed,
    read_failed,
    file_size_changed,
    malformed_package,
    allocation_failed,
};

struct StockShaderContainerFileDiagnostic {
    std::filesystem::path path;
    std::string code;
    std::string message;
    std::size_t offset = 0U;
};

struct StockShaderContainerFileResult {
    StockShaderContainerFileStatus status =
        StockShaderContainerFileStatus::invalid_path;
    StockShaderContainerFileDiagnostic diagnostic;
    std::optional<StockShaderContainer> container;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockShaderContainerFileStatus::ready &&
               container.has_value();
    }
};

class MaterialProfileError final : public std::runtime_error {
public:
    MaterialProfileError(std::string message, std::size_t offset);

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_ = 0;
};

// Decode the v2 stock .shader container header. Payloads are only inspected
// for their DXBC signatures and bounds; no GPU API objects are created here.
[[nodiscard]] StockShaderContainerHeader parse_stock_shader_container_header(
    std::span<const std::uint8_t> bytes);

// Extract and validate a complete v2 package. This stricter boundary rejects
// trailing bytes, non-boolean flags, malformed DXBC chunk tables, overlapping
// chunks, and stage programs whose embedded type does not match their slot.
[[nodiscard]] StockShaderContainer parse_stock_shader_container(
    std::span<const std::uint8_t> bytes,
    const StockShaderContainerLimits& limits = {});

// Load one explicitly selected port package. This path checks the file type
// and size before allocation, then applies the complete bounded container
// parser. It does not reproduce the original editor's .shader marker and
// separate win/*_vs.fxo and win/*_ps.fxo discovery path.
[[nodiscard]] StockShaderContainerFileResult load_stock_shader_container_file(
    const std::filesystem::path& path,
    const StockShaderContainerLimits& limits = {});

struct MaterialInput {
    std::string shader;
    // KN5 stores 0 (opaque), 1 (alpha blend), or 2 (A2C). Keep the integer so
    // malformed/forward-compatible values remain visible to diagnostics.
    int serialized_blend_mode = 0;
    int depth_mode = 0;
};

struct NodeMaterialInput {
    bool transparent = false;
};

struct MaterialOverride {
    std::optional<std::string> shader;
    std::optional<std::string> blend_mode;
    std::optional<std::string> depth_mode;
    std::optional<std::string> cull_mode;
    std::optional<bool> is_transparent;
    // CSP property values are represented textually because INI values are
    // textual at the boundary. The resolver parses only extrefraction.
    std::map<std::string, std::string> properties;
};

struct MaterialRenderProfile {
    std::string shader;
    const StockShaderProfile* stock = nullptr;
    int serialized_blend_mode = 0;
    int native_blend_mode = 0;
    int effective_blend_mode = 0;
    std::string blend_source;
    bool alpha_to_coverage = false;
    bool shadow_alpha_tested = false;
    bool transparent = false;
    bool blend_enabled = false;
    std::string blend = "opaque";
    std::string blend_mode;
    std::string depth_mode;
    bool depth_test = true;
    bool depth_write = true;
    MaterialCullMode cull = MaterialCullMode::back;
    std::string cull_source = "default";
    bool windscreen = false;
    bool broken_glass = false;
    bool reflection_alpha = false;
    bool refractive = false;
    MaterialGlassMode glass_mode = MaterialGlassMode::none;
};

[[nodiscard]] MaterialRenderProfile resolve_material_render_profile(
    const MaterialInput& material, const NodeMaterialInput& node = {},
    const MaterialOverride* override_values = nullptr);

[[nodiscard]] inline MaterialRenderProfile resolve_material_render_profile(
    const MaterialInput& material, const NodeMaterialInput& node,
    const MaterialOverride& override_values) {
    return resolve_material_render_profile(material, node, &override_values);
}

struct MaterialShaderDiagnostic {
    std::string code;
    std::string shader;
    std::string message;
};

struct MaterialShaderAudit {
    std::size_t materials = 0;
    std::size_t known_stock = 0;
    std::size_t alpha_blend = 0;
    std::size_t alpha_to_coverage = 0;
    std::size_t shadow_cutout = 0;
    std::size_t package_defaults = 0;
    std::size_t serialized_overrides = 0;
    std::size_t windscreens = 0;
    std::size_t reflection_glass = 0;
    std::size_t refractive = 0;
    std::vector<std::string> unknown_shaders;
    std::vector<MaterialShaderDiagnostic> diagnostics;
};

[[nodiscard]] MaterialShaderAudit audit_material_shader_profiles(
    std::span<const MaterialInput> materials);

[[nodiscard]] const char* material_cull_mode_name(MaterialCullMode mode) noexcept;
[[nodiscard]] const char* material_glass_mode_name(MaterialGlassMode mode) noexcept;

} // namespace apex::render
