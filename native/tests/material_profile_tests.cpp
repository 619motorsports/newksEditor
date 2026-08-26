#include "apex/render/material_profile.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::render::MaterialInput;
using apex::render::MaterialOverride;
using apex::render::MaterialGlassMode;
using apex::render::NodeMaterialInput;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
        bytes[offset + shift / 8U] =
            static_cast<std::uint8_t>(value >> shift);
}

std::vector<std::uint8_t> dxbc(std::uint16_t program_type,
                               bool two_chunks = false) {
    const std::uint32_t chunk_count = two_chunks ? 2U : 1U;
    const std::uint32_t table_end = 32U + chunk_count * 4U;
    const std::uint32_t total_bytes = table_end + chunk_count * 12U;
    std::vector<std::uint8_t> bytes(total_bytes, 0U);
    bytes[0] = 'D';
    bytes[1] = 'X';
    bytes[2] = 'B';
    bytes[3] = 'C';
    put_u32(bytes, 20U, 1U);
    put_u32(bytes, 24U, total_bytes);
    put_u32(bytes, 28U, chunk_count);
    for (std::uint32_t index = 0U; index < chunk_count; ++index) {
        const std::uint32_t chunk_offset = table_end + index * 12U;
        put_u32(bytes, 32U + index * 4U, chunk_offset);
        const bool program = !two_chunks || index == 1U;
        const std::array<std::uint8_t, 4U> tag =
            program ? std::array<std::uint8_t, 4U>{'S', 'H', 'D', 'R'}
                    : std::array<std::uint8_t, 4U>{'S', 'T', 'A', 'T'};
        std::copy(tag.begin(), tag.end(), bytes.begin() + chunk_offset);
        put_u32(bytes, chunk_offset + 4U, 4U);
        put_u32(bytes, chunk_offset + 8U,
                program ? (static_cast<std::uint32_t>(program_type) << 16U) |
                              0x40U
                        : 0U);
    }
    return bytes;
}

std::vector<std::uint8_t> container(bool alpha_tested = false,
                                    std::uint32_t layout = 0U,
                                    bool geometry = false,
                                    bool two_vertex_chunks = false) {
    const auto vertex = dxbc(1U, two_vertex_chunks);
    const auto pixel = dxbc(0U);
    const auto geometry_stage = geometry ? dxbc(2U) : std::vector<std::uint8_t>{};
    std::vector<std::uint8_t> bytes = {
        2U, static_cast<std::uint8_t>(alpha_tested)};
    append_u32(bytes, layout);
    append_u32(bytes, static_cast<std::uint32_t>(vertex.size()));
    bytes.insert(bytes.end(), vertex.begin(), vertex.end());
    append_u32(bytes, static_cast<std::uint32_t>(pixel.size()));
    bytes.insert(bytes.end(), pixel.begin(), pixel.end());
    append_u32(bytes, static_cast<std::uint32_t>(geometry_stage.size()));
    bytes.insert(bytes.end(), geometry_stage.begin(), geometry_stage.end());
    return bytes;
}

void expects_profile_error(const auto& function, std::size_t offset,
                           std::string_view message) {
    bool threw = false;
    try {
        function();
    } catch (const apex::render::MaterialProfileError& error) {
        threw = true;
        require(error.offset() == offset, message);
    }
    require(threw, message);
}

void header_contract() {
    const auto bytes = container(true, 1);
    const auto header = apex::render::parse_stock_shader_container_header(bytes);
    require(header.version == 2, "container version");
    require(header.alpha_tested, "container A2C flag");
    require(header.vertex_layout == "skinned", "container vertex layout");
    require(header.vertex_bytes == 48U && header.pixel_bytes == 48U &&
                header.geometry_bytes == 0U,
            "container payload lengths");

    auto bad_layout = container(false, 9);
    bool threw = false;
    try {
        (void)apex::render::parse_stock_shader_container_header(bad_layout);
    } catch (const apex::render::MaterialProfileError& error) {
        threw = true;
        require(error.offset() == 2, "layout diagnostic offset");
    }
    require(threw, "unsupported layout must fail");

    threw = false;
    try {
        (void)apex::render::parse_stock_shader_container_header(
            std::span<const std::uint8_t>(bad_layout.data(), 5));
    } catch (const apex::render::MaterialProfileError& error) {
        threw = true;
        require(error.offset() == 0, "truncated diagnostic offset");
    }
    require(threw, "truncated container must fail");
}

