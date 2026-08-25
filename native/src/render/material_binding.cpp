#include "apex/render/material_binding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace apex::render {
namespace {

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

[[nodiscard]] std::string canonical(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' ||
                                    value[begin] == '\r' || value[begin] == '\n'))
        ++begin;
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n'))
        --end;
    return lower(value.substr(begin, end - begin));
}

[[noreturn]] void fail(std::string code, std::string message) {
    throw MaterialBindingError(std::move(code), std::move(message));
}

void check_string(std::string_view value, std::string_view field,
                  const MaterialBindingLimits& limits) {
    if (value.size() > limits.max_string_bytes)
        fail("string_limit", std::string(field) + " exceeds the material string limit");
}

void check_finite(const Kn5MaterialProperty& property) {
    const auto finite = [](float value) { return std::isfinite(value); };
    if (!finite(property.value) || std::any_of(property.value2.begin(), property.value2.end(),
                                               [&](float value) { return !finite(value); }) ||
        std::any_of(property.value3.begin(), property.value3.end(),
                    [&](float value) { return !finite(value); }) ||
        std::any_of(property.value4.begin(), property.value4.end(),
                    [&](float value) { return !finite(value); }))
        fail("non_finite_property", "Material property " + property.name + " contains a non-finite value");
}

[[nodiscard]] MaterialPropertyValue from_kn5(const Kn5MaterialProperty& property) {
    return MaterialPropertyValue{property.name, property.value, property.value2, property.value3,
                                 property.value4, MaterialPropertySource::kn5,
                                 MaterialPropertyValue::Arity::vector3};
}

[[nodiscard]] MaterialPropertyValue from_override(std::string name,
                                                  const MaterialPropertyOverride& value) {
    MaterialPropertyValue result;
    result.name = std::move(name);
    result.source = MaterialPropertySource::csp;
    switch (value.kind) {
    case MaterialPropertyOverride::Kind::scalar:
        result.arity = MaterialPropertyValue::Arity::scalar;
        result.scalar = value.scalar;
        result.vector2 = {value.scalar, value.scalar};
        result.vector3 = {value.scalar, value.scalar, value.scalar};
        result.vector4 = {value.scalar, value.scalar, value.scalar, value.scalar};
        break;
    case MaterialPropertyOverride::Kind::vector2:
        result.arity = MaterialPropertyValue::Arity::vector2;
        result.scalar = value.vector2[0];
        result.vector2 = value.vector2;
        result.vector3 = {value.vector2[0], value.vector2[1], value.vector2[1]};
        result.vector4 = {value.vector2[0], value.vector2[1], value.vector2[1], value.vector2[1]};
        break;
    case MaterialPropertyOverride::Kind::vector3:
        result.arity = MaterialPropertyValue::Arity::vector3;
        result.scalar = value.vector3[0];
        result.vector2 = {value.vector3[0], value.vector3[1]};
        result.vector3 = value.vector3;
        result.vector4 = {value.vector3[0], value.vector3[1], value.vector3[2], value.vector3[2]};
        break;
    case MaterialPropertyOverride::Kind::vector4:
        result.arity = MaterialPropertyValue::Arity::vector4;
        result.scalar = value.vector4[0];
        result.vector2 = {value.vector4[0], value.vector4[1]};
        result.vector3 = {value.vector4[0], value.vector4[1], value.vector4[2]};
        result.vector4 = value.vector4;
        break;
    }
    return result;
}

void check_override_value(const MaterialPropertyOverride& value, std::string_view name) {
    const auto finite = [](float component) { return std::isfinite(component); };
    bool valid = finite(value.scalar);
    if (value.kind == MaterialPropertyOverride::Kind::vector2)
        valid = std::all_of(value.vector2.begin(), value.vector2.end(), finite);
    else if (value.kind == MaterialPropertyOverride::Kind::vector3)
        valid = std::all_of(value.vector3.begin(), value.vector3.end(), finite);
    else if (value.kind == MaterialPropertyOverride::Kind::vector4)
        valid = std::all_of(value.vector4.begin(), value.vector4.end(), finite);
    if (!valid) fail("non_finite_property", "CSP property " + std::string(name) + " contains a non-finite value");
}

