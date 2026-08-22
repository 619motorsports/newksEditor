#include "apex/authoring/project_io.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace apex::authoring {

namespace {

struct JsonValue {
    enum class Kind { nullValue, boolean, number, string, array, object };
    Kind kind = Kind::nullValue;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

[[nodiscard]] AuthoringError ioError(std::string_view code, std::size_t offset,
                                     std::string_view message) {
    return AuthoringError("AUTHORING_PROJECT", "project.json", offset, std::string(code),
                          std::string(message));
}

class JsonParser final {
public:
    JsonParser(std::string_view input, const ProjectIoLimits& limits) : input_(input), limits_(limits) {}

    [[nodiscard]] JsonValue parse() {
        skipSpace();
        const auto value = parseValue(0);
        skipSpace();
        if (position_ != input_.size()) throw ioError("JSON_MALFORMED", position_, "trailing JSON data");
        return value;
    }

private:
    void skipSpace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' || input_[position_] == '\r' ||
                input_[position_] == '\t'))
            ++position_;
    }

    [[nodiscard]] char take() {
        if (position_ >= input_.size()) throw ioError("JSON_TRUNCATED", position_, "truncated JSON value");
        return input_[position_++];
    }

    void expect(char expected) {
        if (take() != expected) throw ioError("JSON_MALFORMED", position_ - 1, "unexpected JSON character");
    }

    [[nodiscard]] JsonValue parseValue(std::size_t depth) {
        if (depth > limits_.maxDepth) throw ioError("JSON_DEPTH_LIMIT", position_, "JSON nesting exceeds its limit");
        skipSpace();
        if (position_ >= input_.size()) throw ioError("JSON_TRUNCATED", position_, "truncated JSON value");
        switch (input_[position_]) {
        case '{': return parseObject(depth + 1);
        case '[': return parseArray(depth + 1);
        case '"': return JsonValue{JsonValue::Kind::string, false, 0.0, parseString(), {}, {}};
        case 't': return parseLiteral("true", JsonValue::Kind::boolean, true);
        case 'f': return parseLiteral("false", JsonValue::Kind::boolean, false);
        case 'n': return parseLiteral("null", JsonValue::Kind::nullValue, false);
        default: return parseNumber();
        }
    }

    [[nodiscard]] JsonValue parseLiteral(std::string_view literal, JsonValue::Kind kind, bool boolean) {
        if (input_.substr(position_, literal.size()) != literal)
            throw ioError("JSON_MALFORMED", position_, "invalid JSON literal");
        position_ += literal.size();
        JsonValue output;
        output.kind = kind;
        output.boolean = boolean;
        return output;
    }

    static void appendCodepoint(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0x10ffffU) {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else throw ioError("JSON_MALFORMED", 0, "JSON Unicode codepoint is out of range");
    }

    [[nodiscard]] std::uint32_t hex4() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const auto character = take();
            value <<= 4U;
            if (character >= '0' && character <= '9') value |= static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f') value |= static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value |= static_cast<std::uint32_t>(character - 'A' + 10);
            else throw ioError("JSON_MALFORMED", position_ - 1, "invalid JSON Unicode escape");
        }
        return value;
    }

    [[nodiscard]] std::string parseString() {
        expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const auto character = take();
            if (character == '"') {
                if (!validUtf8(output)) throw ioError("JSON_UTF8", position_ - 1, "JSON string is not valid UTF-8");
                return output;
            }
            if (static_cast<unsigned char>(character) < 0x20U)
                throw ioError("JSON_MALFORMED", position_ - 1, "JSON string contains a control character");
            if (character != '\\') {
                output.push_back(character);
            } else {
                const auto escape = take();
                switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    const auto first = hex4();
                    std::uint32_t codepoint = first;
                    if (first >= 0xd800U && first <= 0xdbffU) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u')
                            throw ioError("JSON_MALFORMED", position_, "high surrogate must be followed by a low surrogate");
                        position_ += 2;
                        const auto second = hex4();
                        if (second < 0xdc00U || second > 0xdfffU)
                            throw ioError("JSON_MALFORMED", position_, "invalid low surrogate");
                        codepoint = 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
                    } else if (first >= 0xdc00U && first <= 0xdfffU) {
                        throw ioError("JSON_MALFORMED", position_, "lone low surrogate is invalid");
                    }
                    appendCodepoint(output, codepoint);
                    break;
                }
                default: throw ioError("JSON_MALFORMED", position_ - 1, "invalid JSON string escape");
                }
            }
            if (output.size() > limits_.maxStringBytes)
                throw ioError("JSON_STRING_LIMIT", position_, "JSON string exceeds its limit");
        }
        throw ioError("JSON_TRUNCATED", position_, "unterminated JSON string");
    }

    [[nodiscard]] JsonValue parseNumber() {
        const auto begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9')
            throw ioError("JSON_MALFORMED", begin, "invalid JSON number");
        if (input_[position_] == '0') ++position_;
        else while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const auto fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (position_ == fraction) throw ioError("JSON_MALFORMED", position_, "invalid JSON fraction");
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const auto exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (position_ == exponent) throw ioError("JSON_MALFORMED", position_, "invalid JSON exponent");
        }
        const auto text = std::string(input_.substr(begin, position_ - begin));
        char* end = nullptr;
        const auto number = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0' || !std::isfinite(number))
            throw ioError("JSON_NONFINITE", begin, "JSON number must be finite");
        JsonValue output;
        output.kind = JsonValue::Kind::number;
        output.number = number;
        return output;
    }

    [[nodiscard]] JsonValue parseArray(std::size_t depth) {
        expect('[');
        JsonValue output;
        output.kind = JsonValue::Kind::array;
        skipSpace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return output;
        }
        while (true) {
            if (++members_ > limits_.maxMembers) throw ioError("JSON_MEMBER_LIMIT", position_, "JSON member limit exceeded");
            output.array.push_back(parseValue(depth));
            skipSpace();
            const auto separator = take();
            if (separator == ']') return output;
            if (separator != ',') throw ioError("JSON_MALFORMED", position_ - 1, "expected JSON array separator");
            skipSpace();
        }
    }

    [[nodiscard]] JsonValue parseObject(std::size_t depth) {
        expect('{');
        JsonValue output;
        output.kind = JsonValue::Kind::object;
        skipSpace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return output;
        }
        while (true) {
            skipSpace();
            if (position_ >= input_.size() || input_[position_] != '"')
                throw ioError("JSON_MALFORMED", position_, "JSON object key must be a string");
            const auto key = parseString();
            if (!output.object.emplace(key, JsonValue{}).second)
                throw ioError("JSON_DUPLICATE_KEY", position_, "duplicate JSON object key");
            ++members_;
            if (members_ > limits_.maxMembers) throw ioError("JSON_MEMBER_LIMIT", position_, "JSON member limit exceeded");
            skipSpace();
            expect(':');
            output.object[key] = parseValue(depth);
            skipSpace();
            const auto separator = take();
            if (separator == '}') return output;
            if (separator != ',') throw ioError("JSON_MALFORMED", position_ - 1, "expected JSON object separator");
        }
    }

    std::string_view input_;
    const ProjectIoLimits& limits_;
    std::size_t position_ = 0;
    std::size_t members_ = 0;

    static bool validUtf8(std::string_view value) noexcept {
        std::size_t index = 0;
        while (index < value.size()) {
            const auto first = static_cast<unsigned char>(value[index++]);
            std::size_t continuation = 0;
            std::uint32_t codepoint = 0;
            if (first <= 0x7fU) continue;
            if (first >= 0xc2U && first <= 0xdfU) { continuation = 1; codepoint = first & 0x1fU; }
            else if (first >= 0xe0U && first <= 0xefU) { continuation = 2; codepoint = first & 0x0fU; }
            else if (first >= 0xf0U && first <= 0xf4U) { continuation = 3; codepoint = first & 0x07U; }
            else return false;
            if (index + continuation > value.size()) return false;
            for (std::size_t i = 0; i < continuation; ++i) {
                const auto byte = static_cast<unsigned char>(value[index++]);
                if ((byte & 0xc0U) != 0x80U) return false;
                codepoint = (codepoint << 6U) | (byte & 0x3fU);
            }
            if ((continuation == 2 && codepoint < 0x800U) ||
                (continuation == 3 && codepoint < 0x10000U) ||
                codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
        }
        return true;
    }
};

[[nodiscard]] const JsonValue* member(const JsonValue& value, std::string_view key) {
    if (value.kind != JsonValue::Kind::object) return nullptr;
    const auto found = value.object.find(std::string(key));
    return found == value.object.end() ? nullptr : &found->second;
}

[[nodiscard]] bool isObject(const JsonValue* value) { return value != nullptr && value->kind == JsonValue::Kind::object; }

void addDiagnostic(std::vector<ProjectIoDiagnostic>& diagnostics, std::string_view code,
                   std::string_view path, std::string_view message) {
    diagnostics.push_back({std::string(code), std::string(path), std::string(message)});
}

[[nodiscard]] bool getString(const JsonValue& object, std::string_view key, std::string& output,
                             std::vector<ProjectIoDiagnostic>& diagnostics, std::string_view path,
                             bool required = false) {
    const auto value = member(object, key);
    if (value == nullptr) {
        if (required) addDiagnostic(diagnostics, "FIELD_MISSING", std::string(path) + "." + std::string(key), "required field is missing");
        return false;
    }
    if (value->kind != JsonValue::Kind::string) {
        addDiagnostic(diagnostics, "FIELD_TYPE", std::string(path) + "." + std::string(key), "field must be a string");
        return false;
    }
    output = value->string;
    return true;
}

[[nodiscard]] bool getNumber(const JsonValue& object, std::string_view key, double& output,
                             std::vector<ProjectIoDiagnostic>& diagnostics, std::string_view path,
                             bool required = false) {
    const auto value = member(object, key);
    if (value == nullptr) {
        if (required) addDiagnostic(diagnostics, "FIELD_MISSING", std::string(path) + "." + std::string(key), "required field is missing");
        return false;
    }
    if (value->kind != JsonValue::Kind::number) {
        addDiagnostic(diagnostics, "FIELD_TYPE", std::string(path) + "." + std::string(key), "field must be a finite number");
        return false;
    }
    output = value->number;
    return true;
}

