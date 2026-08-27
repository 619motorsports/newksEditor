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
    damage.resources.push_back({"txDiffuse", 0, "body.dds"});
    damage.resources.push_back({"txNormal", 1, "body_nm.dds"});
    damage.resources.push_back({"txMaps", 2, "body_maps.dds"});
    damage.resources.push_back({"txDamage", 0, "damage.dds"});
    damage.resources.push_back({"txDamageMask", 21, "damage_mask.dds"});
    const MaterialBinding binding = build_material_binding(damage, 21);
    const auto* mask = find_material_texture(binding, "txDamageMask");
    require(mask != nullptr && mask->bind_point == 21U &&
                binding.status == MaterialBindingStatus::complete,
            "KN5 shader bind point is not texture-table indexed");
}

void resolves_bounded_dirt_zero_damage_variant() {
    Kn5Material material;
    material.shader = "ksPerPixelMultiMap_damage_dirt";
    material.properties = {
        {"damageZones", 0.0F, {}, {}, {1.0F, 0.5F, 0.25F, 0.0F}},
        {"dirt", 0.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 0U, "body.dds"},
        {"txNormal", 1U, "body_nm.dds"},
        {"txMaps", 2U, "body_maps.dds"},
        {"txDamage", 4U, "damage.dds"},
        {"txDamageMask", 21U, "damage_mask.dds"},
        {"txDust", 5U, "dust.dds"},
    };
    const MaterialBinding binding = build_material_binding(material, 6U);
    const auto resolved = resolve_ks_per_pixel_material_constants(binding);
    require(binding.status == MaterialBindingStatus::complete && resolved.ok() &&
                resolved.constants.damage_zones ==
                    std::array<float, 4>{1.0F, 0.5F, 0.25F, 0.0F},
            "damage_dirt resolves the authored dirt-zero stage constants");

    material.properties.push_back({"useDetail", 1.0F, {}, {}, {}});
    const auto detail = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 6U));
    require(!detail.ok() &&
                detail.status == KsPerPixelMaterialResolveStatus::unsupported &&
                detail.diagnostic.code == "ks_per_pixel_damage_detail_unsupported",
            "damage_dirt does not silently ignore the stock detail branch");
    material.properties.pop_back();

    material.properties.push_back({"sunSpecular", 12.0F, {}, {}, {}});
    const auto sun_specular = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 6U));
    require(!sun_specular.ok() &&
                sun_specular.status == KsPerPixelMaterialResolveStatus::unsupported &&
                sun_specular.diagnostic.code ==
                    "ks_per_pixel_damage_sun_specular_unsupported",
            "damage_dirt does not silently ignore the stock sun-specular branch");

    MaterialBinding malformed_sun_specular = build_material_binding(material, 6U);
    malformed_sun_specular.properties.at("sunspecular").scalar =
        std::numeric_limits<float>::quiet_NaN();
    const auto non_finite_sun_specular = resolve_ks_per_pixel_material_constants(
        malformed_sun_specular);
    require(!non_finite_sun_specular.ok() &&
                non_finite_sun_specular.status ==
                    KsPerPixelMaterialResolveStatus::invalid_input &&
                non_finite_sun_specular.diagnostic.code == "non_finite_constants",
            "damage_dirt rejects a non-finite sun-specular value");
    material.properties.pop_back();

    material.properties[1].value = 0.25F;
    const auto dirty = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 6U));
    require(!dirty.ok() &&
                dirty.status == KsPerPixelMaterialResolveStatus::unsupported &&
                dirty.diagnostic.code == "ks_per_pixel_damage_dirt_unsupported",
            "damage_dirt rejects the unrecovered nonzero dirt branch");

    material.properties.erase(material.properties.begin() + 1);
    const auto missing_property = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 6U));
    require(!missing_property.ok() &&
                missing_property.diagnostic.code ==
                    "ks_per_pixel_damage_properties_missing",
            "damage_dirt requires an authored dirt property");
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

