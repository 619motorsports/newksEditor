#include "apex/domain/track_data.hpp"

#include "apex/core/byte_reader.hpp"
#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace apex::domain {
namespace {

constexpr double kSplineEndpointFactor = 0.9990000128746033;

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
    throw apex::core::ParseError("TRACK", std::string(source), line, std::string(code),
                                 std::string(message));
}

void add_diagnostic(std::vector<TrackDiagnostic>& diagnostics, const TrackDataLimits& limits,
                    TrackDiagnosticSeverity severity, std::string_view code,
                    std::string_view message, std::string_view source, std::size_t line,
                    std::size_t section_index = 0, std::size_t entry_index = 0) {
    if (diagnostics.size() >= limits.maxDiagnostics) {
        fail(source, line, "DIAGNOSTIC_LIMIT", "track diagnostic output exceeds configured limit");
    }
    diagnostics.push_back({severity, std::string(code), std::string(message),
                           std::string(source), line, section_index, entry_index});
}

void append_document_warnings(std::vector<TrackDiagnostic>& diagnostics,
                              const TrackDataLimits& limits,
                              const apex::formats::IniDocument& document) {
    for (const auto& warning : document.warnings) {
        add_diagnostic(diagnostics, limits, TrackDiagnosticSeverity::warning, "INI_WARNING",
                       warning.message, warning.source, warning.line);
    }
}

[[nodiscard]] bool is_surface_name(std::string_view name) noexcept {
    const auto normalized = upper_ascii(name);
    if (normalized.size() <= 8 || normalized.substr(0, 8) != "SURFACE_") return false;
    for (const char character : normalized.substr(8)) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::size_t> suffix_index(std::string_view name,
                                                       std::size_t prefix_length) noexcept {
    const auto normalized = upper_ascii(name);
    if (normalized.size() <= prefix_length) return std::nullopt;
    std::size_t value = 0;
    const auto suffix = std::string_view(normalized).substr(prefix_length);
    for (const char character : suffix) {
        if (character < '0' || character > '9') return std::nullopt;
        const auto digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) return std::nullopt;
        value = value * 10U + digit;
    }
    return value;
}

[[nodiscard]] const apex::formats::IniEntry* ini_last_entry(const apex::formats::IniSection& section,
                                                             std::string_view key) noexcept {
    return section.last_entry(key);
}

double numeric_field(const apex::formats::IniSection& section, std::string_view key,
                     double fallback, std::vector<TrackDiagnostic>& diagnostics,
                     const TrackDataLimits& limits, std::string_view source) {
    const auto* entry = ini_last_entry(section, key);
    if (entry == nullptr || trim(entry->value).empty()) return fallback;
    const auto* number = entry->typed.number_value();
    if (number != nullptr && std::isfinite(*number)) return *number;
    add_diagnostic(diagnostics, limits, TrackDiagnosticSeverity::warning, "NON_FINITE_VALUE",
                   std::string(key) + " must be finite", source, entry->line);
    return fallback;
}

bool boolean_field(const apex::formats::IniSection& section, std::string_view key,
                   bool fallback, std::vector<TrackDiagnostic>& diagnostics,
                   const TrackDataLimits& limits, std::string_view source) {
    return numeric_field(section, key, fallback ? 1.0 : 0.0, diagnostics, limits, source) != 0.0;
}

[[nodiscard]] std::string string_field(const apex::formats::IniSection& section,
                                       std::string_view key) {
    const auto* entry = ini_last_entry(section, key);
    return entry == nullptr ? std::string{} : std::string(trim(entry->value));
}

[[nodiscard]] bool known_surface_key(std::string_view key) noexcept {
    static constexpr std::string_view keys[] = {
        "KEY", "FRICTION", "DAMPING", "DIRT_ADDITIVE", "BLACK_FLAG_TIME",
        "IS_VALID_TRACK", "IS_PITLANE", "SIN_HEIGHT", "SIN_LENGTH", "VIBRATION_GAIN",
        "VIBRATION_LENGTH", "WAV", "WAV_PITCH", "FF_EFFECT"};
    const auto normalized = upper_ascii(key);
    return std::find(std::begin(keys), std::end(keys), normalized) != std::end(keys);
}