[[nodiscard]] bool indexKey(std::string_view key, std::uint32_t& output) {
    if (key.empty()) return false;
    std::uint64_t value = 0;
    const auto result = std::from_chars(key.data(), key.data() + key.size(), value);
    if (result.ec != std::errc{} || result.ptr != key.data() + key.size() || value > std::numeric_limits<std::uint32_t>::max()) return false;
    output = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool validUtf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index++]);
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7fU) continue;
        if (first >= 0xc2U && first <= 0xdfU) { continuation = 1; codepoint = first & 0x1fU; }
        else if (first >= 0xe0U && first <= 0xefU) { continuation = 2; codepoint = first & 0x0fU; }
        else if (first >= 0xf0U && first <= 0xf4U) { continuation = 3; codepoint = first & 0x07U; }
        else return false;
        if (index + continuation > value.size()) return false;
        for (std::size_t i = 0; i < continuation; ++i) {
            const auto byte = static_cast<unsigned char>(value[index++]);
            if ((byte & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if ((continuation == 2 && codepoint < 0x800U) ||
            (continuation == 3 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    }
    return true;
}

[[nodiscard]] std::string jsonEscape(std::string_view value) {
    if (!validUtf8(value)) throw ioError("JSON_UTF8", 0, "string is not valid UTF-8");
    std::string output;
    if (value.size() > std::numeric_limits<std::size_t>::max() - 2U)
        throw ioError("JSON_STRING_LIMIT", 0, "JSON string is too large");
    output.reserve(value.size() + 2U);
    output.push_back('"');
    for (const auto character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(static_cast<unsigned char>(character));
                output += stream.str();
            } else output.push_back(character);
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::string formatNumber(double value) {
    if (!std::isfinite(value) || value == 0.0) return "0";
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(9) << std::defaultfloat << value;
    return stream.str();
}

[[nodiscard]] std::string formatEditorNumber(float value) {
    if (!std::isfinite(value) || value == 0.0F) return "0";
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(6) << static_cast<double>(value);
    auto output = stream.str();
    while (!output.empty() && output.back() == '0') output.pop_back();
    if (!output.empty() && output.back() == '.') output.pop_back();
    return output == "-0" ? "0" : output;
}

class JsonWriter final {
public:
    explicit JsonWriter(std::size_t limit) : limit_(limit) {}
    void raw(std::string_view value) {
        ensure(value.size());
        output_.append(value.data(), value.size());
    }
    void string(std::string_view value) { raw(jsonEscape(value)); }
    void indent(std::size_t depth) {
        if (depth > std::numeric_limits<std::size_t>::max() / 2U)
            throw ioError("OUTPUT_LIMIT", output_.size(), "indentation exceeds its limit");
        const std::size_t spaces = depth * 2U;
        ensure(spaces);
        output_.append(spaces, ' ');
    }
    [[nodiscard]] const std::string& output() const noexcept { return output_; }
private:
    void ensure(std::size_t bytes) const {
        if (bytes > limit_ || output_.size() > limit_ - bytes)
            throw ioError("OUTPUT_LIMIT", output_.size(), "serialized project exceeds its limit");
    }
    std::size_t limit_;
    std::string output_;
};

template <std::size_t N>
void writeArray(JsonWriter& writer, const std::array<float, N>& value, std::size_t depth) {
    writer.raw("[");
    for (std::size_t index = 0; index < N; ++index) {
        if (index != 0) writer.raw(", ");
        writer.raw(formatNumber(value[index]));
    }
    writer.raw("]");
    (void)depth;
}

void writeNodeEdit(JsonWriter& writer, const NodeEdit& edit, std::size_t depth) {
    writer.raw("{");
    bool first = true;
    const auto field = [&](std::string_view key, const auto& value, auto writeValue) {
        if (!value) return;
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeValue(*value); first = false;
    };
    field("name", edit.name, [&](const auto& value) { writer.string(value); });
    field("active", edit.active, [&](const auto& value) { writer.raw(value ? "true" : "false"); });
    field("transform", edit.transform, [&](const auto& value) { writeArray(writer, value, depth + 1); });
    if (!first) { writer.raw("\n"); writer.indent(depth); }
    writer.raw("}");
}

void writeWorkspaceFile(JsonWriter& writer, const WorkspaceFileEdit& edit, std::size_t depth) {
    writer.raw("{");
    bool first = true;
    const auto scalar = [&](std::string_view key, const auto& value) {
        if (!value) return;
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writer.raw(formatNumber(*value)); first = false;
    };
    const auto text = [&](std::string_view key, const std::optional<std::string>& value) {
        if (!value) return;
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writer.string(*value); first = false;
    };
    const auto vector = [&](std::string_view key, const auto& value) {
        if (!value) return;
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeArray(writer, *value, depth + 1); first = false;
    };
    text("name", edit.name); vector("position", edit.position); vector("rotation", edit.rotation);
    scalar("lodIn", edit.lodIn); scalar("lodOut", edit.lodOut); scalar("probability", edit.probability);
    vector("multiplicity", edit.multiplicity); text("posMode", edit.posMode); vector("positionCenter", edit.positionCenter);
    vector("positionRange", edit.positionRange); text("velMode", edit.velMode); vector("velocityBase", edit.velocityBase);
    vector("velocityRange", edit.velocityRange); text("playWav", edit.playWav);
    if (!first) { writer.raw("\n"); writer.indent(depth); }
    writer.raw("}");
}

void parseNodeEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&, const ProjectIoLimits&);
void parseMeshEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&, const ProjectIoLimits&);
void parseGeometryEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&, const ProjectIoLimits&);
void parseWorkspaceEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&, const ProjectIoLimits&);
void parseMaterialEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&, const ProjectIoLimits&);
void parseSurfaceEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&);
void parseColliderEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&, bool);
void parseDamageEdits(const JsonValue&, ProjectSession&, std::vector<ProjectIoDiagnostic>&);
void writeProjectMap(JsonWriter&, const ProjectState&, std::size_t);

constexpr double kJsSafeIntegerMaximum = 9007199254740991.0;

[[nodiscard]] std::optional<SourceIdentity> parseSecondaryAssetIdentity(
    const JsonValue& root, std::string_view key, bool hasEdits,
    std::vector<ProjectIoDiagnostic>& diagnostics) {
    if (!hasEdits) return std::nullopt;
    const auto value = member(root, key);
    if (value == nullptr || value->kind == JsonValue::Kind::nullValue) return std::nullopt;
    if (!isObject(value)) {
        addDiagnostic(diagnostics, "FIELD_TYPE", key, "secondary asset identity must be an object or null");
        return std::nullopt;
    }

    bool valid = true;
    SourceIdentity identity;
    double size = 0.0;
    double kn5Version = 0.0;
    if (!getString(*value, "name", identity.name, diagnostics, key, true)) valid = false;
    if (getNumber(*value, "size", size, diagnostics, key, true)) {
        if (size >= 0.0 && std::trunc(size) == size && size <= kJsSafeIntegerMaximum)
            identity.size = static_cast<std::uint64_t>(size);
        else {
            addDiagnostic(diagnostics, "SOURCE_SIZE", std::string(key) + ".size",
                          "secondary asset size must be a non-negative JavaScript safe integer");
            valid = false;
        }
    } else valid = false;
    if (!getString(*value, "sha256", identity.sha256, diagnostics, key, true)) valid = false;
    if (member(*value, "kn5Version") != nullptr) {
        if (getNumber(*value, "kn5Version", kn5Version, diagnostics, key)) {
            if (kn5Version >= 0.0 && std::trunc(kn5Version) == kn5Version &&
                static_cast<long double>(kn5Version) <=
                    static_cast<long double>(std::numeric_limits<std::uint32_t>::max()))
                identity.kn5Version = static_cast<std::uint32_t>(kn5Version);
            else {
                addDiagnostic(diagnostics, "SOURCE_VERSION", std::string(key) + ".kn5Version",
                              "secondary asset KN5 version must be a non-negative uint32 integer");
                valid = false;
            }
        } else valid = false;
    }
    for (const auto& [field, ignored] : value->object) {
        (void)ignored;
        if (field != "name" && field != "size" && field != "sha256" && field != "kn5Version")
            addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", std::string(key) + "." + field,
                          "secondary asset identity field is not modeled");
    }
    if (!valid) return std::nullopt;
    try {
        return normalizeSecondaryAssetIdentity(std::move(identity));
    } catch (const AuthoringError& errorValue) {
        addDiagnostic(diagnostics, errorValue.code(), key, errorValue.what());
        return std::nullopt;
    }
}

template <typename T>
[[nodiscard]] bool hasValue(const std::optional<T>& value) { return value.has_value(); }

[[nodiscard]] bool hasEdit(const NodeEdit& edit) { return edit.name || edit.active || edit.transform; }
[[nodiscard]] bool hasEdit(const MeshEdit& edit) {
    return edit.transparent || edit.castShadows || edit.layer || edit.lodIn || edit.lodOut;
}
[[nodiscard]] bool hasEdit(const GeometryEdit& edit) {
    return edit.transform || edit.remove_degenerate || edit.reverse_winding || edit.recalculate_normals;
}
[[nodiscard]] bool hasEdit(const WorkspaceFileEdit& edit) {
    return edit.name || edit.position || edit.rotation || edit.lodIn || edit.lodOut || edit.probability ||
           edit.multiplicity || edit.posMode || edit.positionCenter || edit.positionRange || edit.velMode ||
           edit.velocityBase || edit.velocityRange || edit.playWav;
}
[[nodiscard]] bool hasEdit(const MaterialEdit& edit) {
    return !edit.scalars.empty() || !edit.vectors.empty() || !edit.resources.empty();
}
[[nodiscard]] bool hasEdit(const SurfaceEdit& edit) {
    return edit.key || edit.friction || edit.damping || edit.dirtAdditive || edit.blackFlagTime ||
           edit.isValidTrack || edit.isPitlane || edit.sinHeight || edit.sinLength || edit.vibrationGain ||
           edit.vibrationLength || edit.wav || edit.wavPitch || edit.ffEffect;
}
[[nodiscard]] bool hasEdit(const ColliderEdit& edit) {
    return edit.transform || edit.removeDegenerate || edit.reverseWinding || edit.recalculateNormals;
}
[[nodiscard]] bool hasEdit(const BottomColliderEdit& edit) { return edit.centre || edit.size || edit.groundEnabled; }
[[nodiscard]] bool hasEdit(const DamageEdit& edit) {
    return edit.minSpeed || edit.maxSpeed || edit.initialLevel || edit.staticRotationAngle || edit.multG ||
           edit.fullSpeed || edit.oscillationMinAngle || edit.oscillationMaxAngle || edit.staticRotationAxis ||
           edit.oscillationAxis || edit.allowedG || edit.enabled || edit.name || edit.damageZone;
}

void checkIoText(std::string_view value, std::string_view path, const ProjectIoLimits& limits) {
    if (value.size() > limits.maxStringBytes)
        throw ioError("STRING_LIMIT", 0, std::string(path) + " exceeds the project string limit");
    if (!validUtf8(value)) throw ioError("UTF8_INVALID", 0, std::string(path) + " is not valid UTF-8");
}

