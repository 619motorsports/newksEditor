#include "apex/domain/skin_metadata.hpp"

#include "apex/core/byte_reader.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace apex::domain {
namespace {

[[nodiscard]] SkinJsonValue::Pointer pointer(SkinJsonValue value) {
    return std::make_shared<SkinJsonValue>(std::move(value));
}

constexpr std::string_view kKnownText[] = {"skinname", "drivername", "country", "team", "number"};
constexpr std::string_view kKnown[] = {"skinname", "drivername", "country", "team", "number", "priority"};

[[nodiscard]] bool unsafe_key(std::string_view key) {
    return key == "__proto__" || key == "constructor" || key == "prototype";
}

[[nodiscard]] bool known_key(std::string_view key) {
    return std::find(std::begin(kKnown), std::end(kKnown), key) != std::end(kKnown);
}

[[nodiscard]] std::string number_string(double value) {
    // JSON numbers are stored as JS-compatible binary64 values. Serialization
    // therefore follows canonical to_chars formatting, rather than retaining
    // source spelling (for example, 1.0 becomes 1), like JSON.stringify.
    char buffer[64]{};
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value,
                                         std::chars_format::general);
    if (converted.ec != std::errc{}) return {};
    return std::string(buffer, converted.ptr);
}

[[nodiscard]] std::string truncate_utf8(std::string_view value, std::size_t characters) {
    std::size_t offset = 0;
    while (offset < value.size() && characters != 0) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        const std::size_t width = byte < 0x80U ? 1 : byte < 0xE0U ? 2 : byte < 0xF0U ? 3 : 4;
        if (width > value.size() - offset) break;
        offset += width;
        --characters;
    }
    return std::string(value.substr(0, offset));
}

class JsonParser final {
public:
    JsonParser(std::string_view text, SkinMetadataLimits limits)
        : text_(text), limits_(limits) {}

    [[nodiscard]] SkinJsonValue parse() {
        auto value = value_at(0);
        whitespace();
        if (offset_ != text_.size()) fail("TRAILING_BYTES", "trailing JSON data");
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view code, std::string_view message) const {
        throw SkinMetadataError(std::string(message), offset_, std::string(code));
    }

    void whitespace() noexcept {
        while (offset_ < text_.size() && (text_[offset_] == ' ' || text_[offset_] == '\t' ||
                                          text_[offset_] == '\n' || text_[offset_] == '\r')) ++offset_;
    }

    char take() {
        if (offset_ >= text_.size()) fail("TRUNCATED", "truncated JSON input");
        return text_[offset_++];
    }

    void expect(char expected) {
        if (take() != expected) fail("INVALID_JSON", "unexpected JSON character");
    }

