#include "apex/domain/driver.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace apex::domain {
namespace {

[[nodiscard]] std::string upper_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    }
    return result;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
                                    value[first] == '\n' || value[first] == '\f')) ++first;
    std::size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' ||
                            value[last - 1] == '\n' || value[last - 1] == '\f')) --last;
    return value.substr(first, last - first);
}

[[noreturn]] void fail(std::string_view source, std::size_t line, std::string_view code,
                       std::string_view message) {
    throw apex::core::ParseError("DRIVER", std::string(source), line, std::string(code),
                                 std::string(message));
}

void add_diagnostic(std::vector<DriverDiagnostic>& diagnostics, const DriverDataLimits& limits,
                    DriverDiagnosticSeverity severity, std::string_view code,
                    std::string_view message, std::string_view source, std::size_t line,
                    std::size_t section_index = 0, std::size_t entry_index = 0) {
    if (diagnostics.size() >= limits.maxDiagnostics) {
        fail(source, line, "DIAGNOSTIC_LIMIT", "driver diagnostic output exceeds configured limit");
    }
    diagnostics.push_back({severity, std::string(code), std::string(message),
                           std::string(source), line, section_index, entry_index});
}

void append_document_warnings(std::vector<DriverDiagnostic>& diagnostics,
                              const DriverDataLimits& limits,
                              const apex::formats::IniDocument& document) {
    for (const auto& warning : document.warnings) {
        add_diagnostic(diagnostics, limits, DriverDiagnosticSeverity::warning, "INI_WARNING",
                       warning.message, warning.source, warning.line);
    }
}

[[nodiscard]] const apex::formats::IniSection* first_section(
    const apex::formats::IniDocument& document, std::string_view name) noexcept {
    const auto wanted = upper_ascii(name);
    for (const auto& section : document.sections) {
        if (upper_ascii(section.name) == wanted) return &section;
    }
    return nullptr;
}

[[nodiscard]] std::size_t section_index_of(const apex::formats::IniDocument& document,
                                           const apex::formats::IniSection* section) noexcept {
    return section == nullptr ? 0 : static_cast<std::size_t>(section - document.sections.data());
}

[[nodiscard]] float number_field(const apex::formats::IniSection& section,
                                 std::string_view key, float fallback,
                                 std::vector<DriverDiagnostic>& diagnostics,
                                 const DriverDataLimits& limits, std::string_view source) {
    const auto* entry = section.last_entry(key);
    if (entry == nullptr || trim(entry->value).empty()) return fallback;
    const auto raw = apex::formats::parse_csp_value(entry->value);
    const auto* value = raw.number_value();
    if (value != nullptr && std::isfinite(*value) && *value >= -std::numeric_limits<float>::max() &&
        *value <= std::numeric_limits<float>::max()) return static_cast<float>(*value);
    add_diagnostic(diagnostics, limits, DriverDiagnosticSeverity::warning, "NON_FINITE_VALUE",
                   std::string(key) + " must be finite", source, entry->line);
    return fallback;
}

[[nodiscard]] std::array<float, 3> vector_field(const apex::formats::IniSection& section,
                                                std::string_view key,
                                                std::array<float, 3> fallback,
                                                std::vector<DriverDiagnostic>& diagnostics,
                                                const DriverDataLimits& limits,
                                                std::string_view source) {
    const auto* entry = section.last_entry(key);
    if (entry == nullptr || trim(entry->value).empty()) return fallback;
    const auto raw = apex::formats::parse_csp_value(entry->value);
    const auto* values = raw.numbers_value();
    if (values != nullptr && values->size() == 3 &&
        std::all_of(values->begin(), values->end(), [](double value) {
            return std::isfinite(value) && value >= -std::numeric_limits<float>::max() &&
                   value <= std::numeric_limits<float>::max();
        })) {
        return {static_cast<float>((*values)[0]), static_cast<float>((*values)[1]), static_cast<float>((*values)[2])};
    }
    add_diagnostic(diagnostics, limits, DriverDiagnosticSeverity::warning, "INVALID_VECTOR",
                   std::string(key) + " must contain three finite numbers", source, entry->line);
    return fallback;
}