[[nodiscard]] long long physics_sector_id(std::string_view value) noexcept {
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) ++start;
    const auto first = start;
    if (start < value.size() && (value[start] == '+' || value[start] == '-')) ++start;
    const auto digits = start;
    while (start < value.size() && value[start] >= '0' && value[start] <= '9') ++start;
    if (digits == start) return 0;
    long long result = 0;
    const auto parsed = std::from_chars(value.data() + first, value.data() + start, result);
    return parsed.ec == std::errc{} ? result : 0;
}

[[nodiscard]] std::vector<RuntimeSurface> stock_runtime_surfaces() {
    return {{"WALL", "WALL", "Built-in", std::nullopt},
            {"ROAD", "ROAD", "system/data/surfaces.ini", std::nullopt},
            {"GRASS", "GRASS", "system/data/surfaces.ini", std::nullopt},
            {"KERB", "KERB", "system/data/surfaces.ini", std::nullopt},
            {"SAND", "SAND", "system/data/surfaces.ini", std::nullopt}};
}

[[nodiscard]] std::array<double, 3> vector_field(const apex::formats::IniSection& section,
                                                  std::string_view key,
                                                  std::array<double, 3> fallback,
                                                  std::vector<TrackDiagnostic>& diagnostics,
                                                  const TrackDataLimits& limits,
                                                  std::string_view source) {
    const auto* entry = ini_last_entry(section, key);
    if (entry == nullptr || trim(entry->value).empty()) return fallback;
    const auto* values = entry->typed.numbers_value();
    if (values == nullptr || values->size() != 3 ||
        !std::all_of(values->begin(), values->end(), [](double value) { return std::isfinite(value); })) {
        add_diagnostic(diagnostics, limits, TrackDiagnosticSeverity::warning, "INVALID_VECTOR",
                       std::string(key) + " must contain three finite numbers", source, entry->line);
        return fallback;
    }
    return {(*values)[0], (*values)[1], (*values)[2]};
}

