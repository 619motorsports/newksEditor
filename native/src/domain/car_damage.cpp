#include "apex/domain/car_damage.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <set>
#include <type_traits>
#include <sstream>
#include <utility>

namespace apex::domain {
namespace {

constexpr float kFloatMaximum = std::numeric_limits<float>::max();
constexpr std::uint32_t kMaximumVisualObjects = 1'024;

struct RawEntry { std::string key; std::string value; std::size_t line = 0; };
struct RawSection { std::string name; std::size_t line = 0; std::vector<RawEntry> entries; };

[[nodiscard]] bool valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index++]);
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7fU) continue;
        if (first >= 0xc2U && first <= 0xdfU) { count = 1; codepoint = first & 0x1fU; }
        else if (first >= 0xe0U && first <= 0xefU) { count = 2; codepoint = first & 0x0fU; }
        else if (first >= 0xf0U && first <= 0xf4U) { count = 3; codepoint = first & 0x07U; }
        else return false;
        if (index + count > text.size()) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto byte = static_cast<unsigned char>(text[index++]);
            if ((byte & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if ((count == 2 && codepoint < 0x800U) || (count == 3 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    }
    return true;
}

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t begin = 0, end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] std::string upper(std::string_view value) {
    std::string result(value);
    for (char& character : result)
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    return result;
}

void add_diagnostic(std::vector<CarDamageDiagnostic>& diagnostics, const CarDamageLimits& limits,
                   CarDamageDiagnostic::Severity severity, std::string code, std::size_t line,
                   std::string message) {
    if (diagnostics.size() >= limits.max_diagnostics) return;
    diagnostics.push_back({severity, std::move(code), line, std::move(message)});
}

void add_warning(std::vector<CarDamageDiagnostic>& diagnostics, const CarDamageLimits& limits,
                 std::string code, std::size_t line, std::string message) {
    add_diagnostic(diagnostics, limits, CarDamageDiagnostic::Severity::warning, std::move(code), line,
                   std::move(message));
}

[[nodiscard]] std::string strip_comment(std::string_view line) {
    bool quoted = false;
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if ((character == '\'' || character == '"') && (index == 0 || line[index - 1] != '\\')) {
            if (!quoted) { quoted = true; quote = character; }
            else if (quote == character) quoted = false;
        }
        if (character == ';' && !quoted) return std::string(line.substr(0, index));
    }
    return std::string(line);
}

[[nodiscard]] std::vector<RawSection> parse_ini(std::string_view text, const std::string& source,
                                                const CarDamageLimits& limits,
                                                std::vector<CarDamageDiagnostic>& diagnostics,
                                                bool& fatal) {
    std::vector<RawSection> sections;
    RawSection* current = nullptr;
    std::string continued;
    std::size_t line_number = 0;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        const auto raw = text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset);
        offset = end == std::string_view::npos ? text.size() + 1U : end + 1U;
        ++line_number;
        std::string line = continued + std::string(raw);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(strip_comment(line));
        if (!line.empty() && line.back() == '\\') { line.pop_back(); continued = line; continue; }
        continued.clear();
        if (line.empty()) { if (end == std::string_view::npos) break; continue; }
        if (line.front() == '[' && line.back() == ']') {
            if (sections.size() >= limits.max_sections) {
                add_diagnostic(diagnostics, limits, CarDamageDiagnostic::Severity::error, "SECTION_LIMIT", line_number, "damage section count exceeds its limit");
                fatal = true; return {};
            }
            const auto name = trim(std::string_view(line).substr(1, line.size() - 2U));
            if (name.empty() || name.size() > limits.max_string_bytes || name.find_first_of("[]") != std::string::npos || !valid_utf8(name)) {
                add_diagnostic(diagnostics, limits, CarDamageDiagnostic::Severity::error, "SECTION_INVALID", line_number, "damage section name is invalid");
                fatal = true; return {};
            }
            sections.push_back({name, line_number, {}}); current = &sections.back();
        } else {
            const auto equals = line.find('=');
            if (equals == std::string::npos || current == nullptr) {
                add_warning(diagnostics, limits, "INI_ENTRY", line_number, source + ": entry is outside a section or has no '='");
                if (end == std::string_view::npos) break;
                continue;
            }
            if (current->entries.size() >= limits.max_entries_per_section) {
                add_diagnostic(diagnostics, limits, CarDamageDiagnostic::Severity::error, "ENTRY_LIMIT", line_number, "damage entries per section exceed their limit");
                fatal = true; return {};
            }
            const auto key = upper(trim(std::string_view(line).substr(0, equals)));
            const auto value = trim(std::string_view(line).substr(equals + 1U));
            if (key.empty() || key.size() > limits.max_string_bytes || value.size() > limits.max_string_bytes ||
                !valid_utf8(key) || !valid_utf8(value)) {
                add_diagnostic(diagnostics, limits, CarDamageDiagnostic::Severity::error, "STRING_LIMIT", line_number, "damage entry text is invalid or exceeds its limit");
                fatal = true; return {};
            }
            current->entries.push_back({key, value, line_number});
        }
        if (end == std::string_view::npos) break;
    }
    if (!continued.empty()) add_warning(diagnostics, limits, "INI_CONTINUATION", line_number, "unterminated line continuation was ignored");
    return sections;
}