[[nodiscard]] std::string string_field(const apex::formats::IniSection& section,
                                       std::string_view key) {
    const auto* entry = section.last_entry(key);
    return entry == nullptr ? std::string{} : std::string(trim(entry->value));
}

[[nodiscard]] std::optional<std::size_t> decimal_suffix(std::string_view value,
                                                          std::string_view prefix) noexcept {
    const auto normalized = upper_ascii(value);
    const auto wanted = upper_ascii(prefix);
    if (normalized.rfind(wanted, 0) != 0 || normalized.size() == wanted.size()) return std::nullopt;
    std::size_t result = 0;
    for (const char character : std::string_view(normalized).substr(wanted.size())) {
        if (character < '0' || character > '9') return std::nullopt;
        const auto digit = static_cast<std::size_t>(character - '0');
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10U) return std::nullopt;
        result = result * 10U + digit;
    }
    return result;
}

[[nodiscard]] bool safe_relative_path(std::string_view value, std::string& code,
                                      std::string& message) {
    if (value.empty()) {
        code = "EMPTY_ASSET_PATH";
        message = "driver asset path is empty";
        return false;
    }
    if (value.find('\0') != std::string_view::npos || value.front() == '/' || value.front() == '\\' ||
        (value.size() >= 2 && ((value[0] >= 'A' && value[0] <= 'Z') ||
                               (value[0] >= 'a' && value[0] <= 'z')) && value[1] == ':')) {
        code = "UNSAFE_ASSET_PATH";
        message = "driver asset path must be capability-relative";
        return false;
    }
    if (value.find(':') != std::string_view::npos) {
        code = "UNSAFE_ASSET_PATH";
        message = "driver asset path contains a drive or stream separator";
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find_first_of("/\\", start);
        const auto component = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        if (component.empty() || component == "." || component == "..") {
            code = "UNSAFE_ASSET_PATH";
            message = "driver asset path contains an empty or traversal component";
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

[[nodiscard]] std::string basename(std::string_view path) {
    const auto separator = path.find_last_of("/\\");
    return std::string(separator == std::string_view::npos ? path : path.substr(separator + 1));
}

[[nodiscard]] bool finite_matrix(const std::array<float, 16>& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

[[nodiscard]] bool finite_frame(const apex::formats::KsAnimationFrame& frame) noexcept {
    return std::all_of(frame.quaternion.begin(), frame.quaternion.end(), [](float value) {
               return std::isfinite(value);
           }) &&
           std::all_of(frame.position.begin(), frame.position.end(), [](float value) {
               return std::isfinite(value);
           }) &&
           std::all_of(frame.scale.begin(), frame.scale.end(), [](float value) {
               return std::isfinite(value);
           });
}

void walk_kn5_nodes(apex::formats::Kn5Node& node, std::vector<apex::formats::Kn5Node*>& output,
                    const DriverDataLimits& limits) {
    struct Pending { apex::formats::Kn5Node* node; std::size_t depth; };
    std::vector<Pending> pending{{&node, 0}};
    while (!pending.empty()) {
        const auto current_pending = pending.back();
        pending.pop_back();
        auto* current = current_pending.node;
        if (current_pending.depth > limits.maxDepth) fail("KN5", 0, "DEPTH_LIMIT", "driver KN5 hierarchy exceeds configured depth limit");
        if (output.size() >= limits.maxNodes) fail("KN5", 0, "NODE_LIMIT", "driver KN5 node output exceeds configured limit");
        if (current->name.size() > limits.maxNameBytes) fail("KN5", 0, "NAME_LIMIT", "driver KN5 node name exceeds configured limit");
        if (!finite_matrix(current->transform)) fail("KN5", 0, "NON_FINITE_TRANSFORM", "driver KN5 transform is not finite");
        output.push_back(current);
        if (current->children.size() > limits.maxNodes - output.size()) fail("KN5", 0, "NODE_LIMIT", "driver KN5 node output exceeds configured limit");
        for (std::size_t index = current->children.size(); index != 0; --index) {
            pending.push_back({&current->children[index - 1], current_pending.depth + 1});
        }
    }
}

void walk_kn5_const(const apex::formats::Kn5Node& node,
                    std::vector<const apex::formats::Kn5Node*>& output,
                    const DriverDataLimits& limits) {
    struct Pending { const apex::formats::Kn5Node* node; std::size_t depth; };
    std::vector<Pending> pending{{&node, 0}};
    while (!pending.empty()) {
        const auto current_pending = pending.back();
        pending.pop_back();
        const auto* current = current_pending.node;
        if (current_pending.depth > limits.maxDepth) fail("KN5", 0, "DEPTH_LIMIT", "driver KN5 hierarchy exceeds configured depth limit");
        if (output.size() >= limits.maxNodes) fail("KN5", 0, "NODE_LIMIT", "driver KN5 node output exceeds configured limit");
        if (current->name.size() > limits.maxNameBytes) fail("KN5", 0, "NAME_LIMIT", "driver KN5 node name exceeds configured limit");
        if (!finite_matrix(current->transform)) fail("KN5", 0, "NON_FINITE_TRANSFORM", "driver KN5 transform is not finite");
        output.push_back(current);
        if (current->children.size() > limits.maxNodes - output.size()) fail("KN5", 0, "NODE_LIMIT", "driver KN5 node output exceeds configured limit");
        for (std::size_t index = current->children.size(); index != 0; --index) {
            pending.push_back({&current->children[index - 1], current_pending.depth + 1});
        }
    }
}

void collect_meshes(const apex::formats::Kn5Node& node, std::vector<std::string>& meshes,
                    const DriverDataLimits& limits,
                    std::set<const apex::formats::Kn5Node*>& identities,
                    std::set<const apex::formats::Kn5Node*>* all_identities = nullptr) {
    struct Pending { const apex::formats::Kn5Node* node; std::size_t depth; };
    std::vector<Pending> pending{{&node, 0}};
    while (!pending.empty()) {
        const auto current_pending = pending.back();
        pending.pop_back();
        const auto* current = current_pending.node;
        if (current_pending.depth > limits.maxDepth) fail("KN5", 0, "DEPTH_LIMIT", "hidden driver hierarchy exceeds configured depth limit");
        if ((current->kind == "mesh" || current->kind == "skinnedMesh") && identities.insert(current).second) {
            if (meshes.size() >= limits.maxNames) fail("KN5", 0, "NAME_LIMIT", "hidden mesh output exceeds configured limit");
            meshes.push_back(current->name);
            if (all_identities != nullptr) all_identities->insert(current);
        }
        for (std::size_t index = current->children.size(); index != 0; --index) {
            pending.push_back({&current->children[index - 1], current_pending.depth + 1});
        }
    }
}

}  // namespace

bool DriverConfig::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DriverDiagnosticSeverity::error;
    });
}