    [[nodiscard]] static int hex(char value) noexcept {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    void append_codepoint(std::string& output, std::uint32_t codepoint) {
        if (codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
            fail("INVALID_STRING", "invalid Unicode code point");
        if (codepoint <= 0x7FU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
        if (output.size() > limits_.maxStringBytes) fail("STRING_LIMIT", "JSON string exceeds configured limit");
    }

    [[nodiscard]] std::string string_value() {
        expect('"');
        std::string output;
        while (offset_ < text_.size()) {
            const auto value = take();
            if (value == '"') return output;
            if (static_cast<unsigned char>(value) < 0x20U) fail("INVALID_STRING", "control character in JSON string");
            if (value != '\\') {
                output.push_back(value);
                if (output.size() > limits_.maxStringBytes) fail("STRING_LIMIT", "JSON string exceeds configured limit");
                continue;
            }
            const auto escaped = take();
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const auto digit = hex(take());
                    if (digit < 0) fail("INVALID_STRING", "invalid Unicode escape");
                    codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(digit);
                }
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (offset_ + 6 > text_.size() || text_[offset_] != '\\' || text_[offset_ + 1] != 'u')
                        fail("INVALID_STRING", "unpaired Unicode surrogate");
                    offset_ += 2;
                    std::uint32_t low = 0;
                    for (int index = 0; index < 4; ++index) {
                        const auto digit = hex(take());
                        if (digit < 0) fail("INVALID_STRING", "invalid Unicode escape");
                        low = (low << 4U) | static_cast<std::uint32_t>(digit);
                    }
                    if (low < 0xDC00U || low > 0xDFFFU) fail("INVALID_STRING", "unpaired Unicode surrogate");
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + low - 0xDC00U;
                }
                append_codepoint(output, codepoint);
                break;
            }
            default: fail("INVALID_STRING", "invalid JSON escape");
            }
            if (output.size() > limits_.maxStringBytes) fail("STRING_LIMIT", "JSON string exceeds configured limit");
        }
        fail("TRUNCATED", "unterminated JSON string");
    }

    [[nodiscard]] SkinJsonValue value_at(std::size_t depth) {
        if (depth > limits_.maxDepth) fail("DEPTH_LIMIT", "JSON nesting exceeds configured limit");
        if (values_ >= limits_.maxValues) fail("VALUE_LIMIT", "JSON value count exceeds configured limit");
        ++values_;
        whitespace();
        if (offset_ >= text_.size()) fail("TRUNCATED", "truncated JSON value");
        switch (text_[offset_]) {
        case '{': return object_at(depth);
        case '[': return array_at(depth);
        case '"': return {SkinJsonValue::Kind::string, false, 0, string_value(), {}, {}};
        case 't': literal("true"); return {SkinJsonValue::Kind::boolean, true, 0, {}, {}, {}};
        case 'f': literal("false"); return {SkinJsonValue::Kind::boolean, false, 0, {}, {}, {}};
        case 'n': literal("null"); return {};
        default: return number_at();
        }
    }

    void literal(std::string_view value) {
        if (text_.substr(offset_, value.size()) != value) fail("INVALID_JSON", "invalid JSON literal");
        offset_ += value.size();
    }

    [[nodiscard]] SkinJsonValue number_at() {
        const auto begin = offset_;
        if (offset_ < text_.size() && text_[offset_] == '-') ++offset_;
        if (offset_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[offset_]))) fail("INVALID_NUMBER", "invalid JSON number");
        if (text_[offset_] == '0') {
            ++offset_;
            if (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) fail("INVALID_NUMBER", "leading zero in JSON number");
        } else while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_])) != 0) ++offset_;
        if (offset_ < text_.size() && text_[offset_] == '.') {
            ++offset_;
            if (offset_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[offset_]))) fail("INVALID_NUMBER", "invalid JSON fraction");
            while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_])) != 0) ++offset_;
        }
        if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-')) ++offset_;
            if (offset_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[offset_]))) fail("INVALID_NUMBER", "invalid JSON exponent");
            while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_])) != 0) ++offset_;
        }
        double value = 0;
        const auto token = text_.substr(begin, offset_ - begin);
        const auto converted = std::from_chars(token.data(), token.data() + token.size(), value);
        if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size() || !std::isfinite(value)) fail("INVALID_NUMBER", "JSON number is not finite");
        return {SkinJsonValue::Kind::number, false, value, {}, {}, {}};
    }

    [[nodiscard]] SkinJsonValue object_at(std::size_t depth) {
        expect('{');
        SkinJsonValue result;
        result.kind = SkinJsonValue::Kind::object;
        whitespace();
        if (offset_ < text_.size() && text_[offset_] == '}') { ++offset_; return result; }
        while (true) {
            // Count occurrences, not only unique keys. JavaScript's object
            // shape collapses duplicate keys, but accepting unlimited
            // duplicates would bypass the parser's aggregate bound.
            if (members_ >= limits_.maxMembers) fail("MEMBER_LIMIT", "JSON object member count exceeds configured limit");
            ++members_;
            whitespace();
            if (offset_ >= text_.size() || text_[offset_] != '"') fail("INVALID_JSON", "JSON object key must be a string");
            auto key = string_value();
            whitespace(); expect(':');
            auto value = value_at(depth + 1);
            // Keep the first insertion position while replacing a duplicate,
            // matching JavaScript object assignment/order semantics.
            const auto found = std::find_if(result.object.begin(), result.object.end(), [&](const auto& item) { return item.first == key; });
            if (found == result.object.end()) result.object.emplace_back(std::move(key), pointer(std::move(value)));
            else found->second = pointer(std::move(value));
            whitespace();
            const auto delimiter = take();
            if (delimiter == '}') return result;
            if (delimiter != ',') fail("INVALID_JSON", "expected comma in JSON object");
        }
    }

    [[nodiscard]] SkinJsonValue array_at(std::size_t depth) {
        expect('[');
        SkinJsonValue result;
        result.kind = SkinJsonValue::Kind::array;
        whitespace();
        if (offset_ < text_.size() && text_[offset_] == ']') { ++offset_; return result; }
        while (true) {
            if (result.array.size() >= limits_.maxArrayItems) fail("ARRAY_LIMIT", "JSON array item count exceeds configured limit");
            result.array.push_back(pointer(value_at(depth + 1)));
            whitespace();
            const auto delimiter = take();
            if (delimiter == ']') return result;
            if (delimiter != ',') fail("INVALID_JSON", "expected comma in JSON array");
        }
    }

    std::string_view text_;
    SkinMetadataLimits limits_;
    std::size_t offset_ = 0;
    std::size_t values_ = 0;
    std::size_t members_ = 0;
};

