#include "apex/csp/config_model.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace apex::csp {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' ||
                                    value[first] == '\r' || value[first] == '\n' || value[first] == '\f')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' ||
                            value[last - 1] == '\r' || value[last - 1] == '\n' || value[last - 1] == '\f')) {
        --last;
    }
    return value.substr(first, last - first);
}

[[nodiscard]] std::string upper_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    }
    return result;
}

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && upper_ascii(value.substr(0, prefix.size())) == prefix;
}

[[nodiscard]] bool has_operator(std::string_view value) noexcept {
    return value.find_first_of("&|!(){}") != std::string_view::npos;
}

struct IncludePath {
    std::string normalized;
    std::string requested;
    bool safe = false;
    std::string diagnostic;
};

[[nodiscard]] IncludePath normalize_include_path(std::string_view raw) {
    IncludePath result;
    result.requested = std::string(raw);
    std::string source(trim(raw));
    if (source.size() >= 2 &&
        ((source.front() == '\'' && source.back() == '\'') ||
         (source.front() == '"' && source.back() == '"'))) {
        source = source.substr(1, source.size() - 2);
    }
    if (source.find('\0') != std::string::npos) {
        result.diagnostic = "include path contains NUL";
        return result;
    }
    std::replace(source.begin(), source.end(), '\\', '/');
    source = std::string(trim(source));
    if (source.empty()) {
        result.diagnostic = "include path is empty";
        return result;
    }
    if (source.front() == '/') {
        result.diagnostic = "absolute include path is not allowed";
        return result;
    }
    if (source.size() >= 2 && std::isalpha(static_cast<unsigned char>(source[0])) != 0 &&
        source[1] == ':') {
        result.diagnostic = "drive-qualified include path is not allowed";
        return result;
    }

    std::string normalized;
    std::size_t start = 0;
    while (start <= source.size()) {
        const auto end = source.find('/', start);
        const auto part = std::string(trim(std::string_view(source).substr(
            start, end == std::string::npos ? source.size() - start : end - start)));
        if (part == "..") {
            result.diagnostic = "parent traversal in include path is not allowed";
            return result;
        }
        if (!part.empty() && part != ".") {
            if (!normalized.empty()) normalized.push_back('/');
            normalized.append(part);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (normalized.empty()) {
        result.diagnostic = "include path is empty";
        return result;
    }
    result.normalized = std::move(normalized);
    result.safe = true;
    return result;
}

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

[[noreturn]] void fail(std::string_view source, std::size_t line, std::string_view code,
                       std::string_view message) {
    throw apex::core::ParseError("CSP", std::string(source), line, std::string(code),
                                 std::string(message));
}

void add_diagnostic(CspConfigModel& model, const CspEvaluationLimits& limits,
                    CspDiagnosticSeverity severity, std::string_view code,
                    std::string_view message, const CspSourceRef& provenance) {
    if (model.diagnostics.size() >= limits.maxDiagnostics) {
        fail(provenance.source, provenance.line, "DIAGNOSTIC_LIMIT",
             "CSP diagnostic output exceeds configured limit");
    }
    model.diagnostics.push_back({severity, std::string(code), std::string(message),
                                 provenance.source, provenance.line,
                                 provenance.section_index, provenance.entry_index});
}

[[nodiscard]] CspSourceRef section_source(const apex::formats::IniDocument& document,
                                          std::size_t section_index,
                                          std::size_t entry_index = 0) {
    const auto& section = document.sections[section_index];
    std::size_t line = section.line;
    if (entry_index < section.entries.size()) line = section.entries[entry_index].line;
    return {section.source.empty() ? document.source : section.source, line,
            section_index, entry_index};
}

[[nodiscard]] CspSectionKind section_kind(std::string_view normalized) noexcept {
    if (starts_with(normalized, "CONDITION") &&
        (normalized.size() == 9 || normalized[9] == '_' || normalized[9] == ':')) {
        return CspSectionKind::condition;
    }
    if (starts_with(normalized, "INCLUDE") &&
        (normalized.size() == 7 || normalized[7] == '_' || normalized[7] == ':')) {
        return CspSectionKind::include;
    }
    return CspSectionKind::unknown;
}

void identify_index(CspSectionResult& result, const CspSourceRef& provenance,
                    CspConfigModel& model, const CspEvaluationLimits& limits) {
    const auto separator = result.normalized_name.rfind('_');
    if (separator == std::string::npos || separator + 1 >= result.normalized_name.size()) return;
    const auto suffix = std::string_view(result.normalized_name).substr(separator + 1);
    for (const char character : suffix) {
        if (character < '0' || character > '9') return;
    }
    std::size_t index = 0;
    const auto* first = suffix.data();
    const auto* last = first + suffix.size();
    const auto parsed = std::from_chars(first, last, index);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INDEX_LIMIT",
                       "numeric section suffix is outside the native index range", provenance);
        return;
    }
    result.indexed = true;
    result.index_prefix = result.name.substr(0, separator);
    result.numeric_index = index;
}