DriverConfig parse_driver_config(const apex::formats::IniDocument& document,
                                 DriverDataLimits limits) {
    if (document.sections.size() > limits.maxSections) {
        fail(document.source, 0, "SECTION_LIMIT", "driver section count exceeds configured limit");
    }
    DriverConfig result;
    result.source = document.source;
    result.document = document;
    append_document_warnings(result.diagnostics, limits, document);
    std::size_t fields = 0;
    for (const auto& section : document.sections) {
        if (section.name.size() > limits.maxNameBytes) {
            fail(section.source, section.line, "NAME_LIMIT", "driver section name exceeds configured limit");
        }
        if (section.entries.size() > limits.maxFields || fields > limits.maxFields - section.entries.size()) {
            fail(section.source, section.line, "FIELD_LIMIT", "driver field output exceeds configured limit");
        }
        for (const auto& entry : section.entries) {
            if (entry.key.size() > limits.maxNameBytes) {
                fail(section.source, entry.line, "NAME_LIMIT", "driver key name exceeds configured limit");
            }
        }
        fields += section.entries.size();
    }
    const auto* model_section = first_section(document, "MODEL");
    const auto* steer_section = first_section(document, "STEER_ANIMATION");
    const auto* shift_section = first_section(document, "SHIFT_ANIMATION");
    if (model_section == nullptr) {
        add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "MISSING_MODEL",
                       "MODEL section is missing", document.source, 0);
    } else {
        DriverModelConfig model;
        model.section_index = section_index_of(document, model_section);
        model.line = model_section->line;
        model.name = string_field(*model_section, "NAME");
        if (model.name.size() > limits.maxNameBytes) {
            fail(model_section->source, model_section->line, "NAME_LIMIT", "driver model name exceeds configured limit");
        }
        model.position = vector_field(*model_section, "POSITION", {0.0F, 0.0F, 0.0F}, result.diagnostics, limits, document.source);
        if (model.name.empty()) add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "MISSING_MODEL_NAME", "MODEL NAME is missing", document.source, model.line, model.section_index);
        result.model = std::move(model);
    }
    if (steer_section == nullptr) {
        add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "MISSING_STEER", "STEER_ANIMATION section is missing", document.source, 0);
    } else {
        DriverSteerConfig steer;
        steer.section_index = section_index_of(document, steer_section);
        steer.line = steer_section->line;
        steer.name = string_field(*steer_section, "NAME");
        if (steer.name.size() > limits.maxNameBytes) {
            fail(steer_section->source, steer_section->line, "NAME_LIMIT", "driver steering animation name exceeds configured limit");
        }
        steer.lock = number_field(*steer_section, "LOCK", 0.0F, result.diagnostics, limits, document.source);
        if (steer.name.empty()) add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "MISSING_STEER_NAME", "STEER_ANIMATION NAME is missing", document.source, steer.line, steer.section_index);
        if (steer.lock < 0.0F) add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "INVALID_STEER_LOCK", "STEER_ANIMATION LOCK must be nonnegative", document.source, steer.line, steer.section_index);
        result.steer = std::move(steer);
    }
    if (shift_section != nullptr) {
        DriverShiftConfig shift;
        shift.section_index = section_index_of(document, shift_section);
        shift.line = shift_section->line;
        shift.blend_time = number_field(*shift_section, "BLEND_TIME", 0.0F, result.diagnostics, limits, document.source);
        shift.positive_time = number_field(*shift_section, "POSITIVE_TIME", 0.0F, result.diagnostics, limits, document.source);
        shift.static_time = number_field(*shift_section, "STATIC_TIME", 0.0F, result.diagnostics, limits, document.source);
        shift.negative_time = number_field(*shift_section, "NEGATIVE_TIME", 0.0F, result.diagnostics, limits, document.source);
        shift.preload_rpm = number_field(*shift_section, "PRELOAD_RPM", 0.0F, result.diagnostics, limits, document.source);
        shift.invert_shifting_hands = number_field(*shift_section, "INVERT_SHIFTING_HANDS", 0.0F, result.diagnostics, limits, document.source) != 0.0F;
        const float times[] = {shift.blend_time, shift.positive_time, shift.static_time, shift.negative_time, shift.preload_rpm};
        if (std::any_of(std::begin(times), std::end(times), [](float value) { return value < 0.0F; })) add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "INVALID_SHIFT_TIMING", "SHIFT_ANIMATION timing and preload values must be nonnegative", document.source, shift.line, shift.section_index);
        result.shift = std::move(shift);
    }
    for (std::size_t section_index = 0; section_index < document.sections.size(); ++section_index) {
        const auto& section = document.sections[section_index];
        if (!decimal_suffix(section.name, "HIDE_OBJECT_").has_value()) continue;
        const auto name = string_field(section, "NAME");
        if (name.empty()) continue;
        if (result.hide_objects.size() >= limits.maxNames || name.size() > limits.maxNameBytes) {
            fail(section.source, section.line, "NAME_LIMIT", "hidden driver object output exceeds configured limit");
        }
        result.hide_objects.push_back({name, section_index, section.line});
    }
    return result;
}