struct DefaultProperty {
    std::string_view name;
    float value;
    std::array<float, 2> vector2;
};

constexpr std::array<DefaultProperty, 18> kDefaults = {
    DefaultProperty{"ksambient", 0.35F, {0.0F, 0.0F}},
    DefaultProperty{"ksdiffuse", 0.8F, {0.0F, 0.0F}},
    DefaultProperty{"ksspecular", 0.2F, {0.0F, 0.0F}},
    DefaultProperty{"ksspecularexp", 30.0F, {0.0F, 0.0F}},
    DefaultProperty{"fresnelc", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"fresnelexp", 5.0F, {0.0F, 0.0F}},
    DefaultProperty{"fresnelmaxlevel", 0.05F, {0.0F, 0.0F}},
    DefaultProperty{"ksalpharef", 0.5F, {0.0F, 0.0F}},
    DefaultProperty{"extrefraction", 0.02F, {0.0F, 0.0F}},
    DefaultProperty{"pbreflectionblurenv", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"seasonautumn", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"seasonwinter", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"nmobjectspace", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"usedetail", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"detailuvmultiplier", 1.0F, {0.0F, 0.0F}},
    DefaultProperty{"detailnormalblend", 1.0F, {0.0F, 0.0F}},
    DefaultProperty{"multa", 0.0F, {0.0F, 0.0F}},
    DefaultProperty{"detailnmmult", 0.0F, {0.0F, 0.0F}},
};

void add_default(std::map<std::string, MaterialPropertyValue>& properties,
                 const DefaultProperty& value) {
    if (properties.find(std::string(value.name)) != properties.end()) return;
    MaterialPropertyValue output;
    output.name = std::string(value.name);
    output.scalar = value.value;
    output.vector2 = value.vector2;
    output.vector3 = {value.value, value.value, value.value};
    output.vector4 = {value.value, value.value, value.value, value.value};
    output.source = MaterialPropertySource::default_value;
    output.arity = MaterialPropertyValue::Arity::scalar;
    if (value.name == "multa" || value.name == "detailnmmult") {
        output.scalar = value.vector2[0];
        output.vector3 = {value.vector2[0], value.vector2[1], value.vector2[1]};
        output.vector4 = {value.vector2[0], value.vector2[1], value.vector2[1], value.vector2[1]};
    }
    properties.emplace(std::string(value.name), std::move(output));
}

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool is_nmdetail_family(std::string_view canonical_shader) noexcept {
    return canonical_shader == "ksperpixelmultimap_nmdetail" ||
           canonical_shader == "ksperpixelmultimap_at_nmdetail";
}

[[nodiscard]] bool is_base_multimap_family(
    std::string_view canonical_shader) noexcept {
    return canonical_shader == "ksperpixelmultimap" ||
           canonical_shader == "ksperpixelmultimap_at";
}

[[nodiscard]] bool is_damage_dirt_family(std::string_view canonical_shader) noexcept {
    return canonical_shader == "ksperpixelmultimap_damage_dirt";
}

[[nodiscard]] std::vector<std::string> required_slots(std::string_view shader) {
    const std::string key = canonical(shader);
    if (key == "kstyres" || key == "newstefano_kstyres")
        return {"txDiffuse", "txNormal", "txDirty", "txBlur", "txNormalBlur"};
    if (key == "ksbrakedisc")
        return {"txDiffuse", "txNormal", "txGlow", "txBlur", "txNormalBlur"};
    if (key == "ksperpixelmultimap_damage_dirt")
        return {"txDiffuse", "txNormal", "txMaps", "txDamage", "txDamageMask"};
    if (key == "ksperpixelmultimap_damage")
        return {"txDamage", "txDamageMask"};
    if (is_nmdetail_family(key))
        return {"txDiffuse", "txNormal", "txMaps", "txDetail", "txNormalDetail"};
    if (is_base_multimap_family(key))
        return {"txDiffuse", "txNormal", "txMaps"};
    if (starts_with(key, "ksmultilayer"))
        return {"txMask", "txDetailR", "txDetailG", "txDetailB", "txDetailA"};
    if (starts_with(key, "ksperpixel") || starts_with(key, "ksskinnedmesh") ||
        key == "kstree" || key == "ksgrass" || key == "kswindscreen" ||
        key == "ksbrokenglass" || key == "ksorenayar" || key == "kscarpaintsimple" ||
        key == "kssimpleshader" || key == "kscolourshader" || key == "gl" ||
        key == "gltextured" || starts_with(key, "stperpixel"))
        return {"txDiffuse"};
    return {};
}

