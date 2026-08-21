#include "apex/csp/material_evaluator.hpp"

#include "apex/formats/ini.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace apex::csp {
namespace {

using apex::formats::IniEntry;
using apex::formats::IniSection;

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) --last;
    return std::string(value.substr(first, last - first));
}

std::string upper(std::string_view value) {
    std::string result(value);
    for (char& character : result) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return result;
}

std::string lower(std::string_view value) {
    std::string result(value);
    for (char& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

bool finite(double value) { return std::isfinite(value); }

bool looks_like_unsupported_template(std::string_view value) {
    return value.find("{{") != std::string_view::npos || value.find("}}") != std::string_view::npos ||
           value.find("${") != std::string_view::npos || value.find("lua(") != std::string_view::npos ||
           value.find("execute(") != std::string_view::npos;
}

MaterialValue from_ini(const apex::formats::IniValue& value) {
    if (const auto* number = value.number_value(); number != nullptr) return MaterialValue::from_scalar(*number);
    if (const auto* numbers = value.numbers_value(); numbers != nullptr) return MaterialValue::from_vector(*numbers);
    return MaterialValue::from_string(value.string_value() != nullptr ? *value.string_value() : value.raw);
}

const IniEntry* find_last_entry(const IniSection& section, std::string_view key) {
    const auto wanted = upper(trim(key));
    for (auto iterator = section.entries.rbegin(); iterator != section.entries.rend(); ++iterator) {
        if (iterator->key == wanted) return &*iterator;
    }
    return nullptr;
}

std::string find_last_value(const IniSection& section, std::string_view key, std::string_view fallback = {}) {
    const auto* entry = find_last_entry(section, key);
    return entry == nullptr ? std::string(fallback) : entry->value;
}

const IniEntry* nearby_entry(const IniSection& section, std::size_t start, std::string_view key,
                             std::string_view boundary_prefix) {
    for (std::size_t index = start + 1; index < section.entries.size(); ++index) {
        if (section.entries[index].key.rfind(std::string(boundary_prefix), 0) == 0) break;
        if (section.entries[index].key == key) return &section.entries[index];
    }
    return nullptr;
}

CspSourceRef source_for(const CspConfigModel& config, std::size_t section_index,
                        std::size_t entry_index = std::numeric_limits<std::size_t>::max()) {
    CspSourceRef source;
    source.section_index = section_index;
    source.entry_index = entry_index == std::numeric_limits<std::size_t>::max() ? 0 : entry_index;
    if (section_index < config.document.sections.size()) {
        const auto& section = config.document.sections[section_index];
        source.source = section.source;
        source.line = section.line;
        if (entry_index < section.entries.size()) {
            source.line = section.entries[entry_index].line;
        }
    }
    if (section_index < config.sections.size()) {
        const auto& model_section = config.sections[section_index];
        source.source = model_section.source.empty() ? source.source : model_section.source;
        if (entry_index == std::numeric_limits<std::size_t>::max() ||
            section_index >= config.document.sections.size() ||
            entry_index >= config.document.sections[section_index].entries.size()) {
            if (model_section.line != 0) source.line = model_section.line;
        }
    }
    return source;
}

bool safe_resource_file(std::string_view raw, std::size_t max_bytes, std::string& normalized) {
    if (raw.empty() || raw.size() > max_bytes || raw.find('\0') != std::string_view::npos) return false;
    if (raw.front() == '/' || raw.front() == '\\' ||
        (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) != 0 && raw[1] == ':')) return false;
    normalized.clear();
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto end = raw.find_first_of("/\\", start);
        const auto component = raw.substr(start, end == std::string_view::npos ? raw.size() - start : end - start);
        if (component.empty() || component == "." || component == ".." || component.find(':') != std::string_view::npos) return false;
        if (!normalized.empty()) normalized.push_back('/');
        normalized.append(component);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return !normalized.empty() && normalized.size() <= max_bytes;
}

bool glob_match(std::string_view pattern, std::string_view value) {
    // CSP's reference wildcard primitive treats '?' as the multi-character
    // wildcard and all other characters, including '*', literally.
    const std::string p = upper(pattern), v = upper(value);
    std::size_t pi = 0, vi = 0, star = std::string::npos, checkpoint = 0;
    while (vi < v.size()) {
        if (pi < p.size() && p[pi] == '?') {
            star = pi++;
            checkpoint = vi;
        } else if (pi < p.size() && p[pi] == v[vi]) {
            ++pi;
            ++vi;
        } else if (star != std::string::npos) {
            pi = star + 1;
            vi = ++checkpoint;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '?') ++pi;
    return pi == p.size();
}

struct SelectorStatus {
    bool valid = true;
    bool matched = false;
    std::size_t terms = 0;
};

bool balanced_selector(std::string_view text) {
    char quote = '\0';
    int braces = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if ((character == '\'' || character == '"') && (index == 0 || text[index - 1] != '\\')) {
            quote = quote == '\0' ? character : quote == character ? '\0' : quote;
        } else if (quote == '\0' && character == '{') {
            ++braces;
        } else if (quote == '\0' && character == '}') {
            if (braces == 0) return false;
            --braces;
        }
    }
    return quote == '\0' && braces == 0;
}

bool match_term(std::string_view term, const apex::formats::Kn5Node& node,
                const apex::formats::Kn5Material& material, bool material_target,
                std::size_t& selector_texture_scan, std::size_t& total_texture_scan,
                std::size_t max_selector_texture_scan, std::size_t max_total_texture_scan,
                bool& resource_limit) {
    const auto colon = term.find(':');
    if (colon != std::string_view::npos && colon > 0) {
        const auto kind = lower(trim(term.substr(0, colon)));
        const auto pattern = trim(term.substr(colon + 1));
        if (kind == "material") return glob_match(pattern, material.name);
        if (kind == "shader") return glob_match(pattern, material.shader);
        if (kind == "texture") {
            for (const auto& resource : material.resources) {
                if (selector_texture_scan >= max_selector_texture_scan ||
                    total_texture_scan >= max_total_texture_scan) {
                    resource_limit = true;
                    return false;
                }
                ++selector_texture_scan;
                ++total_texture_scan;
                if (glob_match(pattern, resource.texture)) return true;
            }
            return false;
        }
    }
    return glob_match(trim(term), material_target ? material.name : node.name);
}

SelectorStatus match_selector(std::string_view raw, const apex::formats::Kn5Node& node,
                              const apex::formats::Kn5Material& material, bool material_target,
                              std::size_t& term_count, std::size_t& total_texture_scan,
                              const MaterialEvaluationLimits& limits,
                              bool& resource_limit) {
    SelectorStatus status;
    if (!balanced_selector(raw)) {
        status.valid = false;
        return status;
    }
    // Avoid constructing an unbounded selector list when a caller supplies a
    // hostile comma storm. This conservative precheck may reject commas in a
    // quoted value, but never accepts more work than the configured bound.
    std::size_t rough_terms = 1;
    for (const char character : raw) {
        if (character == ',' && rough_terms <= limits.max_selector_terms) ++rough_terms;
    }
    if (rough_terms > limits.max_selector_terms) {
        term_count += rough_terms;
        status.valid = false;
        return status;
    }
    const auto selectors = apex::formats::split_csp_list(raw);
    for (const auto& selector_raw : selectors) {
        auto expression = trim(selector_raw);
        if (expression.size() >= 2 && ((expression.front() == '\'' && expression.back() == '\'') ||
                                       (expression.front() == '"' && expression.back() == '"'))) {
            expression = expression.substr(1, expression.size() - 2);
        }
        if (expression.size() >= 2 && expression.front() == '{' && expression.back() == '}') {
            expression = trim(std::string_view(expression).substr(1, expression.size() - 2));
        }
        if (expression.empty()) {
            status.valid = false;
            continue;
        }
        bool expression_match = true;
        std::size_t selector_texture_scan = 0;
        std::size_t start = 0;
        while (start <= expression.size()) {
            const auto separator = expression.find('&', start);
            const auto term_text = trim(std::string_view(expression).substr(
                start, separator == std::string::npos ? expression.size() - start : separator - start));
            if (term_text.empty()) {
                status.valid = false;
            } else {
                ++term_count;
                ++status.terms;
                if (term_count > limits.max_selector_terms) return status;
                bool negative = term_text.front() == '!';
                const auto term = negative ? trim(std::string_view(term_text).substr(1)) : term_text;
                if (term.empty()) {
                    status.valid = false;
                } else {
                    const bool found = match_term(term, node, material, material_target,
                                                  selector_texture_scan, total_texture_scan,
                                                  limits.max_texture_scan_per_selector,
                                                  limits.max_texture_scan_total, resource_limit);
                    expression_match = expression_match && (negative ? !found : found);
                }
            }
            if (separator == std::string::npos) break;
            start = separator + 1;
        }
        status.matched = status.matched || (status.valid && expression_match);
    }
    return status;
}

MaterialValue original_value(const apex::formats::Kn5Material& material, std::string_view name) {
    const auto wanted = lower(name);
    for (const auto& property : material.properties) {
        if (lower(property.name) != wanted) continue;
        if (wanted == "ksemissive") return MaterialValue::from_vector({property.value3[0], property.value3[1], property.value3[2]});
        return MaterialValue::from_scalar(property.value);
    }
    return MaterialValue::from_scalar(0.0);
}

MaterialValue interpolate(const MaterialValue& a, const MaterialValue& b, double factor) {
    if (a.kind == MaterialValue::Kind::scalar && b.kind == MaterialValue::Kind::scalar) {
        return MaterialValue::from_scalar(a.scalar + (b.scalar - a.scalar) * factor);
    }
    if (a.kind == MaterialValue::Kind::vector && b.kind == MaterialValue::Kind::vector) {
        const auto size = std::max(a.vector.size(), b.vector.size());
        std::vector<double> result;
        result.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            const double av = a.vector.empty() ? 0.0 : index < a.vector.size() ? a.vector[index] : a.vector.back();
            const double bv = b.vector.empty() ? 0.0 : index < b.vector.size() ? b.vector[index] : b.vector.back();
            result.push_back(av + (bv - av) * factor);
        }
        return MaterialValue::from_vector(std::move(result));
    }
    return factor < 0.5 ? a : b;
}