[[nodiscard]] double vector_length(const std::array<double, 3>& value) noexcept {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

[[nodiscard]] double vector_dot(const std::array<double, 3>& left,
                                const std::array<double, 3>& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] std::vector<std::string> split_whitespace(std::string_view value) {
    std::istringstream stream{std::string(value)};
    std::vector<std::string> result;
    std::string part;
    while (stream >> part) result.push_back(std::move(part));
    return result;
}

[[nodiscard]] bool finite_number(std::string_view value, double& output) noexcept {
    const std::string copy(value);
    if (copy.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    output = parsed;
    return true;
}

[[nodiscard]] std::vector<std::string_view> split_commas(std::string_view value) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(',', start);
        result.push_back(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

[[nodiscard]] bool safe_relative_path(std::string_view value, std::string& code,
                                      std::string& message) {
    if (value.empty()) {
        code = "EMPTY_SPLINE_PATH";
        message = "camera spline path is empty";
        return false;
    }
    if (value.find('\0') != std::string_view::npos) {
        code = "UNSAFE_SPLINE_PATH";
        message = "camera spline path contains NUL";
        return false;
    }
    if (value.front() == '/' || value.front() == '\\' ||
        (value.size() >= 2 && ((value[0] >= 'A' && value[0] <= 'Z') ||
                               (value[0] >= 'a' && value[0] <= 'z')) && value[1] == ':')) {
        code = "UNSAFE_SPLINE_PATH";
        message = "camera spline path must be capability-relative";
        return false;
    }
    if (value.find(':') != std::string_view::npos) {
        code = "UNSAFE_SPLINE_PATH";
        message = "camera spline path contains a drive or stream separator";
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find_first_of("/\\", start);
        const auto component = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        if (component.empty() || component == "." || component == "..") {
            code = "UNSAFE_SPLINE_PATH";
            message = "camera spline path contains an empty or traversal component";
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

[[nodiscard]] apex::formats::IniParseLimits ini_limits_for(const TrackDataLimits& limits) {
    apex::formats::IniParseLimits result;
    result.maxInputBytes = limits.maxInputBytes;
    result.maxLineBytes = limits.maxLineBytes;
    result.maxLines = limits.maxLines;
    result.maxSections = limits.maxSections;
    result.maxEntries = limits.maxFields;
    result.maxEntriesPerSection = limits.maxFieldsPerSection;
    result.maxSectionNameBytes = limits.maxStringBytes;
    result.maxKeyBytes = limits.maxStringBytes;
    result.maxValueBytes = limits.maxStringBytes;
    // Comments are not projected by the track domain. Keep their count
    // bounded by the same line budget so a caller's input policy is honored.
    result.maxComments = limits.maxLines;
    return result;
}

}  // namespace

bool TrackSurfaces::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == TrackDiagnosticSeverity::error;
    });
}

TrackSurfaces parse_track_surfaces(const apex::formats::IniDocument& document,
                                   TrackDataLimits limits) {
    if (document.sections.size() > limits.maxSections) {
        fail(document.source, 0, "SECTION_LIMIT", "surface section count exceeds configured limit");
    }
    TrackSurfaces result;
    result.source = document.source;
    append_document_warnings(result.diagnostics, limits, document);
    std::set<std::size_t> indexes;
    std::set<std::string> keys;
    std::size_t field_count = 0;
    for (std::size_t section_index = 0; section_index < document.sections.size(); ++section_index) {
        const auto& section = document.sections[section_index];
        if (!is_surface_name(section.name)) {
            ++result.ignored_sections;
            continue;
        }
        const auto index = suffix_index(section.name, 8);
        if (!index.has_value()) {
            add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "INDEX_LIMIT",
                           "surface index is outside the native range", section.source, section.line, section_index);
            continue;
        }
        if (!indexes.insert(*index).second) {
            add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "DUPLICATE_SURFACE_INDEX",
                           "duplicate surface section index", section.source, section.line, section_index);
        }
        TrackSurface surface;
        surface.index = *index;
        surface.section = section.name;
        surface.line = section.line;
        surface.source_section_index = section_index;
        surface.key = string_field(section, "KEY");
        surface.normalized_key = upper_ascii(surface.key);
        if (surface.key.empty()) {
            add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "MISSING_SURFACE_KEY",
                           section.name + " has no KEY", section.source, section.line, section_index);
        } else if (!keys.insert(surface.normalized_key).second) {
            add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "DUPLICATE_SURFACE_KEY",
                           "duplicate surface KEY " + surface.key, section.source, section.line, section_index);
        }
        for (std::size_t entry_index = 0; entry_index < section.entries.size(); ++entry_index) {
            const auto& entry = section.entries[entry_index];
            if (entry.value.size() > limits.maxStringBytes) {
                fail(section.source, entry.line, "STRING_LIMIT", "surface field exceeds configured string limit");
            }
            if (field_count >= limits.maxFields || surface.fields.size() >= limits.maxFieldsPerSection) {
                fail(section.source, entry.line, "FIELD_LIMIT", "surface field output exceeds configured limit");
            }
            ++field_count;
            surface.fields.push_back({entry.key, entry.value, entry.line, entry_index});
            if (!known_surface_key(entry.key)) {
                add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "UNKNOWN_SURFACE_FIELD",
                               "surface field is preserved but not projected", section.source, entry.line,
                               section_index, entry_index);
            }
        }
        surface.friction = numeric_field(section, "FRICTION", 1.0, result.diagnostics, limits, section.source);
        surface.damping = numeric_field(section, "DAMPING", 0.0, result.diagnostics, limits, section.source);
        surface.dirt_additive = numeric_field(section, "DIRT_ADDITIVE", 0.0, result.diagnostics, limits, section.source);
        surface.black_flag_time = numeric_field(section, "BLACK_FLAG_TIME", 0.0, result.diagnostics, limits, section.source);
        surface.is_valid_track = boolean_field(section, "IS_VALID_TRACK", false, result.diagnostics, limits, section.source);
        surface.is_pitlane = boolean_field(section, "IS_PITLANE", false, result.diagnostics, limits, section.source);
        surface.sin_height = numeric_field(section, "SIN_HEIGHT", 0.0, result.diagnostics, limits, section.source);
        surface.sin_length = numeric_field(section, "SIN_LENGTH", 0.0, result.diagnostics, limits, section.source);
        surface.vibration_gain = numeric_field(section, "VIBRATION_GAIN", 0.0, result.diagnostics, limits, section.source);
        surface.vibration_length = numeric_field(section, "VIBRATION_LENGTH", 0.0, result.diagnostics, limits, section.source);
        surface.wav = string_field(section, "WAV");
        surface.wav_pitch = numeric_field(section, "WAV_PITCH", 0.0, result.diagnostics, limits, section.source);
        surface.ff_effect = string_field(section, "FF_EFFECT");
        result.surfaces.push_back(std::move(surface));
    }
    return result;
}