[[nodiscard]] std::string last_value(const RawSection& section, std::string_view key,
                                     std::string fallback = {}) {
    const auto wanted = upper(key);
    for (auto index = section.entries.size(); index-- > 0;) if (section.entries[index].key == wanted) return section.entries[index].value;
    return fallback;
}

[[nodiscard]] bool has_value(const RawSection& section, std::string_view key) {
    const auto wanted = upper(key);
    return std::any_of(section.entries.begin(), section.entries.end(), [&](const auto& entry) {
        return entry.key == wanted;
    });
}

[[nodiscard]] float finite_number(const RawSection& section, std::string_view key, float fallback,
                                  std::vector<CarDamageDiagnostic>& diagnostics, const CarDamageLimits& limits) {
    if (!has_value(section, key)) return fallback;
    const auto raw = trim(last_value(section, key));
    if (raw.empty()) {
        add_warning(diagnostics, limits, "NUMBER_INVALID", section.line, section.name + " " + upper(key) + " must be a finite float32 number");
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str() || *end != '\0' || !std::isfinite(value) || std::abs(value) > static_cast<double>(kFloatMaximum)) {
        add_warning(diagnostics, limits, "NUMBER_INVALID", section.line, section.name + " " + upper(key) + " must be a finite float32 number");
        return fallback;
    }
    return static_cast<float>(value);
}

[[nodiscard]] Vector3 finite_vector(const RawSection& section, std::string_view key, Vector3 fallback,
                                    std::vector<CarDamageDiagnostic>& diagnostics, const CarDamageLimits& limits) {
    if (!has_value(section, key)) return fallback;
    const auto raw = last_value(section, key);
    std::array<float, 3> result{};
    std::size_t start = 0, count = 0;
    bool valid = true;
    while (start <= raw.size()) {
        const auto comma = raw.find(',', start);
        const auto part = trim(std::string_view(raw).substr(start, comma == std::string::npos ? raw.size() - start : comma - start));
        if (count >= 3 || part.empty()) { valid = false; break; }
        char* end = nullptr;
        const double value = std::strtod(part.c_str(), &end);
        if (end == part.c_str() || *end != '\0' || !std::isfinite(value) || std::abs(value) > static_cast<double>(kFloatMaximum)) valid = false;
        else result[count] = static_cast<float>(value);
        ++count;
        if (comma == std::string::npos) break;
        start = comma + 1U;
    }
    if (count != 3) valid = false;
    if (!valid) {
        add_warning(diagnostics, limits, "VECTOR_INVALID", section.line, section.name + " " + upper(key) + " must contain three finite float32 numbers");
        return fallback;
    }
    return result;
}

[[nodiscard]] std::string safe_token(const std::string& value, std::string fallback,
                                      const RawSection& section, std::string_view key,
                                      std::vector<CarDamageDiagnostic>& diagnostics, const CarDamageLimits& limits) {
    const auto token = upper(trim(value));
    const bool valid = !token.empty() && token.size() <= 64 &&
                       std::all_of(token.begin(), token.end(), [](char c) {
                           return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                       });
    if (!valid) {
        add_warning(diagnostics, limits, "TOKEN_INVALID", section.line, section.name + " " + upper(key) + " must be a safe token");
        return fallback;
    }
    return token;
}