void validateDirectProject(const ProjectState& project, const ProjectIoLimits& limits) {
    ProjectSession session(project.source, AuthoringLimits{0, limits.maxEdits == 0 ? 1 : limits.maxEdits,
                                                            limits.maxEdits, limits.maxStringBytes, limits.maxEdits});
    const auto recovery = session.validateRecovery({project}, project.source);
    if (!recovery.restored) {
        if (!recovery.diagnostics.empty())
            throw ioError(recovery.diagnostics.front().code, 0, recovery.diagnostics.front().message);
        throw ioError("PROJECT_INVALID", 0, "project state is invalid");
    }
    if (project.workspace.cockpitHrDistance && !std::isfinite(*project.workspace.cockpitHrDistance))
        throw ioError("EDIT_INVALID", 0, "workspace cockpit distance must be finite");
    if (project.workspace.driverHrDistance && !std::isfinite(*project.workspace.driverHrDistance))
        throw ioError("EDIT_INVALID", 0, "workspace driver distance must be finite");
    checkIoText(project.source.name, "asset.name", limits);
    checkIoText(project.source.sha256, "asset.sha256", limits);
    const auto checkSecondaryIdentity = [&](const std::optional<SourceIdentity>& identity,
                                            std::string_view path, bool hasEdits) {
        if (!hasEdits || !identity) return;
        checkIoText(identity->name, std::string(path) + ".name", limits);
        checkIoText(identity->sha256, std::string(path) + ".sha256", limits);
    };
    checkSecondaryIdentity(project.colliderAsset, "colliderAsset", !project.colliders.empty());
    checkSecondaryIdentity(project.damageAsset, "damageAsset", !project.damage.empty());
    checkSecondaryIdentity(project.bottomColliderAsset, "bottomColliderAsset", !project.bottomColliders.empty());
    for (const auto& [path, edit] : project.nodes) {
        checkIoText(path, "nodeEdits key", limits);
        if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "node edit has no fields");
        if (edit.name) checkIoText(*edit.name, path, limits);
    }
    for (const auto& [name, edit] : project.meshes) {
        checkIoText(name, "meshEdits key", limits);
        if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "mesh edit has no fields");
    }
    for (const auto& [path, edit] : project.geometry) {
        checkIoText(path, "geometryEdits key", limits);
        if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "geometry edit has no fields");
    }
    for (const auto& [index, edit] : project.workspaceFiles) {
        (void)index;
        if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "workspace edit has no fields");
        if (edit.name) checkIoText(*edit.name, "workspace file name", limits);
        if (edit.posMode) checkIoText(*edit.posMode, "workspace position mode", limits);
        if (edit.velMode) checkIoText(*edit.velMode, "workspace velocity mode", limits);
        if (edit.playWav) checkIoText(*edit.playWav, "workspace playWav", limits);
    }
    for (const auto& [material, edit] : project.materials) {
        checkIoText(material, "material key", limits);
        if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "material edit has no fields");
        for (const auto& [field, value] : edit.scalars) {
            checkIoText(field, "material field", limits);
            if ((field == "shader" || field == "blendMode" || field == "depthMode" || field == "cullMode") &&
                !std::holds_alternative<std::string>(value))
                throw ioError("FIELD_TYPE", 0, "material state fields must be strings");
            if (const auto* text = std::get_if<std::string>(&value)) checkIoText(*text, "material scalar", limits);
        }
        for (const auto& [field, value] : edit.vectors) checkIoText(field, "material vector field", limits);
        for (const auto& [slot, value] : edit.resources) {
            checkIoText(slot, "material resource slot", limits);
            if (value.texture) checkIoText(*value.texture, "material texture", limits);
            if (value.file) checkIoText(*value.file, "material resource file", limits);
        }
    }
    for (const auto& [index, edit] : project.surfaces) { (void)index; if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "surface edit has no fields"); }
    for (const auto& [index, edit] : project.colliders) { (void)index; if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "collider edit has no fields"); }
    for (const auto& [index, edit] : project.bottomColliders) { (void)index; if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "bottom collider edit has no fields"); }
    for (const auto& [section, edit] : project.damage) {
        checkIoText(section, "damage section", limits);
        if (!hasEdit(edit)) throw ioError("EDIT_INVALID", 0, "damage edit has no fields");
        if (edit.name) {
            checkIoText(*edit.name, "damage name", limits);
            if (edit.name->empty() || edit.name->size() > 1024 || edit.name->find_first_of("\r\n;") != std::string::npos)
                throw ioError("EDIT_INVALID", 0, "damage name contains unsafe characters or exceeds its limit");
        }
        if (edit.damageZone) checkIoText(*edit.damageZone, "damage zone", limits);
        const auto check = [&](std::string_view field, bool present) {
            if (present && !damageFieldAllowed(section, field))
                throw ioError("UNSUPPORTED_FIELD", 0, "damage field is not valid for its section");
        };
        check("minSpeed", edit.minSpeed.has_value()); check("maxSpeed", edit.maxSpeed.has_value());
        check("initialLevel", edit.initialLevel.has_value()); check("staticRotationAngle", edit.staticRotationAngle.has_value());
        check("multG", edit.multG.has_value()); check("fullSpeed", edit.fullSpeed.has_value());
        check("oscillationMinAngle", edit.oscillationMinAngle.has_value()); check("oscillationMaxAngle", edit.oscillationMaxAngle.has_value());
        check("staticRotationAxis", edit.staticRotationAxis.has_value()); check("oscillationAxis", edit.oscillationAxis.has_value());
        check("allowedG", edit.allowedG.has_value()); check("enabled", edit.enabled.has_value());
        check("name", edit.name.has_value()); check("damageZone", edit.damageZone.has_value());
    }
    if (project.source.size > static_cast<std::uint64_t>(kJsSafeIntegerMaximum))
        throw ioError("SOURCE_SIZE", 0, "asset size exceeds the JavaScript safe-integer range");
    if (project.revision > static_cast<std::uint64_t>(kJsSafeIntegerMaximum))
        throw ioError("REVISION_INVALID", 0, "project revision exceeds the JavaScript safe-integer range");
}

void appendBounded(std::string& output, std::string_view value, std::size_t limit) {
    if (value.size() > limit || output.size() > limit - value.size())
        throw ioError("OUTPUT_LIMIT", output.size(), "serialized output exceeds its limit");
    output.append(value.data(), value.size());
}

ProjectIoResult parseProjectInternal(std::string_view text, const SourceIdentity* expectedSource,
                                     const ProjectIoLimits& limits) {
    if (text.size() > limits.maxInputBytes)
        throw ioError("INPUT_LIMIT", 0, "project JSON exceeds its input limit");
    const auto root = JsonParser(text, limits).parse();
    if (root.kind != JsonValue::Kind::object) throw ioError("JSON_MALFORMED", 0, "project JSON root must be an object");
    std::vector<ProjectIoDiagnostic> diagnostics;
    std::string format;
    if (!getString(root, "format", format, diagnostics, "project", true))
        throw ioError("PROJECT_INVALID", 0, "project format is missing");
    if (format != kProjectFormat) throw ioError("PROJECT_FORMAT", 0, "unsupported project format");
    double version = 0.0;
    if (!getNumber(root, "version", version, diagnostics, "project", true) || version != kProjectVersion)
        throw ioError("PROJECT_VERSION", 0, "unsupported project version");
    std::uint64_t revision = 0;
    if (const auto value = member(root, "revision"); value != nullptr) {
        if (value->kind != JsonValue::Kind::number || value->number < 0.0 ||
            std::trunc(value->number) != value->number || value->number > kJsSafeIntegerMaximum)
            throw ioError("REVISION_INVALID", 0, "project revision must be a non-negative JavaScript safe integer");
        revision = static_cast<std::uint64_t>(value->number);
    }
    SourceIdentity source;
    const auto asset = member(root, "asset");
    if (isObject(asset)) {
        double size = 0.0, kn5Version = 0.0;
        std::string name, sha256;
        if (getString(*asset, "name", name, diagnostics, "asset", true)) source.name = name;
        if (getNumber(*asset, "size", size, diagnostics, "asset", true)) {
            if (size >= 0.0 && std::trunc(size) == size && size <= kJsSafeIntegerMaximum)
                source.size = static_cast<std::uint64_t>(size);
            else addDiagnostic(diagnostics, "SOURCE_SIZE", "asset.size", "asset size must be a non-negative JavaScript safe integer");
        }
        if (getString(*asset, "sha256", sha256, diagnostics, "asset", true)) source.sha256 = sha256;
        if (getNumber(*asset, "kn5Version", kn5Version, diagnostics, "asset") && kn5Version >= 0.0 && std::trunc(kn5Version) == kn5Version && static_cast<long double>(kn5Version) <= static_cast<long double>(std::numeric_limits<std::uint32_t>::max())) source.kn5Version = static_cast<std::uint32_t>(kn5Version);
        for (const auto& [key, ignored] : asset->object)
            if (key != "name" && key != "size" && key != "sha256" && key != "kn5Version")
                addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", "asset." + key, "asset field is not modeled");
    } else if (expectedSource != nullptr) source = *expectedSource;
    else addDiagnostic(diagnostics, "FIELD_MISSING", "asset", "asset identity is required");
    try { source = normalizeSourceIdentity(std::move(source)); }
    catch (const AuthoringError& errorValue) { addDiagnostic(diagnostics, errorValue.code(), "asset", errorValue.what()); return {std::nullopt, std::move(diagnostics)}; }
    if (expectedSource != nullptr) {
        try { if (!sourceIdentityMatches(normalizeSourceIdentity(*expectedSource), source)) addDiagnostic(diagnostics, "STALE_SOURCE", "asset", "project asset identity does not match the expected source"); }
        catch (const AuthoringError& errorValue) { addDiagnostic(diagnostics, errorValue.code(), "asset", errorValue.what()); }
    }
    ProjectSession session(source, AuthoringLimits{0, limits.maxEdits == 0 ? 1 : limits.maxEdits, limits.maxEdits, limits.maxStringBytes, limits.maxEdits});
    if (const auto value = member(root, "materialEdits"); value != nullptr) parseMaterialEdits(*value, session, diagnostics, limits);
    if (const auto value = member(root, "meshEdits"); value != nullptr) parseMeshEdits(*value, session, diagnostics, limits);
    if (const auto value = member(root, "nodeEdits"); value != nullptr) parseNodeEdits(*value, session, diagnostics, limits);
    if (const auto value = member(root, "geometryEdits"); value != nullptr) parseGeometryEdits(*value, session, diagnostics, limits);
    if (const auto value = member(root, "workspaceEdits"); value != nullptr) parseWorkspaceEdits(*value, session, diagnostics, limits);
    if (const auto value = member(root, "surfaceEdits"); value != nullptr) parseSurfaceEdits(*value, session, diagnostics);
    if (const auto value = member(root, "colliderEdits"); value != nullptr) parseColliderEdits(*value, session, diagnostics, false);
    if (const auto value = member(root, "bottomColliderEdits"); value != nullptr) parseColliderEdits(*value, session, diagnostics, true);
    if (const auto value = member(root, "damageEdits"); value != nullptr) parseDamageEdits(*value, session, diagnostics);
    for (const auto& key : {"skinEdits"})
        if (const auto value = member(root, key); value != nullptr && value->kind != JsonValue::Kind::nullValue && ((isObject(value) && !value->object.empty()) || value->kind != JsonValue::Kind::object))
            addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", key, "project field is not modeled by the native authoring state");
    for (const auto& [key, value] : root.object)
        if (key != "format" && key != "version" && key != "revision" && key != "asset" && key != "materialEdits" && key != "meshEdits" && key != "nodeEdits" && key != "geometryEdits" && key != "colliderEdits" && key != "damageEdits" && key != "bottomColliderEdits" && key != "workspaceEdits" && key != "surfaceEdits" && key != "skinEdits" && key != "colliderAsset" && key != "damageAsset" && key != "bottomColliderAsset")
            addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", key, "project field is not modeled");
    ProjectState parsed = session.state();
    parsed.colliderAsset = parseSecondaryAssetIdentity(root, "colliderAsset", !parsed.colliders.empty(), diagnostics);
    parsed.damageAsset = parseSecondaryAssetIdentity(root, "damageAsset", !parsed.damage.empty(), diagnostics);
    parsed.bottomColliderAsset = parseSecondaryAssetIdentity(root, "bottomColliderAsset", !parsed.bottomColliders.empty(), diagnostics);
    parsed.revision = revision;
    return {std::move(parsed), std::move(diagnostics)};
}

