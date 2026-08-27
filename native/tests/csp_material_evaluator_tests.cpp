#include "apex/csp/material_evaluator.hpp"
#include "apex/formats/ini.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

apex::formats::Kn5Node node() {
    apex::formats::Kn5Node result;
    result.kind = "mesh";
    result.name = "CarBodyMesh";
    result.transparent = false;
    result.castShadows = true;
    result.layer = 2;
    result.lodIn = 3.0F;
    result.lodOut = 20.0F;
    return result;
}

apex::formats::Kn5Material material() {
    apex::formats::Kn5Material result;
    result.name = "BodyPaint";
    result.shader = "ksPerPixel";
    result.blendMode = 0;
    result.depthMode = 1;
    result.properties.push_back({"roughness", 0.2F, {}, {}, {}});
    result.resources.push_back({"txDiffuse", 0, "body_d.dds"});
    result.resources.push_back({"txNormal", 1, "body_nm.dds"});
    return result;
}

void matches_wildcards_and_source_order() {
    const auto document = apex::formats::parse_csp_ini(
        "[SHADER_REPLACEMENT_0]\n"
        "MATERIALS={ material:Body? & shader:ks? }\n"
        "SHADER=smCarPaint\nBLEND_MODE=1\nCULL_MODE=none\n"
        "PROP_0=roughness,0.4\nRESOURCE_0=txDiffuse\nRESOURCE_TEXTURE_0=paint.dds\n"
        "[SHADER_REPLACEMENT_1]\nMESHES=CarBody?\nSHADER=smCarPaint_old\n"
        "PROP_0=roughness,0.6\nRESOURCE_0=txDiffuse\nRESOURCE_FILE_0=paint_override.dds\n",
        "material.ini");
    const auto config = apex::csp::evaluate_csp_config(document);
    const auto result = apex::csp::evaluate_csp_material(config, node(), material());
    require(result.supported, "declarative replacement is supported");
    require(result.state.shader == "smCarPaint_old" && result.state.blend_mode == "1" &&
                result.state.cull_mode.has_value() && *result.state.cull_mode == "none",
            "later replacement state wins");
    require(result.properties.at("roughness").scalar == 0.6, "later property replacement wins");
    require(result.resources.at("txdiffuse").file == "paint_override.dds" &&
                result.resources.at("txdiffuse").file_status ==
                    apex::csp::MaterialResourceAdjustment::PathStatus::safe,
            "later resource replacement wins");
    require(result.matched_sections.size() == 2, "compound wildcard selectors match both sections");
    require(result.property_provenance.back().provenance.source == "material.ini" &&
                result.property_provenance.back().provenance.line == 12,
            "property provenance retains source");
}

void applies_condition_weighted_adjustment_and_mesh_state() {
    const auto document = apex::formats::parse_csp_ini(
        "[CONDITION_SWITCH]\nNAME=SWITCH\nINPUT=ONE\nLUT=(|0=0|1=0.25|)\n"
        "[MATERIAL_ADJUSTMENT_0]\nMATERIALS=BodyPaint\nCONDITION=SWITCH\n"
        "KEY_0=roughness\nVALUE_0=1\nOFF_VALUE_0=0\n"
        "[MESH_ADJUSTMENT_0]\nMESHES=CarBodyMesh\nIS_TRANSPARENT=1\nLAYER=5\nLOD_IN=4\nLOD_OUT=8\nCAST_SHADOWS=0\n",
        "adjustment.ini");
    apex::csp::CspEvaluationContext context;
    context.conditions["SWITCH"] = 0.25;
    const auto config = apex::csp::evaluate_csp_config(document, context);
    const auto result = apex::csp::evaluate_csp_material(config, node(), material());
    require(result.properties.at("roughness").kind == apex::csp::MaterialValue::Kind::scalar &&
                result.properties.at("roughness").scalar > 0.24 && result.properties.at("roughness").scalar < 0.26,
            "condition factor interpolates from original to on value");
    require(result.state.is_transparent && result.state.layer == 5.0 && result.state.lod_in == 4.0 &&
                result.state.lod_out == 8.0 && !result.state.cast_shadows,
            "mesh adjustment state is applied");
}

