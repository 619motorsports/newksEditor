#include "apex/core/parse_error.hpp"
#include "apex/formats/ini.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::core::ParseError;
using apex::formats::IniParseLimits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void expects_error(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const ParseError& error) {
        require(error.code() == code, "unexpected INI error code");
        return;
    }
    throw std::runtime_error("malformed INI was accepted");
}

void preserves_order_duplicates_comments_and_bom() {
    const std::string input = "\xef\xbb\xbf; preamble\r\n[MAIN]\nKEY=1 ; first\nKEY=2\n";
    const auto document = apex::formats::parse_csp_ini(input, "fixture.ini");
    require(document.source == "fixture.ini", "source");
    require(document.sections.size() == 1, "one section");
    require(document.sections[0].name == "MAIN", "section name");
    require(document.sections[0].line == 2, "BOM line attribution");
    require(document.sections[0].entries.size() == 2, "duplicate entries retained");
    require(document.sections[0].entries[0].value == "1", "inline comment removed");
    require(document.sections[0].last_value("key") == "2", "last value");
    require(document.sections[0].all_values("KEY") == std::vector<std::string>{"1", "2"},
            "all values");
    require(document.comments.size() == 2, "comments retained");
    require(document.comments[0].text == "; preamble", "comment text");
    require(document.comments[0].line == 1, "comment line");
    require(document.records.size() == 5, "source records");
    require(document.records[0].kind == apex::formats::IniRecord::Kind::comment,
            "comment record order");
    require(document.records[1].kind == apex::formats::IniRecord::Kind::section,
            "section record order");
    require(document.records[2].kind == apex::formats::IniRecord::Kind::entry,
            "first entry record order");
}

void parses_continuations_and_typed_values() {
    const auto document = apex::formats::parse_ini(
        "[VALUES]\nTEXT=\"a;b\"\nVECTOR=1, 2, 3\nNUMBER=0.25\nJOIN=hello\\\n world\\\n!\n");
    const auto& section = document.sections[0];
    require(section.entries[0].value == "\"a;b\"", "semicolon in quote is data");
    require(section.entries[0].typed.is_string(), "quoted text type");
    require(section.entries[1].typed.is_numbers() && section.entries[1].typed.numbers_value()->size() == 3,
            "list type");
    require(section.entries[2].typed.number_value() != nullptr &&
                *section.entries[2].typed.number_value() == 0.25,
            "number type");
    const auto* joined = section.last_entry("join");
    require(joined != nullptr && joined->value == "helloworld!", "continuation join");
    require(joined->line == 5 && joined->continuation_lines == std::vector<std::size_t>{5, 6},
            "continuation line attribution");
}

void retains_duplicate_sections_and_warns_for_legacy_lines() {
    const auto document = apex::formats::parse_ini(
        "outside=1\n; comment\n[ONE]\nA=x\n[ONE]\nA=y\nnot-an-entry\n");
    require(document.sections.size() == 2, "duplicate sections retained");
    require(document.sections_named("one").size() == 2, "all duplicate sections");
    require(document.warnings.size() == 2, "legacy malformed lines warned");
    require(document.warnings[0].line == 1 && document.warnings[1].line == 7,
            "warning lines");
}

void rejects_untrusted_or_over_limit_input() {
    expects_error([] {
        const std::string invalid("[A]\nX=\xff", 7);
        (void)apex::formats::parse_ini(invalid, "invalid.ini");
    }, "INVALID_UTF8");
    expects_error([] { (void)apex::formats::parse_ini("[A]\nX=one\\", "truncated.ini"); },
                  "TRUNCATED_CONTINUATION");
    IniParseLimits line_limits;
    line_limits.maxLineBytes = 2;
    expects_error([&] { (void)apex::formats::parse_ini("[A]\nX=1", "line.ini", line_limits); },
                  "LINE_LIMIT");
    IniParseLimits section_limits;
    section_limits.maxSections = 1;
    expects_error([&] { (void)apex::formats::parse_ini("[A]\n[B]", "sections.ini", section_limits); },
                  "SECTION_LIMIT");
    IniParseLimits entry_limits;
    entry_limits.maxEntries = 1;
    expects_error([&] { (void)apex::formats::parse_ini("[A]\nX=1\nY=2", "entries.ini", entry_limits); },
                  "ENTRY_LIMIT");
    IniParseLimits value_limits;
    value_limits.maxValueBytes = 2;
    expects_error([&] { (void)apex::formats::parse_ini("[A]\nX=123", "value.ini", value_limits); },
                  "VALUE_LIMIT");
}

void rejects_every_prefix_of_a_continued_value() {
    const std::string valid = "[A]\nX=one\\\ntwo\n";
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::string prefix = valid.substr(0, length);
        // Prefixes that end after the complete first line are valid as an
        // empty/truncated logical file only when no continuation marker is
        // left.  A dangling marker must always be rejected.
        if (!prefix.empty() && prefix.back() == '\\') {
            expects_error([&] { (void)apex::formats::parse_ini(prefix); }, "TRUNCATED_CONTINUATION");
        }
    }
}

}  // namespace

int main() {
    try {
        preserves_order_duplicates_comments_and_bom();
        parses_continuations_and_typed_values();
        retains_duplicate_sections_and_warns_for_legacy_lines();
        rejects_untrusted_or_over_limit_input();
        rejects_every_prefix_of_a_continued_value();
        std::cout << "INI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "INI tests failed: " << error.what() << '\n';
        return 1;
    }
}
