#include "apex/domain/skin_metadata.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using apex::domain::SkinMetadataError;
using apex::domain::SkinMetadataEdit;
using apex::domain::SkinMetadataLimits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void parses_bounded_metadata_and_preserves_unknowns() {
    const auto parsed = apex::domain::parse_skin_metadata(
        "\xEF\xBB\xBF{\"skinname\":\"Rosso\",\"drivername\":\"\",\"country\":\"Italy\",\"team\":\"\",\"number\":33,\"priority\":30,\"extension\":{\"tag\":true}}",
        "ui_skin.json");
    require(parsed.byte_length > 0 && parsed.metadata.skinname == "Rosso" && parsed.metadata.number == "33" &&
                parsed.metadata.priority == 30 && parsed.warnings.empty(), "official metadata fields");
    SkinMetadataEdit edit;
    edit.skinname = "Nuovo";
    edit.number = "07";
    edit.priority = 5;
    const auto output = apex::domain::serialize_skin_metadata(parsed, edit);
    require(output.find("\"extension\": {") != std::string::npos && output.find("\"skinname\": \"Nuovo\"") != std::string::npos,
            "unknown fields and edits serialized");
}

void diagnoses_types_unsafe_keys_and_truncation() {
    const auto parsed = apex::domain::parse_skin_metadata(
        "{\"skinname\":42,\"priority\":1.5,\"__proto__\":{\"polluted\":true},\"number\":33}");
    require(parsed.metadata.skinname.empty() && parsed.metadata.number == "33" && !parsed.metadata.priority &&
                parsed.warnings.size() == 3, "known type and unsafe key diagnostics");
    SkinMetadataEdit edit;
    edit.skinname = std::string(5000, 'x');
    const auto normalized = apex::domain::normalize_skin_metadata_edit(edit);
    require(normalized.has_value() && normalized->skinname->size() == 4096, "edit string truncation");

    SkinMetadataLimits string_limits;
    string_limits.maxStringBytes = 2;
    bool known_string = false;
    try { (void)apex::domain::parse_skin_metadata("{\"skinname\":\"long\"}", "known.json", string_limits); }
    catch (const SkinMetadataError& error) { known_string = error.code() == "STRING_LIMIT"; }
    require(known_string, "known field string limit");

    SkinMetadataEdit invalid_edit;
    invalid_edit.team = std::string("\xC3\x28", 2);
    bool invalid_edit_utf8 = false;
    try { (void)apex::domain::normalize_skin_metadata_edit(invalid_edit); }
    catch (const SkinMetadataError& error) { invalid_edit_utf8 = error.code() == "INVALID_UTF8"; }
    require(invalid_edit_utf8, "edit UTF-8 validation");
}

void rejects_malformed_and_over_limit_input() {
    bool truncated = false;
    try { (void)apex::domain::parse_skin_metadata("{\"skinname\":\"Red\"", "truncated.json"); }
    catch (const SkinMetadataError& error) { truncated = error.code() == "TRUNCATED" || error.code() == "INVALID_JSON"; }
    require(truncated, "truncated JSON");
    const std::string invalid_utf8("\xFF\xFE", 2);
    bool utf8 = false;
    try { (void)apex::domain::parse_skin_metadata(invalid_utf8); }
    catch (const SkinMetadataError& error) { utf8 = error.code() == "INVALID_UTF8"; }
    require(utf8, "invalid UTF-8");
    SkinMetadataLimits limits;
    limits.maxDepth = 2;
    bool depth = false;
    try { (void)apex::domain::parse_skin_metadata("{\"a\":{\"b\":{\"c\":true}}}", "deep.json", limits); }
    catch (const SkinMetadataError& error) { depth = error.code() == "DEPTH_LIMIT"; }
    require(depth, "JSON depth limit");
    limits = {};
    limits.maxBytes = 4;
    bool bytes = false;
    try { (void)apex::domain::parse_skin_metadata("{\"x\":1}", "large.json", limits); }
    catch (const SkinMetadataError& error) { bytes = error.code() == "BYTE_LIMIT"; }
    require(bytes, "JSON byte limit");
    bool nul = false;
    try { (void)apex::domain::parse_skin_metadata(std::string("{\"x\":\0}", 8)); }
    catch (const SkinMetadataError& error) { nul = error.code() == "NUL_BYTE"; }
    require(nul, "JSON NUL rejection");

    SkinMetadataLimits members;
    members.maxMembers = 1;
    bool duplicate_members = false;
    try { (void)apex::domain::parse_skin_metadata("{\"a\":1,\"a\":2}", "duplicate.json", members); }
    catch (const SkinMetadataError& error) { duplicate_members = error.code() == "MEMBER_LIMIT"; }
    require(duplicate_members, "duplicate members count toward limit");

    SkinMetadataLimits output_limits;
    output_limits.maxOutputBytes = 10;
    bool output = false;
    try {
        auto created = apex::domain::create_skin_metadata();
        SkinMetadataEdit edit;
        edit.skinname = "this is too large for the writer";
        (void)apex::domain::serialize_skin_metadata(created, edit, output_limits);
    } catch (const SkinMetadataError& error) { output = error.code() == "BYTE_LIMIT"; }
    require(output, "checked JSON writer output limit");
}

void creates_without_priority() {
    const auto created = apex::domain::create_skin_metadata();
    require(created.original.object.size() == 5 && created.original.object[0].first == "skinname" &&
                created.original.object[4].first == "number", "created known-key shape");
    SkinMetadataEdit edit;
    edit.skinname = "Blue";
    const auto output = apex::domain::serialize_skin_metadata(created, edit);
    require(output.find("\"priority\"") == std::string::npos && output.find("\"skinname\": \"Blue\"") != std::string::npos,
            "new metadata omits priority");
}

void preserves_object_order_and_normalizes_numbers() {
    const auto parsed = apex::domain::parse_skin_metadata(
        "{\"z\":1.0,\"a\":2,\"z\":3}");
    require(parsed.original.object.size() == 2 && parsed.original.object[0].first == "z" &&
                parsed.original.object[1].first == "a", "duplicate key replacement preserves order");
    const auto output = apex::domain::serialize_skin_metadata(parsed);
    require(output.find("\"z\": 3") < output.find("\"a\": 2"), "unknown member order");
}

}  // namespace

int main() {
    try {
        parses_bounded_metadata_and_preserves_unknowns();
        diagnoses_types_unsafe_keys_and_truncation();
        rejects_malformed_and_over_limit_input();
        creates_without_priority();
        preserves_object_order_and_normalizes_numbers();
        std::cout << "Skin metadata tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Skin metadata tests failed: " << error.what() << '\n';
        return 1;
    }
}