[[nodiscard]] std::map<std::string, double> normalized_values(
    const std::map<std::string, double>& values) {
    std::map<std::string, double> result;
    for (const auto& [name, value] : values) result[upper_ascii(name)] = value;
    return result;
}

[[nodiscard]] double input_value(const std::map<std::string, double>& inputs,
                                 std::string_view name) {
    const auto found = inputs.find(upper_ascii(name));
    return found == inputs.end() ? 0.0 : found->second;
}

[[nodiscard]] bool known_key(CspSectionKind kind, std::string_view key) noexcept {
    const auto normalized = upper_ascii(key);
    if (normalized == "ACTIVE" || normalized == "ENABLED") return true;
    if (kind == CspSectionKind::condition) {
        return normalized == "NAME" || normalized == "INPUT" || normalized == "LUT";
    }
    if (kind == CspSectionKind::include) return normalized == "INCLUDE";
    return false;
}

void evaluate_condition_lut(CspConfigModel& model, CspConditionResult& condition,
                            const apex::formats::IniSection& section,
                            const CspEvaluationLimits& limits,
                            std::size_t& condition_point_count) {
    const auto* entry = section.last_entry("LUT");
    if (entry == nullptr || trim(entry->value).empty()) return;
    const auto lut_entry_index = static_cast<std::size_t>(entry - section.entries.data());
    const CspSourceRef lut_provenance{section.source.empty() ? model.document.source : section.source,
                                      entry->line, condition.section_index, lut_entry_index};
    const auto raw = trim(entry->value);
    if (raw.size() < 4 || raw.substr(0, 2) != "(|" || raw.substr(raw.size() - 2) != "|)") {
        condition.supported = false;
        add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_EXPRESSION",
                       "condition LUT must use CSP's (|input=output|...) syntax",
                       lut_provenance);
        return;
    }
    const auto body = raw.substr(2, raw.size() - 4);
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find('|', start);
        const auto part = trim(body.substr(start, end == std::string_view::npos ? body.size() - start : end - start));
        if (!part.empty()) {
            const auto equals = part.find('=');
            if (equals == std::string_view::npos) {
                condition.supported = false;
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_EXPRESSION",
                               "condition LUT point has no '=' separator",
                               lut_provenance);
                return;
            }
            const auto input = apex::formats::parse_csp_value(trim(part.substr(0, equals)));
            const auto output = apex::formats::parse_csp_value(trim(part.substr(equals + 1)));
            const auto* input_number = input.number_value();
            double output_number = 0.0;
            if (output.number_value() != nullptr) output_number = *output.number_value();
            else if (output.numbers_value() != nullptr && !output.numbers_value()->empty()) output_number = output.numbers_value()->front();
            else {
                condition.supported = false;
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_EXPRESSION",
                               "condition LUT output must be a finite number or numeric vector",
                               lut_provenance);
                return;
            }
            if (input_number == nullptr || !finite(*input_number) || !finite(output_number)) {
                condition.supported = false;
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INVALID_CONDITION_POINT",
                               "condition LUT points must contain finite numeric values",
                               lut_provenance);
                return;
            }
            if (condition_point_count >= limits.maxConditionPoints) {
                fail(lut_provenance.source, lut_provenance.line, "CONDITION_LIMIT",
                     "condition point output exceeds configured limit");
            }
            condition.points.push_back({*input_number, output_number});
            ++condition_point_count;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    std::sort(condition.points.begin(), condition.points.end(),
              [](const auto& left, const auto& right) { return left.input < right.input; });
}