DriverConfig parse_driver_config(std::string_view text, std::string source,
                                 DriverDataLimits limits) {
    return parse_driver_config(apex::formats::parse_ini(text, source), limits);
}

DriverAssetRequest request_driver_asset(DriverAssetKind kind, std::string_view relative_path,
                                        std::string source, std::size_t line) {
    DriverAssetRequest result;
    result.kind = kind;
    result.relative_path = std::string(trim(relative_path));
    result.source = std::move(source);
    result.line = line;
    result.accepted = safe_relative_path(result.relative_path, result.code, result.message);
    if (result.accepted && result.relative_path.size() > 1U * 1024U * 1024U) {
        result.accepted = false;
        result.code = "NAME_LIMIT";
        result.message = "driver asset path exceeds the configured name limit";
    }
    if (result.accepted) {
        const auto extension = upper_ascii(basename(result.relative_path));
        const std::string_view expected = kind == DriverAssetKind::model_kn5 ? ".KN5" :
            (kind == DriverAssetKind::base_pose_knh ? ".KNH" : ".KSANIM");
        if (extension.size() < expected.size() ||
            extension.compare(extension.size() - expected.size(), expected.size(), expected) != 0) {
            result.accepted = false;
            result.code = "UNEXPECTED_ASSET_TYPE";
            result.message = "driver asset path has an unexpected extension";
        }
    }
    return result;
}