[[nodiscard]] std::string property_to_text(const MaterialPropertyOverride& value) {
    switch (value.kind) {
    case MaterialPropertyOverride::Kind::scalar:
        return std::to_string(value.scalar);
    case MaterialPropertyOverride::Kind::vector2:
        return std::to_string(value.vector2[0]);
    case MaterialPropertyOverride::Kind::vector3:
        return std::to_string(value.vector3[0]);
    case MaterialPropertyOverride::Kind::vector4:
        return std::to_string(value.vector4[0]);
    }
    return "0";
}

[[nodiscard]] MaterialTextureBinding embedded_binding(const Kn5MaterialResource& resource) {
    return MaterialTextureBinding{resource.slot, MaterialTextureKind::embedded, resource.bind_point,
                                  resource.texture, {}, {}, false, false};
}

[[nodiscard]] bool finite_color(const std::array<float, 4>& value) {
    return std::all_of(value.begin(), value.end(), [](float component) { return std::isfinite(component); });
}

[[nodiscard]] std::array<float, 4> clamp_color(std::array<float, 4> value) {
    for (float& component : value) component = std::max(0.0F, std::min(1.0F, component));
    return value;
}

[[nodiscard]] std::array<float, 4> resolve_emissive(
    const MaterialBinding& binding) {
    const MaterialPropertyValue* emissive =
        find_material_property(binding, "ksEmissive");
    if (emissive == nullptr) return {0.0F, 0.0F, 0.0F, 0.0F};

    std::array<float, 3> color{};
    float strength = 1.0F;
    if (emissive->source == MaterialPropertySource::csp) {
        switch (emissive->arity) {
        case MaterialPropertyValue::Arity::scalar:
            color = {emissive->scalar, emissive->scalar, emissive->scalar};
            break;
        case MaterialPropertyValue::Arity::vector2:
            color = {emissive->vector2[0], emissive->vector2[1], 0.0F};
            break;
        case MaterialPropertyValue::Arity::vector3:
            color = emissive->vector3;
            break;
        case MaterialPropertyValue::Arity::vector4:
            color = {emissive->vector4[0], emissive->vector4[1], emissive->vector4[2]};
            strength = emissive->vector4[3];
            break;
        }
    } else {
        // public/app.js reads KN5 ksemissive from value3, which has no
        // fourth strength component.
        color = emissive->vector3;
    }
    if (std::max({color[0], color[1], color[2]}) > 16.0F)
        for (float& component : color) component /= 255.0F;
    return {color[0] * strength, color[1] * strength, color[2] * strength,
            0.0F};
}

} // namespace

MaterialPropertyOverride MaterialPropertyOverride::scalar_value(float value) noexcept {
    MaterialPropertyOverride result;
    result.kind = Kind::scalar;
    result.scalar = value;
    return result;
}

MaterialPropertyOverride MaterialPropertyOverride::vector2_value(std::array<float, 2> value) noexcept {
    MaterialPropertyOverride result;
    result.kind = Kind::vector2;
    result.vector2 = value;
    return result;
}

MaterialPropertyOverride MaterialPropertyOverride::vector3_value(std::array<float, 3> value) noexcept {
    MaterialPropertyOverride result;
    result.kind = Kind::vector3;
    result.vector3 = value;
    return result;
}

MaterialPropertyOverride MaterialPropertyOverride::vector4_value(std::array<float, 4> value) noexcept {
    MaterialPropertyOverride result;
    result.kind = Kind::vector4;
    result.vector4 = value;
    return result;
}