void owned_container_contract() {
    using namespace apex::render;
    auto bytes = container(true, 1U, true);
    const auto parsed = parse_stock_shader_container(bytes);
    require(parsed.header.version == 2U && parsed.header.alpha_tested &&
                parsed.header.vertex_layout == "skinned",
            "owned container retains package metadata");
    require(parsed.vertex_shader.size() == 48U &&
                parsed.pixel_shader.size() == 48U &&
                parsed.geometry_shader.size() == 48U,
            "owned container extracts all declared stages");
    require(parsed.vertex_metadata.shader_model_major == 4U &&
                parsed.vertex_metadata.shader_model_minor == 0U &&
                parsed.vertex_metadata.chunk_count == 1U &&
                parsed.pixel_metadata.shader_model_major == 4U &&
                parsed.geometry_metadata.has_value() &&
                parsed.geometry_metadata->shader_model_major == 4U,
            "owned container reports bounded DXBC stage metadata");
    bytes.clear();
    require(parsed.vertex_shader[0] == 'D' && parsed.pixel_shader[0] == 'D' &&
                parsed.geometry_shader[0] == 'D',
            "owned container does not retain source spans");
}

void rejects_malformed_owned_containers() {
    using namespace apex::render;
    const auto valid = container();
    for (std::size_t size = 0U; size < valid.size(); ++size) {
        bool threw = false;
        try {
            (void)parse_stock_shader_container(
                std::span<const std::uint8_t>(valid.data(), size));
        } catch (const MaterialProfileError&) {
            threw = true;
        }
        require(threw,
                "every truncated stock-container prefix is rejected");
    }

    auto malformed = valid;
    malformed[1U] = 2U;
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); }, 1U,
        "non-boolean alpha-tested flag is rejected");

    malformed = valid;
    malformed.push_back(0U);
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); }, valid.size(),
        "stock-container trailing byte is rejected");

    StockShaderContainerLimits limits;
    limits.max_container_bytes = valid.size() - 1U;
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(valid, limits); }, 0U,
        "stock-container byte limit is enforced");
    limits = {};
    limits.max_stage_bytes = 47U;
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(valid, limits); }, 6U,
        "stock-stage byte limit is enforced");
    limits = {};
    limits.max_dxbc_chunks = 1U;
    const auto two_chunk_container = container(false, 0U, false, true);
    expects_profile_error(
        [&] {
            (void)parse_stock_shader_container(two_chunk_container, limits);
        },
        38U, "DXBC chunk-count limit is enforced");

    constexpr std::size_t vertex_offset = 10U;
    constexpr std::size_t pixel_offset = 62U;
    malformed = valid;
    put_u32(malformed, vertex_offset + 24U, 47U);
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); },
        vertex_offset + 24U,
        "DXBC declared size mismatch is rejected");

    malformed = valid;
    put_u32(malformed, vertex_offset + 32U, 32U);
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); },
        vertex_offset + 32U,
        "DXBC chunk-table overlap is rejected");

    malformed = valid;
    malformed[vertex_offset + 36U] = 'S';
    malformed[vertex_offset + 37U] = 'T';
    malformed[vertex_offset + 38U] = 'A';
    malformed[vertex_offset + 39U] = 'T';
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); }, vertex_offset,
        "DXBC executable chunk is required");

    malformed = valid;
    put_u32(malformed, pixel_offset + 44U, 0x00010040U);
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); },
        pixel_offset + 44U,
        "DXBC stage identity is enforced");

    malformed = container(false, 0U, false, true);
    put_u32(malformed, vertex_offset + 36U, 40U);
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); },
        vertex_offset + 40U,
        "overlapping DXBC chunks are rejected");

    malformed = container(false, 0U, true);
    const std::size_t geometry_offset = malformed.size() - 48U;
    put_u32(malformed, geometry_offset + 44U, 0x00010040U);
    expects_profile_error(
        [&] { (void)parse_stock_shader_container(malformed); },
        geometry_offset + 44U,
        "geometry DXBC stage identity is enforced");
}