TrackSurfaces parse_track_surfaces(std::string_view text, std::string source,
                                   TrackDataLimits limits) {
    return parse_track_surfaces(apex::formats::parse_ini(text, source, ini_limits_for(limits)), limits);
}

std::vector<RuntimeSurface> runtime_surfaces(const TrackSurfaces* configured) {
    auto result = stock_runtime_surfaces();
    if (configured == nullptr) return result;
    for (const auto& surface : configured->surfaces) {
        const auto normalized = upper_ascii(surface.key);
        if (normalized.empty()) continue;
        RuntimeSurface replacement{surface.key, normalized, configured->source, surface.index};
        const auto found = std::find_if(result.begin(), result.end(), [&](const auto& item) {
            return item.normalized_key == normalized;
        });
        if (found == result.end()) result.push_back(std::move(replacement));
        else *found = std::move(replacement);
    }
    return result;
}

RuntimeSurfaceMatch resolve_runtime_surface(std::string_view physics_name,
                                            const TrackSurfaces* configured) {
    RuntimeSurfaceMatch result;
    result.sector_id = physics_sector_id(physics_name);
    if (result.sector_id == 0) {
        result.status = RuntimeSurfaceStatus::not_physics;
        return result;
    }
    const auto normalized = upper_ascii(physics_name);
    for (const auto& surface : runtime_surfaces(configured)) {
        if (!surface.normalized_key.empty() && normalized.find(surface.normalized_key) != std::string::npos) {
            result.candidates.push_back(surface);
        }
    }
    if (result.candidates.size() == 1) {
        result.status = RuntimeSurfaceStatus::matched;
        result.surface = result.candidates.front();
    } else if (result.candidates.empty()) {
        result.status = RuntimeSurfaceStatus::fallback;
    } else {
        result.status = RuntimeSurfaceStatus::ambiguous;
    }
    return result;
}

bool TrackMarkerAudit::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == TrackDiagnosticSeverity::error;
    });
}