MaterialBindingError::MaterialBindingError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

MaterialBinding build_material_binding(const Kn5Material& material, std::size_t texture_table_count,
                                       const MaterialBindingOverrides* overrides,
                                       const MaterialBindingLimits& limits) {
    (void)texture_table_count;
    check_string(material.name, "material name", limits);
    check_string(material.shader, "shader name", limits);
    if (material.properties.size() > limits.max_properties)
        fail("property_limit", "Material property count exceeds the configured limit");
    if (material.resources.size() > limits.max_resources)
        fail("resource_limit", "Material resource count exceeds the configured limit");
    if (overrides && overrides->properties.size() > limits.max_override_properties)
        fail("override_property_limit", "CSP property override count exceeds the configured limit");
    if (overrides && overrides->resources.size() > limits.max_override_resources)
        fail("override_resource_limit", "CSP resource override count exceeds the configured limit");

    MaterialBinding result;
    result.name = material.name;
    MaterialInput profile_input{material.shader, material.serialized_blend_mode, material.depth_mode};
    NodeMaterialInput node_input{material.transparent};
    MaterialOverride profile_override;
    if (overrides) {
        if (overrides->shader.has_value())
            check_string(*overrides->shader, "CSP shader override", limits);
        if (overrides->blend_mode.has_value())
            check_string(*overrides->blend_mode, "CSP blend-mode override", limits);
        if (overrides->depth_mode.has_value())
            check_string(*overrides->depth_mode, "CSP depth-mode override", limits);
        if (overrides->cull_mode.has_value())
            check_string(*overrides->cull_mode, "CSP cull-mode override", limits);
        profile_override.shader = overrides->shader;
        profile_override.blend_mode = overrides->blend_mode;
        profile_override.depth_mode = overrides->depth_mode;
        profile_override.cull_mode = overrides->cull_mode;
        profile_override.is_transparent = overrides->is_transparent;
        const auto property = std::find_if(
            overrides->properties.begin(), overrides->properties.end(),
            [](const auto& entry) { return canonical(entry.first) == "extrefraction"; });
        if (property != overrides->properties.end())
            profile_override.properties.emplace("extrefraction", property_to_text(property->second));
    }
    result.profile = resolve_material_render_profile(profile_input, node_input,
                                                      overrides ? &profile_override : nullptr);
    result.shader = result.profile.shader;

    if (result.profile.stock == nullptr) {
        result.diagnostics.push_back({"unknown_shader", "shader",
                                      "Unknown shader package; no native pixel execution path is claimed"});
    }
    result.diagnostics.push_back({"shader_execution_staged", "shader",
                                  "Shader execution is staged; this binding contains no GPU API objects or pixel approximation"});

    for (const Kn5MaterialProperty& property : material.properties) {
        check_string(property.name, "property name", limits);
        check_finite(property);
        const std::string key = canonical(property.name);
        if (key.empty()) fail("invalid_property", "Material property name is empty");
        if (result.properties.find(key) != result.properties.end()) {
            result.diagnostics.push_back({"duplicate_property_ignored", property.name,
                                          "Duplicate KN5 property ignored after the first declaration"});
            continue;
        }
        result.properties.emplace(key, from_kn5(property));
    }
    if (overrides) {
        std::map<std::string, std::string> override_property_names;
        for (const auto& [raw_name, value] : overrides->properties) {
            check_string(raw_name, "CSP property name", limits);
            const std::string key = canonical(raw_name);
            if (key.empty()) fail("invalid_property", "CSP property override name is empty");
            if (const auto existing = override_property_names.find(key);
                existing != override_property_names.end())
                fail("duplicate_property_override", "CSP property overrides " + existing->second +
                                                           " and " + raw_name + " address the same slot");
            override_property_names.emplace(key, raw_name);
            check_override_value(value, raw_name);
            result.properties[key] = from_override(raw_name, value);
        }
    }
    for (const DefaultProperty& value : kDefaults) add_default(result.properties, value);

    for (const Kn5MaterialResource& resource : material.resources) {
        check_string(resource.slot, "resource slot", limits);
        check_string(resource.texture, "resource texture", limits);
        const std::string key = canonical(resource.slot);
        if (key.empty()) fail("invalid_resource", "Material resource slot is empty");
        if (result.textures.find(key) != result.textures.end())
            fail("duplicate_resource", "Material resource slot " + resource.slot + " is declared more than once");
        result.textures.emplace(key, embedded_binding(resource));
    }

    if (overrides) {
        std::map<std::string, std::string> override_resource_names;
        for (const auto& [raw_slot, override_resource] : overrides->resources) {
            check_string(raw_slot, "CSP resource slot", limits);
            check_string(override_resource.texture, "CSP resource texture", limits);
            check_string(override_resource.file, "CSP resource file", limits);
            const std::string key = canonical(raw_slot);
            if (key.empty()) fail("invalid_resource", "CSP resource slot is empty");
            if (const auto existing = override_resource_names.find(key);
                existing != override_resource_names.end())
                fail("duplicate_resource_override", "CSP resource overrides " + existing->second +
                                                          " and " + raw_slot + " address the same slot");
            override_resource_names.emplace(key, raw_slot);
            MaterialTextureBinding binding;
            binding.slot = raw_slot;
            if (override_resource.color.has_value()) {
                if (!finite_color(override_resource.color.value()))
                    fail("non_finite_resource", "CSP solid-color resource " + raw_slot + " is not finite");
                binding.kind = MaterialTextureKind::solid_color;
                binding.color = clamp_color(override_resource.color.value());
            } else if (!override_resource.file.empty()) {
                binding.kind = MaterialTextureKind::external_file;
                binding.file = override_resource.file;
                result.diagnostics.push_back({"external_resource_staged", raw_slot,
                                              "External CSP resource is recorded but path resolution is staged"});
            } else if (override_resource.bind_point.has_value()) {
                binding.kind = MaterialTextureKind::embedded;
                binding.bind_point = override_resource.bind_point;
                binding.texture = override_resource.texture;
            } else if (!override_resource.texture.empty()) {
                const auto by_name = std::find_if(
                    material.resources.begin(), material.resources.end(), [&](const Kn5MaterialResource& resource) {
                        return canonical(resource.texture) == canonical(override_resource.texture);
                    });
                if (by_name == material.resources.end()) {
                    binding.kind = MaterialTextureKind::unresolved_override;
                    binding.texture = override_resource.texture;
                    result.diagnostics.push_back({"unresolved_resource_override", raw_slot,
                                                  "CSP texture name is not present in the parsed KN5 texture table"});
                } else {
                    binding.kind = MaterialTextureKind::embedded;
                    binding.bind_point = by_name->bind_point;
                    binding.texture = by_name->texture;
                }
            } else {
                binding.kind = MaterialTextureKind::missing;
                result.diagnostics.push_back({"missing_texture", raw_slot,
                                              "CSP resource override has no texture, file, or solid color"});
            }
            result.textures[key] = std::move(binding);
        }
    }

    for (const std::string& raw_slot : required_slots(result.shader)) {
        const std::string key = canonical(raw_slot);
        // public/app.js accepts txDetailNM as the legacy spelling for the
        // generic txNormalDetail slot. Preserve that source fallback while
        // keeping one required logical binding in the native projection.
        if (key == "txnormaldetail" &&
            is_nmdetail_family(canonical(result.shader)) &&
            result.textures.find("txdetailnm") != result.textures.end()) {
            result.textures.at("txdetailnm").required = true;
            continue;
        }
        const auto found = result.textures.find(key);
        if (found != result.textures.end()) {
            found->second.required = true;
            continue;
        }
        MaterialTextureBinding missing;
        missing.slot = raw_slot;
        missing.kind = MaterialTextureKind::missing;
        missing.required = true;
        result.textures.emplace(key, std::move(missing));
        result.diagnostics.push_back({"missing_texture", raw_slot,
                                      "Required material texture is not assigned; no fallback texture is claimed"});
    }

    const bool missing_required = std::any_of(
        result.textures.begin(), result.textures.end(), [](const auto& entry) {
            if (!entry.second.required) return false;
            return entry.second.kind != MaterialTextureKind::embedded &&
                   entry.second.kind != MaterialTextureKind::solid_color;
        });
    if (result.profile.stock == nullptr)
        result.status = MaterialBindingStatus::unsupported;
    else if (missing_required)
        result.status = MaterialBindingStatus::incomplete;
    else
        result.status = MaterialBindingStatus::complete;
    return result;
}

