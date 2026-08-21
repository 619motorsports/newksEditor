#include "apex/render/material_profile.hpp"

#include <array>
#include <cstdint>
#include <iostream>
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

std::array<std::uint8_t, 26> container(bool alpha_tested = false, std::uint32_t layout = 0) {
    std::array<std::uint8_t, 26> bytes{};
    bytes[0] = 2;
    bytes[1] = static_cast<std::uint8_t>(alpha_tested);
    bytes[2] = static_cast<std::uint8_t>(layout);
    bytes[6] = 4;
    bytes[10] = 'D';
    bytes[11] = 'X';
    bytes[12] = 'B';
    bytes[13] = 'C';
    bytes[14] = 4;
    bytes[18] = 'D';
    bytes[19] = 'X';
    bytes[20] = 'B';
    bytes[21] = 'C';
    return bytes;
}

void header_contract() {
    const auto bytes = container(true, 1);
    const auto header = apex::render::parse_stock_shader_container_header(bytes);
    require(header.version == 2, "container version");
    require(header.alpha_tested, "container A2C flag");
    require(header.vertex_layout == "skinned", "container vertex layout");
    require(header.vertex_bytes == 4 && header.pixel_bytes == 4 && header.geometry_bytes == 0,
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