DriverAssetSelection select_driver_model_asset(std::span<const DriverAssetCandidate> candidates,
                                               std::string_view model_name,
                                               DriverDataLimits limits) {
    DriverAssetSelection result;
    if (candidates.size() > limits.maxNames) {
        fail("asset selection", 0, "CANDIDATE_LIMIT", "driver asset candidate count exceeds configured limit");
    }
    const auto name = trim(model_name);
    if (name.empty()) {
        result.status = DriverAssetSelectionStatus::unconfigured;
        return result;
    }
    if (name.size() > limits.maxNameBytes) {
        fail("asset selection", 0, "NAME_LIMIT", "driver model name exceeds configured limit");
    }
    result.expected_basename = std::string(name) + ".kn5";
    const auto expected = upper_ascii(result.expected_basename);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].relative_path.size() > limits.maxNameBytes) {
            add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "NAME_LIMIT",
                           "driver asset candidate path exceeds configured name limit", "asset index", index);
            continue;
        }
        std::string path_code;
        std::string path_message;
        if (!safe_relative_path(candidates[index].relative_path, path_code, path_message)) {
            add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, path_code,
                           path_message, "asset index", index);
            continue;
        }
        if (upper_ascii(basename(candidates[index].relative_path)) == expected) {
            if (result.matches.size() >= limits.maxNames) fail("asset selection", index, "NAME_LIMIT", "driver asset match output exceeds configured limit");
            result.matches.push_back(index);
        }
    }
    if (result.matches.empty()) result.status = DriverAssetSelectionStatus::missing;
    else if (result.matches.size() == 1) {
        result.status = DriverAssetSelectionStatus::resolved;
        result.selected = result.matches.front();
    } else result.status = DriverAssetSelectionStatus::ambiguous;
    if (result.status == DriverAssetSelectionStatus::missing) {
        add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "MISSING_MODEL_ASSET",
                       "no candidate has the exact requested model basename", "asset selection", 0);
    } else if (result.status == DriverAssetSelectionStatus::ambiguous) {
        add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::error, "AMBIGUOUS_MODEL_ASSET",
                       "more than one candidate has the exact requested model basename", "asset selection", 0);
    }
    return result;
}