MaterialBinding build_material_binding(
    const apex::formats::Kn5Material& material, bool node_transparent,
    std::size_t texture_table_count, const MaterialBindingOverrides* overrides,
    const MaterialBindingLimits& limits) {
    check_string(material.name, "material name", limits);
    check_string(material.shader, "shader name", limits);
    if (material.properties.size() > limits.max_properties)
        fail("property_limit", "Material property count exceeds the configured limit");
    if (material.resources.size() > limits.max_resources)
        fail("resource_limit", "Material resource count exceeds the configured limit");
    if (material.serializedBlendMode > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        material.depthMode > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        fail("material_state_range", "KN5 material state exceeds the native integer range");

    Kn5Material projected;
    projected.name = material.name;
    projected.shader = material.shader;
    projected.serialized_blend_mode = static_cast<int>(material.serializedBlendMode);
    projected.depth_mode = static_cast<int>(material.depthMode);
    projected.transparent = node_transparent;
    projected.properties.reserve(material.properties.size());
    for (const apex::formats::Kn5MaterialProperty& property : material.properties) {
        check_string(property.name, "property name", limits);
        const Kn5MaterialProperty value{property.name, property.value, property.value2,
                                       property.value3, property.value4};
        check_finite(value);
        projected.properties.push_back(value);
    }
    projected.resources.reserve(material.resources.size());
    for (const apex::formats::Kn5MaterialResource& resource : material.resources) {
        check_string(resource.slot, "resource slot", limits);
        check_string(resource.texture, "resource texture", limits);
        projected.resources.push_back(
            {resource.slot, resource.textureId, resource.texture});
    }
    return build_material_binding(projected, texture_table_count, overrides, limits);
}

KsPerPixelMaterialResolveResult resolve_ks_per_pixel_material_constants(
    const MaterialBinding& binding, KsPerPixelMaterialResolveOptions options) {
    KsPerPixelMaterialResolveResult result;
    const std::string shader = canonical(binding.shader);
    const bool detail_stack_variant = is_nmdetail_family(shader);
    const bool base_multimap_variant = is_base_multimap_family(shader);
    const bool damage_dirt_variant = is_damage_dirt_family(shader);
    const bool normal_variant = shader == "ksperpixelnm" ||
                                base_multimap_variant || detail_stack_variant ||
                                damage_dirt_variant;
    if (shader != "ksperpixel" && !normal_variant) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_shader_unsupported", "shader",
                             "The bounded material resolver accepts exact ksPerPixel, ksPerPixelNM, ksPerPixelMultiMap, ksPerPixelMultiMap_AT, ksPerPixelMultiMap_NMDetail, ksPerPixelMultiMap_AT_NMDetail, and dirt-zero ksPerPixelMultiMap_damage_dirt shaders"};
        return result;
    }

    result.constants.lighting = {
        material_scalar(binding, "ksAmbient", 0.35F),
        material_scalar(binding, "ksDiffuse", 0.80F),
        material_scalar(binding, "ksSpecular", 0.20F),
        material_scalar(binding, "ksSpecularEXP", 30.0F),
    };
    result.constants.fresnel = {
        material_scalar(binding, "fresnelC", 0.0F),
        material_scalar(binding, "fresnelEXP", 5.0F),
        material_scalar(binding, "fresnelMaxLevel", 0.05F),
        0.0F,
    };
    result.constants.detail = {
        material_scalar(binding, "useDetail", 0.0F),
        material_scalar(binding, "detailUVMultiplier", 1.0F),
        material_scalar(binding, "detailNormalBlend", 1.0F),
        0.0F,
    };
    if (damage_dirt_variant) {
        const MaterialPropertyValue* damage_zones =
            find_material_property(binding, "damageZones");
        const MaterialPropertyValue* dirt = find_material_property(binding, "dirt");
        if (damage_zones == nullptr || dirt == nullptr) {
            result.status = KsPerPixelMaterialResolveStatus::unsupported;
            result.diagnostic = {"ks_per_pixel_damage_properties_missing", "damageZones",
                                 "The bounded damage stage requires authored damageZones and dirt properties"};
            return result;
        }
        if (!std::isfinite(dirt->scalar)) {
            result.status = KsPerPixelMaterialResolveStatus::invalid_input;
            result.diagnostic = {"non_finite_constants", "dirt",
                                 "The resolved damage dirt value must be finite"};
            return result;
        }
        if (dirt->scalar != 0.0F) {
            result.status = KsPerPixelMaterialResolveStatus::unsupported;
            result.diagnostic = {"ks_per_pixel_damage_dirt_unsupported", "dirt",
                                 "The recovered bounded damage stage supports dirt zero only"};
            return result;
        }
        result.constants.damage_zones = damage_zones->vector4;
    }

    result.constants.emissive = resolve_emissive(binding);

    if (options.capture_pass && options.alpha_to_coverage)
        result.constants.fresnel[3] = std::max(
            0.5F, material_scalar(binding, "ksAlphaRef", 0.5F));

    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(),
                           [](float value) { return std::isfinite(value); });
    };
    if (!finite(result.constants.lighting) || !finite(result.constants.fresnel) ||
        !finite(result.constants.emissive) || !finite(result.constants.detail) ||
        !finite(result.constants.damage_zones)) {
        result.status = KsPerPixelMaterialResolveStatus::invalid_input;
        result.diagnostic = {"non_finite_constants", "ksPerPixel",
                             "Resolved ksPerPixel constants must contain only finite values"};
        return result;
    }
    const float normal_object_space =
        normal_variant ? material_scalar(binding, "nmObjectSpace", 0.0F) : 0.0F;
    if (!std::isfinite(normal_object_space)) {
        result.status = KsPerPixelMaterialResolveStatus::invalid_input;
        result.diagnostic = {"non_finite_constants", "nmObjectSpace",
                             "Resolved normal-map normal-space mode must be finite"};
        return result;
    }
    if (normal_variant && normal_object_space > 0.5F) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_nm_object_space_unsupported", "nmObjectSpace",
                             "The bounded normal-map paths support tangent-space normals only"};
        return result;
    }
    if (normal_variant && result.constants.fresnel[2] > 0.0F) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_nm_fresnel_unsupported", "fresnelMaxLevel",
                             "The bounded normal-map paths require disabled Fresnel reflection"};
        return result;
    }
    if (base_multimap_variant && result.constants.detail[0] > 0.5F) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_multimap_detail_unsupported", "useDetail",
                             "The bounded eight-binding ksPerPixelMultiMap path does not execute the generic detail stack"};
        return result;
    }
    if (damage_dirt_variant && result.constants.detail[0] > 0.5F) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_damage_detail_unsupported", "useDetail",
                             "The bounded damage stage does not execute the stock detail branch"};
        return result;
    }
    if (damage_dirt_variant) {
        const float sun_specular = material_scalar(binding, "sunSpecular", 0.0F);
        if (!std::isfinite(sun_specular)) {
            result.status = KsPerPixelMaterialResolveStatus::invalid_input;
            result.diagnostic = {"non_finite_constants", "sunSpecular",
                                 "The resolved sun-specular value must be finite"};
            return result;
        }
        if (sun_specular != 0.0F) {
            result.status = KsPerPixelMaterialResolveStatus::unsupported;
            result.diagnostic = {"ks_per_pixel_damage_sun_specular_unsupported",
                                 "sunSpecular",
                                 "The bounded damage stage does not execute the stock sun-specular branch"};
            return result;
        }
    }

    if (detail_stack_variant && binding.status != MaterialBindingStatus::complete) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_nmdetail_resources_incomplete", "resources",
                             "The bounded ksPerPixelMultiMap_NMDetail family requires txDiffuse, txNormal, txMaps, txDetail, and txNormalDetail"};
        return result;
    }
    if (base_multimap_variant && binding.status != MaterialBindingStatus::complete) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_multimap_resources_incomplete", "resources",
                             "The bounded ksPerPixelMultiMap family requires txDiffuse, txNormal, and txMaps"};
        return result;
    }
    if (damage_dirt_variant && binding.status != MaterialBindingStatus::complete) {
        result.status = KsPerPixelMaterialResolveStatus::unsupported;
        result.diagnostic = {"ks_per_pixel_damage_resources_incomplete", "resources",
                             "The bounded damage stage requires txDiffuse, txNormal, txMaps, txDamage, and txDamageMask"};
        return result;
    }

    result.status = KsPerPixelMaterialResolveStatus::ready;
    return result;
}