void warning(std::vector<std::string>& warnings, const SkinMetadataLimits& limits,
             std::string message) {
    if (warnings.size() >= limits.maxWarnings)
        throw SkinMetadataError("skin metadata warning count exceeds configured limit", 0, "WARNING_LIMIT");
    warnings.push_back(std::move(message));
}

SkinJsonValue sanitize(SkinJsonValue value, std::vector<std::string>& warnings,
                       const SkinMetadataLimits& limits, std::string path, std::size_t depth) {
    if (depth > limits.maxDepth) throw SkinMetadataError("JSON nesting exceeds configured limit", 0, "DEPTH_LIMIT");
    if (value.kind == SkinJsonValue::Kind::object) {
        std::vector<std::pair<std::string, SkinJsonValue::Pointer>> safe;
        safe.reserve(value.object.size());
        for (auto& item : value.object) {
            if (!item.second) throw SkinMetadataError("JSON object contains a null child", 0, "INVALID_VALUE");
            if (unsafe_key(item.first)) {
                warning(warnings, limits, (path.empty() ? "metadata" : path) + ": ignored unsafe key " + item.first);
                continue;
            }
            const auto child_path = path.empty() ? item.first : path + "." + item.first;
            safe.emplace_back(std::move(item.first), pointer(sanitize(std::move(*item.second), warnings, limits, child_path, depth + 1)));
        }
        value.object = std::move(safe);
    } else if (value.kind == SkinJsonValue::Kind::array) {
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (!value.array[index]) throw SkinMetadataError("JSON array contains a null child", 0, "INVALID_VALUE");
            value.array[index] = pointer(sanitize(std::move(*value.array[index]), warnings, limits, path + "[" + std::to_string(index) + "]", depth + 1));
        }
    }
    return value;
}

const SkinJsonValue* field(const SkinJsonValue& object, std::string_view key) {
    for (const auto& item : object.object) if (item.first == key) return item.second.get();
    return nullptr;
}

std::string text_field(const SkinJsonValue* value, std::string_view key,
                      std::vector<std::string>& warnings, const SkinMetadataLimits& limits) {
    if (value == nullptr) return {};
    if (value->kind == SkinJsonValue::Kind::string) {
        if (value->string.size() > limits.maxStringBytes)
            throw SkinMetadataError("known skin metadata field exceeds string limit", 0, "STRING_LIMIT");
        return truncate_utf8(value->string, 4096);
    }
    if (key == "number" && value->kind == SkinJsonValue::Kind::number && std::isfinite(value->number)) {
        auto result = number_string(value->number);
        if (result.size() > limits.maxStringBytes)
            throw SkinMetadataError("known skin metadata field exceeds string limit", 0, "STRING_LIMIT");
        return result;
    }
    warning(warnings, limits, std::string(key) + " must be a string" + (key == "number" ? " or finite number" : ""));
    return {};
}

std::optional<std::uint64_t> priority_field(const SkinJsonValue* value,
                                             std::vector<std::string>& warnings,
                                             const SkinMetadataLimits& limits) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == SkinJsonValue::Kind::number && std::isfinite(value->number) &&
        value->number >= 0 && value->number <= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) &&
        std::floor(value->number) == value->number && value->number <= 9007199254740991.0)
        return static_cast<std::uint64_t>(value->number);
    warning(warnings, limits, "priority must be a nonnegative integer");
    return std::nullopt;
}