template <typename Section>
void copy_extra(const RawSection& raw, Section& output, std::initializer_list<std::string_view> known,
                std::size_t& extra_count, bool& limit_hit, const CarDamageLimits& limits,
                std::vector<CarDamageDiagnostic>& diagnostics) {
    for (const auto& entry : raw.entries) {
        if (std::find(known.begin(), known.end(), entry.key) != known.end()) continue;
        if (extra_count >= limits.max_extra_entries) {
            limit_hit = true;
            add_diagnostic(diagnostics, limits, CarDamageDiagnostic::Severity::error, "EXTRA_ENTRY_LIMIT", entry.line, "unknown damage entries exceed their limit");
            continue;
        }
        output.extra_entries.push_back({entry.key, entry.value}); ++extra_count;
    }
}

[[nodiscard]] std::string number(float value) {
    if (!std::isfinite(value)) throw CarDamageError("NON_FINITE", "damage value must be finite");
    if (value == 0.0F) return "0";
    std::ostringstream stream; stream.imbue(std::locale::classic());
    stream << std::setprecision(9) << std::defaultfloat << static_cast<double>(value);
    return stream.str();
}

void append_bounded(std::string& output, std::string_view value, const CarDamageLimits& limits) {
    if (value.size() > limits.max_output_bytes || output.size() > limits.max_output_bytes - value.size())
        throw CarDamageError("OUTPUT_LIMIT", "car damage output exceeds its limit");
    output.append(value.data(), value.size());
}

[[nodiscard]] std::string safe_ini_text(std::string_view value, std::size_t maximum, std::string_view label) {
    const auto text = trim(value);
    if (text.empty() || text.size() > maximum || text.find_first_of("\r\n;") != std::string::npos || !valid_utf8(text))
        throw CarDamageError("UNSAFE_TEXT", std::string(label) + " contains unsafe text");
    return text;
}

void validate_vector(const Vector3& value, std::string_view label) {
    for (const auto component : value) if (!std::isfinite(component)) throw CarDamageError("NON_FINITE", std::string(label) + " must be finite");
}

void validate_extra(const std::vector<DamageExtraEntry>& entries, const CarDamageLimits& limits) {
    for (const auto& entry : entries) {
        const auto key = upper(safe_ini_text(entry.key, std::min<std::size_t>(128U, limits.max_string_bytes), "extra key"));
        if (key.empty() || !std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-'; }))
            throw CarDamageError("UNSAFE_TEXT", "extra key contains unsafe characters");
        if (entry.value.size() > limits.max_string_bytes || entry.value.find_first_of("\r\n") != std::string::npos || !valid_utf8(entry.value))
            throw CarDamageError("UNSAFE_TEXT", "extra value contains unsafe text");
    }
}

void append_line(std::string& output, std::string_view line, const CarDamageLimits& limits) {
    append_bounded(output, line, limits); append_bounded(output, "\n", limits);
}

} // namespace

CarDamageError::CarDamageError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

