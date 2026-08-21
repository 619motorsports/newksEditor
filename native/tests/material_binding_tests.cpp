#include "apex/render/material_binding.hpp"

#include <array>
#include <cstdint>
#include <iostream>
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
                diffuse->bind_point == 0 && diffuse->texture == "body.dds",
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
    require(mask != nullptr && mask->bind_point == 21 &&
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

} // namespace

int main() {
    try {
        stock_binding_and_defaults();
        transparent_glass_and_csp_overrides();
        missing_and_duplicate_resources();
        unknown_and_bind_point_references();
        external_override_and_limits();
        std::cout << "material binding tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "material binding tests failed: " << error.what() << '\n';
        return 1;
    }
}
