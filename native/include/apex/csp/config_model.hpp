#pragma once

#include "apex/formats/ini.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::csp {

// The model stage never opens an include or executes CSP expressions.  These
// limits bound both a parser-produced document and a caller-constructed AST.
struct CspEvaluationLimits {
    std::size_t maxInputSections = 100'000;
    std::size_t maxInputEntries = 1'000'000;
    std::size_t maxOutputSections = 100'000;
    std::size_t maxConditions = 100'000;
    std::size_t maxConditionPoints = 1'000'000;
    std::size_t maxIncludes = 100'000;
    std::size_t maxUnknownEntries = 1'000'000;
    std::size_t maxDiagnostics = 1'000'000;
};

// Names are case-insensitive in CSP config keys and condition references. The
// map keys accepted here are normalized to upper case by the evaluator.
struct CspEvaluationContext {
    std::map<std::string, double> inputs;
    std::map<std::string, double> conditions;
    double year_progress = 0.5;
    double time_seconds = 43'200.0;
};

enum class CspDiagnosticSeverity { warning, error };

struct CspDiagnostic {
    CspDiagnosticSeverity severity = CspDiagnosticSeverity::warning;
    std::string code;
    std::string message;
    std::string source;
    std::size_t line = 0;
    std::size_t section_index = 0;
    std::size_t entry_index = 0;
};

struct CspSourceRef {
    std::string source;
    std::size_t line = 0;
    std::size_t section_index = 0;
    std::size_t entry_index = 0;
};

enum class CspSectionKind { unknown, condition, include };

struct CspConditionPoint {
    double input = 0.0;
    double output = 0.0;
};

struct CspConditionResult {
    std::size_t section_index = 0;
    std::string name;
    std::string input_name;
    std::vector<CspConditionPoint> points;
    double input_value = 0.0;
    double value = 0.0;
    bool supported = true;
    CspSourceRef provenance;
};

enum class CspIncludePathStatus { safe, rejected };

struct CspIncludeRef {
    // path is normalized and non-empty only when status == safe. A rejected
    // include never exposes its raw path through path, so downstream code
    // cannot accidentally open an unsafe value.
    std::string path;
    std::string requested_path;
    bool resolved = false;
    CspIncludePathStatus status = CspIncludePathStatus::rejected;
    std::string diagnostic;
    CspSourceRef provenance;
};

struct CspSectionResult {
    std::size_t section_index = 0;
    std::string name;
    std::string normalized_name;
    std::string source;
    std::size_t line = 0;
    CspSectionKind kind = CspSectionKind::unknown;
    bool indexed = false;
    std::string index_prefix;
    std::optional<std::size_t> numeric_index;

    // Activation follows CSP's last-authored-key rule. ACTIVE and ENABLED
    // are both accepted as controls; if both occur, whichever occurs later
    // in the ordered INI entries wins.
    std::string activation_key;
    double activation_value = 1.0;
    bool active = true;

    // CONDITION is a scalar gate. Unknown conditions are retained and produce
    // an explicit diagnostic with a zero factor; they are never approximated.
    std::string condition_name = "ALWAYS_ON";
    double condition_value = 1.0;
    bool effective = true;

    // Entry indexes point into ConfigModel::document.sections[section_index],
    // retaining every unknown key without copying or dropping its value.
    std::vector<std::size_t> unknown_entry_indices;
};

struct CspConfigModel {
    apex::formats::IniDocument document;
    std::vector<CspSectionResult> sections;
    std::vector<CspConditionResult> conditions;
    std::vector<CspIncludeRef> includes;
    std::vector<CspDiagnostic> diagnostics;
    std::map<std::string, double> resolved_conditions;
    std::map<std::string, double> resolved_inputs;

    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] const CspSectionResult* section(std::size_t index) const noexcept;
    [[nodiscard]] const CspConditionResult* condition(std::string_view name) const noexcept;
};

[[nodiscard]] CspConfigModel evaluate_csp_config(
    apex::formats::IniDocument document,
    CspEvaluationContext context = {},
    CspEvaluationLimits limits = {});

// Explicit model naming is useful to callers that do not want to imply that
// shader/material effects have already been evaluated.
[[nodiscard]] inline CspConfigModel build_csp_config_model(
    apex::formats::IniDocument document,
    CspEvaluationContext context = {},
    CspEvaluationLimits limits = {}) {
    return evaluate_csp_config(std::move(document), std::move(context), limits);
}

}  // namespace apex::csp