void loads_stock_container_files_with_preallocation_bounds() {
    using namespace apex::render;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "apex-stock-shader-container-file-test.shader";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    const auto write_bytes = [&](std::span<const std::uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "stock file fixture opens");
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(output), "stock file fixture write completes");
    };

    const std::vector<std::uint8_t> valid = container();
    write_bytes(valid);
    const StockShaderContainerFileResult loaded =
        load_stock_shader_container_file(path);
    require(loaded.ok() && loaded.container->header.version == 2U &&
                loaded.container->vertex_shader.size() == 48U &&
                loaded.container->pixel_shader.size() == 48U,
            "bounded stock file loader owns a complete parsed package");

    StockShaderContainerLimits limits;
    limits.max_container_bytes = valid.size() - 1U;
    const StockShaderContainerFileResult oversized =
        load_stock_shader_container_file(path, limits);
    require(!oversized.ok() &&
                oversized.status ==
                    StockShaderContainerFileStatus::file_too_large &&
                oversized.diagnostic.code == "stock_shader_file_too_large",
            "stock file size limit rejects input before parsing");

    write_bytes(std::span<const std::uint8_t>(valid.data(), valid.size() - 1U));
    const StockShaderContainerFileResult truncated =
        load_stock_shader_container_file(path);
    require(!truncated.ok() &&
                truncated.status ==
                    StockShaderContainerFileStatus::malformed_package &&
                truncated.diagnostic.code ==
                    "stock_shader_file_malformed_package" &&
                !truncated.container.has_value(),
            "truncated stock file returns no parsed package");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const StockShaderContainerFileResult missing =
        load_stock_shader_container_file(path);
    require(!missing.ok() &&
                missing.status ==
                    StockShaderContainerFileStatus::not_regular_file,
            "missing stock file fails before allocation");
    const StockShaderContainerFileResult directory =
        load_stock_shader_container_file(
            std::filesystem::temp_directory_path());
    require(!directory.ok() &&
                directory.status ==
                    StockShaderContainerFileStatus::not_regular_file,
            "stock file loader rejects directories");
}