std::string serializeProjectInternal(const ProjectState& project, ProjectIoLimits limits) {
    validateDirectProject(project, limits);
    auto source = normalizeSourceIdentity(project.source);
    ProjectState normalized = project;
    normalized.source = std::move(source);
    if (normalized.colliders.empty()) normalized.colliderAsset.reset();
    else if (normalized.colliderAsset) normalized.colliderAsset = normalizeSecondaryAssetIdentity(std::move(*normalized.colliderAsset));
    if (normalized.damage.empty()) normalized.damageAsset.reset();
    else if (normalized.damageAsset) normalized.damageAsset = normalizeSecondaryAssetIdentity(std::move(*normalized.damageAsset));
    if (normalized.bottomColliders.empty()) normalized.bottomColliderAsset.reset();
    else if (normalized.bottomColliderAsset) normalized.bottomColliderAsset = normalizeSecondaryAssetIdentity(std::move(*normalized.bottomColliderAsset));
    JsonWriter writer(limits.maxOutputBytes);
    writeProjectMap(writer, normalized, 0);
    writer.raw("\n");
    return writer.output();
}

ProjectIoResult parseProjectInternalNoExpected(std::string_view text, ProjectIoLimits limits) {
    return parseProjectInternal(text, nullptr, limits);
}

ProjectIoResult parseProjectInternalExpected(std::string_view text, const SourceIdentity& expectedSource,
                                             ProjectIoLimits limits) {
    return parseProjectInternal(text, &expectedSource, limits);
}

[[nodiscard]] std::string cspQuote(std::string_view value) {
    const bool quote = value.find_first_of(",; \t\r\n") != std::string_view::npos;
    if (!quote) return std::string(value);
    std::string output = "\"";
    for (const auto character : value) output.push_back(character == '"' ? '\'' : character);
    output.push_back('"');
    return output;
}

std::string serializeEditorCspInternal(const ProjectState& project, ProjectIoLimits limits,
                                       std::vector<ProjectIoDiagnostic>* diagnostics) {
    validateDirectProject(project, limits);
    std::vector<ProjectIoDiagnostic> local;
    auto& messages = diagnostics == nullptr ? local : *diagnostics;
    for (const auto& [key, value] : project.nodes) { (void)value; addDiagnostic(messages, "UNSUPPORTED_FIELD", "nodeEdits." + key, "node edits have no CSP export in this slice"); }
    for (const auto& [key, value] : project.geometry) { (void)value; addDiagnostic(messages, "UNSUPPORTED_FIELD", "geometryEdits." + key, "geometry edits are baked into KN5 and have no CSP export"); }
    if (!project.workspaceFiles.empty() || project.workspace.cockpitHrDistance || project.workspace.driverHrDistance) addDiagnostic(messages, "UNSUPPORTED_FIELD", "workspaceEdits", "workspace edits have no CSP export in this slice");
    if (!project.surfaces.empty()) addDiagnostic(messages, "UNSUPPORTED_FIELD", "surfaceEdits", "surface edits have no CSP export in this slice");
    if (!project.colliders.empty() || !project.bottomColliders.empty()) addDiagnostic(messages, "UNSUPPORTED_FIELD", "colliderEdits", "collider edits have no CSP export in this slice");
    if (!project.damage.empty()) addDiagnostic(messages, "UNSUPPORTED_FIELD", "damageEdits", "damage edits have no CSP export in this slice");
    std::string output;
    appendBounded(output, "; Generated by Apex Editor. Source KN5 is not modified.\n", limits.maxOutputBytes);
    if (!project.materials.empty() || !project.meshes.empty()) appendBounded(output, "\n", limits.maxOutputBytes);
    std::size_t sectionIndex = 0;
    for (const auto& [material, edit] : project.materials) {
        std::vector<std::string> lines;
        std::ostringstream sectionName; sectionName << "[SHADER_REPLACEMENT_APEX_EDITOR_" << std::setw(3) << std::setfill('0') << sectionIndex++ << "]";
        lines.push_back(sectionName.str()); lines.push_back("MATERIALS = " + cspQuote(material));
        const std::array<std::string_view, 4> scalarFields = {"shader", "blendMode", "depthMode", "cullMode"};
        for (const auto field : scalarFields) {
            const auto found = edit.scalars.find(std::string(field));
            if (found == edit.scalars.end()) continue;
            const auto& value = found->second;
            const auto* text = std::get_if<std::string>(&value);
            if (text == nullptr || text->empty()) { addDiagnostic(messages, "UNSUPPORTED_FIELD", "materialEdits." + material + "." + std::string(field), "CSP scalar fields require non-empty strings"); continue; }
            std::string key(field); std::transform(key.begin(), key.end(), key.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            lines.push_back(key + " = " + *text);
        }
        std::vector<std::pair<std::string, std::string>> properties;
        for (const auto& [name, value] : edit.scalars) if (name != "shader" && name != "blendMode" && name != "depthMode" && name != "cullMode") {
            properties.emplace_back(name, std::get_if<std::string>(&value) ? *std::get_if<std::string>(&value) : std::get_if<float>(&value) ? formatEditorNumber(*std::get_if<float>(&value)) : std::get<bool>(value) ? "1" : "0");
        }
        for (const auto& [name, value] : edit.vectors) {
            std::string values;
            for (std::size_t i = 0; i < value.components; ++i) { if (i != 0) values += ", "; values += formatEditorNumber(value.values[i]); }
            properties.emplace_back(name, std::move(values));
        }
        std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
        for (std::size_t propertyIndex = 0; propertyIndex < properties.size(); ++propertyIndex)
            lines.push_back("PROP_" + std::to_string(propertyIndex) + " = " + properties[propertyIndex].first + ", " + properties[propertyIndex].second);
        std::size_t resourceIndex = 0;
        for (const auto& [slot, resource] : edit.resources) {
            lines.push_back("RESOURCE_" + std::to_string(resourceIndex) + " = " + slot);
            if (resource.texture) lines.push_back("RESOURCE_TEXTURE_" + std::to_string(resourceIndex) + " = " + *resource.texture);
            else if (resource.file) lines.push_back("RESOURCE_FILE_" + std::to_string(resourceIndex) + " = " + *resource.file);
            else if (resource.color) { std::string values; for (std::size_t i = 0; i < 4; ++i) { if (i != 0) values += ", "; values += formatEditorNumber((*resource.color)[i]); } lines.push_back("RESOURCE_COLOR_" + std::to_string(resourceIndex) + " = " + values); }
            ++resourceIndex;
        }
        if (sectionIndex > 1) appendBounded(output, "\n", limits.maxOutputBytes);
        for (const auto& line : lines) { appendBounded(output, line, limits.maxOutputBytes); appendBounded(output, "\n", limits.maxOutputBytes); }
    }
    std::size_t meshIndex = 0;
    for (const auto& [mesh, edit] : project.meshes) {
        if (sectionIndex > 0 || meshIndex > 0) appendBounded(output, "\n", limits.maxOutputBytes);
        std::ostringstream sectionName; sectionName << "[MESH_ADJUSTMENT_APEX_EDITOR_" << std::setw(3) << std::setfill('0') << meshIndex++ << "]";
        const auto line = [&](std::string value) { appendBounded(output, value, limits.maxOutputBytes); appendBounded(output, "\n", limits.maxOutputBytes); };
        line(sectionName.str());
        line("MESHES = " + cspQuote(mesh));
        if (edit.transparent) line(std::string("IS_TRANSPARENT = ") + (*edit.transparent ? "1" : "0"));
        if (edit.layer) line("LAYER = " + std::to_string(*edit.layer));
        if (edit.lodIn) line("LOD_IN = " + formatEditorNumber(*edit.lodIn));
        if (edit.lodOut) line("LOD_OUT = " + formatEditorNumber(*edit.lodOut));
        if (edit.castShadows) line(std::string("CAST_SHADOWS = ") + (*edit.castShadows ? "1" : "0"));
    }
    return output;
}

void appendOperation(ProjectSession& session, EditOperation operation, std::string_view path,
                     std::vector<ProjectIoDiagnostic>& diagnostics) {
    try {
        session.commit({"project import", {std::move(operation)}});
    } catch (const AuthoringError& errorValue) {
        addDiagnostic(diagnostics, errorValue.code(), path, errorValue.what());
    }
}

void parseNodeEdits(const JsonValue& value, ProjectSession& session,
                    std::vector<ProjectIoDiagnostic>& diagnostics,
                    const ProjectIoLimits& limits) {
    if (!isObject(&value)) {
        addDiagnostic(diagnostics, "FIELD_TYPE", "nodeEdits", "nodeEdits must be an object");
        return;
    }
    for (const auto& [path, raw] : value.object) {
        if (!isObject(&raw)) {
            addDiagnostic(diagnostics, "FIELD_TYPE", "nodeEdits." + path, "node edit must be an object");
            continue;
        }
        NodeEdit edit;
        bool any = false;
        for (const auto& [key, field] : raw.object) {
            if (key == "name") {
                if (field.kind == JsonValue::Kind::string) { edit.name = field.string; any = true; }
                else addDiagnostic(diagnostics, "FIELD_TYPE", "nodeEdits." + path + ".name", "name must be a string");
            } else if (key == "active") {
                if (field.kind == JsonValue::Kind::boolean) { edit.active = field.boolean; any = true; }
                else addDiagnostic(diagnostics, "FIELD_TYPE", "nodeEdits." + path + ".active", "active must be boolean");
            } else if (key == "transform") {
                Matrix4 transform{};
                if (field.kind == JsonValue::Kind::array && field.array.size() == 16) {
                    bool valid = true;
                    for (std::size_t index = 0; index < transform.size(); ++index) {
                        valid = field.array[index].kind == JsonValue::Kind::number &&
                                std::isfinite(static_cast<float>(field.array[index].number)) && valid;
                        if (valid) transform[index] = static_cast<float>(field.array[index].number);
                    }
                    if (valid) { edit.transform = transform; any = true; }
                    else addDiagnostic(diagnostics, "FIELD_VALUE", "nodeEdits." + path + ".transform", "transform must contain finite float values");
                } else addDiagnostic(diagnostics, "FIELD_TYPE", "nodeEdits." + path + ".transform", "transform must contain 16 numbers");
            } else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", "nodeEdits." + path + "." + key, "node field is not modeled");
        }
        if (any && session.state().nodes.size() < limits.maxEdits)
            appendOperation(session, SetNodeEdit{path, std::move(edit)}, "nodeEdits." + path, diagnostics);
        else if (any) addDiagnostic(diagnostics, "EDIT_LIMIT", "nodeEdits", "project edit limit exceeded");
    }
}

void parseMeshEdits(const JsonValue& value, ProjectSession& session,
                    std::vector<ProjectIoDiagnostic>& diagnostics, const ProjectIoLimits& limits) {
    if (!isObject(&value)) {
        addDiagnostic(diagnostics, "FIELD_TYPE", "meshEdits", "meshEdits must be an object");
        return;
    }
    for (const auto& [name, raw] : value.object) {
        const auto editPath = "meshEdits." + name;
        if (!isObject(&raw)) {
            addDiagnostic(diagnostics, "FIELD_TYPE", editPath, "mesh edit must be an object");
            continue;
        }
        MeshEdit edit;
        bool any = false;
        for (const auto& [key, field] : raw.object) {
            const auto path = editPath + "." + key;
            if (key == "isTransparent" || key == "castShadows") {
                if (field.kind != JsonValue::Kind::boolean) {
                    addDiagnostic(diagnostics, "FIELD_TYPE", path, "mesh flag must be boolean");
                    continue;
                }
                if (key == "isTransparent")
                    edit.transparent = field.boolean;
                else
                    edit.castShadows = field.boolean;
                any = true;
            } else if (key == "layer") {
                if (field.kind != JsonValue::Kind::number || field.number < 0.0 ||
                    std::trunc(field.number) != field.number ||
                    static_cast<long double>(field.number) >
                        static_cast<long double>(std::numeric_limits<std::uint32_t>::max())) {
                    addDiagnostic(diagnostics, "FIELD_VALUE", path,
                                  "mesh layer must be an unsigned 32-bit integer");
                    continue;
                }
                edit.layer = static_cast<std::uint32_t>(field.number);
                any = true;
            } else if (key == "lodIn" || key == "lodOut") {
                if (field.kind != JsonValue::Kind::number ||
                    !std::isfinite(static_cast<float>(field.number))) {
                    addDiagnostic(diagnostics, "FIELD_VALUE", path,
                                  "mesh LOD must be a finite float");
                    continue;
                }
                if (key == "lodIn")
                    edit.lodIn = static_cast<float>(field.number);
                else
                    edit.lodOut = static_cast<float>(field.number);
                any = true;
            } else {
                addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "mesh field is not modeled");
            }
        }
        if (any && session.state().meshes.size() < limits.maxEdits)
            appendOperation(session, SetMeshEdit{name, std::move(edit)}, editPath, diagnostics);
        else if (any)
            addDiagnostic(diagnostics, "EDIT_LIMIT", "meshEdits", "project edit limit exceeded");
        else
            addDiagnostic(diagnostics, "EDIT_EMPTY", editPath,
                          "mesh edit has no valid modeled fields");
    }
}

