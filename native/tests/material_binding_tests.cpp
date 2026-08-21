#include "apex/render/material_binding.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool has_diagnostic(const MaterialBinding& binding, std::string_view code) {
    for (const auto& diagnostic : binding.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

void stock_binding_and_defaults() {
    Kn5Material material;
    material.name = "Body";
    material.shader = "ksPerPixel";
    material.properties.push_back({"ksAmbient", 0.42F, {}, {}, {}});
    material.resources.push_back({"txDiffuse", 0, "body.dds"});

    const MaterialBinding binding = build_material_binding(material, 1);
    require(binding.status == MaterialBindingStatus::complete, "complete stock binding status");
    require(binding.profile.stock != nullptr && binding.profile.shader == "ksPerPixel",
            "stock shader profile classification");
    require(!binding.shader_execution_supported && has_diagnostic(binding, "shader_execution_staged"),
            "pixel execution remains explicitly staged");
    const auto* diffuse = find_material_texture(binding, "TXDIFFUSE");
    require(diffuse != nullptr && diffuse->kind == MaterialTextureKind::embedded &&
                diffuse->bind_point == 0U && diffuse->texture == "body.dds",
            "embedded texture slot binding");
    require(material_scalar(binding, "ksAmbient", 0) == 0.42F, "KN5 scalar property");
    require(material_scalar(binding, "ksDiffuse", 0) == 0.8F, "stock diffuse default");
    require(material_scalar(binding, "ksSpecularEXP", 0) == 30.0F, "stock specular exponent default");
}

void transparent_glass_and_csp_overrides() {
    Kn5Material material;
    material.name = "Glass";
    material.shader = "ksPerPixel";
    material.serialized_blend_mode = 1;
    material.transparent = true;
    material.resources.push_back({"txDiffuse", 0, "glass.dds"});

    MaterialBindingOverrides overrides;
    overrides.shader = "smGlass";
    overrides.blend_mode = "ALPHA_BLEND";
    overrides.cull_mode = "DOUBLE_SIDED";
    overrides.is_transparent = true;
    overrides.properties.emplace("extRefraction", MaterialPropertyOverride::scalar_value(0.03F));
    overrides.properties.emplace("ksDiffuse", MaterialPropertyOverride::vector3_value({0.2F, 0.4F, 0.6F}));
    overrides.resources.emplace("txDiffuse", MaterialTextureOverride{
                                                    std::nullopt, "", "", std::array<float, 4>{1, 0.5F, 0.25F, 1}});

    const MaterialBinding binding = build_material_binding(material, 1, &overrides);
    require(binding.profile.shader == "smGlass" && binding.profile.transparent &&
                binding.profile.blend == "alpha" && binding.profile.cull == MaterialCullMode::none,
            "CSP state overrides feed render profile");
    require(binding.profile.refractive && binding.profile.glass_mode == MaterialGlassMode::refractive,
            "CSP refraction classification");
    const auto* diffuse = find_material_texture(binding, "txDiffuse");
    require(diffuse != nullptr && diffuse->kind == MaterialTextureKind::solid_color && diffuse->color[1] == 0.5F,
            "CSP solid-color resource override");
    require(material_scalar(binding, "ksDiffuse", 0) == 0.2F &&
                material_vector4(binding, "ksDiffuse", {0, 0, 0, 0})[3] == 0.6F,
            "CSP vector property precedence");
}

void missing_and_duplicate_resources() {
    Kn5Material tyres;
    tyres.name = "Tyres";
    tyres.shader = "ksTyres";
    tyres.resources.push_back({"txDiffuse", 0, "tyre.dds"});
    const MaterialBinding incomplete = build_material_binding(tyres, 1);
    require(incomplete.status == MaterialBindingStatus::incomplete, "missing tyre resources status");
    for (const std::string_view slot : {"txNormal", "txDirty", "txBlur", "txNormalBlur"}) {
        const auto* texture = find_material_texture(incomplete, slot);
        require(texture != nullptr && texture->kind == MaterialTextureKind::missing && texture->required,
                "missing required texture is explicit");
    }
    require(has_diagnostic(incomplete, "missing_texture"), "missing texture diagnostic");

    Kn5Material duplicate;
    duplicate.shader = "ksPerPixel";
    duplicate.resources = {{"txDiffuse", 0, "a.dds"}, {"TXDIFFUSE", 0, "b.dds"}};
    bool threw = false;
    try {
        (void)build_material_binding(duplicate, 1);
    } catch (const MaterialBindingError& error) {
        threw = true;
        require(error.code() == "duplicate_resource", "duplicate resource diagnostic code");
    }
    require(threw, "duplicate resource must be rejected");
}

void unknown_and_bind_point_references() {
    Kn5Material unknown;
    unknown.name = "Extension";
    unknown.shader = "extensionCustom";
    const MaterialBinding unknownBinding = build_material_binding(unknown, 0);
    require(unknownBinding.status == MaterialBindingStatus::unsupported && unknownBinding.profile.stock == nullptr,
            "unknown shader status");
    require(has_diagnostic(unknownBinding, "unknown_shader"), "unknown shader diagnostic");

    // The serialized integer is a shader bind point, not a model texture
    // index. txDamageMask=21 is valid even when the model has 21 textures
    // (whose indices are 0 through 20).
    Kn5Material damage;
    damage.shader = "ksPerPixelMultiMap_damage_dirt";
    damage.resources.push_back({"txDamage", 0, "damage.dds"});
    damage.resources.push_back({"txDamageMask", 21, "damage_mask.dds"});
    const MaterialBinding binding = build_material_binding(damage, 21);
    const auto* mask = find_material_texture(binding, "txDamageMask");
    require(mask != nullptr && mask->bind_point == 21U &&
                binding.status == MaterialBindingStatus::complete,
            "KN5 shader bind point is not texture-table indexed");
}

void external_override_and_limits() {
    Kn5Material material;
    material.shader = "ksPerPixel";
    material.properties.push_back({"ksDiffuse", 0.8F, {}, {}, {}});
    material.resources.push_back({"txDiffuse", 0, "body.dds"});
    MaterialBindingOverrides overrides;
    overrides.resources.emplace("txNormal", MaterialTextureOverride{std::nullopt, "", "textures/body_nm.dds", std::nullopt});
    const MaterialBinding binding = build_material_binding(material, 1, &overrides);
    const auto* normal = find_material_texture(binding, "txNormal");
    require(normal != nullptr && normal->kind == MaterialTextureKind::external_file &&
                normal->file == "textures/body_nm.dds" && has_diagnostic(binding, "external_resource_staged"),
            "external resource is staged without pretending it is loaded");

    MaterialBindingOverrides required_external;
    required_external.resources.emplace("txDiffuse", MaterialTextureOverride{
                                                        std::nullopt, "", "textures/body.dds", std::nullopt});
    const MaterialBinding incomplete = build_material_binding(material, 1, &required_external);
    require(incomplete.status == MaterialBindingStatus::incomplete,
            "unresolved required external resource is incomplete");

    MaterialBindingLimits limits;
    limits.max_properties = 0;
    bool threw = false;
    try {
        (void)build_material_binding(material, 1, nullptr, limits);
    } catch (const MaterialBindingError& error) {
        threw = true;
        require(error.code() == "property_limit", "property limit diagnostic code");
    }
    require(threw, "property limit must be enforced");

    limits = {};
    limits.max_string_bytes = 3;
    threw = false;
    try {
        (void)build_material_binding(material, 1, nullptr, limits);
    } catch (const MaterialBindingError& error) {
        threw = true;
        require(error.code() == "string_limit", "string limit diagnostic code");
    }
    require(threw, "string limit must be enforced");
}

void resolves_ks_per_pixel_defaults_and_kn5_values() {
    Kn5Material material;
    material.shader = "ksPerPixel";
    material.properties = {
        {"ksAmbient", 0.42F, {}, {}, {}},
        {"ksDiffuse", 0.63F, {}, {}, {}},
        {"ksSpecular", 0.17F, {}, {}, {}},
        {"ksSpecularEXP", 47.0F, {}, {}, {}},
        {"fresnelC", 0.02F, {}, {}, {}},
        {"fresnelEXP", 6.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.13F, {}, {}, {}},
        {"ksEmissive", 0.0F, {}, {32.0F, 64.0F, 128.0F}, {}},
    };
    const MaterialBinding binding = build_material_binding(material, 0);
    const auto resolved = resolve_ks_per_pixel_material_constants(binding);
    require(resolved.ok(), "KN5 ksPerPixel constants resolve");
    require(resolved.constants.lighting == std::array<float, 4>{0.42F, 0.63F, 0.17F, 47.0F},
            "KN5 lighting values preserve source order");
    require(resolved.constants.fresnel == std::array<float, 4>{0.02F, 6.0F, 0.13F, 0.0F},
            "KN5 fresnel values preserve opaque alpha semantics");
    require(resolved.constants.emissive[0] == 32.0F / 255.0F &&
                resolved.constants.emissive[1] == 64.0F / 255.0F &&
                resolved.constants.emissive[2] == 128.0F / 255.0F &&
                resolved.constants.emissive[3] == 0.0F,
            "KN5 emissive values normalize the WebGL >16 representation");

    Kn5Material defaults;
    defaults.shader = "ksPerPixel";
    const auto default_result = resolve_ks_per_pixel_material_constants(
        build_material_binding(defaults, 0));
    require(default_result.ok() &&
                default_result.constants.lighting == std::array<float, 4>{0.35F, 0.8F, 0.2F, 30.0F} &&
                default_result.constants.fresnel == std::array<float, 4>{0.0F, 5.0F, 0.05F, 0.0F} &&
                default_result.constants.emissive == std::array<float, 4>{0, 0, 0, 0},
            "ksPerPixel defaults match the production binder");
}

void adapts_the_bounded_kn5_parser_model() {
    apex::formats::Kn5Material parsed;
    parsed.name = "Body";
    parsed.shader = "ksPerPixel";
    parsed.serializedBlendMode = 0U;
    parsed.depthMode = 1U;
    parsed.properties = {
        {"ksDiffuse", 0.61F, {}, {}, {}},
        {"ksEmissive", 0.0F, {}, {4.0F, 5.0F, 6.0F}, {}},
    };
    parsed.resources = {{"txDiffuse", 21U, "body.dds"}};
    const MaterialBinding binding = build_material_binding(parsed, false, 1U);
    const auto constants = resolve_ks_per_pixel_material_constants(binding);
    require(constants.ok() && constants.constants.lighting[1] == 0.61F &&
                constants.constants.emissive ==
                    std::array<float, 4>{4.0F, 5.0F, 6.0F, 0.0F} &&
                find_material_texture(binding, "txdiffuse") != nullptr &&
                find_material_texture(binding, "txdiffuse")->bind_point == 21U,
            "the parser model feeds the portable material resolver without a duplicate caller adapter");

    parsed.depthMode = std::numeric_limits<std::uint32_t>::max();
    bool threw = false;
    try {
        (void)build_material_binding(parsed, false, 1U);
    } catch (const MaterialBindingError& error) {
        threw = true;
        require(error.code() == "material_state_range",
                "out-of-range parsed material state diagnostic");
    }
    require(threw, "out-of-range parsed material state is rejected");
}

void resolves_ks_per_pixel_csp_precedence_and_alpha_capture() {
    Kn5Material material;
    material.shader = "ksPerPixel";
    material.properties = {
        {"ksAmbient", 0.1F, {}, {}, {}},
        {"ksEmissive", 0.0F, {}, {1.0F, 2.0F, 3.0F}, {}},
        {"ksAlphaRef", 0.2F, {}, {}, {}},
    };
    MaterialBindingOverrides overrides;
    overrides.properties.emplace("ksAmbient", MaterialPropertyOverride::scalar_value(0.9F));
    overrides.properties.emplace("ksEmissive",
                                 MaterialPropertyOverride::vector4_value({64.0F, 128.0F, 192.0F, 0.5F}));
    overrides.properties.emplace("ksAlphaRef", MaterialPropertyOverride::scalar_value(0.2F));
    const MaterialBinding binding = build_material_binding(material, 0, &overrides);

    const auto ordinary = resolve_ks_per_pixel_material_constants(binding);
    require(ordinary.ok() && ordinary.constants.lighting[0] == 0.9F &&
                ordinary.constants.emissive[0] == 64.0F / 255.0F * 0.5F &&
                ordinary.constants.emissive[1] == 128.0F / 255.0F * 0.5F &&
                ordinary.constants.emissive[2] == 192.0F / 255.0F * 0.5F &&
                ordinary.constants.fresnel[3] == 0.0F,
            "CSP values override KN5 values and ordinary alpha remains zero");

    const auto capture = resolve_ks_per_pixel_material_constants(
        binding, KsPerPixelMaterialResolveOptions{true, true});
    require(capture.ok() && capture.constants.fresnel[3] == 0.5F,
            "capture alpha-to-coverage applies the production minimum alpha reference");
}

void rejects_invalid_ks_per_pixel_constants() {
    Kn5Material material;
    material.shader = "ksPerPixel";
    material.properties.push_back({"ksDiffuse", std::numeric_limits<float>::quiet_NaN(), {}, {}, {}});
    bool threw = false;
    try {
        (void)build_material_binding(material, 0);
    } catch (const MaterialBindingError& error) {
        threw = true;
        require(error.code() == "non_finite_property", "KN5 non-finite property diagnostic");
    }
    require(threw, "KN5 non-finite property is rejected");

    material.properties.clear();
    MaterialBindingOverrides overrides;
    overrides.properties.emplace("ksDiffuse",
                                 MaterialPropertyOverride::scalar_value(
                                     std::numeric_limits<float>::infinity()));
    threw = false;
    try {
        (void)build_material_binding(material, 0, &overrides);
    } catch (const MaterialBindingError& error) {
        threw = true;
        require(error.code() == "non_finite_property", "CSP non-finite property diagnostic");
    }
    require(threw, "CSP non-finite property is rejected");

    const MaterialBinding valid = build_material_binding(material, 0);
    auto malformed = valid;
    malformed.properties.at("ksdiffuse").scalar = std::numeric_limits<float>::quiet_NaN();
    const auto resolved = resolve_ks_per_pixel_material_constants(malformed);
    require(!resolved.ok() &&
                resolved.status == KsPerPixelMaterialResolveStatus::invalid_input &&
                resolved.diagnostic.code == "non_finite_constants",
            "resolver rejects non-finite values even after binding construction");

    malformed.shader = "ksPerPixelNM";
    const auto unsupported = resolve_ks_per_pixel_material_constants(malformed);
    require(!unsupported.ok() &&
                unsupported.status == KsPerPixelMaterialResolveStatus::unsupported &&
                unsupported.diagnostic.code == "ks_per_pixel_shader_unsupported",
            "resolver does not relabel unsupported stock shader variants");
}

} // namespace

int main() {
    try {
        stock_binding_and_defaults();
        transparent_glass_and_csp_overrides();
        missing_and_duplicate_resources();
        unknown_and_bind_point_references();
        external_override_and_limits();
        resolves_ks_per_pixel_defaults_and_kn5_values();
        adapts_the_bounded_kn5_parser_model();
        resolves_ks_per_pixel_csp_precedence_and_alpha_capture();
        rejects_invalid_ks_per_pixel_constants();
        std::cout << "material binding tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "material binding tests failed: " << error.what() << '\n';
        return 1;
    }
}
