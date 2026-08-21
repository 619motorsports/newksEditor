#include "apex/core/parse_error.hpp"
#include "apex/csp/config_model.hpp"
#include "apex/formats/ini.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using apex::core::ParseError;
using apex::csp::CspEvaluationContext;
using apex::csp::CspEvaluationLimits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void expects_error(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const ParseError& error) {
        require(error.code() == code, "unexpected CSP model error code");
        return;
    }
    throw std::runtime_error("CSP model accepted over-limit input");
}

void matches_fixture_include_activation_and_unknown_preservation() {
    // These declarations are taken from the repository's
    // test/content/cars/619_gen6_arca_base/extension/ext_config.ini fixture.
    const auto config = apex::formats::parse_csp_ini(
        "[INCLUDE]\n"
        "INCLUDE=common/conditions.ini, common/materials_interior.ini, common/materials_carpaint.ini, common/materials_glass.ini\n"
        "[BASIC]\nRACING_CAR = 1\n"
        "[TYRES_FX]\nENABLED = 1\n"
        "[BRAKEDISC_FX]\nACTIVE = 1\n"
        "[Material_Metal_v2]\nMaterials=backside\nUnknownFutureKey=retained\n",
        "fixture-ext_config.ini");
    const auto model = apex::csp::evaluate_csp_config(config);
    require(model.document.sections.size() == 5, "fixture sections preserved");
    require(model.includes.size() == 4, "fixture include list");
    require(model.includes[0].path == "common/conditions.ini", "first include path");
    require(model.includes[0].provenance.source == "fixture-ext_config.ini" &&
                model.includes[0].provenance.line == 2,
            "include provenance");
    require(model.includes[0].resolved == false, "include remains unresolved");
    require(model.sections[2].activation_key == "ENABLED" && model.sections[2].active,
            "ENABLED activation");
    require(model.sections[3].activation_key == "ACTIVE" && model.sections[3].active,
            "ACTIVE activation");
    require(model.sections[4].unknown_entry_indices.size() == 2,
            "unknown section keys retained by index");
    require(model.document.sections[4].entries[1].key == "UNKNOWNFUTUREKEY" &&
                model.document.sections[4].entries[1].value == "retained",
            "unknown key/value preserved");
    require(!model.diagnostics.empty(), "unresolved and unsupported diagnostics");
}

void evaluates_conditions_and_indexed_sections() {
    const auto document = apex::formats::parse_csp_ini(
        "[CONDITION_DAYLIGHT]\nNAME=DAYLIGHT\nINPUT=YEAR_PROGRESS\nLUT=(|0=0|0.5=0.25|1=1|)\n"
        "[CAMERA_0]\nACTIVE=1\nCONDITION=DAYLIGHT\nPOSITION=0,1,2\n"
        "[CAMERA_1]\nACTIVE=0\nCONDITION=ALWAYS_ON\n"
        "[CAMERA_1]\nACTIVE=1\nCONDITION=DAYLIGHT\n",
        "conditions.ini");
    CspEvaluationContext context;
    context.year_progress = 0.75;
    const auto model = apex::csp::evaluate_csp_config(document, context);
    require(model.conditions.size() == 1, "condition section count");
    require(model.conditions[0].points.size() == 3, "condition LUT points");
    require(model.conditions[0].input_value == 0.75, "condition input context");
    require(model.conditions[0].value > 0.25 && model.conditions[0].value < 1.0,
            "condition interpolation");
    require(model.sections[1].indexed && model.sections[1].numeric_index == 0 &&
                model.sections[1].index_prefix == "CAMERA",
            "indexed section metadata");
    require(model.sections[1].effective, "active condition section effective");
    require(!model.sections[2].effective, "inactive section effective state");
    require(model.sections[3].effective, "duplicate indexed section retained");
}

void applies_last_key_precedence_and_context_overrides() {
    const auto document = apex::formats::parse_csp_ini(
        "[A]\nACTIVE=0\nACTIVE=1\nENABLED=0\n"
        "[B]\nENABLED=0\nACTIVE=1\n"
        "[C]\nCONDITION=USER_SWITCH\n",
        "precedence.ini");
    CspEvaluationContext context;
    context.conditions["USER_SWITCH"] = 2.0;
    const auto model = apex::csp::evaluate_csp_config(document, context);
    require(model.sections[0].activation_key == "ENABLED" && !model.sections[0].active,
            "last activation key wins");
    require(model.sections[1].activation_key == "ACTIVE" && model.sections[1].active,
            "last cross-key activation wins");
    require(model.sections[2].condition_value == 2.0 && model.sections[2].effective,
            "context condition override");
}

void diagnoses_unsupported_expressions_without_approximation() {
    const auto document = apex::formats::parse_csp_ini(
        "[CONDITION_BAD]\nNAME=BAD\nINPUT=ONE\nLUT=lua(setting('x'))\n"
        "[MATERIAL_ADJUSTMENT_0]\nCONDITION=BAD && OTHER\nVALUE=kept\n",
        "unsupported.ini");
    const auto model = apex::csp::evaluate_csp_config(document);
    require(model.conditions.size() == 1 && !model.conditions[0].supported,
            "unsupported LUT marked unsupported");
    require(model.conditions[0].value == 0.0, "unsupported LUT is not approximated");
    require(model.sections[1].condition_value == 0.0 && !model.sections[1].effective,
            "unsupported condition expression is not approximated");
    bool saw_expression = false, saw_section = false;
    for (const auto& diagnostic : model.diagnostics) {
        saw_expression = saw_expression || diagnostic.code == "UNSUPPORTED_EXPRESSION";
        saw_section = saw_section || diagnostic.code == "UNSUPPORTED_SECTION";
    }
    require(saw_expression && saw_section, "unsupported diagnostics emitted");
    require(model.document.sections[1].entries[1].value == "kept",
            "unsupported section entry retained");
}