void parseGeometryEdits(const JsonValue& value, ProjectSession& session,
                        std::vector<ProjectIoDiagnostic>& diagnostics,
                        const ProjectIoLimits& limits) {
    if (!isObject(&value)) {
        addDiagnostic(diagnostics, "FIELD_TYPE", "geometryEdits",
                      "geometryEdits must be an object");
        return;
    }
    for (const auto& [nodePath, raw] : value.object) {
        const auto editPath = "geometryEdits." + nodePath;
        if (!isObject(&raw)) {
            addDiagnostic(diagnostics, "FIELD_TYPE", editPath, "geometry edit must be an object");
            continue;
        }
        GeometryEdit edit;
        bool any = false;
        for (const auto& [key, field] : raw.object) {
            const auto path = editPath + "." + key;
            if (key == "transform") {
                Matrix4 transform{};
                if (field.kind != JsonValue::Kind::array ||
                    field.array.size() != transform.size()) {
                    addDiagnostic(diagnostics, "FIELD_TYPE", path,
                                  "geometry transform must contain 16 numbers");
                    continue;
                }
                bool valid = true;
                for (std::size_t index = 0; index < transform.size(); ++index) {
                    if (field.array[index].kind != JsonValue::Kind::number ||
                        !std::isfinite(static_cast<float>(field.array[index].number))) {
                        valid = false;
                        break;
                    }
                    transform[index] = static_cast<float>(field.array[index].number);
                }
                if (!valid) {
                    addDiagnostic(diagnostics, "FIELD_VALUE", path,
                                  "geometry transform must contain finite float values");
                    continue;
                }
                edit.transform = transform;
                any = true;
            } else if (key == "removeDegenerate" || key == "reverseWinding" ||
                       key == "recalculateNormals") {
                if (field.kind != JsonValue::Kind::boolean) {
                    addDiagnostic(diagnostics, "FIELD_TYPE", path, "geometry flag must be boolean");
                    continue;
                }
                if (!field.boolean)
                    continue;
                if (key == "removeDegenerate")
                    edit.remove_degenerate = true;
                else if (key == "reverseWinding")
                    edit.reverse_winding = true;
                else
                    edit.recalculate_normals = true;
                any = true;
            } else {
                addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path,
                              "geometry field is not modeled");
            }
        }
        if (any && session.state().geometry.size() < limits.maxEdits)
            appendOperation(session, SetGeometryEdit{nodePath, std::move(edit)}, editPath,
                            diagnostics);
        else if (any)
            addDiagnostic(diagnostics, "EDIT_LIMIT", "geometryEdits",
                          "project edit limit exceeded");
        else
            addDiagnostic(diagnostics, "EDIT_EMPTY", editPath,
                          "geometry edit has no valid modeled fields");
    }
}

void parseWorkspaceEdits(const JsonValue& value, ProjectSession& session,
                         std::vector<ProjectIoDiagnostic>& diagnostics,
                         const ProjectIoLimits& limits) {
    if (!isObject(&value)) {
        addDiagnostic(diagnostics, "FIELD_TYPE", "workspaceEdits", "workspaceEdits must be an object");
        return;
    }
    if (const auto files = member(value, "files"); files != nullptr) {
        if (!isObject(files)) addDiagnostic(diagnostics, "FIELD_TYPE", "workspaceEdits.files", "files must be an object");
        else for (const auto& [key, raw] : files->object) {
            std::uint32_t index = 0;
            if (!indexKey(key, index)) { addDiagnostic(diagnostics, "UNSAFE_PATH", "workspaceEdits.files." + key, "file index is invalid"); continue; }
            if (!isObject(&raw)) { addDiagnostic(diagnostics, "FIELD_TYPE", "workspaceEdits.files." + key, "workspace file edit must be an object"); continue; }
            WorkspaceFileEdit edit;
            bool any = false;
            for (const auto& [fieldName, field] : raw.object) {
                const auto path = "workspaceEdits.files." + key + "." + fieldName;
                if (fieldName == "name") { if (field.kind == JsonValue::Kind::string) { edit.name = field.string; any = true; } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "name must be a string"); }
                else if (fieldName == "position" || fieldName == "rotation" || fieldName == "positionCenter" || fieldName == "positionRange" || fieldName == "velocityBase" || fieldName == "velocityRange") {
                    Vector3 vector{};
                    if (field.kind == JsonValue::Kind::array && field.array.size() == 3) {
                        bool valid = true;
                        for (std::size_t i = 0; i < 3; ++i) { valid = field.array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.array[i].number)) && valid; if (valid) vector[i] = static_cast<float>(field.array[i].number); }
                        if (valid) { if (fieldName == "position") edit.position = vector; else if (fieldName == "rotation") edit.rotation = vector; else if (fieldName == "positionCenter") edit.positionCenter = vector; else if (fieldName == "positionRange") edit.positionRange = vector; else if (fieldName == "velocityBase") edit.velocityBase = vector; else edit.velocityRange = vector; any = true; }
                        else addDiagnostic(diagnostics, "FIELD_VALUE", path, "vector components must be finite floats");
                    } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "field must contain three numbers");
                } else if (fieldName == "multiplicity") {
                    std::array<float, 2> vector{};
                    if (field.kind == JsonValue::Kind::array && field.array.size() == 2) {
                        bool valid = true; for (std::size_t i = 0; i < 2; ++i) { valid = field.array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.array[i].number)) && valid; if (valid) vector[i] = static_cast<float>(field.array[i].number); }
                        if (valid) { edit.multiplicity = vector; any = true; } else addDiagnostic(diagnostics, "FIELD_VALUE", path, "multiplicity must be finite floats");
                    } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "multiplicity must contain two numbers");
                } else if (fieldName == "lodIn" || fieldName == "lodOut" || fieldName == "probability") {
                    if (field.kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.number))) { if (fieldName == "lodIn") edit.lodIn = static_cast<float>(field.number); else if (fieldName == "lodOut") edit.lodOut = static_cast<float>(field.number); else edit.probability = static_cast<float>(field.number); any = true; } else addDiagnostic(diagnostics, "FIELD_VALUE", path, "field must be a finite float");
                } else if (fieldName == "posMode" || fieldName == "velMode" || fieldName == "playWav") {
                    if (field.kind == JsonValue::Kind::string) { if (fieldName == "posMode") edit.posMode = field.string; else if (fieldName == "velMode") edit.velMode = field.string; else edit.playWav = field.string; any = true; }
                    else if (fieldName == "playWav" && field.kind == JsonValue::Kind::nullValue) addDiagnostic(diagnostics, "UNSUPPORTED_NULL", path, "null playWav is not representable by the native optional string model");
                    else addDiagnostic(diagnostics, "FIELD_TYPE", path, "field must be a string");
                } else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "workspace field is not modeled");
            }
            if (any) appendOperation(session, SetWorkspaceFileEdit{index, std::move(edit)}, "workspaceEdits.files." + key, diagnostics);
            else if (session.state().workspaceFiles.size() >= limits.maxEdits) addDiagnostic(diagnostics, "EDIT_LIMIT", "workspaceEdits", "project edit limit exceeded");
        }
    }
    WorkspaceSettingsEdit settings;
    bool anySettings = false;
    if (const auto field = member(value, "cockpitHrDistance"); field != nullptr) { if (field->kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field->number))) { settings.cockpitHrDistance = static_cast<float>(field->number); anySettings = true; } else addDiagnostic(diagnostics, "FIELD_VALUE", "workspaceEdits.cockpitHrDistance", "distance must be finite"); }
    if (const auto field = member(value, "driverHrDistance"); field != nullptr) { if (field->kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field->number))) { settings.driverHrDistance = static_cast<float>(field->number); anySettings = true; } else addDiagnostic(diagnostics, "FIELD_VALUE", "workspaceEdits.driverHrDistance", "distance must be finite"); }
    for (const auto& [key, field] : value.object) if (key != "files" && key != "cockpitHrDistance" && key != "driverHrDistance") addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", "workspaceEdits." + key, "workspace field is not modeled");
    if (anySettings) appendOperation(session, SetWorkspaceSettingsEdit{settings}, "workspaceEdits", diagnostics);
}