TrackMarkerAudit audit_track_markers(std::span<const std::string> node_names,
                                     std::span<const TrackMeshName> meshes,
                                     const TrackSurfaces* configured,
                                     TrackDataLimits limits) {
    if (node_names.size() > limits.maxNodes || meshes.size() > limits.maxNodes) {
        fail("track model", 0, "NODE_LIMIT", "track marker input exceeds configured limit");
    }
    TrackMarkerAudit result;
    const auto add = [&](TrackDiagnosticSeverity severity, std::string_view code,
                         std::string_view message) {
        add_diagnostic(result.diagnostics, limits, severity, code, message, "track model", 0);
    };
    std::set<std::size_t> starts, pits;
    std::map<std::size_t, unsigned> times;
    std::set<std::string> names;
    const auto numbered = [&](std::string_view name, std::string_view prefix,
                              std::set<std::size_t>& output) {
        const auto normalized = upper_ascii(name);
        const auto wanted = upper_ascii(prefix) + "_";
        if (normalized.rfind(wanted, 0) != 0) return;
        const auto parsed = suffix_index(normalized, wanted.size());
        if (!parsed.has_value()) {
            add(TrackDiagnosticSeverity::warning, "INVALID_MARKER", std::string(prefix) + " index is invalid");
            return;
        }
        output.insert(*parsed);
    };
    for (const auto& name : node_names) {
        numbered(name, "AC_START", starts);
        numbered(name, "AC_PIT", pits);
        const auto normalized = upper_ascii(name);
        if (normalized == "AC_HOTLAP_START_0") result.hotlap = true;
        constexpr std::string_view time_prefix = "AC_TIME_";
        if (normalized.rfind(time_prefix, 0) == 0 && normalized.size() > time_prefix.size() + 2 &&
            normalized[normalized.size() - 2] == '_') {
            const auto side = normalized.back();
            if (side == 'L' || side == 'R') {
                const auto index = suffix_index(normalized.substr(0, normalized.size() - 2), time_prefix.size());
                if (index.has_value()) times[*index] |= side == 'L' ? 1U : 2U;
            }
        }
        names.insert(name);
    }
    result.starts = starts.size();
    result.pits = pits.size();
    result.time_gates = times.size();
    const auto gaps = [&](const std::set<std::size_t>& values, std::string_view label,
                          std::string_view marker_prefix) {
        if (values.empty()) return;
        if (!values.contains(0)) add(TrackDiagnosticSeverity::error, "MISSING_MARKER", std::string(marker_prefix) + "_0 is missing");
        if (*values.rbegin() > limits.maxNodes) {
            add(TrackDiagnosticSeverity::warning, "INDEX_LIMIT", std::string(label) + " index exceeds configured marker limit");
            return;
        }
        std::vector<std::string> missing;
        for (std::size_t index = 0; index <= *values.rbegin(); ++index) {
            if (!values.contains(index)) missing.push_back(std::to_string(index));
        }
        if (!missing.empty()) {
            std::string message = std::string(label) + " indices have gaps: ";
            for (std::size_t index = 0; index < missing.size(); ++index) {
                if (index != 0) message += ", ";
                message += missing[index];
            }
            add(TrackDiagnosticSeverity::warning, "MARKER_GAP", message);
        }
    };
    gaps(starts, "Starting-grid", "AC_START");
    gaps(pits, "Pit", "AC_PIT");
    if (!times.contains(0)) add(TrackDiagnosticSeverity::error, "MISSING_TIME_GATE", "AC_TIME_0_L/R timing gate is missing");
    for (const auto& [index, sides] : times) {
        if (sides != 3U) add(TrackDiagnosticSeverity::error, "INCOMPLETE_TIME_GATE",
                              "AC_TIME_" + std::to_string(index) + " is missing its " +
                                  (sides == 1U ? "R" : "L") + " endpoint");
    }
    if (!result.hotlap) add(TrackDiagnosticSeverity::warning, "MISSING_HOTLAP", "AC_HOTLAP_START_0 is missing");

    const auto configured_surfaces = runtime_surfaces(configured);
    result.runtime_surface_count = configured_surfaces.size();
    std::map<std::string, std::size_t> counts;
    for (const auto& surface : configured_surfaces) counts[surface.normalized_key] = 0;
    std::set<std::string> unmatched_physical, ambiguous_physical;
    for (const auto& mesh : meshes) {
        if (!mesh.mesh) continue;
        const auto resolution = resolve_runtime_surface(mesh.name, configured);
        if (resolution.status == RuntimeSurfaceStatus::matched && resolution.surface.has_value()) {
            ++counts[resolution.surface->normalized_key];
        } else if (resolution.status == RuntimeSurfaceStatus::fallback) {
            unmatched_physical.insert(mesh.name);
        } else if (resolution.status == RuntimeSurfaceStatus::ambiguous) {
            ambiguous_physical.insert(mesh.name);
            std::vector<std::string> keys;
            for (const auto& candidate : resolution.candidates) keys.push_back(candidate.key);
            result.ambiguous_physical.emplace_back(mesh.name, std::move(keys));
        }
    }
    for (const auto& surface : configured_surfaces) result.surface_matches.emplace_back(surface.key, counts[surface.normalized_key]);
    if (configured != nullptr) {
        for (const auto& surface : configured->surfaces) {
            if (counts[surface.normalized_key] == 0) result.unmatched_surfaces.push_back(surface.key);
        }
        std::sort(result.unmatched_surfaces.begin(), result.unmatched_surfaces.end());
        result.unmatched_surfaces.erase(std::unique(result.unmatched_surfaces.begin(), result.unmatched_surfaces.end()), result.unmatched_surfaces.end());
    }
    result.unmatched_physical.assign(unmatched_physical.begin(), unmatched_physical.end());
    if (!result.unmatched_surfaces.empty()) add(TrackDiagnosticSeverity::warning, "UNMATCHED_SURFACES", "configured surfaces have no uniquely matching physical meshes");
    if (!result.unmatched_physical.empty()) add(TrackDiagnosticSeverity::warning, "UNMATCHED_PHYSICAL", "physics meshes fall back because no runtime surface key matches");
    if (!result.ambiguous_physical.empty()) add(TrackDiagnosticSeverity::error, "AMBIGUOUS_SURFACE", "physics meshes match multiple runtime surface keys");
    return result;
}

SplineRequest request_camera_spline(const CameraData& camera, std::string source) {
    SplineRequest result;
    result.relative_path = std::string(trim(camera.spline));
    result.source = std::move(source);
    result.line = camera.line;
    result.accepted = safe_relative_path(result.relative_path, result.code, result.message);
    return result;
}