void append_checked(std::string& output, std::string_view value,
                    const SkinMetadataLimits& limits) {
    if (output.size() > limits.maxOutputBytes || value.size() > limits.maxOutputBytes - output.size())
        throw SkinMetadataError("serialized skin metadata exceeds byte limit", 0, "BYTE_LIMIT");
    output.append(value);
}

void append_char(std::string& output, char value, const SkinMetadataLimits& limits) {
    append_checked(output, std::string_view(&value, 1), limits);
}

void append_escaped(std::string& output, std::string_view value,
                    const SkinMetadataLimits& limits) {
    append_char(output, '"', limits);
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': append_checked(output, "\\\"", limits); break;
        case '\\': append_checked(output, "\\\\", limits); break;
        case '\b': append_checked(output, "\\b", limits); break;
        case '\f': append_checked(output, "\\f", limits); break;
        case '\n': append_checked(output, "\\n", limits); break;
        case '\r': append_checked(output, "\\r", limits); break;
        case '\t': append_checked(output, "\\t", limits); break;
        default:
            if (character < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                char buffer[6] = {'\\', 'u', '0', '0', '0', '0'};
                buffer[2] = digits[(character >> 4U) & 0x0FU];
                buffer[3] = digits[character & 0x0FU];
                append_checked(output, std::string_view(buffer, sizeof(buffer)), limits);
            } else append_char(output, static_cast<char>(character), limits);
        }
    }
    append_char(output, '"', limits);
}

void append_indent(std::string& output, std::size_t depth,
                   const SkinMetadataLimits& limits) {
    if (depth > std::numeric_limits<std::size_t>::max() / 2U ||
        depth * 2U > limits.maxOutputBytes ||
        output.size() > limits.maxOutputBytes - depth * 2U)
        throw SkinMetadataError("serialized skin metadata exceeds byte limit", 0, "BYTE_LIMIT");
    output.append(depth * 2U, ' ');
}

void check_output(const std::string& output, const SkinMetadataLimits& limits) {
    if (output.size() > limits.maxOutputBytes)
        throw SkinMetadataError("serialized skin metadata exceeds byte limit", 0, "BYTE_LIMIT");
}

void append_json(std::string& output, const SkinJsonValue& value, std::size_t depth,
                 const SkinMetadataLimits& limits, std::size_t& values) {
    check_output(output, limits);
    if (depth > limits.maxDepth) throw SkinMetadataError("JSON nesting exceeds configured limit", 0, "DEPTH_LIMIT");
    if (values >= limits.maxValues) throw SkinMetadataError("JSON value count exceeds configured limit", 0, "VALUE_LIMIT");
    ++values;
    switch (value.kind) {
    case SkinJsonValue::Kind::null_value: append_checked(output, "null", limits); return;
    case SkinJsonValue::Kind::boolean: append_checked(output, value.boolean ? "true" : "false", limits); return;
    case SkinJsonValue::Kind::number:
        if (!std::isfinite(value.number)) throw SkinMetadataError("non-finite JSON number", 0, "NON_FINITE");
        append_checked(output, number_string(value.number), limits); return;
    case SkinJsonValue::Kind::string:
        if (value.string.size() > limits.maxStringBytes)
            throw SkinMetadataError("JSON string exceeds configured limit", 0, "STRING_LIMIT");
        append_escaped(output, value.string, limits); return;
    case SkinJsonValue::Kind::array:
        if (value.array.size() > limits.maxArrayItems) throw SkinMetadataError("JSON array item count exceeds configured limit", 0, "ARRAY_LIMIT");
        append_char(output, '[', limits);
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (!value.array[index]) throw SkinMetadataError("JSON array contains a null child", 0, "INVALID_VALUE");
            append_checked(output, index == 0 ? "\n" : ",\n", limits);
            append_indent(output, depth + 1, limits); append_json(output, *value.array[index], depth + 1, limits, values);
            check_output(output, limits);
        }
        if (!value.array.empty()) { append_char(output, '\n', limits); append_indent(output, depth, limits); }
        append_char(output, ']', limits); return;
    case SkinJsonValue::Kind::object:
        if (value.object.size() > limits.maxMembers) throw SkinMetadataError("JSON object member count exceeds configured limit", 0, "MEMBER_LIMIT");
        append_char(output, '{', limits);
        for (std::size_t index = 0; index < value.object.size(); ++index) {
            if (!value.object[index].second) throw SkinMetadataError("JSON object contains a null child", 0, "INVALID_VALUE");
            append_checked(output, index == 0 ? "\n" : ",\n", limits);
            if (value.object[index].first.size() > limits.maxStringBytes)
                throw SkinMetadataError("JSON object key exceeds configured limit", 0, "STRING_LIMIT");
            append_indent(output, depth + 1, limits); append_escaped(output, value.object[index].first, limits); append_checked(output, ": ", limits);
            append_json(output, *value.object[index].second, depth + 1, limits, values);
            check_output(output, limits);
        }
        if (!value.object.empty()) { append_char(output, '\n', limits); append_indent(output, depth, limits); }
        append_char(output, '}', limits); return;
    }
}