void rejects_oversized_state_overrides() {
    Kn5Material material;
    material.shader = "ksPerPixel";
    MaterialBindingLimits limits;
    limits.max_string_bytes = 32U;

    const auto require_rejected = [&](MaterialBindingOverrides overrides,
                                      std::string_view message) {
        bool threw = false;
        try {
            (void)build_material_binding(material, 0, &overrides, limits);
        } catch (const MaterialBindingError& error) {
            threw = true;
            require(error.code() == "string_limit", message);
        }
        require(threw, message);
    };

    const std::string oversized(33U, 'x');
    MaterialBindingOverrides shader;
    shader.shader = oversized;
    require_rejected(shader, "oversized CSP shader override is rejected");

    MaterialBindingOverrides blend;
    blend.blend_mode = oversized;
    require_rejected(blend, "oversized CSP blend-mode override is rejected");

    MaterialBindingOverrides depth;
    depth.depth_mode = oversized;
    require_rejected(depth, "oversized CSP depth-mode override is rejected");

    MaterialBindingOverrides cull;
    cull.cull_mode = oversized;
    require_rejected(cull, "oversized CSP cull-mode override is rejected");
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

    defaults.shader = "ksPerPixelAT";
    defaults.resources.push_back({"txDiffuse", 0U, "alpha.dds"});
    const MaterialBinding alpha_tested = build_material_binding(defaults, 1);
    const auto alpha_tested_result =
        resolve_ks_per_pixel_material_constants(alpha_tested);
    require(alpha_tested.status == MaterialBindingStatus::complete &&
                alpha_tested.profile.alpha_to_coverage &&
                alpha_tested_result.ok() &&
                alpha_tested_result.constants.lighting ==
                    default_result.constants.lighting,
            "ksPerPixelAT reuses the exact diffuse-only material ABI");
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

void resolves_stock_shadow_caster_material_constants() {
    Kn5Material material;
    material.shader = "ksPerPixelAT";
    material.properties = {
        {"ksAmbient", 0.42F, {}, {}, {}},
        {"ksDiffuse", 0.63F, {}, {}, {}},
        {"ksSpecular", 0.17F, {}, {}, {}},
        {"ksSpecularEXP", 47.0F, {}, {}, {}},
        {"ksEmissive", 0.0F, {}, {32.0F, 64.0F, 128.0F}, {}},
    };
    const auto default_alpha = resolve_stock_shadow_caster_material_constants(
        build_material_binding(material, 0));
    require(default_alpha.ok() &&
                default_alpha.constants.lighting ==
                    std::array<float, 4>{0.42F, 0.63F, 0.17F, 47.0F} &&
                default_alpha.constants.emissive_and_alpha_ref ==
                    std::array<float, 4>{32.0F / 255.0F, 64.0F / 255.0F,
                                         128.0F / 255.0F, 0.0F},
            "stock shadow constants preserve KN5 values and use zero alpha fallback");

    material.properties.push_back({"ksAlphaRef", 0.2F, {}, {}, {}});
    MaterialBindingOverrides overrides;
    overrides.properties.emplace(
        "ksDiffuse", MaterialPropertyOverride::scalar_value(0.9F));
    overrides.properties.emplace(
        "ksEmissive",
        MaterialPropertyOverride::vector4_value({64.0F, 128.0F, 192.0F, 0.5F}));
    overrides.properties.emplace(
        "ksAlphaRef", MaterialPropertyOverride::scalar_value(0.75F));
    const auto overridden = resolve_stock_shadow_caster_material_constants(
        build_material_binding(material, 0, &overrides));
    require(overridden.ok() && overridden.constants.lighting[1] == 0.9F &&
                overridden.constants.emissive_and_alpha_ref ==
                    std::array<float, 4>{64.0F / 255.0F * 0.5F,
                                         128.0F / 255.0F * 0.5F,
                                         192.0F / 255.0F * 0.5F, 0.75F},
            "stock shadow constants preserve CSP precedence and emissive strength");
}

void rejects_invalid_stock_shadow_caster_alpha_reference() {
    Kn5Material material;
    material.shader = "ksPerPixelAT";
    material.properties.push_back({"ksAlphaRef", 0.5F, {}, {}, {}});

    MaterialBinding non_finite = build_material_binding(material, 0);
    non_finite.properties.at("ksalpharef").scalar =
        std::numeric_limits<float>::quiet_NaN();
    const auto non_finite_result =
        resolve_stock_shadow_caster_material_constants(non_finite);
    require(!non_finite_result.ok() &&
                non_finite_result.status ==
                    StockShadowCasterMaterialResolveStatus::invalid_input &&
                non_finite_result.diagnostic.code == "non_finite_constants",
            "stock shadow constants reject non-finite alpha reference");

    for (const float value : {-0.001F, 1.001F}) {
        MaterialBinding out_of_range = build_material_binding(material, 0);
        out_of_range.properties.at("ksalpharef").scalar = value;
        const auto result =
            resolve_stock_shadow_caster_material_constants(out_of_range);
        require(!result.ok() &&
                    result.status ==
                        StockShadowCasterMaterialResolveStatus::invalid_input &&
                    result.diagnostic.code == "ks_alpha_ref_range" &&
                    result.diagnostic.field == "ksAlphaRef",
                "stock shadow constants reject alpha references outside [0, 1]");
    }
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
    auto skinned = valid;
    skinned.shader = "ksSkinnedMesh";
    const auto skinned_resolved =
        resolve_ks_per_pixel_material_constants(skinned);
    require(skinned_resolved.ok(),
            "ksSkinnedMesh reuses the bounded diffuse-only material constants");
    auto malformed = valid;
    malformed.properties.at("ksdiffuse").scalar = std::numeric_limits<float>::quiet_NaN();
    const auto resolved = resolve_ks_per_pixel_material_constants(malformed);
    require(!resolved.ok() &&
                resolved.status == KsPerPixelMaterialResolveStatus::invalid_input &&
                resolved.diagnostic.code == "non_finite_constants",
            "resolver rejects non-finite values even after binding construction");

    malformed.shader = "ksPerPixelReflection";
    const auto unsupported = resolve_ks_per_pixel_material_constants(malformed);
    require(!unsupported.ok() &&
                unsupported.status == KsPerPixelMaterialResolveStatus::unsupported &&
                unsupported.diagnostic.code == "ks_per_pixel_shader_unsupported",
            "resolver does not relabel unsupported stock shader variants");
}

void resolves_bounded_tangent_space_nm_variant() {
    Kn5Material material;
    material.shader = "ksPerPixelNM";
    material.properties = {
        {"ksSpecular", 0.4F, {}, {}, {}},
        {"ksSpecularEXP", 20.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.0F, {}, {}, {}},
        {"nmObjectSpace", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 21U, "body.dds"},
        {"txNormal", 22U, "body_nm.dds"},
    };
    const auto resolved = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 2U));
    require(resolved.ok() && resolved.constants.lighting[2] == 0.4F &&
                resolved.constants.lighting[3] == 20.0F &&
                resolved.constants.fresnel[2] == 0.0F,
            "bounded tangent-space ksPerPixelNM constants resolve");

    material.properties.back().value = 1.0F;
    const auto object_space = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 2U));
    require(!object_space.ok() &&
                object_space.status == KsPerPixelMaterialResolveStatus::unsupported &&
                object_space.diagnostic.code ==
                    "ks_per_pixel_nm_object_space_unsupported",
            "object-space normal maps remain explicit");

    material.properties.back().value = 0.0F;
    MaterialBinding non_finite_binding = build_material_binding(material, 2U);
    non_finite_binding.properties["nmobjectspace"].scalar =
        std::numeric_limits<float>::quiet_NaN();
    const auto non_finite_space = resolve_ks_per_pixel_material_constants(
        non_finite_binding);
    require(!non_finite_space.ok() &&
                non_finite_space.status == KsPerPixelMaterialResolveStatus::invalid_input &&
                non_finite_space.diagnostic.code == "non_finite_constants",
            "non-finite normal-space mode is rejected");

    material.properties.back().value = 0.0F;
    material.properties[2].value = 0.05F;
    const auto fresnel = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 2U));
    require(!fresnel.ok() &&
                fresnel.status == KsPerPixelMaterialResolveStatus::unsupported &&
                fresnel.diagnostic.code == "ks_per_pixel_nm_fresnel_unsupported",
            "normal-map Fresnel reflection remains explicit");
}