bool CameraSet::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == TrackDiagnosticSeverity::error;
    });
}

CameraSet parse_track_cameras(const apex::formats::IniDocument& document,
                              TrackDataLimits limits) {
    if (document.sections.size() > limits.maxSections) {
        fail(document.source, 0, "SECTION_LIMIT", "camera section count exceeds configured limit");
    }
    CameraSet result;
    result.source = document.source;
    append_document_warnings(result.diagnostics, limits, document);
    std::optional<std::size_t> header_index;
    std::map<std::size_t, std::size_t> camera_sections;
    for (std::size_t section_index = 0; section_index < document.sections.size(); ++section_index) {
        const auto& section = document.sections[section_index];
        const auto normalized = upper_ascii(section.name);
        if (!header_index.has_value() && normalized == "HEADER") header_index = section_index;
        if (normalized.rfind("CAMERA_", 0) != 0) continue;
        const auto index = suffix_index(normalized, 7);
        if (!index.has_value()) continue;
        if (camera_sections.contains(*index)) {
            add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "DUPLICATE_CAMERA_INDEX",
                           "duplicate CAMERA_" + std::to_string(*index) + " replaces the earlier section",
                           section.source, section.line, section_index);
        }
        camera_sections[*index] = section_index;
    }
    if (!header_index.has_value()) {
        add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "MISSING_HEADER",
                       "HEADER section is missing", result.source, 0);
    } else {
        const auto& header = document.sections[*header_index];
        result.version = numeric_field(header, "VERSION", 0.0, result.diagnostics, limits, result.source);
        result.declared_count = numeric_field(header, "CAMERA_COUNT", 0.0, result.diagnostics, limits, result.source);
        result.name = string_field(header, "SET_NAME");
    }
    std::size_t expected = 0;
    std::size_t camera_field_count = 0;
    while (camera_sections.contains(expected)) {
        if (result.cameras.size() >= limits.maxSections) {
            fail(result.source, 0, "CAMERA_LIMIT", "camera output exceeds configured limit");
        }
        const auto section_index = camera_sections[expected];
        const auto& section = document.sections[section_index];
        CameraData camera;
        camera.index = expected;
        camera.name = string_field(section, "NAME");
        if (camera.name.empty()) camera.name = "Camera " + std::to_string(expected);
        camera.line = section.line;
        camera.source_section_index = section_index;
        for (std::size_t entry_index = 0; entry_index < section.entries.size(); ++entry_index) {
            const auto& entry = section.entries[entry_index];
            if (entry.value.size() > limits.maxStringBytes) fail(section.source, entry.line, "STRING_LIMIT", "camera field exceeds configured string limit");
            if (entry_index >= limits.maxFieldsPerSection || camera_field_count >= limits.maxFields) fail(section.source, entry.line, "FIELD_LIMIT", "camera field output exceeds configured limit");
            ++camera_field_count;
            camera.fields.push_back({entry.key, entry.value, entry.line, entry_index});
        }
        camera.position = vector_field(section, "POSITION", {0.0, 0.0, 0.0}, result.diagnostics, limits, result.source);
        camera.forward = vector_field(section, "FORWARD", {0.0, 0.0, -1.0}, result.diagnostics, limits, result.source);
        camera.up = vector_field(section, "UP", {0.0, 1.0, 0.0}, result.diagnostics, limits, result.source);
        camera.min_fov = numeric_field(section, "MIN_FOV", 45.0, result.diagnostics, limits, result.source);
        camera.max_fov = numeric_field(section, "MAX_FOV", camera.min_fov, result.diagnostics, limits, result.source);
        camera.in_point = numeric_field(section, "IN_POINT", -1.0, result.diagnostics, limits, result.source);
        camera.out_point = numeric_field(section, "OUT_POINT", -1.0, result.diagnostics, limits, result.source);
        camera.near_plane = numeric_field(section, "NEAR_PLANE", 0.1, result.diagnostics, limits, result.source);
        camera.far_plane = numeric_field(section, "FAR_PLANE", 10'000.0, result.diagnostics, limits, result.source);
        camera.min_exposure = numeric_field(section, "MIN_EXPOSURE", 0.0, result.diagnostics, limits, result.source);
        camera.max_exposure = numeric_field(section, "MAX_EXPOSURE", 0.0, result.diagnostics, limits, result.source);
        camera.dof_factor = numeric_field(section, "DOF_FACTOR", 0.0, result.diagnostics, limits, result.source);
        camera.dof_range = numeric_field(section, "DOF_RANGE", 0.0, result.diagnostics, limits, result.source);
        camera.dof_focus = numeric_field(section, "DOF_FOCUS", 0.0, result.diagnostics, limits, result.source);
        camera.dof_manual = numeric_field(section, "DOF_MANUAL", 0.0, result.diagnostics, limits, result.source) != 0.0;
        camera.spline = string_field(section, "SPLINE");
        camera.spline_rotation = numeric_field(section, "SPLINE_ROTATION", 0.0, result.diagnostics, limits, result.source);
        camera.spline_animation_length = numeric_field(section, "SPLINE_ANIMATION_LENGTH", 0.0, result.diagnostics, limits, result.source);
        camera.fov_gamma = numeric_field(section, "FOV_GAMMA", 1.0, result.diagnostics, limits, result.source);
        camera.fixed = numeric_field(section, "IS_FIXED", 0.0, result.diagnostics, limits, result.source) != 0.0;
        const auto forward_length = vector_length(camera.forward), up_length = vector_length(camera.up);
        const auto label = result.source + ":" + std::to_string(camera.line) + ": CAMERA_" + std::to_string(expected);
        if (std::abs(forward_length - 1.0) > 0.02) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "NON_NORMALIZED_BASIS", label + " FORWARD should be normalized", result.source, camera.line, section_index);
        if (std::abs(up_length - 1.0) > 0.02) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "NON_NORMALIZED_BASIS", label + " UP should be normalized", result.source, camera.line, section_index);
        if (std::abs(vector_dot(camera.forward, camera.up)) > 0.02) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "NON_ORTHOGONAL_BASIS", label + " FORWARD and UP should be perpendicular", result.source, camera.line, section_index);
        if (camera.min_fov <= 0.0 || camera.max_fov <= 0.0 || camera.min_fov > camera.max_fov || camera.max_fov > 180.0) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "INVALID_FOV", label + " FOV range is invalid", result.source, camera.line, section_index);
        if (!((camera.in_point == -1.0 && camera.out_point == -1.0) || (camera.in_point >= 0.0 && camera.out_point >= camera.in_point && camera.out_point <= 1.0))) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "INVALID_LAP_INTERVAL", label + " lap interval must be -1/-1 or ordered from 0 to 1", result.source, camera.line, section_index);
        if (camera.near_plane <= 0.0 || camera.far_plane <= camera.near_plane) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "INVALID_CLIP_PLANES", label + " clip planes are invalid", result.source, camera.line, section_index);
        if (!camera.spline.empty()) {
            const auto request = request_camera_spline(camera, result.source);
            if (!request.accepted) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, request.code, request.message, result.source, camera.line, section_index);
        }
        result.cameras.push_back(std::move(camera));
        ++expected;
    }
    for (const auto& [index, section_index] : camera_sections) {
        if (index >= result.cameras.size()) {
            add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "NON_CONTIGUOUS_CAMERA",
                           result.source + ": CAMERA_" + std::to_string(index) + " is ignored because CAMERA_" +
                               std::to_string(result.cameras.size()) + " is missing", document.sections[section_index].source,
                           document.sections[section_index].line, section_index);
        }
    }
    const std::size_t header_count = header_index.has_value() ? 1U : 0U;
    result.ignored_sections = document.sections.size() >= camera_sections.size() + header_count
                                  ? document.sections.size() - camera_sections.size() - header_count
                                  : 0;
    if (result.declared_count != static_cast<double>(result.cameras.size())) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "CAMERA_COUNT_MISMATCH", result.source + ": HEADER declares " + std::to_string(result.declared_count) + " cameras but " + std::to_string(result.cameras.size()) + " contiguous sections were found", result.source, 0);
    return result;
}