CarDamageParseResult parse_car_damage_ini(std::string_view text, std::string source, CarDamageLimits limits) {
    CarDamageParseResult result;
    if (source.size() > limits.max_string_bytes || !valid_utf8(source)) {
        add_diagnostic(result.diagnostics, limits, CarDamageDiagnostic::Severity::error, "SOURCE_INVALID", 0, "car damage source name is invalid");
        return result;
    }
    if (text.size() > limits.max_input_bytes) {
        result.limit_exceeded = true;
        add_diagnostic(result.diagnostics, limits, CarDamageDiagnostic::Severity::error, "INPUT_LIMIT", 0, "car damage input exceeds its byte limit");
        return result;
    }
    if (!valid_utf8(text)) {
        add_diagnostic(result.diagnostics, limits, CarDamageDiagnostic::Severity::error, "UTF8_INVALID", 0, "car damage input is not valid UTF-8");
        return result;
    }
    bool fatal = false;
    const auto sections = parse_ini(text, source, limits, result.diagnostics, fatal);
    if (fatal) { result.limit_exceeded = true; return result; }
    CarDamageConfig config; config.source = std::move(source);
    config.scratches.name = "SCRATCHES"; config.oscillations.name = "OSCILLATIONS"; config.damage.name = "DAMAGE";
    std::map<std::string, const RawSection*> by_name;
    std::set<std::uint32_t> seen_indices;
    std::size_t extra_count = 0;
    bool extra_limit = false;
    for (const auto& section : sections) {
        const auto upper_name = upper(section.name);
        if (!by_name.emplace(upper_name, &section).second) {
            add_warning(result.diagnostics, limits, "DUPLICATE_SECTION", section.line, "duplicate " + section.name + " section was ignored");
            continue;
        }
        if (upper_name.rfind("VISUAL_OBJECT_", 0) == 0) {
            const auto suffix = upper_name.substr(14);
            std::uint64_t index = 0;
            const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || index >= kMaximumVisualObjects) {
                add_warning(result.diagnostics, limits, "VISUAL_INDEX", section.line, section.name + " index is outside 0..1023");
                continue;
            }
            if (!seen_indices.insert(static_cast<std::uint32_t>(index)).second) continue;
            const auto object_name = trim(last_value(section, "NAME"));
            if (object_name.empty() || object_name.size() > 1024 || object_name.find_first_of("\r\n;") != std::string::npos) {
                add_warning(result.diagnostics, limits, "VISUAL_NAME", section.line, section.name + " NAME is invalid");
                continue;
            }
            VisualObject object; object.name = section.name; object.line = section.line; object.index = static_cast<std::uint32_t>(index); object.object_name = object_name;
            object.static_rotation_axis = finite_vector(section, "STATIC_ROTATION_AXIS", object.static_rotation_axis, result.diagnostics, limits);
            object.static_rotation_angle = finite_number(section, "STATIC_ROTATION_ANGLE", 0.0F, result.diagnostics, limits);
            object.mult_g = finite_number(section, "MULT_G", 0.0F, result.diagnostics, limits);
            object.damage_zone = safe_token(last_value(section, "DAMAGE_ZONE", "FRONT"), "FRONT", section, "DAMAGE_ZONE", result.diagnostics, limits);
            object.min_speed = finite_number(section, "MIN_SPEED", 0.0F, result.diagnostics, limits);
            object.full_speed = finite_number(section, "FULL_SPEED", 0.0F, result.diagnostics, limits);
            object.oscillation_axis = finite_vector(section, "OSCILLATION_AXIS", object.oscillation_axis, result.diagnostics, limits);
            object.oscillation_min_angle = finite_number(section, "OSCILLATION_MIN_ANGLE", 0.0F, result.diagnostics, limits);
            object.oscillation_max_angle = finite_number(section, "OSCILLATION_MAX_ANGLE", 0.0F, result.diagnostics, limits);
            object.allowed_g = finite_vector(section, "ALLOWED_G", object.allowed_g, result.diagnostics, limits);
            copy_extra(section, object, {"NAME", "STATIC_ROTATION_AXIS", "STATIC_ROTATION_ANGLE", "MULT_G", "DAMAGE_ZONE", "MIN_SPEED", "FULL_SPEED", "OSCILLATION_AXIS", "OSCILLATION_MIN_ANGLE", "OSCILLATION_MAX_ANGLE", "ALLOWED_G"}, extra_count, extra_limit, limits, result.diagnostics);
            if (object.min_speed < 0.0F || object.full_speed < 0.0F || object.full_speed < object.min_speed) add_warning(result.diagnostics, limits, "SPEED_ORDER", section.line, section.name + " has invalid speed ordering");
            if (object.oscillation_max_angle < object.oscillation_min_angle) add_warning(result.diagnostics, limits, "ANGLE_ORDER", section.line, section.name + " has invalid oscillation angle ordering");
            config.visual_objects.push_back(std::move(object));
        } else if (upper_name != "SCRATCHES" && upper_name != "OSCILLATIONS" && upper_name != "DAMAGE") {
            ExtraDamageSection extra{section.name, {}};
            for (const auto& entry : section.entries) {
                if (extra_count >= limits.max_extra_entries) { extra_limit = true; add_diagnostic(result.diagnostics, limits, CarDamageDiagnostic::Severity::error, "EXTRA_ENTRY_LIMIT", entry.line, "unknown damage entries exceed their limit"); break; }
                extra.entries.push_back({entry.key, entry.value}); ++extra_count;
            }
            config.extra_sections.push_back(std::move(extra));
        }
    }
    if (const auto found = by_name.find("SCRATCHES"); found != by_name.end()) {
        const auto& section = *found->second; config.scratches.name = section.name; config.scratches.line = section.line;
        config.scratches.min_speed = finite_number(section, "MIN_SPEED", 0.0F, result.diagnostics, limits);
        config.scratches.max_speed = finite_number(section, "MAX_SPEED", 20.0F, result.diagnostics, limits);
        copy_extra(section, config.scratches, {"MIN_SPEED", "MAX_SPEED"}, extra_count, extra_limit, limits, result.diagnostics);
        if (config.scratches.min_speed < 0.0F || config.scratches.max_speed < config.scratches.min_speed) add_warning(result.diagnostics, limits, "SPEED_ORDER", section.line, "SCRATCHES has invalid speed ordering");
    }
    if (const auto found = by_name.find("OSCILLATIONS"); found != by_name.end()) {
        const auto& section = *found->second; config.oscillations.name = section.name; config.oscillations.line = section.line;
        config.oscillations.enabled = finite_number(section, "ENABLED", 1.0F, result.diagnostics, limits) != 0.0F;
        copy_extra(section, config.oscillations, {"ENABLED"}, extra_count, extra_limit, limits, result.diagnostics);
    }
    if (const auto found = by_name.find("DAMAGE"); found != by_name.end()) {
        const auto& section = *found->second; config.damage.name = section.name; config.damage.line = section.line;
        config.damage.initial_level = finite_number(section, "INITIAL_LEVEL", 0.0F, result.diagnostics, limits);
        copy_extra(section, config.damage, {"INITIAL_LEVEL"}, extra_count, extra_limit, limits, result.diagnostics);
        if (config.damage.initial_level < 0.0F || config.damage.initial_level > 100.0F) add_warning(result.diagnostics, limits, "LEVEL_RANGE", section.line, "DAMAGE INITIAL_LEVEL must be from 0 to 100");
    }
    if (extra_limit) {
        result.limit_exceeded = true;
        result.config.reset();
        return result;
    }
    std::sort(config.visual_objects.begin(), config.visual_objects.end(), [](const auto& left, const auto& right) { return left.index < right.index; });
    result.config = std::move(config);
    return result;
}