void resolves_bounded_base_multimap_families() {
    Kn5Material material;
    material.shader = "ksPerPixelMultiMap";
    material.properties = {
        {"ksSpecular", 0.4F, {}, {}, {}},
        {"ksSpecularEXP", 20.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.0F, {}, {}, {}},
        {"nmObjectSpace", 0.0F, {}, {}, {}},
        {"useDetail", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 21U, "body.dds"},
        {"txNormal", 22U, "body_nm.dds"},
        {"txMaps", 23U, "body_maps.dds"},
    };
    const MaterialBinding complete = build_material_binding(material, 3U);
    const auto resolved = resolve_ks_per_pixel_material_constants(complete);
    require(complete.status == MaterialBindingStatus::complete && resolved.ok() &&
                resolved.constants.lighting[2] == 0.4F &&
                resolved.constants.lighting[3] == 20.0F,
            "bounded base MultiMap requires and resolves its three-resource stack");

    MaterialBindingOverrides overrides;
    overrides.properties.emplace(
        "ksSpecular", MaterialPropertyOverride::scalar_value(0.7F));
    overrides.properties.emplace(
        "ksSpecularEXP", MaterialPropertyOverride::scalar_value(8.0F));
    const auto overridden = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 3U, &overrides));
    require(overridden.ok() && overridden.constants.lighting[2] == 0.7F &&
                overridden.constants.lighting[3] == 8.0F,
            "base MultiMap keeps CSP specular overrides in the maps equation");

    material.shader = "ksPerPixelMultiMap_AT";
    const MaterialBinding at_binding = build_material_binding(material, 3U);
    const auto at_result = resolve_ks_per_pixel_material_constants(at_binding);
    require(at_binding.status == MaterialBindingStatus::complete && at_result.ok(),
            "bounded base AT MultiMap uses the same three-resource constants path");

    Kn5Material missing = material;
    missing.resources.pop_back();
    const MaterialBinding incomplete = build_material_binding(missing, 2U);
    const auto incomplete_result =
        resolve_ks_per_pixel_material_constants(incomplete);
    require(incomplete.status == MaterialBindingStatus::incomplete &&
                !incomplete_result.ok() &&
                incomplete_result.diagnostic.code ==
                    "ks_per_pixel_multimap_resources_incomplete",
            "base MultiMap rejects a missing txMaps resource");

    material.properties[4].value = 1.0F;
    const auto detail = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 3U));
    require(!detail.ok() &&
                detail.diagnostic.code ==
                    "ks_per_pixel_multimap_detail_unsupported",
            "eight-binding base MultiMap does not silently ignore an active detail stack");

    material.properties[4].value = 0.0F;
    material.properties[3].value = 1.0F;
    const auto object_space = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 3U));
    require(!object_space.ok() &&
                object_space.diagnostic.code ==
                    "ks_per_pixel_nm_object_space_unsupported",
            "base MultiMap keeps object-space normals outside the bounded path");

    material.properties[3].value = 0.0F;
    material.properties[2].value = 0.05F;
    const auto fresnel = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 3U));
    require(!fresnel.ok() &&
                fresnel.diagnostic.code ==
                    "ks_per_pixel_nm_fresnel_unsupported",
            "base MultiMap keeps maps.b reflection outside the bounded path");
    const auto reflection_enabled = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 3U), {false, false, true});
    require(reflection_enabled.ok() &&
                reflection_enabled.constants.fresnel[2] == 0.05F,
            "base MultiMap accepts Fresnel only with the explicit portable reflection contract");
}

