#include "apex/render/material_binding.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Kn5Material tyre_material(std::string shader = "ksTyres") {
    Kn5Material material;
    material.name = "Tyres";
    material.shader = std::move(shader);
    material.properties = {
        {"ksAmbient", -0.25F, {}, {}, {}},
        {"ksDiffuse", 1.25F, {}, {}, {}},
        {"ksSpecular", 0.375F, {}, {}, {}},
        {"ksSpecularEXP", -7.0F, {}, {}, {}},
        {"ksEmissive", 0.0F, {}, {0.125F, 0.25F, 0.5F}, {}},
        {"ksAlphaRef", 0.27F, {}, {}, {}},
        {"blurLevel", 1.25F, {}, {}, {}},
        {"dirtyLevel", -0.5F, {}, {}, {}},
        {"fresnelC", 0.125F, {}, {}, {}},
        {"fresnelEXP", 4.5F, {}, {}, {}},
        {"isAdditive", 2.0F, {}, {}, {}},
        {"fresnelMaxLevel", -0.75F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 0U, "tyre.dds"},
        {"txNormal", 1U, "tyre_nm.dds"},
        {"txDirty", 2U, "tyre_dirty.dds"},
        {"txBlur", 3U, "tyre_blur.dds"},
        {"txNormalBlur", 4U, "tyre_nm_blur.dds"},
    };
    return material;
}

void resolves_both_exact_shader_aliases_without_clamping() {
    for (const std::string_view shader : {"ksTyres", "NEWSTEFANO_KSTYRES"}) {
        Kn5Material material = tyre_material(std::string(shader));
        const MaterialBinding binding = build_material_binding(material, 5U);
        const StockTyresSourceMaterialResolveResult result =
            resolve_stock_tyres_source_constants(binding);
        require(binding.status == MaterialBindingStatus::complete,
                "complete tyre binding status");
        require(result.ok(), "complete tyre source constants");
        require(result.material.ambient == -0.25F &&
                    result.material.diffuse == 1.25F &&
                    result.material.specular == 0.375F &&
                    result.material.specular_exponent == -7.0F,
                "tyre lighting values preserve authored arithmetic inputs");
        require(result.material.emissive ==
                    std::array<float, 3U>{0.125F, 0.25F, 0.5F} &&
                    result.material.alpha_reference == 0.27F,
                "tyre emissive and alpha values resolve independently");
        require(result.constants.blur_level == 1.25F &&
                    result.constants.dirty_level == -0.5F &&
                    result.constants.fresnel_c == 0.125F &&
                    result.constants.fresnel_exp == 4.5F &&
                    result.constants.is_additive == 2.0F &&
                    result.constants.fresnel_max_level == -0.75F &&
                    result.constants.reserved == std::array<float, 2U>{0.0F, 0.0F},
                "tyre source values are not clamped or normalized");
    }
}

void rejects_missing_or_extra_resources() {
    Kn5Material missing = tyre_material();
    missing.resources.pop_back();
    const MaterialBinding incomplete = build_material_binding(missing, 4U);
    const auto missing_result = resolve_stock_tyres_source_constants(incomplete);
    require(incomplete.status == MaterialBindingStatus::incomplete &&
                missing_result.status == KsTyreMaterialResolveStatus::unsupported &&
                missing_result.diagnostic.code ==
                    "stock_tyres_source_resources_incomplete",
            "missing tyre texture is rejected with a bounded diagnostic");

    Kn5Material extra = tyre_material();
    extra.resources.push_back({"txUnexpected", 6U, "unexpected.dds"});
    const MaterialBinding extra_binding = build_material_binding(extra, 6U);
    const auto extra_result = resolve_stock_tyres_source_constants(extra_binding);
    require(extra_binding.status == MaterialBindingStatus::complete &&
                extra_result.status == KsTyreMaterialResolveStatus::unsupported &&
                extra_result.diagnostic.code ==
                    "stock_tyres_source_resources_unsupported",
            "extra tyre texture is rejected instead of silently ignored");
}

void rejects_non_tyre_shader_alias() {
    const MaterialBinding binding = build_material_binding(
        tyre_material("ksPerPixel"), 5U);
    const auto result = resolve_stock_tyres_source_constants(binding);
    require(result.status == KsTyreMaterialResolveStatus::unsupported &&
                result.diagnostic.code == "stock_tyres_source_shader_unsupported",
            "nearby shader family cannot select the tyre source resolver");
}

void rejects_non_finite_resolved_values() {
    MaterialBinding binding = build_material_binding(tyre_material(), 5U);
    binding.properties.at("blurlevel").scalar =
        std::numeric_limits<float>::quiet_NaN();
    const auto blur_result = resolve_stock_tyres_source_constants(binding);
    require(blur_result.status == KsTyreMaterialResolveStatus::invalid_input &&
                blur_result.diagnostic.code ==
                    "stock_tyres_source_constants_non_finite",
            "non-finite tyre controls are rejected");

    binding = build_material_binding(tyre_material(), 5U);
    binding.properties.at("ksemissive").vector3[1] =
        std::numeric_limits<float>::infinity();
    const auto material_result = resolve_stock_tyres_source_constants(binding);
    require(material_result.status == KsTyreMaterialResolveStatus::invalid_input &&
                material_result.diagnostic.code ==
                    "stock_tyres_source_constants_non_finite",
            "non-finite lighting values are rejected");
}

} // namespace

int main() {
    try {
        static_assert(sizeof(StockTyresSourceMaterialConstants) == 32U);
        static_assert(sizeof(StockTyresSourceConstants) == 32U);
        resolves_both_exact_shader_aliases_without_clamping();
        rejects_missing_or_extra_resources();
        rejects_non_tyre_shader_alias();
        rejects_non_finite_resolved_values();
        std::cout << "tyre material resolver tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tyre material resolver tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