std::string serialize_car_damage_ini(const CarDamageConfig& config, CarDamageLimits limits) {
    (void)safe_ini_text(config.source, limits.max_string_bytes, "damage source");
    if (config.visual_objects.size() > limits.max_visual_objects)
        throw CarDamageError("COUNT_LIMIT", "car damage section count exceeds its limit");
    constexpr std::size_t known_section_count = 3U;
    if (limits.max_sections < known_section_count ||
        config.visual_objects.size() > limits.max_sections - known_section_count)
        throw CarDamageError("COUNT_LIMIT", "car damage section count exceeds its limit");
    const auto sections_with_visuals = known_section_count + config.visual_objects.size();
    if (config.extra_sections.size() > limits.max_sections - sections_with_visuals)
        throw CarDamageError("COUNT_LIMIT", "car damage section count exceeds its limit");
    std::size_t extra_count = 0;
    const auto count_extra = [&](std::size_t count) {
        if (count > limits.max_extra_entries - extra_count)
            throw CarDamageError("EXTRA_ENTRY_LIMIT", "car damage extra entries exceed their limit");
        extra_count += count;
    };
    for (const auto& extra : config.extra_sections) {
        count_extra(extra.entries.size());
        validate_extra(extra.entries, limits);
    }
    count_extra(config.scratches.extra_entries.size());
    validate_extra(config.scratches.extra_entries, limits);
    count_extra(config.oscillations.extra_entries.size());
    validate_extra(config.oscillations.extra_entries, limits);
    count_extra(config.damage.extra_entries.size());
    validate_extra(config.damage.extra_entries, limits);
    for (const auto& object : config.visual_objects) {
        count_extra(object.extra_entries.size());
        validate_extra(object.extra_entries, limits);
    }
    const auto bounded_string = [&limits](std::size_t intrinsic) {
        return std::min(intrinsic, limits.max_string_bytes);
    };
    std::string output;
    for (const auto& extra : config.extra_sections) {
        const auto name = safe_ini_text(extra.name, bounded_string(128U), "section name");
        if (name.find_first_of("[]") != std::string::npos)
            throw CarDamageError("UNSAFE_TEXT", "section name contains a bracket");
        append_line(output, "[" + name + "]", limits);
        validate_extra(extra.entries, limits);
        for (const auto& entry : extra.entries) append_line(output, upper(entry.key) + "=" + entry.value, limits);
        append_bounded(output, "\n", limits);
    }
    const auto write_extra = [&](const auto& section) { validate_extra(section.extra_entries, limits); for (const auto& entry : section.extra_entries) append_line(output, upper(entry.key) + "=" + entry.value, limits); };
    append_line(output, "[SCRATCHES]", limits); append_line(output, "MAX_SPEED=" + number(config.scratches.max_speed), limits); append_line(output, "MIN_SPEED=" + number(config.scratches.min_speed), limits); write_extra(config.scratches); append_bounded(output, "\n", limits);
    if (config.scratches.min_speed < 0.0F || config.scratches.max_speed < config.scratches.min_speed) throw CarDamageError("SPEED_ORDER", "SCRATCHES has invalid speed ordering");
    append_line(output, "[OSCILLATIONS]", limits); append_line(output, std::string("ENABLED=") + (config.oscillations.enabled ? "1" : "0"), limits); write_extra(config.oscillations); append_bounded(output, "\n", limits);
    append_line(output, "[DAMAGE]", limits); if (!std::isfinite(config.damage.initial_level) || config.damage.initial_level < 0.0F || config.damage.initial_level > 100.0F) throw CarDamageError("LEVEL_RANGE", "DAMAGE INITIAL_LEVEL is invalid"); append_line(output, "INITIAL_LEVEL=" + number(config.damage.initial_level), limits); write_extra(config.damage); append_bounded(output, "\n", limits);
    std::set<std::uint32_t> used;
    for (const auto& object : config.visual_objects) {
        if (!used.insert(object.index).second || object.index >= kMaximumVisualObjects) throw CarDamageError("VISUAL_INDEX", "visual-object indices must be unique and below 1024");
        if (object.min_speed < 0.0F || object.full_speed < object.min_speed || object.oscillation_max_angle < object.oscillation_min_angle) throw CarDamageError("ORDER", "visual-object ranges are invalid");
        const auto name = safe_ini_text(object.object_name, bounded_string(1024U), "visual-object name");
        const auto zone = upper(safe_ini_text(object.damage_zone, bounded_string(64U), "damage zone"));
        if (!std::all_of(zone.begin(), zone.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; })) throw CarDamageError("TOKEN_INVALID", "damage zone is invalid");
        validate_vector(object.static_rotation_axis, "static rotation axis"); validate_vector(object.oscillation_axis, "oscillation axis"); validate_vector(object.allowed_g, "allowed G");
        append_bounded(output, "[VISUAL_OBJECT_" + std::to_string(object.index) + "]\n", limits);
        append_line(output, "NAME=" + name, limits); append_line(output, "STATIC_ROTATION_AXIS=" + number(object.static_rotation_axis[0]) + ", " + number(object.static_rotation_axis[1]) + ", " + number(object.static_rotation_axis[2]), limits); append_line(output, "STATIC_ROTATION_ANGLE=" + number(object.static_rotation_angle), limits); append_line(output, "MULT_G=" + number(object.mult_g), limits); append_line(output, "DAMAGE_ZONE=" + zone, limits); append_line(output, "MIN_SPEED=" + number(object.min_speed), limits); append_line(output, "FULL_SPEED=" + number(object.full_speed), limits); append_line(output, "OSCILLATION_AXIS=" + number(object.oscillation_axis[0]) + ", " + number(object.oscillation_axis[1]) + ", " + number(object.oscillation_axis[2]), limits); append_line(output, "OSCILLATION_MIN_ANGLE=" + number(object.oscillation_min_angle), limits); append_line(output, "OSCILLATION_MAX_ANGLE=" + number(object.oscillation_max_angle), limits); append_line(output, "ALLOWED_G=" + number(object.allowed_g[0]) + ", " + number(object.allowed_g[1]) + ", " + number(object.allowed_g[2]), limits); write_extra(object); append_bounded(output, "\n", limits);
    }
    if (!output.empty() && output.back() == '\n') output.pop_back();
    append_bounded(output, "\n", limits);
    return output;
}

