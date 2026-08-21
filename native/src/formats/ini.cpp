#include "apex/formats/ini.hpp"

#include "apex/core/byte_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::string_view kFormat = "INI";

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) --last;
    return value.substr(first, last - first);
}

[[nodiscard]] std::string upper_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] bool escaped(std::string_view value, std::size_t index) noexcept {
    std::size_t slashes = 0;
    while (index > 0 && value[index - 1] == '\\') {
        ++slashes;
        --index;
    }
    return (slashes & 1U) != 0U;
}

struct CommentSplit {
    std::string_view body;
    std::string_view comment;
};

[[nodiscard]] CommentSplit strip_comment(std::string_view line) noexcept {
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if ((character == '\'' || character == '"') && !escaped(line, index)) {
            quote = quote == '\0' ? character : quote == character ? '\0' : quote;
        } else if (character == ';' && quote == '\0') {
            return {line.substr(0, index), line.substr(index)};
        }
    }
    return {line, {}};
}

[[nodiscard]] bool finite_number(std::string_view value, double& output) noexcept {
    const std::string copy(value);
    if (copy.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(parsed)) return false;
    output = parsed;
    return true;
}

[[nodiscard]] std::string unquote(std::string_view value) {
    if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') &&
        value.back() == value.front()) {
        return std::string(value.substr(1, value.size() - 2));
    }
    return std::string(value);
}

[[nodiscard]] IniValue typed_value(std::string_view raw) {
    const auto text = trim(raw);
    const std::string unquoted = unquote(text);
    const std::string upper = upper_ascii(unquoted);
    if (upper == "ORIGINAL" || upper == "DISCARD") return {std::string(text), upper};

    double number = 0;
    if (finite_number(unquoted, number)) return {std::string(text), number};
    const auto parts = split_csp_list(text);
    if (parts.size() > 1) {
        std::vector<double> numbers;
        numbers.reserve(parts.size());
        for (const auto& part : parts) {
            double component = 0;
            if (!finite_number(trim(part), component)) return {std::string(text), unquoted};
            numbers.push_back(component);
        }
        return {std::string(text), std::move(numbers)};
    }
    return {std::string(text), unquoted};
}

[[noreturn]] void fail(std::string source, std::size_t offset, std::string_view code,
                       std::string_view message) {
    throw apex::core::ParseError(std::string(kFormat), std::move(source), offset,
                                 std::string(code), std::string(message));
}

void limit(std::size_t actual, std::size_t maximum, std::string source, std::size_t offset,
           std::string_view code, std::string_view what) {
    if (actual > maximum) fail(std::move(source), offset, code,
                               std::string(what) + " exceeds configured limit");
}

}  // namespace

std::vector<std::string> split_csp_list(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    char quote = '\0';
    int depth = 0;
    for (std::size_t index = 0; index <= value.size(); ++index) {
        const char character = index == value.size() ? '\0' : value[index];
        if ((character == '\'' || character == '"') && index < value.size() &&
            !escaped(value, index)) {
            quote = quote == '\0' ? character : quote == character ? '\0' : quote;
        } else if (quote == '\0' && (character == '(' || character == '{')) {
            ++depth;
        } else if (quote == '\0' && (character == ')' || character == '}')) {
            if (depth > 0) --depth;
        }
        if (index == value.size() || (character == ',' && quote == '\0' && depth == 0)) {
            const auto item = trim(value.substr(start, index - start));
            const auto unquoted_item = unquote(item);
            if (!unquoted_item.empty()) result.emplace_back(unquoted_item);
            start = index + 1;
        }
    }
    return result;
}

IniValue parse_csp_value(std::string_view value) { return typed_value(value); }

const IniEntry* IniSection::last_entry(std::string_view key) const noexcept {
    const auto wanted = upper_ascii(trim(key));
    for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
        if (iterator->key == wanted) return &*iterator;
    }
    return nullptr;
}

std::vector<const IniEntry*> IniSection::all_entries(std::string_view key) const {
    const auto wanted = upper_ascii(trim(key));
    std::vector<const IniEntry*> result;
    for (const auto& entry : entries) {
        if (entry.key == wanted) result.push_back(&entry);
    }
    return result;
}