void parseMaterialEdits(const JsonValue& value, ProjectSession& session,
                        std::vector<ProjectIoDiagnostic>& diagnostics,
                        const ProjectIoLimits& limits) {
    if (!isObject(&value)) { addDiagnostic(diagnostics, "FIELD_TYPE", "materialEdits", "materialEdits must be an object"); return; }
    for (const auto& [material, raw] : value.object) {
        if (!isObject(&raw)) { addDiagnostic(diagnostics, "FIELD_TYPE", "materialEdits." + material, "material edit must be an object"); continue; }
        for (const auto& [fieldName, field] : raw.object) {
            const auto path = "materialEdits." + material + "." + fieldName;
            if (fieldName == "properties") {
                if (!isObject(&field)) { addDiagnostic(diagnostics, "FIELD_TYPE", path, "properties must be an object"); continue; }
                for (const auto& [property, propertyValue] : field.object) {
                    if (propertyValue.kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(propertyValue.number))) appendOperation(session, SetMaterialScalarEdit{material, property, static_cast<float>(propertyValue.number)}, path + "." + property, diagnostics);
                    else if (propertyValue.kind == JsonValue::Kind::string) appendOperation(session, SetMaterialScalarEdit{material, property, propertyValue.string}, path + "." + property, diagnostics);
                    else if (propertyValue.kind == JsonValue::Kind::array && propertyValue.array.size() >= 2 && propertyValue.array.size() <= 4) {
                        MaterialVector vector; vector.components = static_cast<std::uint8_t>(propertyValue.array.size()); bool valid = true; for (std::size_t i = 0; i < propertyValue.array.size(); ++i) { valid = propertyValue.array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(propertyValue.array[i].number)) && valid; if (valid) vector.values[i] = static_cast<float>(propertyValue.array[i].number); } if (valid) appendOperation(session, SetMaterialVectorEdit{material, property, vector}, path + "." + property, diagnostics); else addDiagnostic(diagnostics, "FIELD_VALUE", path + "." + property, "property vector must be finite floats");
                    } else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path + "." + property, "property value is not modeled");
                }
            } else if (fieldName == "resources") {
                if (!isObject(&field)) { addDiagnostic(diagnostics, "FIELD_TYPE", path, "resources must be an object"); continue; }
                for (const auto& [slot, resourceValue] : field.object) {
                    if (!isObject(&resourceValue)) { addDiagnostic(diagnostics, "FIELD_TYPE", path + "." + slot, "resource must be an object"); continue; }
                    MaterialResource resource; bool any = false;
                    if (const auto item = member(resourceValue, "texture"); item != nullptr) { if (item->kind == JsonValue::Kind::string) { resource.texture = item->string; any = true; } else addDiagnostic(diagnostics, "FIELD_TYPE", path + "." + slot + ".texture", "texture must be a string"); }
                    if (const auto item = member(resourceValue, "file"); item != nullptr) { if (item->kind == JsonValue::Kind::string) { resource.file = item->string; any = true; } else addDiagnostic(diagnostics, "FIELD_TYPE", path + "." + slot + ".file", "file must be a string"); }
                    if (const auto item = member(resourceValue, "color"); item != nullptr) { if (item->kind == JsonValue::Kind::array && item->array.size() == 4) { std::array<float, 4> color{}; bool valid = true; for (std::size_t i = 0; i < 4; ++i) { valid = item->array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(item->array[i].number)) && valid; if (valid) color[i] = static_cast<float>(item->array[i].number); } if (valid) { resource.color = color; any = true; } } else addDiagnostic(diagnostics, "FIELD_TYPE", path + "." + slot + ".color", "color must contain four finite numbers"); }
                    for (const auto& [resourceKey, ignored] : resourceValue.object) if (resourceKey != "texture" && resourceKey != "file" && resourceKey != "color") addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path + "." + slot + "." + resourceKey, "resource field is not modeled");
                    if (any) appendOperation(session, SetMaterialResourceEdit{material, slot, std::move(resource)}, path + "." + slot, diagnostics);
                }
            } else if (fieldName == "shader" || fieldName == "blendMode" || fieldName == "depthMode" || fieldName == "cullMode") {
                if (field.kind == JsonValue::Kind::string) appendOperation(session, SetMaterialScalarEdit{material, fieldName, field.string}, path, diagnostics);
                else addDiagnostic(diagnostics, "FIELD_TYPE", path, "material state field must be a string");
            } else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "material field is not modeled");
        }
    }
    (void)limits;
}

void parseSurfaceEdits(const JsonValue& value, ProjectSession& session,
                       std::vector<ProjectIoDiagnostic>& diagnostics) {
    if (!isObject(&value)) { addDiagnostic(diagnostics, "FIELD_TYPE", "surfaceEdits", "surfaceEdits must be an object"); return; }
    for (const auto& [key, raw] : value.object) {
        std::uint32_t index = 0;
        if (!indexKey(key, index) || !isObject(&raw)) { addDiagnostic(diagnostics, "FIELD_TYPE", "surfaceEdits." + key, "surface edit index or object is invalid"); continue; }
        SurfaceEdit edit;
        bool any = false;
        for (const auto& [fieldName, field] : raw.object) {
            const auto path = "surfaceEdits." + key + "." + fieldName;
            if (fieldName == "key" || fieldName == "wav" || fieldName == "ffEffect") { if (field.kind == JsonValue::Kind::string) { if (fieldName == "key") edit.key = field.string; else if (fieldName == "wav") edit.wav = field.string; else edit.ffEffect = field.string; any = true; } else if (field.kind == JsonValue::Kind::nullValue && fieldName != "key") { addDiagnostic(diagnostics, "UNSUPPORTED_NULL", path, "null surface text is not representable by the native optional string model"); } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "surface text field is invalid"); }
            else if (fieldName == "isValidTrack" || fieldName == "isPitlane") { if (field.kind == JsonValue::Kind::boolean) { if (fieldName == "isValidTrack") edit.isValidTrack = field.boolean; else edit.isPitlane = field.boolean; any = true; } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "surface flag must be boolean"); }
            else if (fieldName == "friction" || fieldName == "damping" || fieldName == "dirtAdditive" || fieldName == "blackFlagTime" || fieldName == "sinHeight" || fieldName == "sinLength" || fieldName == "vibrationGain" || fieldName == "vibrationLength" || fieldName == "wavPitch") { if (field.kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.number))) { const auto number = static_cast<float>(field.number); if (fieldName == "friction") edit.friction = number; else if (fieldName == "damping") edit.damping = number; else if (fieldName == "dirtAdditive") edit.dirtAdditive = number; else if (fieldName == "blackFlagTime") edit.blackFlagTime = number; else if (fieldName == "sinHeight") edit.sinHeight = number; else if (fieldName == "sinLength") edit.sinLength = number; else if (fieldName == "vibrationGain") edit.vibrationGain = number; else if (fieldName == "vibrationLength") edit.vibrationLength = number; else edit.wavPitch = number; any = true; } else addDiagnostic(diagnostics, "FIELD_VALUE", path, "surface number must be finite"); }
            else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "surface field is not modeled");
        }
        if (any) appendOperation(session, SetSurfaceEdit{index, std::move(edit)}, "surfaceEdits." + key, diagnostics);
        else addDiagnostic(diagnostics, "EDIT_EMPTY", "surfaceEdits." + key, "surface edit has no valid modeled fields");
    }
}

void parseColliderEdits(const JsonValue& value, ProjectSession& session,
                        std::vector<ProjectIoDiagnostic>& diagnostics, bool bottom) {
    const auto prefix = bottom ? "bottomColliderEdits" : "colliderEdits";
    if (!isObject(&value)) { addDiagnostic(diagnostics, "FIELD_TYPE", prefix, "collider edits must be an object"); return; }
    for (const auto& [key, raw] : value.object) {
        std::uint32_t index = 0;
        if ((bottom && !indexKey(key, index)) || !isObject(&raw)) { addDiagnostic(diagnostics, "FIELD_TYPE", std::string(prefix) + "." + key, bottom ? "collider index or object is invalid" : "collider path or object is invalid"); continue; }
        if (bottom) {
            BottomColliderEdit edit;
            for (const auto& [fieldName, field] : raw.object) {
                const auto path = std::string(prefix) + "." + key + "." + fieldName;
                if (fieldName == "centre" || fieldName == "size") { Vector3 vector{}; if (field.kind == JsonValue::Kind::array && field.array.size() == 3) { bool valid = true; for (std::size_t i = 0; i < 3; ++i) { valid = field.array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.array[i].number)) && valid; if (valid) vector[i] = static_cast<float>(field.array[i].number); } if (valid) { if (fieldName == "centre") edit.centre = vector; else edit.size = vector; } else addDiagnostic(diagnostics, "FIELD_VALUE", path, "collider vector must be finite"); } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "collider vector must contain three numbers"); }
                else if (fieldName == "groundEnabled") { if (field.kind == JsonValue::Kind::boolean) edit.groundEnabled = field.boolean; else addDiagnostic(diagnostics, "FIELD_TYPE", path, "groundEnabled must be boolean"); }
                else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "bottom collider field is not modeled");
            }
            appendOperation(session, SetBottomColliderEdit{index, std::move(edit)}, std::string(prefix) + "." + key, diagnostics);
        } else {
            ColliderEdit edit;
            for (const auto& [fieldName, field] : raw.object) {
                const auto path = std::string(prefix) + "." + key + "." + fieldName;
                if (fieldName == "transform") { Matrix4 matrix{}; if (field.kind == JsonValue::Kind::array && field.array.size() == 16) { bool valid = true; for (std::size_t i = 0; i < 16; ++i) { valid = field.array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.array[i].number)) && valid; if (valid) matrix[i] = static_cast<float>(field.array[i].number); } if (valid) edit.transform = matrix; else addDiagnostic(diagnostics, "FIELD_VALUE", path, "transform must be finite"); } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "transform must contain 16 numbers"); }
                else if (fieldName == "removeDegenerate" || fieldName == "reverseWinding" || fieldName == "recalculateNormals") { if (field.kind == JsonValue::Kind::boolean) { if (fieldName == "removeDegenerate") edit.removeDegenerate = field.boolean; else if (fieldName == "reverseWinding") edit.reverseWinding = field.boolean; else edit.recalculateNormals = field.boolean; } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "collider flag must be boolean"); }
                else addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "collider field is not modeled");
            }
            appendOperation(session, SetColliderEdit{key, std::move(edit)}, std::string(prefix) + "." + key, diagnostics);
        }
    }
}