DriverPoseApplication apply_driver_base_pose(apex::formats::Kn5File& model,
                                             const apex::formats::KnhFile& pose,
                                             DriverDataLimits limits) {
    DriverPoseApplication result;
    if (pose.node_count > limits.maxNodes) {
        fail(pose.source, 0, "NODE_LIMIT", "driver pose node output exceeds configured limit");
    }
    std::vector<apex::formats::KnhWalkEntry> pose_rows = apex::formats::walk_knh(pose.root);
    std::vector<apex::formats::Kn5Node*> nodes;
    walk_kn5_nodes(model.root, nodes, limits);
    result.pose_nodes = pose_rows.size();
    if (pose_rows.size() > limits.maxNodes) fail(pose.source, 0, "NODE_LIMIT", "driver pose node output exceeds configured limit");
    std::map<std::string, const std::array<float, 16>*> by_name;
    for (const auto& row : pose_rows) {
        if (row.node->name.size() > limits.maxNameBytes) {
            fail(pose.source, 0, "NAME_LIMIT", "driver pose node name exceeds configured limit");
        }
        if (!finite_matrix(row.node->transform)) {
            add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::error, "NON_FINITE_TRANSFORM", "driver pose transform is not finite", pose.source, 0);
            continue;
        }
        const auto key = upper_ascii(row.node->name);
        if (by_name.contains(key)) add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning, "DUPLICATE_POSE_NAME", "later duplicate pose name is ignored", pose.source, 0);
        else by_name[key] = &row.node->transform;
    }
    std::set<std::string> matched;
    for (auto* node : nodes) {
        const auto found = by_name.find(upper_ascii(node->name));
        if (found == by_name.end()) continue;
        node->transform = *found->second;
        ++result.applied;
        matched.insert(upper_ascii(node->name));
    }
    bool unmatched_limit_reported = false;
    for (const auto& row : pose_rows) {
        if (!matched.contains(upper_ascii(row.node->name))) {
            if (result.unmatched_pose.size() >= limits.maxNames) {
                if (!unmatched_limit_reported) {
                    add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::error,
                                   "UNMATCHED_OUTPUT_LIMIT", "unmatched pose output was truncated",
                                   pose.source, 0);
                    unmatched_limit_reported = true;
                }
                continue;
            }
            result.unmatched_pose.push_back(row.node->name);
        }
    }
    return result;
}

DriverAnimationApplication apply_driver_animation(apex::formats::Kn5File& model,
                                                  const apex::formats::KsAnimation& animation,
                                                  float position, DriverDataLimits limits) {
    DriverAnimationApplication result;
    result.source = animation.source;
    result.tracks = animation.tracks.size();
    if (result.tracks > limits.maxNames) fail(animation.source, 0, "TRACK_LIMIT", "driver animation track output exceeds configured limit");
    std::vector<apex::formats::Kn5Node*> nodes;
    walk_kn5_nodes(model.root, nodes, limits);
    std::vector<const apex::formats::KsAnimationTrack*> tracks;
    std::map<std::string, std::size_t> track_indices;
    for (const auto& track : animation.tracks) {
        if (!track.animated) continue;
        if (track.name.size() > limits.maxNameBytes) {
            fail(animation.source, 0, "NAME_LIMIT", "driver animation track name exceeds configured limit");
        }
        ++result.animated_tracks;
        const auto key = upper_ascii(track.name);
        const auto found = track_indices.find(key);
        if (found == track_indices.end()) {
            track_indices.emplace(key, tracks.size());
            tracks.push_back(&track);
        } else {
            // Keep source order while retaining the existing last-track
            // precedence used by the browser workspace.
            tracks[found->second] = &track;
            add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::warning,
                           "DUPLICATE_ANIMATION_TRACK", "later duplicate animation track wins",
                           animation.source, 0);
        }
    }
    std::set<std::string> matched_tracks;
    bool unmatched_limit_reported = false;
    for (const auto* track : tracks) {
        const auto name = upper_ascii(track->name);
        bool matched = false;
        const auto sampled = apex::formats::sampleKsAnimationTrack(*track, position);
        if (!sampled.has_value()) continue;
        if (!finite_matrix(*sampled)) {
            add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::error,
                           "NON_FINITE_TRANSFORM", "sampled driver animation transform is not finite",
                           animation.source, 0);
            continue;
        }
        for (auto* node : nodes) {
            if (node->kind != "node" || upper_ascii(node->name) != name) continue;
            node->transform = *sampled;
            ++result.applied;
            matched = true;
        }
        if (matched) {
            matched_tracks.insert(name);
            ++result.matched_tracks;
        } else if (result.unmatched_tracks.size() < limits.maxNames) {
            result.unmatched_tracks.push_back(track->name);
        } else if (!unmatched_limit_reported) {
            add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::error,
                           "UNMATCHED_OUTPUT_LIMIT", "unmatched animation output was truncated",
                           animation.source, 0);
            unmatched_limit_reported = true;
        }
    }
    return result;
}