std::string IniSection::last_value(std::string_view key, std::string_view fallback) const {
    const auto* entry = last_entry(key);
    return entry == nullptr ? std::string(fallback) : entry->value;
}

std::vector<std::string> IniSection::all_values(std::string_view key) const {
    const auto entries_for_key = all_entries(key);
    std::vector<std::string> result;
    result.reserve(entries_for_key.size());
    for (const auto* entry : entries_for_key) result.push_back(entry->value);
    return result;
}

const IniValue* IniSection::last_typed_value(std::string_view key) const noexcept {
    const auto* entry = last_entry(key);
    return entry == nullptr ? nullptr : &entry->typed;
}

std::vector<const IniValue*> IniSection::all_typed_values(std::string_view key) const {
    const auto entries_for_key = all_entries(key);
    std::vector<const IniValue*> result;
    result.reserve(entries_for_key.size());
    for (const auto* entry : entries_for_key) result.push_back(&entry->typed);
    return result;
}

const IniSection* IniDocument::section(std::string_view name) const noexcept {
    const auto wanted = upper_ascii(trim(name));
    for (const auto& item : sections) {
        if (upper_ascii(item.name) == wanted) return &item;
    }
    return nullptr;
}

std::vector<const IniSection*> IniDocument::sections_named(std::string_view name) const {
    const auto wanted = upper_ascii(trim(name));
    std::vector<const IniSection*> result;
    for (const auto& item : sections) {
        if (upper_ascii(item.name) == wanted) result.push_back(&item);
    }
    return result;
}

IniDocument parse_ini(std::span<const std::uint8_t> bytes, std::string source,
                      IniParseLimits limits) {
    limit(bytes.size(), limits.maxInputBytes, source, 0, "INPUT_LIMIT", "input");
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    return parse_ini(std::string_view(data, bytes.size()), std::move(source), limits);
}

IniDocument parse_ini(std::span<const std::byte> bytes, std::string source,
                      IniParseLimits limits) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return parse_ini(std::span<const std::uint8_t>(data, bytes.size()), std::move(source), limits);
}