void add_known(std::string& output, bool& first, std::string_view key, std::string_view value,
               const SkinMetadataLimits& limits) {
    append_checked(output, first ? "\n" : ",\n", limits); first = false;
    append_indent(output, 1, limits); append_escaped(output, key, limits); append_checked(output, ": ", limits); append_escaped(output, value, limits);
}

}  // namespace

SkinMetadataError::SkinMetadataError(std::string message, std::size_t offset, std::string code)
    : std::runtime_error(std::move(message)), offset_(offset), code_(std::move(code)) {}

SkinMetadata parse_skin_metadata(std::string_view text, std::string source, SkinMetadataLimits limits) {
    if (text.size() > limits.maxBytes) throw SkinMetadataError("skin metadata exceeds byte limit", 0, "BYTE_LIMIT");
    const auto byte_length = text.size();
    if (text.find('\0') != std::string_view::npos) throw SkinMetadataError("skin metadata contains NUL", 0, "NUL_BYTE");
    try {
        apex::core::ByteReader::validateUtf8(text, "skin metadata", "SKIN_METADATA", source, 0);
    } catch (const apex::core::ParseError& error) {
        throw SkinMetadataError(error.what(), error.offset(), "INVALID_UTF8");
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU && static_cast<unsigned char>(text[2]) == 0xBFU) text.remove_prefix(3);
    JsonParser parser(text, limits);
    auto original = parser.parse();
    if (original.kind != SkinJsonValue::Kind::object)
        throw SkinMetadataError("skin metadata must contain one JSON object", 0, "ROOT_TYPE");
    SkinMetadata result;
    result.source = std::move(source);
    result.byte_length = byte_length;
    result.original = sanitize(std::move(original), result.warnings, limits, {}, 0);
    for (const auto key : kKnownText) {
        auto& destination = key == "skinname" ? result.metadata.skinname : key == "drivername" ? result.metadata.drivername : key == "country" ? result.metadata.country : key == "team" ? result.metadata.team : result.metadata.number;
        destination = text_field(field(result.original, key), key, result.warnings, limits);
    }
    result.metadata.priority = priority_field(field(result.original, "priority"), result.warnings, limits);
    return result;
}

SkinMetadata parse_skin_metadata(std::span<const std::uint8_t> bytes, std::string source,
                                 SkinMetadataLimits limits) {
    if (bytes.size() > limits.maxBytes) throw SkinMetadataError("skin metadata exceeds byte limit", 0, "BYTE_LIMIT");
    return parse_skin_metadata(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::move(source), limits);
}

SkinMetadata parse_skin_metadata(std::span<const std::byte> bytes, std::string source,
                                 SkinMetadataLimits limits) {
    return parse_skin_metadata(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), std::move(source), limits);
}

SkinMetadata create_skin_metadata(std::string source) {
    SkinMetadata result;
    result.source = std::move(source);
    result.original.kind = SkinJsonValue::Kind::object;
    // Match createSkinMetadata in the browser model: known text keys exist
    // as empty strings, while priority is omitted until explicitly set.
    for (const auto key : kKnownText)
        result.original.object.emplace_back(std::string(key), pointer(SkinJsonValue{
            SkinJsonValue::Kind::string, false, 0.0, {}, {}, {}}));
    return result;
}