void rejects_model_limits_and_malformed_condition_points() {
    const auto document = apex::formats::parse_csp_ini(
        "[CONDITION_A]\nNAME=A\nLUT=(|0=0|1=1|)\n"
        "[CONDITION_B]\nNAME=B\nLUT=(|0=0|1=1|)\n"
        "[INCLUDE]\nINCLUDE=a.ini,b.ini\n",
        "limits.ini");
    CspEvaluationLimits output_limits;
    output_limits.maxOutputSections = 1;
    expects_error([&] { (void)apex::csp::evaluate_csp_config(document, {}, output_limits); },
                  "OUTPUT_LIMIT");
    CspEvaluationLimits condition_limits;
    condition_limits.maxConditions = 1;
    expects_error([&] { (void)apex::csp::evaluate_csp_config(document, {}, condition_limits); },
                  "CONDITION_LIMIT");
    CspEvaluationLimits include_limits;
    include_limits.maxIncludes = 1;
    expects_error([&] { (void)apex::csp::evaluate_csp_config(document, {}, include_limits); },
                  "INCLUDE_LIMIT");
    CspEvaluationLimits point_limits;
    point_limits.maxConditionPoints = 1;
    expects_error([&] { (void)apex::csp::evaluate_csp_config(document, {}, point_limits); },
                  "CONDITION_LIMIT");
    const auto unknown = apex::formats::parse_csp_ini("[UNKNOWN]\nA=1\nB=2\n", "unknown.ini");
    CspEvaluationLimits unknown_limits;
    unknown_limits.maxUnknownEntries = 1;
    expects_error([&] { (void)apex::csp::evaluate_csp_config(unknown, {}, unknown_limits); },
                  "UNKNOWN_KEY_LIMIT");
    const auto malformed = apex::formats::parse_csp_ini(
        "[CONDITION_BAD]\nNAME=BAD\nLUT=(|0=0|broken|)\n", "malformed.ini");
    const auto model = apex::csp::evaluate_csp_config(malformed);
    require(!model.conditions[0].supported, "malformed point unsupported");
}

void classifies_include_paths_and_sanitizes_context() {
    const auto document = apex::formats::parse_csp_ini(
        "[INCLUDE]\nINCLUDE=./common\\conditions.ini,../escape.ini,/absolute.ini,C:\\\\outside.ini\n"
        "[MATERIAL_ADJUSTMENT_0]\nCONDITION=BAD_CONTEXT\nVALUE=kept\n",
        "unsafe-includes.ini");
    CspEvaluationContext context;
    context.conditions["BAD_CONTEXT"] = std::numeric_limits<double>::quiet_NaN();
    const auto model = apex::csp::evaluate_csp_config(document, context);
    require(model.includes.size() == 4, "all include declarations retained");
    require(model.includes[0].status == apex::csp::CspIncludePathStatus::safe &&
                model.includes[0].path == "common/conditions.ini" &&
                model.includes[0].requested_path == "./common\\conditions.ini",
            "safe include is normalized with requested provenance");
    for (std::size_t index = 1; index < model.includes.size(); ++index) {
        require(model.includes[index].status == apex::csp::CspIncludePathStatus::rejected &&
                    model.includes[index].path.empty() && !model.includes[index].requested_path.empty() &&
                    !model.includes[index].diagnostic.empty(),
                "unsafe include does not expose an openable path");
    }
    require(model.resolved_conditions.at("BAD_CONTEXT") == 0.0 &&
                model.sections[1].condition_value == 0.0 && !model.sections[1].effective,
            "non-finite condition context is sanitized and inactive");
    bool unsafe = false, invalidContext = false;
    for (const auto& diagnostic : model.diagnostics) {
        unsafe = unsafe || diagnostic.code == "UNSAFE_INCLUDE_PATH";
        invalidContext = invalidContext || diagnostic.code == "INVALID_CONTEXT";
    }
    require(unsafe && invalidContext, "include and context diagnostics");

    apex::formats::IniDocument nul_document;
    nul_document.source = "nul-include.ini";
    apex::formats::IniSection section;
    section.name = "INCLUDE";
    apex::formats::IniEntry entry;
    entry.key = "INCLUDE";
    entry.value = std::string("safe\0unsafe", 11);
    entry.typed = {entry.value, std::string(entry.value)};
    section.entries.push_back(std::move(entry));
    nul_document.sections.push_back(std::move(section));
    const auto nul_model = apex::csp::evaluate_csp_config(std::move(nul_document));
    require(nul_model.includes.size() == 1 && nul_model.includes[0].path.empty() &&
                nul_model.includes[0].status == apex::csp::CspIncludePathStatus::rejected,
            "NUL include path rejected");
}

}  // namespace

int main() {
    try {
        matches_fixture_include_activation_and_unknown_preservation();
        evaluates_conditions_and_indexed_sections();
        applies_last_key_precedence_and_context_overrides();
        diagnoses_unsupported_expressions_without_approximation();
        rejects_model_limits_and_malformed_condition_points();
        classifies_include_paths_and_sanitizes_context();
        std::cout << "CSP config model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CSP config model tests failed: " << error.what() << '\n';
        return 1;
    }
}