[[nodiscard]] double evaluate_points(const std::vector<CspConditionPoint>& points,
                                     double input) noexcept {
    if (points.empty()) return input;
    if (input <= points.front().input) return points.front().output;
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (input <= points[index].input) {
            const double denominator = points[index].input - points[index - 1].input;
            const double t = denominator == 0.0 ? 1.0 : (input - points[index - 1].input) / denominator;
            return points[index - 1].output + (points[index].output - points[index - 1].output) * t;
        }
    }
    return points.back().output;
}

}  // namespace

bool CspConfigModel::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == CspDiagnosticSeverity::error;
    });
}

const CspSectionResult* CspConfigModel::section(std::size_t index) const noexcept {
    return index < sections.size() ? &sections[index] : nullptr;
}

const CspConditionResult* CspConfigModel::condition(std::string_view name) const noexcept {
    const auto wanted = upper_ascii(trim(name));
    for (auto iterator = conditions.rbegin(); iterator != conditions.rend(); ++iterator) {
        if (upper_ascii(iterator->name) == wanted) return &*iterator;
    }
    return nullptr;
}

CspConfigModel evaluate_csp_config(apex::formats::IniDocument document,
                                   CspEvaluationContext context,
                                   CspEvaluationLimits limits) {
    if (document.sections.size() > limits.maxInputSections) {
        fail(document.source, 0, "SECTION_LIMIT", "CSP input section count exceeds configured limit");
    }
    if (document.sections.size() > limits.maxOutputSections) {
        fail(document.source, 0, "OUTPUT_LIMIT", "CSP section output exceeds configured limit");
    }
    std::size_t input_entries = 0;
    for (const auto& section : document.sections) {
        if (section.entries.size() > limits.maxInputEntries - std::min(limits.maxInputEntries, input_entries)) {
            fail(section.source.empty() ? document.source : section.source, section.line,
                 "ENTRY_LIMIT", "CSP input entry count exceeds configured limit");
        }
        input_entries += section.entries.size();
    }

    CspConfigModel model;
    model.document = std::move(document);
    for (const auto& warning : model.document.warnings) {
        add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INI_WARNING",
                       warning.message, {warning.source, warning.line, 0, 0});
    }
    model.sections.reserve(model.document.sections.size());
    const auto supplied_inputs = normalized_values(context.inputs);
    auto supplied_conditions = normalized_values(context.conditions);
    std::size_t unknown_entry_count = 0;
    std::size_t condition_point_count = 0;
    model.resolved_inputs = {{"ONE", 1.0}, {"YEAR_PROGRESS", context.year_progress},
                             {"TIME", context.time_seconds}};
    for (const auto& [name, value] : supplied_inputs) model.resolved_inputs[name] = value;
    for (auto& [name, value] : model.resolved_inputs) {
        if (!finite(value)) {
            add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INVALID_CONTEXT",
                           "non-finite input replaced with zero", {model.document.source, 0, 0, 0});
            value = 0.0;
        }
    }
    for (auto& [name, value] : supplied_conditions) {
        if (!finite(value)) {
            add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INVALID_CONTEXT",
                           "non-finite condition replaced with zero", {model.document.source, 0, 0, 0});
            value = 0.0;
        }
    }

    for (std::size_t section_index = 0; section_index < model.document.sections.size(); ++section_index) {
        const auto& source_section = model.document.sections[section_index];
        const auto provenance = section_source(model.document, section_index);
        CspSectionResult result;
        result.section_index = section_index;
        result.name = source_section.name;
        result.normalized_name = upper_ascii(source_section.name);
        result.source = source_section.source.empty() ? model.document.source : source_section.source;
        result.line = source_section.line;
        result.kind = section_kind(result.normalized_name);
        identify_index(result, provenance, model, limits);

        std::size_t activation_entry = std::numeric_limits<std::size_t>::max();
        std::size_t condition_entry = std::numeric_limits<std::size_t>::max();
        std::size_t name_entry = std::numeric_limits<std::size_t>::max();
        std::size_t input_entry = std::numeric_limits<std::size_t>::max();
        for (std::size_t entry_index = 0; entry_index < source_section.entries.size(); ++entry_index) {
            const auto& entry = source_section.entries[entry_index];
            if (entry.key == "ACTIVE" || entry.key == "ENABLED") activation_entry = entry_index;
            if (entry.key == "CONDITION") condition_entry = entry_index;
            if (entry.key == "NAME") name_entry = entry_index;
            if (entry.key == "INPUT") input_entry = entry_index;
            if (!known_key(result.kind, entry.key)) {
                if (unknown_entry_count >= limits.maxUnknownEntries) {
                    fail(result.source, entry.line, "UNKNOWN_KEY_LIMIT",
                         "unknown CSP key output exceeds configured limit");
                }
                result.unknown_entry_indices.push_back(entry_index);
                ++unknown_entry_count;
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_KEY",
                               "CSP key is preserved but has no native evaluator in this stage",
                               section_source(model.document, section_index, entry_index));
            }
        }
        if (result.kind == CspSectionKind::unknown) {
            add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_SECTION",
                           "CSP section is preserved but has no native evaluator in this stage", provenance);
        }

        if (activation_entry != std::numeric_limits<std::size_t>::max()) {
            const auto& entry = source_section.entries[activation_entry];
            result.activation_key = entry.key;
            if (const auto* number = entry.typed.number_value(); number != nullptr && finite(*number)) {
                result.activation_value = *number;
                result.active = *number != 0.0;
            } else {
                result.activation_value = 1.0;
                result.active = true;
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INVALID_ACTIVATION",
                               "ACTIVE/ENABLED must be numeric; preserving CSP's active fallback",
                               section_source(model.document, section_index, activation_entry));
            }
        }
        if (condition_entry != std::numeric_limits<std::size_t>::max()) {
            result.condition_name = upper_ascii(trim(source_section.entries[condition_entry].value));
            if (result.condition_name.empty()) result.condition_name = "ALWAYS_ON";
        }
        result.effective = result.active;
        model.sections.push_back(std::move(result));

        if (source_section.entries.size() > limits.maxInputEntries) {
            fail(provenance.source, provenance.line, "ENTRY_LIMIT", "section entry count exceeds configured limit");
        }
        // Condition and include records are collected in the same source order
        // as the INI AST; this also makes duplicate definitions deterministic.
        if (model.sections.back().kind == CspSectionKind::condition) {
            if (model.conditions.size() >= limits.maxConditions) {
                fail(provenance.source, provenance.line, "CONDITION_LIMIT",
                     "condition output exceeds configured limit");
            }
            CspConditionResult condition;
            condition.section_index = section_index;
            condition.provenance = provenance;
            condition.name = name_entry == std::numeric_limits<std::size_t>::max()
                                 ? ""
                                 : upper_ascii(trim(source_section.entries[name_entry].value));
            condition.input_name = input_entry == std::numeric_limits<std::size_t>::max()
                                       ? "ONE"
                                       : upper_ascii(trim(source_section.entries[input_entry].value));
            if (condition.name.empty()) {
                condition.supported = false;
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INVALID_CONDITION",
                               "condition section has no NAME", provenance);
            }
            if (condition.input_name.empty()) condition.input_name = "ONE";
            evaluate_condition_lut(model, condition, source_section, limits, condition_point_count);
            condition.input_value = input_value(model.resolved_inputs, condition.input_name);
            condition.value = condition.supported
                                  ? evaluate_points(condition.points, condition.input_value)
                                  : 0.0;
            if (condition.name.empty()) {
                // Keep the row for provenance, but do not create an empty map key.
            } else {
                const auto normalized_name = upper_ascii(condition.name);
                if (model.resolved_conditions.contains(normalized_name)) {
                    add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "DUPLICATE_CONDITION",
                                   "later condition definition wins by source order", provenance);
                }
                model.resolved_conditions[normalized_name] = condition.value;
            }
            model.conditions.push_back(std::move(condition));
        }

        if (model.sections.back().kind == CspSectionKind::include) {
            std::vector<std::pair<std::string, std::size_t>> paths;
            const auto colon = source_section.name.find(':');
            if (colon != std::string::npos && !trim(source_section.name.substr(colon + 1)).empty()) {
                paths.emplace_back(std::string(trim(source_section.name.substr(colon + 1))), 0);
            }
            for (std::size_t entry_index = 0; entry_index < source_section.entries.size(); ++entry_index) {
                const auto& entry = source_section.entries[entry_index];
                if (entry.key != "INCLUDE") continue;
                for (const auto& path : apex::formats::split_csp_list(entry.value)) {
                    paths.emplace_back(path, entry_index);
                }
            }
            for (const auto& [path, entry_index] : paths) {
                if (model.includes.size() >= limits.maxIncludes) {
                    fail(provenance.source, provenance.line, "INCLUDE_LIMIT",
                         "include output exceeds configured limit");
                }
                const auto include_provenance = section_source(model.document, section_index, entry_index);
                const auto normalized = normalize_include_path(path);
                if (!normalized.safe) {
                    model.includes.push_back({{}, normalized.requested, false,
                                              CspIncludePathStatus::rejected,
                                              normalized.diagnostic, include_provenance});
                    add_diagnostic(model, limits, CspDiagnosticSeverity::warning,
                                   "UNSAFE_INCLUDE_PATH", normalized.diagnostic,
                                   include_provenance);
                } else {
                    model.includes.push_back({normalized.normalized, normalized.requested, false,
                                              CspIncludePathStatus::safe, {}, include_provenance});
                    add_diagnostic(model, limits, CspDiagnosticSeverity::warning, "INCLUDE_UNRESOLVED",
                                   "include is recorded but file loading is outside this evaluator stage",
                                   include_provenance);
                }
            }
        }
    }

    for (const auto& [name, value] : supplied_conditions) model.resolved_conditions[name] = value;
    for (auto& result : model.sections) {
        if (result.condition_name == "ALWAYS_ON") result.condition_value = 1.0;
        else if (result.condition_name == "ALWAYS_OFF" || result.condition_name == "ALWAYS OFF") result.condition_value = 0.0;
        else {
            const auto found = model.resolved_conditions.find(result.condition_name);
            if (found == model.resolved_conditions.end()) {
                result.condition_value = 0.0;
                const auto source = section_source(model.document, result.section_index);
                add_diagnostic(model, limits, CspDiagnosticSeverity::warning,
                               has_operator(result.condition_name) ? "UNSUPPORTED_EXPRESSION" : "UNRESOLVED_CONDITION",
                               has_operator(result.condition_name)
                                   ? "condition expression is preserved but not supported by this evaluator stage"
                                   : "condition name has no definition or context value",
                               source);
            } else {
                result.condition_value = found->second;
            }
        }
        result.effective = result.active && result.condition_value != 0.0;
    }
    return model;
}

}  // namespace apex::csp