void parses_installed_stock_package_when_available() {
    const char* root = std::getenv("ASSETTO_CORSA_ROOT");
    if (root == nullptr || *root == '\0') return;
    const std::filesystem::path shader_root =
        std::filesystem::path(root) / "sdk" / "editor" / "system" /
        "shaders";
    if (!std::filesystem::is_directory(shader_root)) return;
    std::size_t parsed_count = 0U;
    std::size_t empty_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(shader_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".shader")
            continue;
        std::ifstream input(entry.path(), std::ios::binary);
        require(static_cast<bool>(input),
                "installed stock shader package opens");
        const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (bytes.empty()) {
            ++empty_count;
            continue;
        }
        const auto parsed = apex::render::parse_stock_shader_container(bytes);
        const auto* profile = apex::render::stock_shader_profile(
            entry.path().stem().string());
        require(profile != nullptr &&
                    profile->vertex_layout == parsed.header.vertex_layout &&
                    profile->alpha_tested == parsed.header.alpha_tested &&
                    parsed.geometry_shader.empty(),
                "installed package matches its stock catalog metadata");
        if (entry.path().filename() == "ksPerPixel.shader") {
            require(bytes.size() == 11'254U &&
                        parsed.vertex_shader.size() == 3'728U &&
                        parsed.pixel_shader.size() == 7'508U &&
                        parsed.vertex_metadata.shader_model_major == 4U &&
                        parsed.pixel_metadata.shader_model_major == 4U,
                    "installed ksPerPixel stage sizes match recovered evidence");
            require(apex::render::validate_stock_ks_per_pixel_container_shape(
                        parsed, apex::render::StockKsPerPixelVariant::base) ==
                        apex::render::StockKsPerPixelContainerStatus::ready,
                    "installed ksPerPixel package matches the native ABI shape");
            require(apex::render::validate_stock_ks_per_pixel_reflection(
                        parsed) ==
                        apex::render::StockKsPerPixelReflectionStatus::ready,
                    "installed ksPerPixel RDEF matches the native resource ABI");
            require(apex::render::validate_stock_ks_per_pixel_signatures(
                        parsed) ==
                        apex::render::StockKsPerPixelSignatureStatus::ready,
                    "installed ksPerPixel signatures match the native stage ABI");
            require(apex::render::validate_stock_ks_per_pixel_native_program(
                        parsed, apex::render::StockKsPerPixelVariant::base) ==
                        apex::render::StockKsPerPixelNativeProgramStatus::ready,
                    "installed ksPerPixel passes the complete native allocation gate");
            const auto owned =
                apex::render::create_validated_stock_ks_per_pixel_native_program(
                    parsed, apex::render::StockKsPerPixelVariant::base);
            require(owned.ok() && owned.program->vertex_shader().size() == 3'728U &&
                        owned.program->pixel_shader().size() == 7'508U,
                    "installed ksPerPixel creates the owned native program boundary");
        }
        if (entry.path().filename() == "ksPerPixelAT.shader") {
            require(bytes.size() == 11'330U &&
                        parsed.vertex_shader.size() == 3'728U &&
                        parsed.pixel_shader.size() == 7'584U,
                    "installed ksPerPixelAT stage sizes match recovered evidence");
            require(apex::render::validate_stock_ks_per_pixel_container_shape(
                        parsed,
                        apex::render::StockKsPerPixelVariant::alpha_to_coverage) ==
                        apex::render::StockKsPerPixelContainerStatus::ready,
                    "installed ksPerPixelAT package matches the native ABI shape");
            require(apex::render::validate_stock_ks_per_pixel_reflection(
                        parsed) ==
                        apex::render::StockKsPerPixelReflectionStatus::ready,
                    "installed ksPerPixelAT RDEF matches the native resource ABI");
            require(apex::render::validate_stock_ks_per_pixel_signatures(
                        parsed) ==
                        apex::render::StockKsPerPixelSignatureStatus::ready,
                    "installed ksPerPixelAT signatures match the native stage ABI");
            require(apex::render::validate_stock_ks_per_pixel_native_program(
                        parsed,
                        apex::render::StockKsPerPixelVariant::alpha_to_coverage) ==
                        apex::render::StockKsPerPixelNativeProgramStatus::ready,
                    "installed ksPerPixelAT passes the complete native allocation gate");
            const auto owned =
                apex::render::create_validated_stock_ks_per_pixel_native_program(
                    parsed,
                    apex::render::StockKsPerPixelVariant::alpha_to_coverage);
            require(owned.ok() && owned.program->vertex_shader().size() == 3'728U &&
                        owned.program->pixel_shader().size() == 7'584U,
                    "installed ksPerPixelAT creates the owned native program boundary");
        }
        ++parsed_count;
    }
    require(parsed_count == 79U && empty_count == 2U,
            "installed stock shader catalog matches the recovered package count");
}

void package_and_serialized_precedence() {
    using namespace apex::render;
    const auto package_only = resolve_material_render_profile({"ksPerPixelAT", 0, 0});
    require(package_only.alpha_to_coverage && package_only.effective_blend_mode == 2,
            "shader package defaults to A2C");
    require(package_only.blend_source == "shader-package" && !package_only.transparent,
            "package default source and transparency");
    require(!package_only.blend_enabled && package_only.cull == MaterialCullMode::back,
            "package default blend/cull");

    const auto alpha_override = resolve_material_render_profile({"ksPerPixelAT", 1, 0});
    require(!alpha_override.alpha_to_coverage && alpha_override.effective_blend_mode == 1,
            "serialized alpha blend overrides package A2C");
    require(alpha_override.blend_source == "kn5" && alpha_override.transparent && alpha_override.blend_enabled,
            "serialized alpha blend state");

    const auto explicit_coverage = resolve_material_render_profile({"ksPerPixel", 2, 0});
    require(explicit_coverage.alpha_to_coverage && explicit_coverage.blend_source == "kn5",
            "serialized A2C override");

    const auto sorted_opaque = resolve_material_render_profile({"ksPerPixel", 0, 0}, {true});
    require(sorted_opaque.transparent && !sorted_opaque.blend_enabled && !sorted_opaque.depth_write,
            "node transparency disables depth write independently of blend");
}