CarDamageBaseline capture_car_damage_baseline(const CarDamageConfig& config) { return {config}; }

std::size_t car_damage_edit_count(const CarDamageEdits& edits) {
    std::size_t count = 0; for (const auto& [section, values] : edits.values) { (void)section; count += values.size(); } return count;
}

std::size_t apply_car_damage_edits(CarDamageConfig& config, const CarDamageEdits& edits,
                                   const CarDamageBaseline& baseline, CarDamageLimits limits) {
    if (car_damage_edit_count(edits) > limits.max_extra_entries) throw CarDamageError("EDIT_LIMIT", "car damage edit count exceeds its limit");
    CarDamageConfig candidate = baseline.config;
    std::size_t applied = 0;
    const auto float_value = [](std::string_view raw) {
        const auto text = trim(raw); char* end = nullptr; const double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0' || !std::isfinite(value) || std::abs(value) > static_cast<double>(kFloatMaximum)) throw CarDamageError("EDIT_INVALID", "damage edit number is invalid");
        return static_cast<float>(value);
    };
    const auto vector_value = [&](std::string_view raw) {
        Vector3 result{}; std::size_t start = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto comma = raw.find(',', start); const auto part = trim(raw.substr(start, comma == std::string_view::npos ? raw.size() - start : comma - start));
            if (part.empty()) throw CarDamageError("EDIT_INVALID", "damage edit vector is invalid");
            result[axis] = float_value(part);
            if (comma == std::string_view::npos) {
                if (axis != 2) throw CarDamageError("EDIT_INVALID", "damage edit vector needs three components");
                break;
            }
            if (axis == 2) throw CarDamageError("EDIT_INVALID", "damage edit vector needs exactly three components");
            start = comma + 1U;
        }
        return result;
    };
    for (const auto& [raw_section, values] : edits.values) {
        const auto section = upper(raw_section);
        auto apply = [&](auto& target, std::string_view key, std::string_view value) {
            const auto field = std::string(key);
            if constexpr (std::is_same_v<std::decay_t<decltype(target)>, Scratches>) {
                if (field == "minSpeed") target.min_speed = float_value(value); else if (field == "maxSpeed") target.max_speed = float_value(value); else throw CarDamageError("EDIT_INVALID", "unsupported SCRATCHES field");
            } else if constexpr (std::is_same_v<std::decay_t<decltype(target)>, Oscillations>) {
                if (field != "enabled") throw CarDamageError("EDIT_INVALID", "unsupported OSCILLATIONS field");
                target.enabled = float_value(value) != 0.0F;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(target)>, DamageDefaults>) {
                if (field != "initialLevel") throw CarDamageError("EDIT_INVALID", "unsupported DAMAGE field");
                target.initial_level = float_value(value);
            } else {
                if (field == "name") target.object_name = safe_ini_text(value, 1024, "visual-object name");
                else if (field == "staticRotationAxis") target.static_rotation_axis = vector_value(value);
                else if (field == "staticRotationAngle") target.static_rotation_angle = float_value(value);
                else if (field == "multG") target.mult_g = float_value(value);
                else if (field == "damageZone") {
                    const auto token = upper(trim(value));
                    if (token.empty() || token.size() > 64 || !std::all_of(token.begin(), token.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }))
                        throw CarDamageError("EDIT_INVALID", "damage zone is invalid");
                    target.damage_zone = token;
                }
                else if (field == "minSpeed") target.min_speed = float_value(value);
                else if (field == "fullSpeed") target.full_speed = float_value(value);
                else if (field == "oscillationAxis") target.oscillation_axis = vector_value(value);
                else if (field == "oscillationMinAngle") target.oscillation_min_angle = float_value(value);
                else if (field == "oscillationMaxAngle") target.oscillation_max_angle = float_value(value);
                else if (field == "allowedG") target.allowed_g = vector_value(value);
                else throw CarDamageError("EDIT_INVALID", "unsupported visual-object field");
            }
            ++applied;
        };
        if (section == "SCRATCHES") for (const auto& [key, value] : values) apply(candidate.scratches, key, value);
        else if (section == "OSCILLATIONS") for (const auto& [key, value] : values) apply(candidate.oscillations, key, value);
        else if (section == "DAMAGE") for (const auto& [key, value] : values) apply(candidate.damage, key, value);
        else if (section.rfind("VISUAL_OBJECT_", 0) == 0) {
            const auto suffix = section.substr(14); std::uint64_t index = 0; const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) throw CarDamageError("EDIT_INVALID", "visual-object section is invalid");
            const auto found = std::find_if(candidate.visual_objects.begin(), candidate.visual_objects.end(), [&](const auto& object) { return object.index == index; });
            if (found == candidate.visual_objects.end()) continue;
            for (const auto& [key, value] : values) apply(*found, key, value);
        } else throw CarDamageError("EDIT_INVALID", "unsupported damage section");
    }
    config = std::move(candidate);
    return applied;
}