void resolves_multimap_reflection_controls_separately() {
    Kn5Material material;
    material.shader = "ksPerPixelMultiMap";
    material.properties = {
        {"fresnelC", 0.02F, {}, {}, {}},
        {"fresnelEXP", 6.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.13F, {}, {}, {}},
        {"isAdditive", 2.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 0U, "body.dds"},
        {"txNormal", 1U, "body_nm.dds"},
        {"txMaps", 2U, "body_maps.dds"},
    };

    const MaterialBinding binding = build_material_binding(material, 3U);
    const auto resolved =
        resolve_ks_per_pixel_multimap_reflection_constants(binding);
    require(resolved.ok() &&
                resolved.constants.fresnel_and_additive ==
                    std::array<float, 4>{0.02F, 6.0F, 0.13F, 2.0F},
            "base MultiMap preserves recovered Fresnel and additive controls");

    MaterialBindingOverrides overrides;
    overrides.properties.emplace(
        "fresnelMaxLevel", MaterialPropertyOverride::scalar_value(0.4F));
    overrides.properties.emplace(
        "isAdditive", MaterialPropertyOverride::scalar_value(1.0F));
    const auto overridden = resolve_ks_per_pixel_multimap_reflection_constants(
        build_material_binding(material, 3U, &overrides));
    require(overridden.ok() &&
                overridden.constants.fresnel_and_additive ==
                    std::array<float, 4>{0.02F, 6.0F, 0.4F, 1.0F},
            "CSP overrides retain precedence for MultiMap reflection controls");

    material.shader = "ksPerPixelMultiMap_AT_NMDetail";
    material.resources.push_back({"txDetail", 3U, "detail.dds"});
    material.resources.push_back(
        {"txNormalDetail", 4U, "detail_nm.dds"});
    const auto nm_detail = resolve_ks_per_pixel_multimap_reflection_constants(
        build_material_binding(material, 5U));
    require(nm_detail.ok() &&
                nm_detail.constants.fresnel_and_additive[3U] == 2.0F,
            "AT NMDetail uses the same portable reflection semantics without claiming native row identity");

    MaterialBinding default_branch = binding;
    default_branch.properties.at("isadditive").scalar = 3.0F;
    const auto default_result =
        resolve_ks_per_pixel_multimap_reflection_constants(default_branch);
    require(default_result.ok() &&
                default_result.constants.fresnel_and_additive[3U] == 3.0F,
            "finite non-special isAdditive values preserve the recovered default branch");

    MaterialBinding non_finite = binding;
    non_finite.properties.at("fresnelc").scalar =
        std::numeric_limits<float>::quiet_NaN();
    const auto invalid =
        resolve_ks_per_pixel_multimap_reflection_constants(non_finite);
    require(!invalid.ok() &&
                invalid.status ==
                    KsPerPixelMultiMapReflectionResolveStatus::invalid_input &&
                invalid.diagnostic.code == "non_finite_constants",
            "non-finite reflection controls are rejected before allocation");

    material.shader = "ksPerPixel";
    const auto wrong_family =
        resolve_ks_per_pixel_multimap_reflection_constants(
            build_material_binding(material, 5U));
    require(!wrong_family.ok() &&
                wrong_family.diagnostic.code ==
                    "ks_per_pixel_multimap_reflection_shader_unsupported",
            "non-MultiMap shaders cannot request the frame-owned cube path");
}