StockShadowCasterMaterialResolveResult
resolve_stock_shadow_caster_material_constants(const MaterialBinding& binding) {
    StockShadowCasterMaterialResolveResult result;
    result.constants.lighting = {
        material_scalar(binding, "ksAmbient", 0.35F),
        material_scalar(binding, "ksDiffuse", 0.80F),
        material_scalar(binding, "ksSpecular", 0.20F),
        material_scalar(binding, "ksSpecularEXP", 30.0F),
    };
    result.constants.emissive_and_alpha_ref = resolve_emissive(binding);

    // build_material_binding adds a renderer default for the reflection
    // capture path. That default is not part of the recovered shadow-caster
    // ABI, so only authored KN5/CSP values participate here.
    const MaterialPropertyValue* alpha_ref =
        find_material_property(binding, "ksAlphaRef");
    if (alpha_ref != nullptr &&
        alpha_ref->source != MaterialPropertySource::default_value)
        result.constants.emissive_and_alpha_ref[3] = alpha_ref->scalar;

    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(),
                           [](float value) { return std::isfinite(value); });
    };
    if (!finite(result.constants.lighting) ||
        !finite(result.constants.emissive_and_alpha_ref)) {
        result.status = StockShadowCasterMaterialResolveStatus::invalid_input;
        result.diagnostic = {
            "non_finite_constants", "StockShadowCasterMaterialConstants",
            "Resolved stock shadow caster constants must contain only finite values"};
        return result;
    }
    const float alpha_ref_value = result.constants.emissive_and_alpha_ref[3];
    if (alpha_ref_value < 0.0F || alpha_ref_value > 1.0F) {
        result.status = StockShadowCasterMaterialResolveStatus::invalid_input;
        result.diagnostic = {
            "ks_alpha_ref_range", "ksAlphaRef",
            "The stock shadow caster alpha reference must be within [0, 1]"};
        return result;
    }

    result.status = StockShadowCasterMaterialResolveStatus::ready;
    return result;
}

const MaterialPropertyValue* find_material_property(const MaterialBinding& binding,
                                                    std::string_view name) noexcept {
    const auto found = binding.properties.find(canonical(name));
    return found == binding.properties.end() ? nullptr : &found->second;
}

const MaterialTextureBinding* find_material_texture(const MaterialBinding& binding,
                                                    std::string_view slot) noexcept {
    const auto found = binding.textures.find(canonical(slot));
    return found == binding.textures.end() ? nullptr : &found->second;
}

float material_scalar(const MaterialBinding& binding, std::string_view name, float fallback) noexcept {
    const MaterialPropertyValue* property = find_material_property(binding, name);
    return property == nullptr ? fallback : property->scalar;
}

std::array<float, 2> material_vector2(const MaterialBinding& binding, std::string_view name,
                                      std::array<float, 2> fallback) noexcept {
    const MaterialPropertyValue* property = find_material_property(binding, name);
    return property == nullptr ? fallback : property->vector2;
}

std::array<float, 4> material_vector4(const MaterialBinding& binding, std::string_view name,
                                      std::array<float, 4> fallback) noexcept {
    const MaterialPropertyValue* property = find_material_property(binding, name);
    return property == nullptr ? fallback : property->vector4;
}

} // namespace apex::render