std::optional<MaterialValue> parse_value(std::string_view text) {
    const auto value = apex::formats::parse_csp_value(text);
    if (const auto* number = value.number_value(); number != nullptr && finite(*number)) return from_ini(value);
    if (const auto* values = value.numbers_value(); values != nullptr) {
        if (std::all_of(values->begin(), values->end(), finite)) return from_ini(value);
        return std::nullopt;
    }
    return from_ini(value);
}

void diagnostic(MaterialEvaluationResult& result, const MaterialEvaluationLimits& limits,
                 CspDiagnosticSeverity severity, std::string code, std::string message,
                 CspSourceRef provenance) {
    if (result.diagnostics.size() >= limits.max_diagnostics) {
        result.limit_exceeded = true;
        result.supported = false;
        return;
    }
    result.diagnostics.push_back({severity, std::move(code), std::move(message), std::move(provenance)});
    if (severity == CspDiagnosticSeverity::error) result.supported = false;
}

void add_state_provenance(MaterialEvaluationResult& result, const MaterialEvaluationLimits& limits,
                          MaterialStateProvenance provenance) {
    if (result.state_provenance.size() >= limits.max_state_overrides) {
        result.limit_exceeded = true;
        diagnostic(result, limits, CspDiagnosticSeverity::error, "STATE_LIMIT",
                   "material state provenance exceeds configured limit", provenance.provenance);
        return;
    }
    result.state_provenance.push_back(std::move(provenance));
}