CameraSet parse_track_cameras(std::string_view text, std::string source,
                              TrackDataLimits limits) {
    return parse_track_cameras(apex::formats::parse_ini(text, source, ini_limits_for(limits)), limits);
}

bool SplinePointSet::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == TrackDiagnosticSeverity::error;
    });
}

SplinePointSet parse_camera_spline(std::string_view text, std::string source,
                                   TrackDataLimits limits) {
    if (text.size() > limits.maxInputBytes) fail(source, 0, "INPUT_LIMIT", "spline input exceeds configured size limit");
    apex::core::ByteReader::validateUtf8(text, "camera spline", "TRACK", source, 0);
    SplinePointSet result;
    result.source = source;
    std::size_t position = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xefU &&
                                   static_cast<unsigned char>(text[1]) == 0xbbU && static_cast<unsigned char>(text[2]) == 0xbfU
                               ? 3U
                               : 0U;
    bool comma = false;
    bool delimiter_set = false;
    std::size_t line = 1;
    while (position < text.size()) {
        if (line > limits.maxLines) fail(source, position, "LINE_LIMIT", "spline line count exceeds configured limit");
        const auto start = position;
        while (position < text.size() && text[position] != '\r' && text[position] != '\n') ++position;
        if (position - start > limits.maxLineBytes) fail(source, start, "LINE_LIMIT", "spline row exceeds configured line limit");
        const auto raw = trim(text.substr(start, position - start));
        if (!raw.empty()) {
            if (!delimiter_set) {
                comma = raw.find(',') != std::string_view::npos;
                delimiter_set = true;
            }
            const auto parts = comma ? split_commas(raw) : std::vector<std::string_view>{};
            std::vector<std::string> whitespace;
            std::vector<std::string_view> values;
            if (comma) values = parts;
            else {
                whitespace = split_whitespace(raw);
                for (const auto& item : whitespace) values.push_back(item);
            }
            if (values.size() != 3) {
                add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "INVALID_SPLINE_ROW", "expected three finite coordinates", source, line);
            } else {
                std::array<double, 3> point{};
                bool valid = true;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (!finite_number(trim(values[axis]), point[axis])) valid = false;
                }
                if (!valid) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "INVALID_SPLINE_ROW", "expected three finite coordinates", source, line);
                else {
                    if (result.points.size() >= limits.maxSplinePoints) fail(source, line, "SPLINE_LIMIT", "spline point output exceeds configured limit");
                    if (!result.points.empty()) {
                        const auto& previous = result.points.back();
                        result.length += std::sqrt((point[0] - previous[0]) * (point[0] - previous[0]) +
                                                   (point[1] - previous[1]) * (point[1] - previous[1]) +
                                                   (point[2] - previous[2]) * (point[2] - previous[2]));
                    }
                    result.points.push_back(point);
                }
            }
        }
        if (position == text.size()) break;
        position += text[position] == '\r' && position + 1 < text.size() && text[position + 1] == '\n' ? 2U : 1U;
        ++line;
    }
    if (result.points.empty()) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "NO_SPLINE_POINTS", "no valid spline points were found", source, 0);
    else if (result.points.size() == 1) add_diagnostic(result.diagnostics, limits, TrackDiagnosticSeverity::warning, "ONE_SPLINE_POINT", "one spline point cannot describe camera motion", source, 1);
    return result;
}