IniDocument parse_ini(std::string_view text, std::string source, IniParseLimits limits) {
    limit(text.size(), limits.maxInputBytes, source, 0, "INPUT_LIMIT", "input");
    try {
        apex::core::ByteReader::validateUtf8(text, "INI text", std::string(kFormat), source, 0);
    } catch (const apex::core::ParseError&) {
        throw;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\0') fail(source, index, "NUL_BYTE", "NUL bytes are not valid INI text");
    }

    IniDocument document;
    document.source = source;
    document.bytes_read = text.size();
    std::size_t current_section = std::numeric_limits<std::size_t>::max();
    std::size_t entry_count = 0;
    std::size_t position = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xefU &&
                                   static_cast<unsigned char>(text[1]) == 0xbbU &&
                                   static_cast<unsigned char>(text[2]) == 0xbfU
                               ? 3
                               : 0;
    std::size_t line_number = 1;

    auto add_warning = [&](std::size_t line, std::string_view message) {
        document.warnings.push_back({source, line, std::string(message)});
    };
    auto add_comment = [&](std::string_view comment, std::size_t line) {
        if (comment.empty()) return;
        limit(document.comments.size() + 1, limits.maxComments, source, 0,
              "COMMENT_LIMIT", "comment count");
        const auto index = document.comments.size();
        document.comments.push_back({std::string(comment), line});
        document.records.push_back({IniRecord::Kind::comment, line, 0, 0, index});
    };
    auto process_logical = [&](std::string logical, std::size_t first_line,
                               std::vector<std::size_t> continuation_lines,
                               const std::vector<std::pair<std::string, std::size_t>>& comments) {
        const auto split = strip_comment(logical);
        const auto line = trim(split.body);
        const auto record_comments = [&] {
            for (const auto& comment : comments) add_comment(comment.first, comment.second);
        };
        if (line.empty()) {
            record_comments();
            return;
        }
        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']' || line.find(']') != line.size() - 1) {
                add_warning(first_line, "Malformed section header");
                record_comments();
                return;
            }
            const auto name = trim(line.substr(1, line.size() - 2));
            limit(name.size(), limits.maxSectionNameBytes, source, 0,
                  "SECTION_LIMIT", "section name");
            if (name.empty()) {
                add_warning(first_line, "Empty section name");
                record_comments();
                return;
            }
            limit(document.sections.size() + 1, limits.maxSections, source, 0,
                  "SECTION_LIMIT", "section count");
            document.sections.push_back({std::string(name), first_line, source, {}});
            current_section = document.sections.size() - 1;
            document.records.push_back({IniRecord::Kind::section, first_line,
                                        current_section, 0, 0});
            record_comments();
            return;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos || current_section == std::numeric_limits<std::size_t>::max()) {
            add_warning(first_line, "Entry outside a section or without '='");
            record_comments();
            return;
        }
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        limit(key.size(), limits.maxKeyBytes, source, 0, "ENTRY_LIMIT", "entry key");
        limit(value.size(), limits.maxValueBytes, source, 0, "VALUE_LIMIT", "entry value");
        auto& section = document.sections[current_section];
        limit(section.entries.size() + 1, limits.maxEntriesPerSection, source, 0,
              "ENTRY_LIMIT", "entries in section");
        limit(entry_count + 1, limits.maxEntries, source, 0,
              "ENTRY_LIMIT", "entry count");
        IniEntry entry;
        entry.key = upper_ascii(key);
        entry.value = std::string(value);
        entry.typed = typed_value(value);
        entry.line = first_line;
        entry.continuation_lines = std::move(continuation_lines);
        const auto entry_index = section.entries.size();
        section.entries.push_back(std::move(entry));
        ++entry_count;
        document.records.push_back({IniRecord::Kind::entry, first_line, current_section,
                                    entry_index, 0});
        record_comments();
    };

    while (position < text.size()) {
        limit(line_number, limits.maxLines, source, position, "LINE_LIMIT", "line count");
        const auto physical_line = line_number;
        const auto start_offset = position;
        std::size_t end = position;
        while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
        limit(end - position, limits.maxLineBytes, source, start_offset,
              "LINE_LIMIT", "physical line");
        std::string logical;
        std::vector<std::size_t> continuation_lines;
        std::vector<std::pair<std::string, std::size_t>> logical_comments;
        std::size_t consumed_line = physical_line;
        while (true) {
            const auto raw = text.substr(position, end - position);
            const auto split = strip_comment(raw);
            const auto trimmed = trim(split.body);
            if (!split.comment.empty()) logical_comments.emplace_back(std::string(split.comment), consumed_line);
            bool has_continuation = !trimmed.empty() && trimmed.back() == '\\';
            auto piece = has_continuation ? trimmed.substr(0, trimmed.size() - 1) : trimmed;
            logical.append(piece);
            if (!has_continuation) break;
            continuation_lines.push_back(consumed_line);
            limit(continuation_lines.size(), limits.maxContinuationLines, source, start_offset,
                  "CONTINUATION_LIMIT", "continuation lines");

            if (end == text.size()) {
                fail(source, start_offset, "TRUNCATED_CONTINUATION",
                     "continued entry has no following line");
            }
            const std::size_t newline_width = text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2U : 1U;
            position = end + newline_width;
            ++line_number;
            limit(line_number, limits.maxLines, source, position, "LINE_LIMIT", "line count");
            consumed_line = line_number;
            const auto next_start = position;
            end = position;
            while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
            limit(end - next_start, limits.maxLineBytes, source, next_start,
                  "LINE_LIMIT", "physical line");
        }
        process_logical(std::move(logical), physical_line, std::move(continuation_lines), logical_comments);
        if (end == text.size()) {
            position = end;
        } else {
            position = end + (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2 : 1);
        }
        ++line_number;
    }
    document.line_count = line_number == 1 ? 0 : line_number - 1;
    return document;
}

IniDocument parse_csp_ini(std::string_view text, std::string source, IniParseLimits limits) {
    return parse_ini(text, std::move(source), limits);
}

IniDocument parse_csp_ini(std::span<const std::uint8_t> bytes, std::string source,
                          IniParseLimits limits) {
    return parse_ini(bytes, std::move(source), limits);
}

IniDocument parse_csp_ini(std::span<const std::byte> bytes, std::string source,
                          IniParseLimits limits) {
    return parse_ini(bytes, std::move(source), limits);
}

}  // namespace apex::formats