void parseDamageEdits(const JsonValue& value, ProjectSession& session,
                      std::vector<ProjectIoDiagnostic>& diagnostics) {
    if (!isObject(&value)) { addDiagnostic(diagnostics, "FIELD_TYPE", "damageEdits", "damageEdits must be an object"); return; }
    for (const auto& [section, raw] : value.object) {
        if (!isObject(&raw)) { addDiagnostic(diagnostics, "FIELD_TYPE", "damageEdits." + section, "damage edit must be an object"); continue; }
        DamageEdit edit;
        bool any = false;
        for (const auto& [fieldName, field] : raw.object) {
            const auto path = "damageEdits." + section + "." + fieldName;
            if (!damageFieldAllowed(section, fieldName)) {
                addDiagnostic(diagnostics, "UNSUPPORTED_FIELD", path, "damage field is not valid for its section");
                continue;
            }
            const auto number = [&](std::optional<float>& destination) { if (field.kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.number))) destination = static_cast<float>(field.number); else addDiagnostic(diagnostics, "FIELD_VALUE", path, "damage number must be finite"); };
            const auto vector = [&](std::optional<Vector3>& destination) { if (field.kind == JsonValue::Kind::array && field.array.size() == 3) { Vector3 components{}; bool valid = true; for (std::size_t i = 0; i < 3; ++i) { valid = field.array[i].kind == JsonValue::Kind::number && std::isfinite(static_cast<float>(field.array[i].number)) && valid; if (valid) components[i] = static_cast<float>(field.array[i].number); } if (valid) { destination = components; any = true; } else addDiagnostic(diagnostics, "FIELD_VALUE", path, "damage vector must be finite"); } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "damage vector must contain three numbers"); };
            if (fieldName == "minSpeed") { number(edit.minSpeed); any = any || edit.minSpeed.has_value(); }
            else if (fieldName == "maxSpeed") { number(edit.maxSpeed); any = any || edit.maxSpeed.has_value(); }
            else if (fieldName == "initialLevel") { number(edit.initialLevel); any = any || edit.initialLevel.has_value(); }
            else if (fieldName == "staticRotationAngle") { number(edit.staticRotationAngle); any = any || edit.staticRotationAngle.has_value(); }
            else if (fieldName == "multG") { number(edit.multG); any = any || edit.multG.has_value(); }
            else if (fieldName == "fullSpeed") { number(edit.fullSpeed); any = any || edit.fullSpeed.has_value(); }
            else if (fieldName == "oscillationMinAngle") { number(edit.oscillationMinAngle); any = any || edit.oscillationMinAngle.has_value(); }
            else if (fieldName == "oscillationMaxAngle") { number(edit.oscillationMaxAngle); any = any || edit.oscillationMaxAngle.has_value(); }
            else if (fieldName == "staticRotationAxis") vector(edit.staticRotationAxis);
            else if (fieldName == "oscillationAxis") vector(edit.oscillationAxis);
            else if (fieldName == "allowedG") vector(edit.allowedG);
            else if (fieldName == "enabled") { if (field.kind == JsonValue::Kind::boolean) { edit.enabled = field.boolean; any = true; } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "damage enabled must be boolean"); }
            else if (fieldName == "name") {
                if (field.kind != JsonValue::Kind::string) addDiagnostic(diagnostics, "FIELD_TYPE", path, "damage name must be a string");
                else if (field.string.empty() || field.string.size() > 1024 || field.string.find_first_of("\r\n;") != std::string::npos)
                    addDiagnostic(diagnostics, "FIELD_VALUE", path, "damage name contains unsafe characters or exceeds its limit");
                else { edit.name = field.string; any = true; }
            }
            else if (fieldName == "damageZone") { if (field.kind == JsonValue::Kind::string) { edit.damageZone = field.string; any = true; } else addDiagnostic(diagnostics, "FIELD_TYPE", path, "damage zone must be a string"); }
        }
        if (any) appendOperation(session, SetDamageEdit{section, std::move(edit)}, "damageEdits." + section, diagnostics);
        else addDiagnostic(diagnostics, "EDIT_EMPTY", "damageEdits." + section, "damage edit has no valid modeled fields");
    }
}

void writeMaterial(JsonWriter& writer, const MaterialEdit& edit, std::size_t depth) {
    writer.raw("{");
    bool first = true;
    const auto emit = [&](std::string_view key, const auto& value) {
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); value(); first = false;
    };
    for (const auto& [key, value] : edit.scalars) {
        if (key == "shader" || key == "blendMode" || key == "depthMode" || key == "cullMode") {
            emit(key, [&] {
                std::visit([&](const auto& item) { using T = std::decay_t<decltype(item)>; if constexpr (std::is_same_v<T, std::string>) writer.string(item); else if constexpr (std::is_same_v<T, bool>) writer.raw(item ? "true" : "false"); else writer.raw(formatNumber(item)); }, value);
            });
        }
    }
    if (!edit.vectors.empty() || !edit.scalars.empty()) {
        bool propertyFirst = true;
        emit("properties", [&] {
            writer.raw("{");
            for (const auto& [key, value] : edit.scalars) {
                if (key == "shader" || key == "blendMode" || key == "depthMode" || key == "cullMode") continue;
                if (!propertyFirst) writer.raw(",");
                writer.raw("\n"); writer.indent(depth + 2); writer.string(key); writer.raw(": ");
                std::visit([&](const auto& item) { using T = std::decay_t<decltype(item)>; if constexpr (std::is_same_v<T, std::string>) writer.string(item); else if constexpr (std::is_same_v<T, bool>) writer.raw(item ? "true" : "false"); else writer.raw(formatNumber(item)); }, value);
                propertyFirst = false;
            }
            for (const auto& [key, value] : edit.vectors) {
                if (!propertyFirst) writer.raw(",");
                writer.raw("\n"); writer.indent(depth + 2); writer.string(key); writer.raw(": [");
                for (std::size_t i = 0; i < value.components; ++i) { if (i != 0) writer.raw(", "); writer.raw(formatNumber(value.values[i])); }
                writer.raw("]"); propertyFirst = false;
            }
            if (!propertyFirst) { writer.raw("\n"); writer.indent(depth + 1); }
            writer.raw("}");
        });
    }
    if (!edit.resources.empty()) {
        emit("resources", [&] {
            writer.raw("{"); bool resourceFirst = true;
            for (const auto& [slot, resource] : edit.resources) {
                if (!resourceFirst) writer.raw(",");
                writer.raw("\n"); writer.indent(depth + 2); writer.string(slot); writer.raw(": {");
                bool valueFirst = true;
                const auto resourceText = [&](std::string_view key, const std::string& value) { if (!valueFirst) writer.raw(", "); writer.string(key); writer.raw(": "); writer.string(value); valueFirst = false; };
                if (resource.texture) resourceText("texture", *resource.texture);
                if (resource.file) resourceText("file", *resource.file);
                if (resource.color) { if (!valueFirst) writer.raw(", "); writer.string("color"); writer.raw(": ["); for (std::size_t i = 0; i < 4; ++i) { if (i != 0) writer.raw(", "); writer.raw(formatNumber((*resource.color)[i])); } writer.raw("]"); valueFirst = false; }
                writer.raw("}"); resourceFirst = false;
            }
            if (!resourceFirst) { writer.raw("\n"); writer.indent(depth + 1); }
            writer.raw("}");
        });
    }
    if (!first) { writer.raw("\n"); writer.indent(depth); }
    writer.raw("}");
}

template <typename EditWriter>
void writeIndexedMap(JsonWriter& writer, const auto& map, std::size_t depth, EditWriter writeEdit) {
    writer.raw("{"); bool first = true;
    for (const auto& [key, edit] : map) {
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(std::to_string(key)); writer.raw(": "); writeEdit(writer, edit, depth + 1); first = false;
    }
    if (!first) { writer.raw("\n"); writer.indent(depth); }
    writer.raw("}");
}

template <typename EditWriter>
void writePathMap(JsonWriter& writer, const auto& map, std::size_t depth, EditWriter writeEdit) {
    writer.raw("{"); bool first = true;
    for (const auto& [key, edit] : map) {
        if (!first) writer.raw(",");
        writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeEdit(writer, edit, depth + 1); first = false;
    }
    if (!first) { writer.raw("\n"); writer.indent(depth); }
    writer.raw("}");
}

void writeSurface(JsonWriter& writer, const SurfaceEdit& edit, std::size_t depth) {
    writer.raw("{"); bool first = true;
    const auto text = [&](std::string_view key, const auto& value) { if (!value) return; if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writer.string(*value); first = false; };
    const auto number = [&](std::string_view key, const auto& value) { if (!value) return; if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writer.raw(formatNumber(*value)); first = false; };
    const auto boolean = [&](std::string_view key, const auto& value) { if (!value) return; if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writer.raw(*value ? "true" : "false"); first = false; };
    text("key", edit.key); number("friction", edit.friction); number("damping", edit.damping); number("dirtAdditive", edit.dirtAdditive); number("blackFlagTime", edit.blackFlagTime); boolean("isValidTrack", edit.isValidTrack); boolean("isPitlane", edit.isPitlane); number("sinHeight", edit.sinHeight); number("sinLength", edit.sinLength); number("vibrationGain", edit.vibrationGain); number("vibrationLength", edit.vibrationLength); text("wav", edit.wav); number("wavPitch", edit.wavPitch); text("ffEffect", edit.ffEffect);
    if (!first) { writer.raw("\n"); writer.indent(depth); } writer.raw("}");
}

void writeDamage(JsonWriter& writer, const DamageEdit& edit, std::size_t depth) {
    writer.raw("{");
    bool first = true;
    const auto number = [&](std::string_view key, const auto& value) { if (!value) return; if (!first) writer.raw(", "); writer.string(key); writer.raw(": "); writer.raw(formatNumber(*value)); first = false; };
    const auto text = [&](std::string_view key, const auto& value) { if (!value) return; if (!first) writer.raw(", "); writer.string(key); writer.raw(": "); writer.string(*value); first = false; };
    const auto vector = [&](std::string_view key, const std::optional<Vector3>& value) { if (!value) return; if (!first) writer.raw(", "); writer.string(key); writer.raw(": "); writeArray(writer, *value, depth + 1); first = false; };
    number("minSpeed", edit.minSpeed); number("maxSpeed", edit.maxSpeed); number("initialLevel", edit.initialLevel); number("staticRotationAngle", edit.staticRotationAngle); number("multG", edit.multG); number("fullSpeed", edit.fullSpeed); number("oscillationMinAngle", edit.oscillationMinAngle); number("oscillationMaxAngle", edit.oscillationMaxAngle); vector("staticRotationAxis", edit.staticRotationAxis); vector("oscillationAxis", edit.oscillationAxis); vector("allowedG", edit.allowedG);
    if (edit.enabled) { if (!first) writer.raw(", "); writer.string("enabled"); writer.raw(": "); writer.raw(*edit.enabled ? "true" : "false"); first = false; }
    text("name", edit.name); text("damageZone", edit.damageZone);
    writer.raw("}");
}