void csp_state_precedence_and_classification() {
    using namespace apex::render;
    MaterialOverride tree_override;
    tree_override.blend_mode = "ALPHA_BLEND";
    tree_override.depth_mode = "READ_ONLY";
    tree_override.cull_mode = "BACK";
    const auto tree = resolve_material_render_profile({"ksTree", 0, 0}, {}, &tree_override);
    require(!tree.alpha_to_coverage && tree.shadow_alpha_tested && tree.transparent,
            "CSP alpha blend and shadow cutout");
    require(tree.blend == "alpha" && tree.depth_test && !tree.depth_write,
            "CSP blend/depth override");
    require(tree.cull == MaterialCullMode::back && tree.cull_source == "override",
            "CSP cull override");

    MaterialOverride explicit_alpha;
    explicit_alpha.blend_mode = "ALPHA_TEST";
    explicit_alpha.cull_mode = "DOUBLE_SIDED";
    const auto custom = resolve_material_render_profile({"extensionCustom", 0, 0}, {}, &explicit_alpha);
    require(custom.alpha_to_coverage && custom.blend_source == "override" &&
                custom.cull == MaterialCullMode::none,
            "unknown shader explicit alpha/cull override");

    const auto windscreen = resolve_material_render_profile({"ksWindscreen", 1, 0}, {true});
    require(windscreen.glass_mode == MaterialGlassMode::windscreen && !windscreen.reflection_alpha &&
                !windscreen.refractive,
            "windscreen classification");

    const auto reflection = resolve_material_render_profile({"ksPerPixelReflection", 1, 0}, {true});
    require(reflection.glass_mode == MaterialGlassMode::reflection && reflection.reflection_alpha,
            "reflection glass classification");

    const auto broken = resolve_material_render_profile({"ksBrokenGlass", 1, 0}, {true});
    require(broken.glass_mode == MaterialGlassMode::broken_glass_csp_refraction && broken.broken_glass &&
                broken.reflection_alpha && broken.refractive,
            "broken glass classification");

    MaterialOverride configured;
    configured.shader = "smGlass";
    configured.properties.emplace("extrefraction", "0.03");
    const auto refractive = resolve_material_render_profile({"ksPerPixel", 1, 0}, {true}, &configured);
    require(refractive.glass_mode == MaterialGlassMode::refractive && refractive.refractive,
            "configured refraction classification");
}

void audit_contract() {
    using namespace apex::render;
    const std::vector<MaterialInput> materials = {
        {"ksTree", 2, 0}, {"ksPerPixelAT", 0, 0}, {"ksPerPixel", 1, 0}, {"customShader", 0, 0}};
    const auto audit = audit_material_shader_profiles(materials);
    require(audit.materials == 4 && audit.known_stock == 3, "audit material counts");
    require(audit.alpha_blend == 1 && audit.alpha_to_coverage == 2 && audit.shadow_cutout == 3,
            "audit blend counts");
    require(audit.package_defaults == 1 && audit.serialized_overrides == 2,
            "audit precedence counts");
    require(audit.unknown_shaders.size() == 1 && audit.unknown_shaders[0] == "customShader",
            "audit unknown shader list");
    require(audit.diagnostics.size() == 1 && audit.diagnostics[0].code == "unknown_shader",
            "unknown shader diagnostic");
    require(apex::render::stock_shader_profile(" KSTREE ") != nullptr &&
                apex::render::stock_shader_profile("unknown") == nullptr,
            "case-insensitive stock lookup");
    require(apex::render::stock_shader_profiles().size() == 81, "complete stock shader catalog");
}

} // namespace

int main() {
    try {
        header_contract();
        owned_container_contract();
        rejects_malformed_owned_containers();
        loads_stock_container_files_with_preallocation_bounds();
        parses_installed_stock_package_when_available();
        package_and_serialized_precedence();
        csp_state_precedence_and_classification();
        audit_contract();
        std::cout << "material profile tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "material profile tests failed: " << error.what() << '\n';
        return 1;
    }
}