DriverHiddenResolution resolve_driver_hidden_subtrees(const apex::formats::Kn5File& model,
                                                      std::span<const std::string> names,
                                                      DriverDataLimits limits) {
    if (names.size() > limits.maxNames) fail(model.source, 0, "NAME_LIMIT", "hidden driver name input exceeds configured limit");
    DriverHiddenResolution result;
    std::vector<const apex::formats::Kn5Node*> nodes;
    walk_kn5_const(model.root, nodes, limits);
    std::set<const apex::formats::Kn5Node*> all_mesh_identities;
    std::vector<std::pair<std::string, std::string>> requested;
    std::set<std::string> seen;
    for (const auto& name : names) {
        const auto trimmed = trim(name);
        if (trimmed.empty()) continue;
        if (trimmed.size() > limits.maxNameBytes) {
            fail(model.source, 0, "NAME_LIMIT", "hidden driver object name exceeds configured limit");
        }
        const auto key = upper_ascii(trimmed);
        if (seen.insert(key).second) requested.emplace_back(key, std::string(trimmed));
    }
    result.requested = requested.size();
    bool unmatched_limit_reported = false;
    for (const auto& [key, requested_name] : requested) {
        DriverHiddenMatch match;
        match.requested_name = requested_name;
        std::set<const apex::formats::Kn5Node*> match_mesh_identities;
        for (const auto* node : nodes) {
            if (upper_ascii(node->name) != key) continue;
            ++match.node_count;
            collect_meshes(*node, match.mesh_names, limits, match_mesh_identities,
                           &all_mesh_identities);
        }
        match.mesh_count = match.mesh_names.size();
        if (match.node_count == 0) {
            if (result.unmatched.size() < limits.maxNames) result.unmatched.push_back(requested_name);
            else if (!unmatched_limit_reported) {
                add_diagnostic(result.diagnostics, limits, DriverDiagnosticSeverity::error,
                               "UNMATCHED_OUTPUT_LIMIT", "unmatched hidden-object output was truncated",
                               model.source, 0);
                unmatched_limit_reported = true;
            }
        }
        else {
            ++result.matched;
        }
        if (all_mesh_identities.size() > limits.maxNames) {
            fail(model.source, 0, "NAME_LIMIT", "hidden mesh output exceeds configured limit");
        }
        result.matches.push_back(std::move(match));
    }
    result.mesh_count = all_mesh_identities.size();
    return result;
}

bool DriverRigAudit::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DriverDiagnosticSeverity::error;
    });
}