std::vector<std::array<double, 3>> rotate_camera_spline(
    std::span<const std::array<double, 3>> points, double degrees) {
    if (!std::isfinite(degrees)) fail("camera spline", 0, "NON_FINITE", "spline rotation must be finite");
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    std::vector<std::array<double, 3>> result;
    result.reserve(points.size());
    for (const auto& point : points) result.push_back({point[0] * cosine + point[2] * sine, point[1], -point[0] * sine + point[2] * cosine});
    return result;
}

std::array<double, 3> sample_camera_spline(std::span<const std::array<double, 3>> points,
                                           double position) {
    if (points.empty()) return {0.0, 0.0, 0.0};
    if (points.size() == 1) return points.front();
    const double input = std::isfinite(position) ? position : 0.0;
    const double t = std::max(0.0, std::min(1.0, input));
    const auto last = points.size() - 1U;
    const auto index = static_cast<std::size_t>(std::trunc(static_cast<double>(last) * t * kSplineEndpointFactor));
    const double blend = (t * kSplineEndpointFactor - static_cast<double>(index) / static_cast<double>(last)) /
                         (1.0 / static_cast<double>(points.size()));
    const auto next = (index + 1U) % points.size();
    return {points[index][0] + (points[next][0] - points[index][0]) * blend,
            points[index][1] + (points[next][1] - points[index][1]) * blend,
            points[index][2] + (points[next][2] - points[index][2]) * blend};
}

}  // namespace apex::domain