namespace {
void validate_collider_vector(const Vector3& value, bool size, std::string_view label) {
    for (const auto component : value) {
        if (!std::isfinite(component) || std::abs(component) > kFloatMaximum) throw CarDamageError("COLLIDER_INVALID", std::string(label) + " must be finite float32");
        if (size && component <= 0.0F) throw CarDamageError("COLLIDER_INVALID", "size components must be positive float32 values");
    }
}
void update_bounds(BottomCollider& collider) {
    validate_collider_vector(collider.centre, false, "centre"); validate_collider_vector(collider.size, true, "size");
    Vector3 minimum_bounds{};
    Vector3 maximum_bounds{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float half = collider.size[axis] / 2.0F;
        const float minimum = collider.centre[axis] - half;
        const float maximum = collider.centre[axis] + half;
        if (!std::isfinite(minimum) || !std::isfinite(maximum))
            throw CarDamageError("COLLIDER_INVALID", "collider bounds exceed finite float32 range");
        minimum_bounds[axis] = minimum;
        maximum_bounds[axis] = maximum;
    }
    collider.bounds_min = minimum_bounds;
    collider.bounds_max = maximum_bounds;
}

void validate_existing_bounds(const BottomCollider& collider) {
    if (collider.bounds_min) validate_collider_vector(*collider.bounds_min, false, "bounds minimum");
    if (collider.bounds_max) validate_collider_vector(*collider.bounds_max, false, "bounds maximum");
}

void fill_missing_bounds(BottomCollider& collider) {
    validate_existing_bounds(collider);
    if (collider.bounds_min && collider.bounds_max) return;
    BottomCollider calculated = collider;
    update_bounds(calculated);
    if (!collider.bounds_min) collider.bounds_min = calculated.bounds_min;
    if (!collider.bounds_max) collider.bounds_max = calculated.bounds_max;
}
}