DriverRigAudit audit_driver_rig(const DriverConfig& config,
                                const apex::formats::Kn5File& model,
                                const apex::formats::KnhFile* pose,
                                const apex::formats::KsAnimation* steering,
                                const apex::formats::KsAnimation* shifting,
                                DriverDataLimits limits) {
    DriverRigAudit result;
    if (config.diagnostics.size() > limits.maxDiagnostics) {
        fail(config.source, 0, "DIAGNOSTIC_LIMIT", "driver configuration diagnostics exceed configured limit");
    }
    result.diagnostics = config.diagnostics;
    auto add = [&](DriverDiagnosticSeverity severity, std::string_view code, std::string_view message) {
        add_diagnostic(result.diagnostics, limits, severity, code, message, config.source, 0);
    };
    std::vector<const apex::formats::Kn5Node*> nodes;
    walk_kn5_const(model.root, nodes, limits);
    std::set<std::string> names;
    for (const auto* node : nodes) names.insert(upper_ascii(node->name));
    if (pose == nullptr) add(DriverDiagnosticSeverity::warning, "MISSING_POSE", "driver_base_pos.knh is not available");
    else {
        if (pose->node_count > limits.maxNodes) {
            add(DriverDiagnosticSeverity::error, "POSE_NODE_LIMIT",
                "base pose node count exceeds configured limit");
            return result;
        }
        const auto rows = apex::formats::walk_knh(pose->root);
        result.pose_nodes = rows.size();
        if (rows.size() > limits.maxNodes) {
            add(DriverDiagnosticSeverity::error, "POSE_NODE_LIMIT",
                "base pose node count exceeds configured limit");
            return result;
        }
        for (const auto required : {"DRIVER:RIG_CENTER", "DRIVER:RIG_HAND_L", "DRIVER:RIG_HAND_R", "DRIVER:RIG_HEAD"}) {
            bool found = false;
            for (const auto& row : rows) found = found || upper_ascii(row.node->name) == required;
            if (!found) add(DriverDiagnosticSeverity::error, "MISSING_POSE_NODE", std::string("base pose is missing ") + required);
        }
        std::set<std::string> pose_names;
        for (const auto& row : rows) {
            if (finite_matrix(row.node->transform)) pose_names.insert(upper_ascii(row.node->name));
            else add(DriverDiagnosticSeverity::error, "NON_FINITE_TRANSFORM",
                     "base pose transform is not finite");
        }
        for (const auto* node : nodes) {
            if (pose_names.contains(upper_ascii(node->name))) ++result.pose_applied;
        }
    }
    const auto audit_animation = [&](const apex::formats::KsAnimation* animation, std::string_view label,
                                     std::size_t& tracks, std::size_t& matched) {
        if (animation == nullptr) {
            add(DriverDiagnosticSeverity::warning, std::string("MISSING_") + std::string(label), std::string(label) + " animation is not available");
            return;
        }
        if (animation->tracks.size() > limits.maxNames) {
            add(DriverDiagnosticSeverity::error, std::string(label) + "_TRACK_LIMIT",
                std::string(label) + " animation track count exceeds configured limit");
            return;
        }
        tracks = animation->tracks.size();
        for (const auto& track : animation->tracks) {
            if (track.name.size() > limits.maxNameBytes) {
                add(DriverDiagnosticSeverity::error, std::string(label) + "_NAME_LIMIT",
                    std::string(label) + " animation track name exceeds configured limit");
                continue;
            }
            if (!track.animated) continue;
            if (!std::all_of(track.frames.begin(), track.frames.end(), finite_frame)) {
                add(DriverDiagnosticSeverity::error, std::string(label) + "_NON_FINITE_TRANSFORM",
                    std::string(label) + " animation contains a non-finite frame");
                continue;
            }
            bool found = false;
            for (const auto* node : nodes) found = found || upper_ascii(node->name) == upper_ascii(track.name);
            if (found) ++matched;
        }
        if (pose != nullptr && matched == 0) {
            add(DriverDiagnosticSeverity::error, std::string(label) + "_NO_MATCH",
                std::string(label) + " animation has no animated tracks matching the base pose");
        }
    };
    audit_animation(steering, "STEERING", result.steering_tracks, result.steering_matched);
    if (config.shift.has_value()) audit_animation(shifting, "SHIFT", result.shift_tracks, result.shift_matched);
    return result;
}

}  // namespace apex::domain