void resolves_bounded_nmdetail_stack() {
    Kn5Material material;
    material.shader = "ksPerPixelMultiMap_NMDetail";
    material.properties = {
        {"useDetail", 0.75F, {}, {}, {}},
        {"detailUVMultiplier", 2.5F, {}, {}, {}},
        {"detailNormalBlend", 0.6F, {}, {}, {}},
        {"nmObjectSpace", 0.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 21U, "body.dds"},
        {"txNormal", 22U, "body_nm.dds"},
        {"txMaps", 23U, "body_maps.dds"},
        {"txDetail", 24U, "body_detail.dds"},
        {"txNormalDetail", 25U, "body_detail_nm.dds"},
    };
    const auto resolved = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U));
    require(resolved.ok() &&
                resolved.constants.detail == std::array<float, 4>{0.75F, 2.5F, 0.6F, 0.0F},
            "bounded ksPerPixelMultiMap_NMDetail controls preserve KN5 values");

    MaterialBindingOverrides overrides;
    overrides.properties.emplace("useDetail",
                                 MaterialPropertyOverride::scalar_value(1.0F));
    overrides.properties.emplace("detailUVMultiplier",
                                 MaterialPropertyOverride::scalar_value(3.0F));
    overrides.properties.emplace("detailNormalBlend",
                                 MaterialPropertyOverride::scalar_value(0.25F));
    const auto csp = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U, &overrides));
    require(csp.ok() &&
                csp.constants.detail == std::array<float, 4>{1.0F, 3.0F, 0.25F, 0.0F},
            "CSP detail controls override KN5 values");

    material.resources.back().slot = "txDetailNM";
    const auto legacy_alias = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U));
    require(legacy_alias.ok(),
            "txDetailNM remains the accepted public normal-detail alias");
    material.resources.back().slot = "txNormalDetail";

    Kn5Material missing = material;
    missing.resources.clear();
    const auto incomplete = build_material_binding(missing, 0U);
    const auto incomplete_result =
        resolve_ks_per_pixel_material_constants(incomplete);
    require(!incomplete_result.ok() &&
                incomplete_result.status == KsPerPixelMaterialResolveStatus::unsupported &&
                incomplete_result.diagnostic.code ==
                    "ks_per_pixel_nmdetail_resources_incomplete",
            "bounded ksPerPixelMultiMap_NMDetail requires its complete texture stack");

    material.properties[3].value = 1.0F;
    const auto object_space = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U));
    require(!object_space.ok() &&
                object_space.diagnostic.code ==
                    "ks_per_pixel_nm_object_space_unsupported",
            "NMDdetail object-space normals remain explicit");

    material.properties[3].value = 0.0F;
    material.properties[4].value = 0.05F;
    const auto fresnel = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U));
    require(!fresnel.ok() &&
                fresnel.diagnostic.code == "ks_per_pixel_nm_fresnel_unsupported",
            "NMDdetail Fresnel reflection remains explicitly disabled");

    material.properties[4].value = 0.0F;
    MaterialBinding malformed = build_material_binding(material, 5U);
    malformed.properties.at("detailnormalblend").scalar =
        std::numeric_limits<float>::quiet_NaN();
    const auto non_finite =
        resolve_ks_per_pixel_material_constants(malformed);
    require(!non_finite.ok() &&
                non_finite.status == KsPerPixelMaterialResolveStatus::invalid_input &&
                non_finite.diagnostic.code == "non_finite_constants",
            "NMDdetail rejects non-finite detail controls");
}