void diagnoses_unsupported_selector_template_and_limits() {
    const auto document = apex::formats::parse_csp_ini(
        "[SHADER_REPLACEMENT_BAD]\nMATERIALS=BodyPaint|Other\nSHADER=lua(setting('x'))\n"
        "[CUSTOM_TEMPLATE]\nVALUE={{ execute() }}\n",
        "unsupported-material.ini");
    const auto config = apex::csp::evaluate_csp_config(document);
    auto result = apex::csp::evaluate_csp_material(config, node(), material());
    bool malformed = false, saw_template = false;
    for (const auto& diagnostic : result.diagnostics) {
        malformed = malformed || diagnostic.code == "MALFORMED_SELECTOR";
        saw_template = saw_template || diagnostic.code == "UNSUPPORTED_TEMPLATE";
    }
    require(!malformed && saw_template && !result.supported && result.matched_sections.empty(),
            "pipe is a literal selector character and templates are attributed");

    const auto limited_document = apex::formats::parse_csp_ini(
        "[SHADER_REPLACEMENT_0]\nMATERIALS=BodyPaint\nPROP_0=a,1\nPROP_1=b,2\n", "limit.ini");
    apex::csp::MaterialEvaluationLimits limits;
    limits.max_properties = 1;
    const auto limited = apex::csp::evaluate_csp_material(
        apex::csp::evaluate_csp_config(limited_document), node(), material(), limits);
    require(limited.limit_exceeded, "property output limit is enforced before growth");
}

void preserves_js_condition_gates_and_confines_resources() {
    const auto document = apex::formats::parse_csp_ini(
        "[SHADER_REPLACEMENT_OFF]\nMATERIALS=BodyPaint\nCONDITION=ALWAYS_OFF\nSHADER=offShader\n"
        "RESOURCE_0=txDiffuse\nRESOURCE_FILE_0=../escape.dds\nRESOURCE_COLOR_0=not-a-color\n"
        "[MATERIAL_ADJUSTMENT_OFF]\nMATERIALS=BodyPaint\nCONDITION=ALWAYS_OFF\n"
        "KEY_0=roughness\nVALUE_0=1\nOFF_VALUE_0=0.9\n"
        "[MESH_ADJUSTMENT_OFF]\nMESHES=CarBodyMesh\nCONDITION=ALWAYS_OFF\nIS_TRANSPARENT=1\n",
        "gates.ini");
    const auto result = apex::csp::evaluate_csp_material(
        apex::csp::evaluate_csp_config(document), node(), material());
    require(result.state.shader == "offShader", "replacement ignores CONDITION like JS");
    require(result.properties.at("roughness").scalar == 0.9, "zero condition applies OFF_VALUE");
    require(result.state.is_transparent, "mesh adjustment ignores CONDITION like JS");
    const auto& resource = result.resources.at("txdiffuse");
    require(resource.file.empty() &&
                resource.file_status == apex::csp::MaterialResourceAdjustment::PathStatus::rejected &&
                !resource.color.has_value(),
            "unsafe resource path and invalid color are not exposed");
    bool unsafe = false, invalid_color = false;
    for (const auto& diagnostic : result.diagnostics) {
        unsafe = unsafe || diagnostic.code == "UNSAFE_RESOURCE_PATH";
        invalid_color = invalid_color || diagnostic.code == "INVALID_RESOURCE_COLOR";
    }
    require(unsafe && invalid_color, "resource path and color diagnostics are attributed");
}

void bounds_direct_inputs_and_resets_texture_scan() {
    const auto document = apex::formats::parse_csp_ini(
        "[SHADER_REPLACEMENT_0]\nMATERIALS=texture:missing\nSHADER=one\n"
        "[SHADER_REPLACEMENT_1]\nMATERIALS=texture:missing\nSHADER=two\n", "scan.ini");
    apex::csp::MaterialEvaluationLimits scan_limits;
    scan_limits.max_texture_scan_per_selector = 2;
    scan_limits.max_texture_scan_total = 4;
    const auto result = apex::csp::evaluate_csp_material(
        apex::csp::evaluate_csp_config(document), node(), material(), scan_limits);
    require(result.matched_sections.empty(), "missing texture selector does not match");
    for (const auto& diagnostic : result.diagnostics) {
        require(diagnostic.code != "RESOURCE_SCAN_LIMIT", "per-selector texture scans reset");
    }

    auto direct = apex::formats::parse_csp_ini(
        "[SHADER_REPLACEMENT_0]\nMATERIALS=BodyPaint\nSHADER=shader\n", "direct.ini");
    apex::csp::MaterialEvaluationLimits input_limits;
    input_limits.max_string_bytes = 3;
    const auto bounded = apex::csp::evaluate_csp_material(
        apex::csp::evaluate_csp_config(direct), node(), material(), input_limits);
    require(bounded.limit_exceeded && !bounded.supported, "direct input string limit is enforced");
}

}  // namespace

int main() {
    try {
        matches_wildcards_and_source_order();
        applies_condition_weighted_adjustment_and_mesh_state();
        diagnoses_unsupported_selector_template_and_limits();
        preserves_js_condition_gates_and_confines_resources();
        bounds_direct_inputs_and_resets_texture_scan();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