BottomColliderBaseline capture_bottom_collider_baseline(const BottomColliderConfig& config, BottomColliderLimits limits) {
    if (config.colliders.size() > limits.max_colliders) throw CarDamageError("COLLIDER_LIMIT", "collider count exceeds its limit");
    BottomColliderBaseline result{config}; for (auto& collider : result.config.colliders) fill_missing_bounds(collider); return result;
}

std::size_t bottom_collider_edit_count(const BottomColliderEdits& edits) { std::size_t count = 0; for (const auto& [index, edit] : edits) { (void)index; count += static_cast<std::size_t>(edit.centre.has_value()) + static_cast<std::size_t>(edit.size.has_value()) + static_cast<std::size_t>(edit.ground_enabled.has_value()); } return count; }

BottomColliderApplyResult apply_bottom_collider_edits(BottomColliderConfig& config, const BottomColliderEdits& edits, const BottomColliderBaseline& baseline, BottomColliderLimits limits) {
    BottomColliderApplyResult result;
    if (baseline.config.colliders.size() > limits.max_colliders || bottom_collider_edit_count(edits) > limits.max_edits) { result.diagnostics.push_back({"LIMIT", 0, "bottom collider input exceeds its limit"}); return result; }
    BottomColliderConfig candidate = baseline.config;
    for (const auto& [index, edit] : edits) {
        if (index >= candidate.colliders.size()) continue;
        try {
            auto& collider = candidate.colliders[index];
            if (edit.centre) { validate_collider_vector(*edit.centre, false, "centre"); collider.centre = *edit.centre; ++result.applied; }
            if (edit.size) { validate_collider_vector(*edit.size, true, "size"); collider.size = *edit.size; ++result.applied; }
            if (edit.ground_enabled) { collider.ground_enabled = *edit.ground_enabled; ++result.applied; }
            update_bounds(collider);
        } catch (const CarDamageError& error) { result.diagnostics.push_back({error.code(), index, error.what()}); result.applied = 0; return result; }
    }
    config = std::move(candidate);
    return result;
}

} // namespace apex::domain