void resolves_bounded_at_nmdetail_stack() {
    Kn5Material material;
    material.shader = "ksPerPixelMultiMap_AT_NMDetail";
    material.properties = {
        {"useDetail", 0.8F, {}, {}, {}},
        {"detailUVMultiplier", 1.75F, {}, {}, {}},
        {"detailNormalBlend", 0.4F, {}, {}, {}},
        {"nmObjectSpace", 0.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 21U, "body.dds"},
        {"txNormal", 22U, "body_nm.dds"},
        {"txMaps", 23U, "body_maps.dds"},
        {"txDetail", 24U, "body_detail.dds"},
        {"txNormalDetail", 25U, "body_detail_nm.dds"},
    };

    const MaterialBinding complete = build_material_binding(material, 5U);
    require(complete.status == MaterialBindingStatus::complete &&
                complete.profile.alpha_to_coverage && complete.profile.shadow_alpha_tested,
            "AT_NMDetail resolves as a complete alpha-tested material family");
    const auto resolved = resolve_ks_per_pixel_material_constants(complete);
    require(resolved.ok() &&
                resolved.constants.detail == std::array<float, 4>{0.8F, 1.75F, 0.4F, 0.0F} &&
                resolved.constants.fresnel[3] == 0.0F,
            "AT_NMDetail preserves detail controls and ordinary main-pass alpha semantics");

    MaterialBindingOverrides overrides;
    overrides.properties.emplace("detailNormalBlend",
                                 MaterialPropertyOverride::scalar_value(0.25F));
    const auto overridden = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U, &overrides));
    require(overridden.ok() && overridden.constants.detail[2] == 0.25F,
            "AT_NMDetail accepts the existing CSP detail override path");

    material.resources.back().slot = "txDetailNM";
    const MaterialBinding alias = build_material_binding(material, 5U);
    const auto alias_result = resolve_ks_per_pixel_material_constants(alias);
    require(alias.status == MaterialBindingStatus::complete && alias_result.ok() &&
                find_material_texture(alias, "txDetailNM") != nullptr,
            "AT_NMDetail accepts the txDetailNM normal-detail alias");

    material.resources.clear();
    const MaterialBinding missing = build_material_binding(material, 0U);
    const auto missing_result = resolve_ks_per_pixel_material_constants(missing);
    require(missing.status == MaterialBindingStatus::incomplete &&
                !missing_result.ok() &&
                missing_result.diagnostic.code == "ks_per_pixel_nmdetail_resources_incomplete",
            "AT_NMDetail rejects an incomplete five-resource stack");

    material.resources = {
        {"txDiffuse", 21U, "body.dds"},
        {"txNormal", 22U, "body_nm.dds"},
        {"txMaps", 23U, "body_maps.dds"},
        {"txDetail", 24U, "body_detail.dds"},
        {"txNormalDetail", 25U, "body_detail_nm.dds"},
    };
    MaterialBinding malformed = build_material_binding(material, 5U);
    malformed.properties.at("detailnormalblend").scalar =
        std::numeric_limits<float>::infinity();
    const auto non_finite = resolve_ks_per_pixel_material_constants(malformed);
    require(!non_finite.ok() &&
                non_finite.status == KsPerPixelMaterialResolveStatus::invalid_input &&
                non_finite.diagnostic.code == "non_finite_constants",
            "AT_NMDetail rejects non-finite detail controls");

    material.properties[3].value = 1.0F;
    const auto object_space = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U));
    require(!object_space.ok() &&
                object_space.status == KsPerPixelMaterialResolveStatus::unsupported &&
                object_space.diagnostic.code == "ks_per_pixel_nm_object_space_unsupported",
            "AT_NMDetail keeps object-space normals outside the bounded family");

    material.properties[3].value = 0.0F;
    material.properties[4].value = 0.05F;
    const auto fresnel = resolve_ks_per_pixel_material_constants(
        build_material_binding(material, 5U));
    require(!fresnel.ok() &&
                fresnel.status == KsPerPixelMaterialResolveStatus::unsupported &&
                fresnel.diagnostic.code == "ks_per_pixel_nm_fresnel_unsupported",
            "AT_NMDetail keeps Fresnel reflection outside the bounded family");
}

} // namespace

int main() {
    try {
        stock_binding_and_defaults();
        transparent_glass_and_csp_overrides();
        missing_and_duplicate_resources();
        unknown_and_bind_point_references();
        external_override_and_limits();
        rejects_oversized_state_overrides();
        resolves_ks_per_pixel_defaults_and_kn5_values();
        adapts_the_bounded_kn5_parser_model();
        resolves_ks_per_pixel_csp_precedence_and_alpha_capture();
        resolves_stock_shadow_caster_material_constants();
        rejects_invalid_stock_shadow_caster_alpha_reference();
        rejects_invalid_ks_per_pixel_constants();
        resolves_bounded_tangent_space_nm_variant();
        resolves_bounded_base_multimap_families();
        resolves_multimap_reflection_controls_separately();
        resolves_bounded_nmdetail_stack();
        resolves_bounded_at_nmdetail_stack();
        resolves_bounded_dirt_zero_damage_variant();
        std::cout << "material binding tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "material binding tests failed: " << error.what() << '\n';
        return 1;
    }
}