void writeMesh(JsonWriter& writer, const MeshEdit& edit, std::size_t depth) {
    writer.raw("{");
    bool first = true;
    const auto field = [&](std::string_view key, const auto& value, auto writeValue) {
        if (!value)
            return;
        if (!first)
            writer.raw(",");
        writer.raw("\n");
        writer.indent(depth + 1);
        writer.string(key);
        writer.raw(": ");
        writeValue(*value);
        first = false;
    };
    field("isTransparent", edit.transparent,
          [&](bool value) { writer.raw(value ? "true" : "false"); });
    field("castShadows", edit.castShadows,
          [&](bool value) { writer.raw(value ? "true" : "false"); });
    field("layer", edit.layer, [&](std::uint32_t value) { writer.raw(std::to_string(value)); });
    field("lodIn", edit.lodIn, [&](float value) { writer.raw(formatNumber(value)); });
    field("lodOut", edit.lodOut, [&](float value) { writer.raw(formatNumber(value)); });
    if (!first) {
        writer.raw("\n");
        writer.indent(depth);
    }
    writer.raw("}");
}

void writeGeometry(JsonWriter& writer, const GeometryEdit& edit, std::size_t depth) {
    writer.raw("{");
    bool first = true;
    const auto field = [&](std::string_view key, auto writeValue) {
        if (!first)
            writer.raw(",");
        writer.raw("\n");
        writer.indent(depth + 1);
        writer.string(key);
        writer.raw(": ");
        writeValue();
        first = false;
    };
    if (edit.transform)
        field("transform", [&] { writeArray(writer, *edit.transform, depth + 1); });
    if (edit.remove_degenerate)
        field("removeDegenerate", [&] { writer.raw("true"); });
    if (edit.reverse_winding)
        field("reverseWinding", [&] { writer.raw("true"); });
    if (edit.recalculate_normals)
        field("recalculateNormals", [&] { writer.raw("true"); });
    if (!first) {
        writer.raw("\n");
        writer.indent(depth);
    }
    writer.raw("}");
}

void writeIdentity(JsonWriter& writer, const std::optional<SourceIdentity>& identity,
                   std::size_t depth) {
    if (!identity) {
        writer.raw("null");
        return;
    }
    writer.raw("{\n");
    writer.indent(depth + 1); writer.string("name"); writer.raw(": "); writer.string(identity->name); writer.raw(",\n");
    writer.indent(depth + 1); writer.string("size"); writer.raw(": "); writer.raw(std::to_string(identity->size)); writer.raw(",\n");
    writer.indent(depth + 1); writer.string("sha256"); writer.raw(": "); writer.string(identity->sha256);
    if (identity->kn5Version) {
        writer.raw(",\n");
        writer.indent(depth + 1); writer.string("kn5Version"); writer.raw(": "); writer.raw(std::to_string(*identity->kn5Version));
    }
    writer.raw("\n");
    writer.indent(depth); writer.raw("}");
}

void writeProjectMap(JsonWriter& writer, const ProjectState& project, std::size_t depth) {
    writer.raw("{\n"); writer.indent(depth); writer.string("format"); writer.raw(": "); writer.string(kProjectFormat); writer.raw(",\n");
    writer.indent(depth); writer.string("version"); writer.raw(": 1,\n");
    writer.indent(depth); writer.string("revision"); writer.raw(": "); writer.raw(std::to_string(project.revision)); writer.raw(",\n");
    writer.indent(depth); writer.string("asset"); writer.raw(": {\n"); writer.indent(depth + 1); writer.string("name"); writer.raw(": "); writer.string(project.source.name); writer.raw(",\n"); writer.indent(depth + 1); writer.string("size"); writer.raw(": "); writer.raw(std::to_string(project.source.size)); writer.raw(",\n"); writer.indent(depth + 1); writer.string("sha256"); writer.raw(": "); writer.string(project.source.sha256); if (project.source.kn5Version) { writer.raw(",\n"); writer.indent(depth + 1); writer.string("kn5Version"); writer.raw(": "); writer.raw(std::to_string(*project.source.kn5Version)); } writer.raw("\n"); writer.indent(depth); writer.raw("},\n");
    writer.indent(depth); writer.string("colliderAsset"); writer.raw(": "); writeIdentity(writer, project.colliderAsset, depth); writer.raw(",\n");
    writer.indent(depth); writer.string("damageAsset"); writer.raw(": "); writeIdentity(writer, project.damageAsset, depth); writer.raw(",\n");
    writer.indent(depth); writer.string("bottomColliderAsset"); writer.raw(": "); writeIdentity(writer, project.bottomColliderAsset, depth); writer.raw(",\n");

    writer.indent(depth); writer.string("materialEdits"); writer.raw(": {"); bool first = true; for (const auto& [key, edit] : project.materials) { if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeMaterial(writer, edit, depth + 1); first = false; } if (!first) { writer.raw("\n"); writer.indent(depth); } writer.raw("},\n");
    writer.indent(depth); writer.string("meshEdits"); writer.raw(": {"); first = true; for (const auto& [key, edit] : project.meshes) { if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeMesh(writer, edit, depth + 1); first = false; } if (!first) { writer.raw("\n"); writer.indent(depth); } writer.raw("},\n");
    writer.indent(depth); writer.string("nodeEdits"); writer.raw(": {"); first = true; for (const auto& [key, edit] : project.nodes) { if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeNodeEdit(writer, edit, depth + 1); first = false; } if (!first) { writer.raw("\n"); writer.indent(depth); } writer.raw("},\n");
    writer.indent(depth); writer.string("geometryEdits"); writer.raw(": {"); first = true; for (const auto& [key, edit] : project.geometry) { if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeGeometry(writer, edit, depth + 1); first = false; } if (!first) { writer.raw("\n"); writer.indent(depth); } writer.raw("},\n");
    writer.indent(depth); writer.string("colliderEdits"); writer.raw(": "); writePathMap(writer, project.colliders, depth, [](JsonWriter& w, const ColliderEdit& edit, std::size_t d) { w.raw("{"); bool firstField = true; const auto emit = [&](std::string_view key, const auto& value, auto callback) { if (!value) return; if (!firstField) w.raw(","); w.raw("\n"); w.indent(d + 1); w.string(key); w.raw(": "); callback(*value); firstField = false; }; emit("transform", edit.transform, [&](const Matrix4& value) { writeArray(w, value, d + 1); }); emit("removeDegenerate", edit.removeDegenerate, [&](bool value) { w.raw(value ? "true" : "false"); }); emit("reverseWinding", edit.reverseWinding, [&](bool value) { w.raw(value ? "true" : "false"); }); emit("recalculateNormals", edit.recalculateNormals, [&](bool value) { w.raw(value ? "true" : "false"); }); if (!firstField) { w.raw("\n"); w.indent(d); } w.raw("}"); }); writer.raw(",\n");
    writer.indent(depth); writer.string("damageEdits"); writer.raw(": {"); first = true; for (const auto& [key, edit] : project.damage) { if (!first) writer.raw(","); writer.raw("\n"); writer.indent(depth + 1); writer.string(key); writer.raw(": "); writeDamage(writer, edit, depth + 1); first = false; } if (!first) { writer.raw("\n"); writer.indent(depth); } writer.raw("},\n");
    writer.indent(depth); writer.string("bottomColliderEdits"); writer.raw(": "); writeIndexedMap(writer, project.bottomColliders, depth, [](JsonWriter& w, const BottomColliderEdit& edit, std::size_t d) { w.raw("{"); bool firstField = true; const auto emitVector = [&](std::string_view key, const std::optional<Vector3>& value) { if (!value) return; if (!firstField) w.raw(","); w.raw("\n"); w.indent(d + 1); w.string(key); w.raw(": "); writeArray(w, *value, d + 1); firstField = false; }; emitVector("centre", edit.centre); emitVector("size", edit.size); if (edit.groundEnabled) { if (!firstField) w.raw(","); w.raw("\n"); w.indent(d + 1); w.string("groundEnabled"); w.raw(": "); w.raw(*edit.groundEnabled ? "true" : "false"); firstField = false; } if (!firstField) { w.raw("\n"); w.indent(d); } w.raw("}"); }); writer.raw(",\n");
    writer.indent(depth); writer.string("workspaceEdits"); writer.raw(": {\n"); writer.indent(depth + 1); writer.string("files"); writer.raw(": "); writeIndexedMap(writer, project.workspaceFiles, depth + 1, [](JsonWriter& w, const WorkspaceFileEdit& edit, std::size_t d) { writeWorkspaceFile(w, edit, d); }); if (project.workspace.cockpitHrDistance || project.workspace.driverHrDistance) writer.raw(",\n"); else writer.raw("\n"); if (project.workspace.cockpitHrDistance) { writer.indent(depth + 1); writer.string("cockpitHrDistance"); writer.raw(": "); writer.raw(formatNumber(*project.workspace.cockpitHrDistance)); if (project.workspace.driverHrDistance) writer.raw(",\n"); else writer.raw("\n"); } if (project.workspace.driverHrDistance) { writer.indent(depth + 1); writer.string("driverHrDistance"); writer.raw(": "); writer.raw(formatNumber(*project.workspace.driverHrDistance)); writer.raw("\n"); } writer.indent(depth); writer.raw("},\n");
    writer.indent(depth); writer.string("surfaceEdits"); writer.raw(": "); writeIndexedMap(writer, project.surfaces, depth, [](JsonWriter& w, const SurfaceEdit& edit, std::size_t d) { writeSurface(w, edit, d); }); writer.raw(",\n");
    writer.indent(depth); writer.string("skinEdits"); writer.raw(": {}\n"); writer.indent(depth); writer.raw("}");
}

} // namespace

std::string serializeProject(const ProjectState& project, ProjectIoLimits limits) {
    return serializeProjectInternal(project, limits);
}

ProjectIoResult parseProject(std::string_view text, ProjectIoLimits limits) {
    return parseProjectInternalNoExpected(text, limits);
}

ProjectIoResult parseProject(std::string_view text, const SourceIdentity& expectedSource,
                            ProjectIoLimits limits) {
    return parseProjectInternalExpected(text, expectedSource, limits);
}

std::string serializeEditorCsp(const ProjectState& project, ProjectIoLimits limits,
                              std::vector<ProjectIoDiagnostic>* diagnostics) {
    return serializeEditorCspInternal(project, limits, diagnostics);
}

} // namespace apex::authoring