void apply_replacement(const CspConfigModel& config, const IniSection& section, std::size_t section_index,
                       const apex::formats::Kn5Material& material, MaterialEvaluationResult& result,
                       const MaterialEvaluationLimits& limits) {
    (void)material;
    const auto apply_state = [&](std::string_view key, std::string_view field, std::string& destination) {
        const auto* entry = find_last_entry(section, key);
        if (entry == nullptr) return;
        destination = entry->value;
        add_state_provenance(result, limits, {std::string(field), entry->value,
                                              source_for(config, section_index,
                                                         static_cast<std::size_t>(entry - section.entries.data()))});
    };
    apply_state("SHADER", "shader", result.state.shader);
    apply_state("BLEND_MODE", "blend_mode", result.state.blend_mode);
    apply_state("DEPTH_MODE", "depth_mode", result.state.depth_mode);
    if (const auto* entry = find_last_entry(section, "CULL_MODE"); entry != nullptr) {
        result.state.cull_mode = entry->value;
        add_state_provenance(result, limits, {"cull_mode", entry->value, source_for(config, section_index,
                                                                                      static_cast<std::size_t>(entry - section.entries.data()))});
    }
    for (std::size_t index = 0; index < section.entries.size(); ++index) {
        const auto& entry = section.entries[index];
        if (entry.key.rfind("PROP_", 0) != 0) continue;
        const auto parts = apex::formats::split_csp_list(entry.value);
        if (parts.size() < 2) {
            diagnostic(result, limits, CspDiagnosticSeverity::warning, "MALFORMED_PROPERTY",
                       "PROP_ entry needs a name and value", source_for(config, section_index, index));
            continue;
        }
        if (result.properties.size() >= limits.max_properties && !result.properties.contains(lower(parts[0]))) {
            diagnostic(result, limits, CspDiagnosticSeverity::error, "PROPERTY_LIMIT",
                       "material property output exceeds configured limit", source_for(config, section_index, index));
            result.limit_exceeded = true;
            continue;
        }
        const auto value_text = [&] {
            std::string joined = parts[1];
            for (std::size_t part = 2; part < parts.size(); ++part) joined += "," + parts[part];
            return joined;
        }();
        const auto parsed = parse_value(value_text);
        if (!parsed.has_value()) {
            diagnostic(result, limits, CspDiagnosticSeverity::warning, "NON_FINITE_PROPERTY",
                       "non-finite property value is not evaluated", source_for(config, section_index, index));
            continue;
        }
        const auto key = lower(parts[0]);
        if (result.properties.contains(key)) {
            diagnostic(result, limits, CspDiagnosticSeverity::warning, "DUPLICATE_PROPERTY",
                       "later matching replacement wins by source order", source_for(config, section_index, index));
        }
        result.properties[key] = *parsed;
        if (result.property_provenance.size() < limits.max_properties) {
            result.property_provenance.push_back({key, *parsed, source_for(config, section_index, index)});
        }
    }
    for (std::size_t index = 0; index < section.entries.size(); ++index) {
        const auto& entry = section.entries[index];
        if (entry.key.rfind("RESOURCE_", 0) != 0 || entry.key.size() <= 9 ||
            !std::all_of(entry.key.begin() + 9, entry.key.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) continue;
        const auto suffix = entry.key.substr(9);
        std::string texture, file, color_text;
        bool has_texture = false, has_file = false, has_color = false;
        std::size_t value_entry = index;
        for (std::size_t cursor = index + 1; cursor < section.entries.size(); ++cursor) {
            if (section.entries[cursor].key.rfind("RESOURCE_", 0) == 0 &&
                section.entries[cursor].key.size() > 9 &&
                std::all_of(section.entries[cursor].key.begin() + 9, section.entries[cursor].key.end(),
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) break;
            if (section.entries[cursor].key == "RESOURCE_TEXTURE_" + suffix && !has_texture) { texture = section.entries[cursor].value; has_texture = true; value_entry = cursor; }
            if (section.entries[cursor].key == "RESOURCE_FILE_" + suffix && !has_file) { file = section.entries[cursor].value; has_file = true; value_entry = cursor; }
            if (section.entries[cursor].key == "RESOURCE_COLOR_" + suffix && !has_color) { color_text = section.entries[cursor].value; has_color = true; value_entry = cursor; }
        }
        if (!has_texture) { if (const auto* fallback = find_last_entry(section, "RESOURCE_TEXTURE_" + suffix); fallback != nullptr) { texture = fallback->value; value_entry = static_cast<std::size_t>(fallback - section.entries.data()); } }
        if (!has_file) { if (const auto* fallback = find_last_entry(section, "RESOURCE_FILE_" + suffix); fallback != nullptr) { file = fallback->value; value_entry = static_cast<std::size_t>(fallback - section.entries.data()); } }
        if (!has_color) { if (const auto* fallback = find_last_entry(section, "RESOURCE_COLOR_" + suffix); fallback != nullptr) { color_text = fallback->value; value_entry = static_cast<std::size_t>(fallback - section.entries.data()); } }
        const auto slots = apex::formats::split_csp_list(entry.value);
        const auto slot = lower(slots.empty() ? entry.value : slots.front());
        if (result.resources.size() >= limits.max_resources && !result.resources.contains(slot)) {
            diagnostic(result, limits, CspDiagnosticSeverity::error, "RESOURCE_LIMIT",
                       "material resource output exceeds configured limit", source_for(config, section_index, index));
            result.limit_exceeded = true;
            continue;
        }
        std::optional<MaterialValue> color;
        if (!color_text.empty()) {
            const auto parsed_color = parse_value(color_text);
            if (parsed_color.has_value() && parsed_color->kind != MaterialValue::Kind::string) {
                color = parsed_color;
            } else {
                diagnostic(result, limits, CspDiagnosticSeverity::warning, "INVALID_RESOURCE_COLOR",
                           "RESOURCE_COLOR must be a finite numeric scalar or vector",
                           source_for(config, section_index, value_entry));
            }
        }
        MaterialResourceAdjustment adjustment{slot, texture, file, color, source_for(config, section_index, value_entry)};
        if (!file.empty()) {
            std::string normalized_file;
            if (safe_resource_file(file, limits.max_string_bytes, normalized_file)) {
                adjustment.file = std::move(normalized_file);
                adjustment.file_status = MaterialResourceAdjustment::PathStatus::safe;
            } else {
                adjustment.file.clear();
                adjustment.file_status = MaterialResourceAdjustment::PathStatus::rejected;
                diagnostic(result, limits, CspDiagnosticSeverity::warning, "UNSAFE_RESOURCE_PATH",
                           "RESOURCE_FILE is rejected because it is not a safe relative path",
                           adjustment.provenance);
            }
        }
        if (result.resources.contains(slot)) diagnostic(result, limits, CspDiagnosticSeverity::warning,
                                                         "DUPLICATE_RESOURCE", "later matching replacement wins by source order",
                                                         adjustment.provenance);
        result.resources[slot] = adjustment;
        if (result.resource_provenance.size() < limits.max_resources) result.resource_provenance.push_back(adjustment);
    }
}

}  // namespace

MaterialValue MaterialValue::from_string(std::string value) {
    MaterialValue result;
    result.kind = Kind::string;
    result.text = std::move(value);
    return result;
}

MaterialValue MaterialValue::from_scalar(double value) {
    MaterialValue result;
    result.kind = Kind::scalar;
    result.scalar = value;
    return result;
}

MaterialValue MaterialValue::from_vector(std::vector<double> value) {
    MaterialValue result;
    result.kind = Kind::vector;
    result.vector = std::move(value);
    return result;
}

MaterialEvaluationResult evaluate_csp_material(
    const CspConfigModel& config, const apex::formats::Kn5Node& node,
    const apex::formats::Kn5Material& material, MaterialEvaluationLimits limits) {
    MaterialEvaluationResult result;
    const auto reject_input = [&](std::string code, std::string message) {
        diagnostic(result, limits, CspDiagnosticSeverity::error, std::move(code), std::move(message), {});
        result.limit_exceeded = true;
    };
    if (node.name.size() > limits.max_string_bytes || material.name.size() > limits.max_string_bytes ||
        material.shader.size() > limits.max_string_bytes) {
        reject_input("STRING_LIMIT", "material evaluator input string exceeds configured limit");
        return result;
    }
    if (config.document.source.size() > limits.max_string_bytes) {
        reject_input("STRING_LIMIT", "CSP provenance input exceeds configured string limit");
        return result;
    }
    if (config.sections.size() > limits.max_sections) {
        reject_input("SECTION_LIMIT", "CSP section model exceeds configured limit");
        return result;
    }
    for (const auto& model_section : config.sections) {
        if (model_section.source.size() > limits.max_string_bytes ||
            model_section.name.size() > limits.max_string_bytes) {
            reject_input("STRING_LIMIT", "CSP section provenance exceeds configured string limit");
            return result;
        }
    }
    if (material.properties.size() > limits.max_input_properties || material.resources.size() > limits.max_input_resources) {
        reject_input("INPUT_LIMIT", "material property/resource input exceeds configured limit");
        return result;
    }
    if (config.document.sections.size() > limits.max_sections) {
        reject_input("SECTION_LIMIT", "CSP section input exceeds configured limit");
        return result;
    }
    for (const auto& property : material.properties) {
        if (property.name.size() > limits.max_string_bytes) {
            reject_input("STRING_LIMIT", "material property name exceeds configured limit");
            return result;
        }
    }
    for (const auto& resource : material.resources) {
        if (resource.slot.size() > limits.max_string_bytes || resource.texture.size() > limits.max_string_bytes) {
            reject_input("STRING_LIMIT", "material resource name exceeds configured limit");
            return result;
        }
    }
    std::size_t input_entries = 0;
    for (const auto& section : config.document.sections) {
        if (section.source.size() > limits.max_string_bytes || section.name.size() > limits.max_string_bytes ||
            section.entries.size() > limits.max_input_entries ||
            input_entries > limits.max_input_entries - section.entries.size()) {
            reject_input("INPUT_LIMIT", "CSP section/entry input exceeds configured limit");
            return result;
        }
        input_entries += section.entries.size();
        for (const auto& entry : section.entries) {
            if (entry.key.size() > limits.max_string_bytes || entry.value.size() > limits.max_string_bytes) {
                reject_input("STRING_LIMIT", "CSP section entry string exceeds configured limit");
                return result;
            }
        }
    }
    result.state.shader = material.shader;
    result.state.blend_mode = std::to_string(material.blendMode);
    result.state.depth_mode = std::to_string(material.depthMode);
    result.state.is_transparent = node.transparent;
    result.state.layer = static_cast<double>(node.layer);
    result.state.lod_in = node.lodIn;
    result.state.lod_out = node.lodOut;
    result.state.cast_shadows = node.castShadows;

    std::size_t total_texture_scan = 0;
    bool resource_limit = false;
    std::size_t sections_seen = 0, selector_terms = 0, expansions = 0;
    for (std::size_t section_index = 0; section_index < config.document.sections.size(); ++section_index) {
        if (sections_seen++ >= limits.max_sections) {
            diagnostic(result, limits, CspDiagnosticSeverity::error, "SECTION_LIMIT",
                       "material evaluator section scan exceeds configured limit", source_for(config, section_index));
            result.limit_exceeded = true;
            break;
        }
        const auto& section = config.document.sections[section_index];
        const auto name = upper(section.name);
        const bool replacement = name.rfind("SHADER_REPLACEMENT_", 0) == 0;
        const bool adjustment = name.rfind("MATERIAL_ADJUSTMENT_", 0) == 0;
        const bool mesh_adjustment = name.rfind("MESH_ADJUSTMENT_", 0) == 0;
        if (!replacement && !adjustment && !mesh_adjustment) {
            bool template_value = section.source.rfind("<built-in:", 0) == 0;
            for (const auto& entry : section.entries) template_value = template_value || looks_like_unsupported_template(entry.value);
            if (template_value) {
                result.supported = false;
                diagnostic(result, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_TEMPLATE",
                            "built-in CSP template is not expanded by the native material evaluator",
                            source_for(config, section_index));
            }
            continue;
        }
        // JS gates these sections only by ACTIVE. CONDITION is interpreted
        // below for material adjustments; replacements and mesh adjustments
        // intentionally ignore it.
        if (section_index < config.sections.size() && !config.sections[section_index].active) continue;
        const auto material_selector = find_last_value(section, "MATERIALS");
        const auto mesh_selector = find_last_value(section, "MESHES");
        if (material_selector.empty() && mesh_selector.empty()) {
            diagnostic(result, limits, CspDiagnosticSeverity::warning, "MISSING_SELECTOR",
                       "material effect has neither MATERIALS nor MESHES", source_for(config, section_index));
            continue;
        }
        bool matched = true;
        if (!material_selector.empty()) {
            const auto status = match_selector(material_selector, node, material, true, selector_terms, total_texture_scan, limits, resource_limit);
            if (!status.valid) {
                result.supported = false;
                diagnostic(result, limits, CspDiagnosticSeverity::warning, "MALFORMED_SELECTOR",
                            "MATERIALS selector is malformed or uses unsupported alternation",
                            source_for(config, section_index));
            }
            matched = matched && status.valid && status.matched;
        }
        if (!mesh_selector.empty()) {
            const auto status = match_selector(mesh_selector, node, material, false, selector_terms, total_texture_scan, limits, resource_limit);
            if (!status.valid) {
                result.supported = false;
                diagnostic(result, limits, CspDiagnosticSeverity::warning, "MALFORMED_SELECTOR",
                            "MESHES selector is malformed or uses unsupported alternation",
                            source_for(config, section_index));
            }
            matched = matched && status.valid && status.matched;
        }
        if (selector_terms > limits.max_selector_terms) {
            diagnostic(result, limits, CspDiagnosticSeverity::error, "SELECTOR_LIMIT",
                       "selector term count exceeds configured limit", source_for(config, section_index));
            result.limit_exceeded = true;
            break;
        }
        if (resource_limit) {
            diagnostic(result, limits, CspDiagnosticSeverity::warning, "RESOURCE_SCAN_LIMIT",
                       "texture selector resource scan was bounded", source_for(config, section_index));
            resource_limit = false;
        }
        if (!matched) continue;
        result.matched_sections.push_back(source_for(config, section_index));
        if (replacement) {
            for (const auto& entry : section.entries) {
                if (looks_like_unsupported_template(entry.value)) {
                    result.supported = false;
                    diagnostic(result, limits, CspDiagnosticSeverity::warning, "UNSUPPORTED_EXPRESSION",
                                "shader/material expression is retained but not executed by this evaluator",
                                source_for(config, section_index));
                }
            }
            if (section.source.rfind("<built-in:", 0) == 0 && ++expansions > limits.max_expansions) {
                diagnostic(result, limits, CspDiagnosticSeverity::error, "EXPANSION_LIMIT",
                           "safe declarative built-in expansion count exceeds configured limit", source_for(config, section_index));
                result.limit_exceeded = true;
                break;
            }
            apply_replacement(config, section, section_index, material, result, limits);
        } else if (adjustment) {
            const auto condition_name = upper(find_last_value(section, "CONDITION", "ALWAYS_ON"));
            double factor = condition_name == "ALWAYS_ON" ? 1.0 :
                            (condition_name == "ALWAYS_OFF" || condition_name == "ALWAYS OFF") ? 0.0 : 0.0;
            if (factor == 0.0 && condition_name != "ALWAYS_OFF" && condition_name != "ALWAYS OFF") {
                const auto found = config.resolved_conditions.find(condition_name);
                if (found != config.resolved_conditions.end()) factor = found->second;
                else diagnostic(result, limits, CspDiagnosticSeverity::warning, "UNRESOLVED_CONDITION",
                                "material adjustment condition is unresolved and has zero effect", source_for(config, section_index));
            }
            factor = std::clamp(finite(factor) ? factor : 0.0, 0.0, 1.0);
            for (std::size_t index = 0; index < section.entries.size(); ++index) {
                const auto& entry = section.entries[index];
                if (entry.key.rfind("KEY_", 0) != 0) continue;
                const auto suffix = entry.key.substr(4);
                if (suffix.empty()) continue;
                const std::string value_key = "VALUE_" + suffix;
                const std::string off_key = "OFF_VALUE_" + suffix;
                const std::string alternate_off_key = value_key + "_OFF";
                const IniEntry* on_entry = nearby_entry(section, index, value_key, "KEY_");
                if (on_entry == nullptr) on_entry = find_last_entry(section, value_key);
                const IniEntry* off_entry = nearby_entry(section, index, off_key, "KEY_");
                if (off_entry == nullptr) off_entry = nearby_entry(section, index, alternate_off_key, "KEY_");
                if (off_entry == nullptr) off_entry = find_last_entry(section, off_key);
                if (off_entry == nullptr) off_entry = find_last_entry(section, alternate_off_key);
                const auto original = result.properties.contains(lower(entry.value)) ? result.properties.at(lower(entry.value)) : original_value(material, entry.value);
                const auto on = on_entry == nullptr ? original : parse_value(on_entry->value).value_or(original);
                const auto off = off_entry == nullptr || upper(trim(off_entry->value)) == "ORIGINAL" || off_entry->value.empty()
                                     ? original
                                     : parse_value(off_entry->value).value_or(original);
                const auto value = interpolate(off, on, factor);
                const auto key = lower(entry.value);
                if (result.properties.size() >= limits.max_properties && !result.properties.contains(key)) {
                    diagnostic(result, limits, CspDiagnosticSeverity::error, "PROPERTY_LIMIT",
                               "material property output exceeds configured limit", source_for(config, section_index, index));
                    result.limit_exceeded = true;
                    continue;
                }
                result.properties[key] = value;
                const auto provenance_index = on_entry == nullptr ? index : static_cast<std::size_t>(on_entry - section.entries.data());
                if (result.property_provenance.size() < limits.max_properties) {
                    result.property_provenance.push_back({key, value, source_for(config, section_index, provenance_index)});
                }
            }
        } else {
            const auto apply_number = [&](std::string_view key, std::string_view field, double& destination) {
                const auto* entry = find_last_entry(section, key);
                if (entry == nullptr) return;
                const auto value = parse_value(entry->value);
                if (!value.has_value() || value->kind != MaterialValue::Kind::scalar || !finite(value->scalar)) {
                    diagnostic(result, limits, CspDiagnosticSeverity::warning, "INVALID_MESH_OVERRIDE",
                               "mesh override must be a finite scalar", source_for(config, section_index));
                    return;
                }
                destination = value->scalar;
                add_state_provenance(result, limits, {std::string(field), entry->value,
                                                      source_for(config, section_index,
                                                                 static_cast<std::size_t>(entry - section.entries.data()))});
            };
            if (const auto* entry = find_last_entry(section, "IS_TRANSPARENT"); entry != nullptr) {
                const auto value = parse_value(entry->value);
                if (value.has_value() && value->kind == MaterialValue::Kind::scalar) {
                    result.state.is_transparent = value->scalar != 0.0;
                    add_state_provenance(result, limits, {"is_transparent", entry->value,
                                                          source_for(config, section_index,
                                                                     static_cast<std::size_t>(entry - section.entries.data()))});
                }
                else diagnostic(result, limits, CspDiagnosticSeverity::warning, "INVALID_MESH_OVERRIDE",
                                "IS_TRANSPARENT must be numeric", source_for(config, section_index));
            }
            apply_number("LAYER", "layer", result.state.layer);
            apply_number("LOD_IN", "lod_in", result.state.lod_in);
            apply_number("LOD_OUT", "lod_out", result.state.lod_out);
            if (const auto* entry = find_last_entry(section, "CAST_SHADOWS"); entry != nullptr) {
                const auto value = parse_value(entry->value);
                if (value.has_value() && value->kind == MaterialValue::Kind::scalar) {
                    result.state.cast_shadows = value->scalar != 0.0;
                    add_state_provenance(result, limits, {"cast_shadows", entry->value,
                                                          source_for(config, section_index,
                                                                     static_cast<std::size_t>(entry - section.entries.data()))});
                }
                else diagnostic(result, limits, CspDiagnosticSeverity::warning, "INVALID_MESH_OVERRIDE",
                                "CAST_SHADOWS must be numeric", source_for(config, section_index));
            }
        }
    }
    return result;
}

}  // namespace apex::csp