std::optional<SkinMetadataEdit> normalize_skin_metadata_edit(const SkinMetadataEdit& edit,
                                                             SkinMetadataLimits limits) {
    SkinMetadataEdit result;
    const auto normalize = [&](const std::optional<std::string>& value) -> std::optional<std::string> {
        if (!value.has_value()) return std::nullopt;
        if (value->size() > limits.maxStringBytes) throw SkinMetadataError("skin metadata edit exceeds string limit", 0, "STRING_LIMIT");
        try {
            apex::core::ByteReader::validateUtf8(*value, "skin metadata edit", "SKIN_METADATA", "edit", 0);
        } catch (const apex::core::ParseError& error) {
            throw SkinMetadataError(error.what(), error.offset(), "INVALID_UTF8");
        }
        return truncate_utf8(*value, 4096);
    };
    result.skinname = normalize(edit.skinname); result.drivername = normalize(edit.drivername);
    result.country = normalize(edit.country); result.team = normalize(edit.team); result.number = normalize(edit.number);
    result.priority = edit.priority;
    if (!result.skinname && !result.drivername && !result.country && !result.team && !result.number && !result.priority) return std::nullopt;
    return result;
}

SkinMetadataFields effective_skin_metadata(const SkinMetadata& parsed, const SkinMetadataEdit& edit,
                                           SkinMetadataLimits limits) {
    const auto normalized = normalize_skin_metadata_edit(edit, limits);
    auto result = parsed.metadata;
    if (!normalized) return result;
    if (normalized->skinname) result.skinname = *normalized->skinname;
    if (normalized->drivername) result.drivername = *normalized->drivername;
    if (normalized->country) result.country = *normalized->country;
    if (normalized->team) result.team = *normalized->team;
    if (normalized->number) result.number = *normalized->number;
    if (normalized->priority) result.priority = normalized->priority;
    return result;
}

std::string serialize_skin_metadata(const SkinMetadata& parsed, const SkinMetadataEdit& edit,
                                    SkinMetadataLimits limits) {
    const auto metadata = effective_skin_metadata(parsed, edit, limits);
    for (const auto value : {std::string_view(metadata.skinname), std::string_view(metadata.drivername),
                             std::string_view(metadata.country), std::string_view(metadata.team),
                             std::string_view(metadata.number)}) {
        if (value.size() > limits.maxStringBytes)
            throw SkinMetadataError("known skin metadata field exceeds string limit", 0, "STRING_LIMIT");
    }
    std::string output;
    append_checked(output, "{", limits);
    bool first = true;
    const std::size_t known_members = 5U + (metadata.priority.has_value() ? 1U : 0U);
    if (known_members > limits.maxMembers)
        throw SkinMetadataError("serialized skin metadata member limit exceeded", 0, "MEMBER_LIMIT");
    if (limits.maxValues == 0 || known_members > limits.maxValues - 1U)
        throw SkinMetadataError("serialized skin metadata value limit exceeded", 0, "VALUE_LIMIT");
    std::size_t output_members = known_members;
    std::size_t values = 1U + known_members;
    add_known(output, first, "skinname", metadata.skinname, limits); add_known(output, first, "drivername", metadata.drivername, limits);
    add_known(output, first, "country", metadata.country, limits); add_known(output, first, "team", metadata.team, limits);
    add_known(output, first, "number", metadata.number, limits);
    check_output(output, limits);
    if (metadata.priority) {
        append_checked(output, first ? "\n" : ",\n", limits); first = false; append_indent(output, 1, limits); append_escaped(output, "priority", limits); append_checked(output, ": ", limits); append_checked(output, std::to_string(*metadata.priority), limits);
        check_output(output, limits);
    }
    if (parsed.original.kind == SkinJsonValue::Kind::object) {
        for (const auto& item : parsed.original.object) {
            if (known_key(item.first) || unsafe_key(item.first)) continue;
            if (output_members >= limits.maxMembers)
                throw SkinMetadataError("serialized skin metadata member limit exceeded", 0, "MEMBER_LIMIT");
            ++output_members;
            if (!item.second) throw SkinMetadataError("JSON object contains a null child", 0, "INVALID_VALUE");
            append_checked(output, first ? "\n" : ",\n", limits); first = false; append_indent(output, 1, limits); append_escaped(output, item.first, limits); append_checked(output, ": ", limits); append_json(output, *item.second, 1, limits, values);
            check_output(output, limits);
        }
    }
    if (!first) { append_char(output, '\n', limits); append_indent(output, 0, limits); }
    append_checked(output, "}\n", limits);
    return output;
}

}  // namespace apex::domain
