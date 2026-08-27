#include "apex/formats/fbx_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace apex::formats {
namespace {

using scene::Matrix4;
using scene::NodeId;

[[noreturn]] void fail(std::string_view code, std::string_view message, std::string path) {
    throw FbxConversionError({FbxConversionSeverity::error, std::string(code),
                              std::string(message), std::move(path)});
}

struct Budget {
    std::size_t used = 0;
    std::size_t maximum = 0;
    void add(std::size_t amount, std::string_view path) {
        if (amount > std::numeric_limits<std::size_t>::max() - used || used + amount > maximum)
            fail("output_limit", "FBX converted scene exceeds its output budget", std::string(path));
        used += amount;
    }
};

std::size_t checkedMultiply(std::size_t left, std::size_t right, std::string_view path) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        fail("size_overflow", "FBX conversion size arithmetic overflow", std::string(path));
    return left * right;
}

std::size_t checkedAdd(std::size_t left, std::size_t right, std::string_view path) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        fail("size_overflow", "FBX conversion size arithmetic overflow", std::string(path));
    return left + right;
}

std::string cleanName(std::string value, std::string_view fallback) {
    const auto separator = value.find("::");
    if (separator != std::string::npos) value.erase(0, separator + 2u);
    if (value.empty()) value = std::string(fallback);
    return value;
}

std::vector<const FbxValue*> values(const FbxNode& node, Budget* budget = nullptr,
                                    std::string_view path = "FBX values") {
    std::size_t count = 0u;
    for (const auto& property : node.properties)
        count = checkedAdd(count, property.values.size(), path);
    if (budget != nullptr) budget->add(checkedMultiply(count, sizeof(const FbxValue*), path), path);
    std::vector<const FbxValue*> result;
    result.reserve(count);
    for (const auto& property : node.properties)
        for (const auto& value : property.values) result.push_back(&value);
    return result;
}

const std::string* stringValue(const FbxValue* value) {
    return value == nullptr ? nullptr : std::get_if<std::string>(value);
}

bool finiteNumber(const FbxValue* value, double& result) {
    if (value == nullptr) return false;
    if (const auto integer = std::get_if<std::int64_t>(value)) {
        result = static_cast<double>(*integer);
        return true;
    }
    if (const auto real = std::get_if<double>(value)) {
        if (!std::isfinite(*real)) return false;
        result = *real;
        return true;
    }
    return false;
}

bool integerValue(const FbxValue* value, std::int64_t& result) {
    if (value == nullptr) return false;
    if (const auto integer = std::get_if<std::int64_t>(value)) {
        result = *integer;
        return true;
    }
    const auto real = std::get_if<double>(value);
    constexpr double int64ExclusiveUpper = 9223372036854775808.0;
    if (real == nullptr || !std::isfinite(*real) || std::floor(*real) != *real ||
        *real < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        *real >= int64ExclusiveUpper) return false;
    result = static_cast<std::int64_t>(*real);
    return true;
}

std::int64_t objectId(const FbxNode& node, std::string_view path, Budget& budget) {
    const auto flattened = values(node, &budget, path);
    std::int64_t id = 0;
    if (flattened.empty() || !integerValue(flattened[0], id) || id <= 0)
        fail("invalid_id", "FBX object ID is not a finite integer", std::string(path));
    return id;
}

std::string objectName(const FbxNode& node, std::string_view path, Budget& budget) {
    const auto flattened = values(node, &budget, path);
    if (flattened.size() < 2u || stringValue(flattened[1]) == nullptr)
        fail("invalid_object", "FBX object has no string name", std::string(path));
    const auto raw = *stringValue(flattened[1]);
    const auto separator = raw.find("::");
    const auto suffixBytes = separator == std::string::npos ? raw.size() : raw.size() - separator - 2u;
    budget.add(suffixBytes == 0u ? 6u : suffixBytes, path);
    return cleanName(raw, "Object");
}

std::string objectType(const FbxNode& node, std::string_view path, Budget& budget) {
    const auto flattened = values(node, &budget, path);
    if (flattened.size() < 3u || stringValue(flattened[2]) == nullptr) return {};
    budget.add(stringValue(flattened[2])->size(), path);
    return *stringValue(flattened[2]);
}

const FbxNode* rootNamed(const FbxDocument& document, std::string_view name) {
    for (const auto& root : document.roots)
        if (root.name == name) return &root;
    return nullptr;
}

struct ObjectRecord {
    const FbxNode* node = nullptr;
    std::int64_t id = 0;
    std::string name;
    std::string type;
};

struct Link {
    std::string kind;
    std::int64_t source = 0;
    std::int64_t target = 0;
    std::string property;
    std::size_t connection_order = 0;
};

std::string fileTextureBasename(const ObjectRecord& record,
                                const FbxConversionLimits& limits,
                                Budget& budget, std::string_view path) {
    const FbxNode* fileName = nullptr;
    for (const auto& child : record.node->children) {
        if (child.name != "FileName") continue;
        if (fileName != nullptr)
            fail("invalid_texture", "FBX file texture has duplicate FileName records",
                 std::string(path));
        fileName = &child;
    }
    if (fileName == nullptr) return {};
    const auto flattened = values(*fileName, &budget, path);
    if (flattened.size() != 1u || stringValue(flattened.front()) == nullptr)
        fail("invalid_texture", "FBX FileName record must contain one string",
             std::string(path));
    const auto& source = *stringValue(flattened.front());
    if (source.empty()) return {};
    if (source.find('\0') != std::string::npos)
        fail("invalid_texture", "FBX file texture path contains a null byte",
             std::string(path));
    const auto separator = source.find_last_of("/\\");
    const auto basename = source.substr(
        separator == std::string::npos ? 0u : separator + 1u);
    if (basename.empty() || basename == "." || basename == "..")
        fail("invalid_texture", "FBX file texture has no safe basename",
             std::string(path));
    if (basename.size() > limits.max_string_bytes)
        fail("string_limit", "FBX file texture basename exceeds its limit",
             std::string(path));
    budget.add(basename.size(), path);
    return basename;
}

struct EmbeddedVideoSource {
    std::string basename;
    const std::vector<std::uint8_t>* content = nullptr;
};

EmbeddedVideoSource embeddedVideoSource(const ObjectRecord& record,
                                        const FbxConversionLimits& limits,
                                        Budget& budget,
                                        std::string_view path) {
    const FbxNode* content = nullptr;
    const FbxNode* relative_name = nullptr;
    const FbxNode* file_name = nullptr;
    for (const auto& child : record.node->children) {
        const auto assign_unique = [&](const FbxNode*& destination,
                                       std::string_view field) {
            if (destination != nullptr)
                fail("invalid_embedded_image",
                     "FBX embedded image has duplicate " +
                         std::string(field) + " records",
                     std::string(path));
            destination = &child;
        };
        if (child.name == "Content") assign_unique(content, "Content");
        else if (child.name == "RelativeFilename")
            assign_unique(relative_name, "RelativeFilename");
        else if (child.name == "FileName")
            assign_unique(file_name, "FileName");
    }
    if (content == nullptr) return {};
    const auto content_values = values(*content, &budget, path);
    if (content_values.size() != 1u)
        fail("invalid_embedded_image",
             "FBX embedded image Content must contain one raw payload",
             std::string(path));
    const auto* bytes =
        std::get_if<std::vector<std::uint8_t>>(content_values.front());
    if (bytes == nullptr)
        fail("invalid_embedded_image",
             "FBX embedded image Content is not a raw payload",
             std::string(path));
    if (bytes->empty()) return {};
    if (bytes->size() > limits.max_embedded_image_bytes)
        fail("embedded_image_limit",
             "FBX embedded image exceeds its byte limit", std::string(path));

    const auto read_basename = [&](const FbxNode* node,
                                   std::string_view field) -> std::string {
        if (node == nullptr) return {};
        const auto flattened = values(*node, &budget, path);
        const auto* text = flattened.size() == 1u
                               ? stringValue(flattened.front())
                               : nullptr;
        if (text == nullptr)
            fail("invalid_embedded_image",
                 "FBX embedded image " + std::string(field) +
                 " must contain one string",
                 std::string(path));
        if (text->empty()) return {};
        if (text->find('\0') != std::string::npos)
            fail("invalid_embedded_image",
                 "FBX embedded image filename contains a null byte",
                 std::string(path));
        const auto separator = text->find_last_of("/\\");
        auto basename = text->substr(separator == std::string::npos
                                         ? 0u
                                         : separator + 1u);
        if (basename.empty() || basename == "." || basename == "..")
            fail("invalid_embedded_image",
                 "FBX embedded image has no safe basename",
                 std::string(path));
        if (basename.size() > limits.max_string_bytes)
            fail("string_limit",
                 "FBX embedded image basename exceeds its limit",
                 std::string(path));
        budget.add(basename.size(), path);
        return basename;
    };
    auto basename = read_basename(relative_name, "RelativeFilename");
    if (basename.empty()) basename = read_basename(file_name, "FileName");
    if (basename.empty()) {
        basename = "embedded-" + std::to_string(record.id) + ".bin";
        if (basename.size() > limits.max_string_bytes)
            fail("string_limit",
                 "FBX embedded image generated name exceeds its limit",
                 std::string(path));
        budget.add(basename.size(), path);
    }
    return {std::move(basename), bytes};
}

constexpr std::size_t kAssociativeNodeOverhead = sizeof(void*) * 4u;

void chargeAssociativeNode(Budget& budget, std::size_t valueBytes, std::string_view path) {
    budget.add(checkedAdd(valueBytes, kAssociativeNodeOverhead, path), path);
}

template <typename T>
void reserveForAppend(std::vector<T>& values, std::size_t required, Budget& budget,
                      std::string_view path) {
    if (required <= values.capacity()) return;
    const auto current = values.capacity();
    auto next = current == 0u ? 1u : current > std::numeric_limits<std::size_t>::max() / 2u
                                     ? std::numeric_limits<std::size_t>::max()
                                     : current * 2u;
    if (next < required) next = required;
    budget.add(checkedMultiply(next - current, sizeof(T), path), path);
    values.reserve(next);
}

template <typename Key, typename Value>
void appendMappedVector(std::map<Key, std::vector<Value>>& values, const Key& key,
                        Value value, Budget& budget, std::string_view path) {
    auto [iterator, inserted] = values.try_emplace(key);
    if (inserted) {
        chargeAssociativeNode(
            budget, sizeof(std::pair<const Key, std::vector<Value>>), path);
    }
    reserveForAppend(iterator->second, iterator->second.size() + 1u, budget, path);
    iterator->second.push_back(std::move(value));
}

template <typename Value>
bool insertSet(std::set<Value>& values, const Value& value, Budget& budget,
               std::string_view path) {
    const auto [iterator, inserted] = values.insert(value);
    (void)iterator;
    if (inserted) chargeAssociativeNode(budget, sizeof(Value), path);
    return inserted;
}

struct DiagnosticBudget {
    std::size_t count = 0;
    std::size_t bytes = 0;
    const FbxConversionLimits& limits;
    Budget* output = nullptr;
};

void addDiagnostic(FbxSceneConversion& result, DiagnosticBudget& budget,
                   FbxConversionSeverity severity, std::string_view code,
                   std::string_view message, std::string_view path,
                   bool incomplete = true) {
    if (incomplete) result.complete = false;
    if (path.size() > budget.limits.max_diagnostic_path_bytes ||
        code.size() > budget.limits.max_string_bytes || message.size() > budget.limits.max_string_bytes)
        fail("diagnostic_limit", "FBX conversion diagnostic exceeds its string limit", "diagnostics");
    if (code.size() > std::numeric_limits<std::size_t>::max() - message.size() ||
        code.size() + message.size() > std::numeric_limits<std::size_t>::max() - path.size())
        fail("size_overflow", "FBX conversion diagnostic size overflows", "diagnostics");
    const auto bytes = code.size() + message.size() + path.size();
    if (budget.bytes > budget.limits.max_diagnostic_bytes ||
        bytes > budget.limits.max_diagnostic_bytes - budget.bytes ||
        budget.count >= budget.limits.max_diagnostics) return;
    if (budget.output != nullptr) budget.output->add(bytes, "diagnostics");
    budget.bytes += bytes;
    ++budget.count;
    if (budget.output != nullptr && result.diagnostics.size() == result.diagnostics.capacity()) {
        const auto current = result.diagnostics.capacity();
        const auto next = current == 0u ? 1u : current > std::numeric_limits<std::size_t>::max() / 2u
                                                ? std::numeric_limits<std::size_t>::max()
                                                : current * 2u;
        budget.output->add(checkedMultiply(next - current, sizeof(FbxConversionDiagnostic), "diagnostics"),
                           "diagnostics");
        result.diagnostics.reserve(next);
    }
    result.diagnostics.push_back({severity, std::string(code), std::string(message), std::string(path)});
}

std::string childPath(std::string_view parent, std::size_t index,
                      const FbxConversionLimits& limits, Budget* output = nullptr) {
    const auto suffix = "/" + std::to_string(index);
    if (parent.size() > limits.max_diagnostic_path_bytes ||
        suffix.size() > limits.max_diagnostic_path_bytes - parent.size())
        fail("diagnostic_limit", "FBX conversion diagnostic path exceeds its limit", "diagnostics");
    const auto bytes = checkedAdd(parent.size(), suffix.size(), "diagnostic path");
    if (output != nullptr) output->add(bytes, "diagnostic paths");
    return std::string(parent) + suffix;
}

std::string boundedPath(std::string path, const FbxConversionLimits& limits) {
    if (path.size() > limits.max_diagnostic_path_bytes)
        fail("diagnostic_limit", "FBX conversion diagnostic path exceeds its limit", "diagnostics");
    return path;
}

void finiteMatrix(const Matrix4& matrix, std::string_view path) {
    for (const auto value : matrix)
        if (!std::isfinite(value)) fail("non_finite", "FBX transform contains a non-finite value", std::string(path));
}

Matrix4 multiply(const Matrix4& left, const Matrix4& right) {
    Matrix4 output{};
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t index = 0; index < 4u; ++index)
                output[column * 4u + row] += left[index * 4u + row] * right[column * 4u + index];
    return output;
}

std::optional<Matrix4> inverseMatrix(const Matrix4& input) {
    const double a00 = input[0], a01 = input[1], a02 = input[2],
                 a03 = input[3];
    const double a10 = input[4], a11 = input[5], a12 = input[6],
                 a13 = input[7];
    const double a20 = input[8], a21 = input[9], a22 = input[10],
                 a23 = input[11];
    const double a30 = input[12], a31 = input[13], a32 = input[14],
                 a33 = input[15];
    const double b00 = a00 * a11 - a01 * a10;
    const double b01 = a00 * a12 - a02 * a10;
    const double b02 = a00 * a13 - a03 * a10;
    const double b03 = a01 * a12 - a02 * a11;
    const double b04 = a01 * a13 - a03 * a11;
    const double b05 = a02 * a13 - a03 * a12;
    const double b06 = a20 * a31 - a21 * a30;
    const double b07 = a20 * a32 - a22 * a30;
    const double b08 = a20 * a33 - a23 * a30;
    const double b09 = a21 * a32 - a22 * a31;
    const double b10 = a21 * a33 - a23 * a31;
    const double b11 = a22 * a33 - a23 * a32;
    const double determinant = b00 * b11 - b01 * b10 + b02 * b09 +
                               b03 * b08 - b04 * b07 + b05 * b06;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        return std::nullopt;
    const double scale = 1.0 / determinant;
    const std::array<double, 16u> values = {
        (a11 * b11 - a12 * b10 + a13 * b09) * scale,
        (-a01 * b11 + a02 * b10 - a03 * b09) * scale,
        (a31 * b05 - a32 * b04 + a33 * b03) * scale,
        (-a21 * b05 + a22 * b04 - a23 * b03) * scale,
        (-a10 * b11 + a12 * b08 - a13 * b07) * scale,
        (a00 * b11 - a02 * b08 + a03 * b07) * scale,
        (-a30 * b05 + a32 * b02 - a33 * b01) * scale,
        (a20 * b05 - a22 * b02 + a23 * b01) * scale,
        (a10 * b10 - a11 * b08 + a13 * b06) * scale,
        (-a00 * b10 + a01 * b08 - a03 * b06) * scale,
        (a30 * b04 - a31 * b02 + a33 * b00) * scale,
        (-a20 * b04 + a21 * b02 - a23 * b00) * scale,
        (-a10 * b09 + a11 * b07 - a12 * b06) * scale,
        (a00 * b09 - a01 * b07 + a02 * b06) * scale,
        (-a30 * b03 + a31 * b01 - a32 * b00) * scale,
        (a20 * b03 - a21 * b01 + a22 * b00) * scale};
    Matrix4 output{};
    for (std::size_t index = 0u; index < output.size(); ++index) {
        if (!std::isfinite(values[index]) ||
            values[index] < -static_cast<double>(
                                std::numeric_limits<float>::max()) ||
            values[index] > static_cast<double>(
                                std::numeric_limits<float>::max()))
            return std::nullopt;
        output[index] = static_cast<float>(values[index]);
    }
    return output;
}

Matrix4 localTransform(const FbxNode& model, std::string_view path,
                       const FbxConversionLimits& limits,
                       FbxSceneConversion& result, DiagnosticBudget& diagnostics) {
    Matrix4 translation = scene::identity_matrix;
    Matrix4 rotation = scene::identity_matrix;
    Matrix4 scale = scene::identity_matrix;
    bool visible = true;
    const FbxNode* properties70 = nullptr;
    for (const auto& child : model.children) {
        if (child.name == "Properties70" || child.name == "Properties60") properties70 = &child;
    }
    if (properties70 != nullptr) {
        for (std::size_t propertyIndex = 0; propertyIndex < properties70->children.size(); ++propertyIndex) {
            const auto& property = properties70->children[propertyIndex];
            const auto flattened = values(property, diagnostics.output, path);
            if (flattened.empty() || stringValue(flattened[0]) == nullptr) continue;
            const auto& name = *stringValue(flattened[0]);
            const bool supported = name == "Lcl Translation" || name == "Lcl Rotation" ||
                                   name == "Lcl Scaling" || name == "GeometricTranslation" ||
                                   name == "GeometricRotation" || name == "GeometricScaling" ||
                                   name == "Visibility";
            if (!supported) {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning,
                              "unsupported_transform_property",
                              "FBX transform property is outside the supported local transform subset",
                              childPath(std::string(path) + "/Properties70", propertyIndex, limits, diagnostics.output));
                continue;
            }
            std::array<double, 3> numbers{};
            std::size_t found = 0;
            for (std::size_t index = 1u; index < flattened.size() && found < numbers.size(); ++index) {
                double value = 0.0;
                if (finiteNumber(flattened[index], value)) numbers[found++] = value;
            }
            if (name == "Visibility") {
                if (found == 0u) fail("invalid_transform", "FBX Visibility property has no numeric value", std::string(path));
                visible = numbers[0] != 0.0;
                continue;
            }
            if (found < 3u) fail("invalid_transform", "FBX local transform property has fewer than three numeric values", std::string(path));
            const auto finiteFloat = [&](double value) {
                if (!std::isfinite(value) || value < -static_cast<double>(std::numeric_limits<float>::max()) ||
                    value > static_cast<double>(std::numeric_limits<float>::max()))
                    fail("non_finite", "FBX transform component is not finite", std::string(path));
                return static_cast<float>(value);
            };
            if (name == "Lcl Translation") {
                translation[12] = finiteFloat(numbers[0]); translation[13] = finiteFloat(numbers[1]); translation[14] = finiteFloat(numbers[2]);
            } else if (name == "Lcl Scaling") {
                scale[0] = finiteFloat(numbers[0]); scale[5] = finiteFloat(numbers[1]); scale[10] = finiteFloat(numbers[2]);
            } else if (name == "Lcl Rotation") {
                const auto radians = [](float degrees) { return degrees * 0.017453292519943295769F; };
                const float x = radians(finiteFloat(numbers[0]));
                const float y = radians(finiteFloat(numbers[1]));
                const float z = radians(finiteFloat(numbers[2]));
                Matrix4 rx = scene::identity_matrix, ry = scene::identity_matrix, rz = scene::identity_matrix;
                rx[5] = std::cos(x); rx[9] = -std::sin(x); rx[6] = std::sin(x); rx[10] = std::cos(x);
                ry[0] = std::cos(y); ry[8] = std::sin(y); ry[2] = -std::sin(y); ry[10] = std::cos(y);
                rz[0] = std::cos(z); rz[4] = -std::sin(z); rz[1] = std::sin(z); rz[5] = std::cos(z);
                rotation = multiply(multiply(rz, ry), rx);
            }
        }
    }
    (void)visible;
    const auto matrix = multiply(multiply(translation, rotation), scale);
    finiteMatrix(matrix, path);
    return matrix;
}

Matrix4 geometricTransform(const FbxNode& model, std::string_view path) {
    std::array<float, 3> translation{0.0F, 0.0F, 0.0F};
    std::array<float, 3> rotation{0.0F, 0.0F, 0.0F};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
    for (const auto& group : model.children) {
        if (group.name != "Properties70" && group.name != "Properties60") continue;
        for (const auto& property : group.children) {
            const auto flattened = values(property, nullptr, path);
            if (flattened.empty() || stringValue(flattened.front()) == nullptr) continue;
            const auto name = *stringValue(flattened.front());
            std::array<float, 3>* target = name == "GeometricTranslation" ? &translation :
                                            name == "GeometricRotation" ? &rotation :
                                            name == "GeometricScaling" ? &scale : nullptr;
            if (target == nullptr) continue;
            std::size_t found = 0u;
            for (std::size_t index = 1u; index < flattened.size() && found < 3u; ++index) {
                double number = 0.0;
                if (!finiteNumber(flattened[index], number)) continue;
                if (number < -static_cast<double>(std::numeric_limits<float>::max()) ||
                    number > static_cast<double>(std::numeric_limits<float>::max()))
                    fail("invalid_transform", "FBX geometric transform contains an invalid component", std::string(path));
                (*target)[found++] = static_cast<float>(number);
            }
            if (found != 3u)
                fail("invalid_transform", "FBX geometric transform has fewer than three values", std::string(path));
        }
    }
    const auto radians = [](float degrees) { return degrees * 0.017453292519943295769F; };
    const float x = radians(rotation[0]), y = radians(rotation[1]), z = radians(rotation[2]);
    Matrix4 translationMatrix = scene::identity_matrix;
    Matrix4 rx = scene::identity_matrix, ry = scene::identity_matrix, rz = scene::identity_matrix;
    Matrix4 scaleMatrix = scene::identity_matrix;
    translationMatrix[12] = translation[0];
    translationMatrix[13] = translation[1];
    translationMatrix[14] = translation[2];
    rx[5] = std::cos(x); rx[9] = -std::sin(x); rx[6] = std::sin(x); rx[10] = std::cos(x);
    ry[0] = std::cos(y); ry[8] = std::sin(y); ry[2] = -std::sin(y); ry[10] = std::cos(y);
    rz[0] = std::cos(z); rz[4] = -std::sin(z); rz[1] = std::sin(z); rz[5] = std::cos(z);
    scaleMatrix[0] = scale[0]; scaleMatrix[5] = scale[1]; scaleMatrix[10] = scale[2];
    const auto matrix = multiply(multiply(translationMatrix, multiply(rz, multiply(ry, rx))), scaleMatrix);
    finiteMatrix(matrix, path);
    return matrix;
}

bool modelVisible(const FbxNode& model, Budget* budget = nullptr) {
    for (const auto& child : model.children) {
        if (child.name != "Properties70" && child.name != "Properties60") continue;
        for (const auto& property : child.children) {
            const auto flattened = values(property, budget, "model visibility");
            if (flattened.empty() || stringValue(flattened[0]) == nullptr || *stringValue(flattened[0]) != "Visibility") continue;
            for (std::size_t index = 1u; index < flattened.size(); ++index) {
                double number = 0.0;
                if (finiteNumber(flattened[index], number)) return number != 0.0;
            }
        }
    }
    return true;
}

bool asciiEqualFold(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0u; index < left.size(); ++index) {
        const auto lower = [](unsigned char value) {
            return value >= static_cast<unsigned char>('A') &&
                           value <= static_cast<unsigned char>('Z')
                       ? static_cast<unsigned char>(value + ('a' - 'A'))
                       : value;
        };
        if (lower(static_cast<unsigned char>(left[index])) !=
            lower(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

float materialFloat(const FbxValue* value, std::string_view path) {
    double number = 0.0;
    if (!finiteNumber(value, number) ||
        number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max()))
        fail("invalid_material",
             "FBX material property is not a finite representable float",
             std::string(path));
    return static_cast<float>(number);
}

FbxMaterialParameters materialParameters(const FbxNode& material,
                                         std::string_view shading_model,
                                         std::string_view path,
                                         Budget& budget) {
    FbxMaterialParameters result;
    result.recognized_surface =
        asciiEqualFold(shading_model, "phong") ||
        asciiEqualFold(shading_model, "lambert");
    if (!result.recognized_surface) return result;
    std::optional<float> shininess_exponent;
    const auto set_color = [&](std::optional<std::array<float, 3u>>& target,
                               const std::vector<const FbxValue*>& flattened,
                               std::string_view property_path) {
        if (target.has_value())
            fail("invalid_material", "FBX material property is duplicated",
                 std::string(property_path));
        if (flattened.size() != 7u)
            fail("invalid_material",
                 "FBX material color property has an invalid value count",
                 std::string(property_path));
        target = std::array<float, 3u>{
            materialFloat(flattened[4], property_path),
            materialFloat(flattened[5], property_path),
            materialFloat(flattened[6], property_path)};
    };
    const auto set_scalar = [&](std::optional<float>& target,
                                const std::vector<const FbxValue*>& flattened,
                                std::string_view property_path) {
        if (target.has_value())
            fail("invalid_material", "FBX material property is duplicated",
                 std::string(property_path));
        if (flattened.size() != 5u)
            fail("invalid_material",
                 "FBX material scalar property has an invalid value count",
                 std::string(property_path));
        target = materialFloat(flattened[4], property_path);
    };

    for (std::size_t group_index = 0u; group_index < material.children.size();
         ++group_index) {
        const auto& group = material.children[group_index];
        if (group.name != "Properties70" && group.name != "Properties60")
            continue;
        for (std::size_t property_index = 0u;
             property_index < group.children.size(); ++property_index) {
            const auto& property = group.children[property_index];
            if (property.name != "P") continue;
            const auto property_path =
                std::string(path) + "/" + group.name + "/" +
                std::to_string(property_index);
            const auto flattened = values(property, &budget, property_path);
            if (flattened.empty() || stringValue(flattened.front()) == nullptr)
                continue;
            const auto& name = *stringValue(flattened.front());
            if (name == "AmbientColor")
                set_color(result.ambient_color, flattened, property_path);
            else if (name == "DiffuseColor")
                set_color(result.diffuse_color, flattened, property_path);
            else if (name == "SpecularColor")
                set_color(result.specular_color, flattened, property_path);
            else if (name == "Shininess")
                set_scalar(result.shininess, flattened, property_path);
            else if (name == "ShininessExponent")
                set_scalar(shininess_exponent, flattened, property_path);
        }
    }
    if (!result.shininess.has_value()) result.shininess = shininess_exponent;
    return result;
}

std::vector<float> geometryPositions(const FbxNode& geometry, std::string_view path,
                                     const FbxConversionLimits& limits, Budget& budget) {
    const FbxNode* vertices = nullptr;
    for (const auto& child : geometry.children) if (child.name == "Vertices") vertices = &child;
    if (vertices == nullptr) fail("missing_geometry", "FBX geometry has no Vertices array", std::string(path));
    const auto flattened = values(*vertices, &budget, path);
    if (flattened.empty()) fail("missing_geometry", "FBX Vertices property is empty", std::string(path));
    const FbxArray* array = std::get_if<FbxArray>(flattened[0]);
    if (array == nullptr) fail("invalid_geometry", "FBX Vertices is not an array", std::string(path));
    std::vector<float> result;
    if (const auto doubles = std::get_if<std::vector<double>>(array)) {
        if (doubles->size() % 3u != 0u) fail("invalid_geometry", "FBX Vertices is not xyz-aligned", std::string(path));
        if (doubles->size() / 3u > limits.max_vertices) fail("vertex_limit", "FBX vertex count exceeds its limit", std::string(path));
        budget.add(checkedMultiply(doubles->size(), sizeof(float), path), path);
        result.resize(doubles->size());
        for (std::size_t index = 0; index < doubles->size(); ++index) {
            if (!std::isfinite((*doubles)[index]) || (*doubles)[index] < -std::numeric_limits<float>::max() || (*doubles)[index] > std::numeric_limits<float>::max())
                fail("non_finite", "FBX vertex is not finite", std::string(path));
            result[index] = static_cast<float>((*doubles)[index]);
        }
    } else if (const auto floats = std::get_if<std::vector<float>>(array)) {
        if (floats->size() % 3u != 0u || floats->size() / 3u > limits.max_vertices)
            fail("vertex_limit", "FBX vertex count is invalid", std::string(path));
        budget.add(checkedMultiply(floats->size(), sizeof(float), path), path);
        result = *floats;
        for (const auto value : result) if (!std::isfinite(value)) fail("non_finite", "FBX vertex is not finite", std::string(path));
    } else fail("invalid_geometry", "FBX Vertices has an unsupported numeric type", std::string(path));
    if (result.size() / 3u >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "FBX vertex indices exceed their uint32 range", std::string(path));
    return result;
}

const FbxValue* firstChildValue(const FbxNode& node, std::string_view childName) {
    for (const auto& child : node.children) {
        if (child.name != childName) continue;
        for (const auto& property : child.properties)
            if (!property.values.empty()) return &property.values.front();
        return nullptr;
    }
    return nullptr;
}

const FbxNode* firstChildNode(const FbxNode& node, std::string_view childName) {
    for (const auto& child : node.children)
        if (child.name == childName) return &child;
    return nullptr;
}

bool supportedUvLayer(const FbxNode& layer) {
    const auto* mapping = firstChildValue(layer, "MappingInformationType");
    const auto* reference = firstChildValue(layer, "ReferenceInformationType");
    const auto* mappingText = stringValue(mapping);
    const auto* referenceText = stringValue(reference);
    return mappingText != nullptr && referenceText != nullptr &&
           *mappingText == "ByPolygonVertex" && *referenceText == "IndexToDirect";
}

enum class NormalMapping { polygon_vertex, control_point, polygon, all_same };

std::optional<NormalMapping> normalMapping(const FbxNode& layer) {
    const auto* value = stringValue(firstChildValue(layer, "MappingInformationType"));
    if (value == nullptr) return std::nullopt;
    if (*value == "ByPolygonVertex") return NormalMapping::polygon_vertex;
    if (*value == "ByControlPoint" || *value == "ByVertice" ||
        *value == "ByVertex")
        return NormalMapping::control_point;
    if (*value == "ByPolygon") return NormalMapping::polygon;
    if (*value == "AllSame") return NormalMapping::all_same;
    return std::nullopt;
}

bool supportedNormalLayer(const FbxNode& layer) {
    const auto* reference = firstChildValue(layer, "ReferenceInformationType");
    const auto* referenceText = stringValue(reference);
    return normalMapping(layer).has_value() && referenceText != nullptr &&
           (*referenceText == "Direct" || *referenceText == "IndexToDirect");
}

enum class MaterialMapping { polygon, all_same };

std::optional<MaterialMapping> materialMapping(const FbxNode& layer) {
    const auto* value =
        stringValue(firstChildValue(layer, "MappingInformationType"));
    if (value == nullptr) return std::nullopt;
    if (*value == "ByPolygon") return MaterialMapping::polygon;
    if (*value == "AllSame") return MaterialMapping::all_same;
    return std::nullopt;
}

bool supportedMaterialLayer(const FbxNode& layer) {
    const auto* reference =
        stringValue(firstChildValue(layer, "ReferenceInformationType"));
    return materialMapping(layer).has_value() && reference != nullptr &&
           (*reference == "Direct" || *reference == "IndexToDirect");
}

struct ParsedUvLayer {
    const FbxArray* direct = nullptr;
    const std::vector<std::int64_t>* indices = nullptr;
    std::size_t directCount = 0u;
};

struct ParsedNormalLayer {
    const FbxArray* direct = nullptr;
    const std::vector<std::int64_t>* indices = nullptr;
    std::size_t direct_count = 0u;
    NormalMapping mapping = NormalMapping::polygon_vertex;
    bool indexed = false;
};

struct ParsedMaterialLayer {
    const std::vector<std::int64_t>* slots = nullptr;
    MaterialMapping mapping = MaterialMapping::polygon;
};

ParsedMaterialLayer parseMaterialLayer(const FbxNode& layer,
                                       std::string_view path,
                                       const FbxConversionLimits& limits,
                                       Budget& budget) {
    const FbxNode* mapping_node = nullptr;
    const FbxNode* reference_node = nullptr;
    const FbxNode* materials_node = nullptr;
    for (const auto& child : layer.children) {
        const auto assign_unique = [&](const FbxNode*& target,
                                       std::string_view field) {
            if (target != nullptr)
                fail("invalid_material_layer",
                     "FBX material layer contains duplicate " +
                         std::string(field),
                     std::string(path));
            target = &child;
        };
        if (child.name == "MappingInformationType")
            assign_unique(mapping_node, "MappingInformationType");
        else if (child.name == "ReferenceInformationType")
            assign_unique(reference_node, "ReferenceInformationType");
        else if (child.name == "Materials")
            assign_unique(materials_node, "Materials data");
    }
    const auto required_text = [&](const FbxNode* node,
                                   std::string_view field) -> std::string_view {
        if (node == nullptr)
            fail("invalid_material_layer",
                 "FBX material layer is missing " + std::string(field),
                 std::string(path));
        const auto flattened = values(*node, &budget, path);
        const auto* text = flattened.size() == 1u
                               ? stringValue(flattened.front())
                               : nullptr;
        if (text == nullptr)
            fail("invalid_material_layer",
                 "FBX material layer " + std::string(field) +
                     " is malformed",
                 std::string(path));
        return *text;
    };
    const auto mapping_text =
        required_text(mapping_node, "MappingInformationType");
    const auto reference =
        required_text(reference_node, "ReferenceInformationType");
    std::optional<MaterialMapping> mapping;
    if (mapping_text == "ByPolygon") mapping = MaterialMapping::polygon;
    else if (mapping_text == "AllSame") mapping = MaterialMapping::all_same;
    if (!mapping.has_value() ||
        (reference != "Direct" && reference != "IndexToDirect"))
        fail("unsupported_material_layer",
             "FBX material layer mapping is unsupported", std::string(path));
    if (materials_node == nullptr)
        fail("invalid_material_layer",
             "FBX material layer is missing Materials data",
             std::string(path));
    const auto material_values = values(*materials_node, &budget, path);
    if (material_values.size() != 1u)
        fail("invalid_material_layer",
             "FBX Materials data must contain exactly one array",
             std::string(path));
    const auto* material_array =
        std::get_if<FbxArray>(material_values.front());
    const auto* slots = material_array == nullptr
                            ? nullptr
                            : std::get_if<std::vector<std::int64_t>>(
                                  material_array);
    if (slots == nullptr)
        fail("invalid_material_layer",
             "FBX Materials data has an unsupported numeric type",
             std::string(path));
    if (slots->size() > limits.max_indices)
        fail("index_limit", "FBX material slot count exceeds its limit",
             std::string(path));
    for (const auto slot : *slots) {
        if (slot < std::numeric_limits<std::int32_t>::min() ||
            slot > std::numeric_limits<std::int32_t>::max())
            fail("invalid_material_layer",
                 "FBX material slot exceeds the native signed integer range",
                 std::string(path));
    }
    if (*mapping == MaterialMapping::all_same && slots->size() != 1u)
        fail("invalid_material_layer",
             "FBX AllSame material layer must contain one slot",
             std::string(path));
    return {slots, *mapping};
}

ParsedNormalLayer parseNormalLayer(const FbxNode& layer, std::string_view path,
                                   const FbxConversionLimits& limits,
                                   Budget& budget) {
    const auto mapping = normalMapping(layer);
    const auto* reference = stringValue(
        firstChildValue(layer, "ReferenceInformationType"));
    if (!mapping.has_value() || reference == nullptr ||
        (*reference != "Direct" && *reference != "IndexToDirect"))
        fail("unsupported_normal", "FBX normal layer mapping is unsupported",
             std::string(path));
    const auto* normals_node = firstChildNode(layer, "Normals");
    if (normals_node == nullptr)
        fail("invalid_normal", "FBX normal layer is missing Normals data",
             std::string(path));
    const auto normal_values = values(*normals_node, &budget, path);
    if (normal_values.empty())
        fail("invalid_normal", "FBX Normals array is empty", std::string(path));
    const auto* direct = std::get_if<FbxArray>(normal_values.front());
    if (direct == nullptr)
        fail("invalid_normal", "FBX Normals data is not an array",
             std::string(path));
    std::size_t component_count = 0u;
    if (const auto* doubles = std::get_if<std::vector<double>>(direct))
        component_count = doubles->size();
    else if (const auto* floats = std::get_if<std::vector<float>>(direct))
        component_count = floats->size();
    else
        fail("invalid_normal", "FBX Normals data has an unsupported numeric type",
             std::string(path));
    if (component_count == 0u || component_count % 3u != 0u)
        fail("invalid_normal", "FBX Normals data is not xyz-aligned",
             std::string(path));
    const auto direct_count = component_count / 3u;
    if (direct_count > limits.max_vertices)
        fail("vertex_limit", "FBX normal values exceed the vertex limit",
             std::string(path));
    const auto check_finite = [&](double value) {
        if (!std::isfinite(value) ||
            value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max()))
            fail("non_finite",
                 "FBX normal is not representable as a finite float",
                 std::string(path));
    };
    if (const auto* doubles = std::get_if<std::vector<double>>(direct)) {
        for (const auto value : *doubles) check_finite(value);
    } else {
        for (const auto value : *std::get_if<std::vector<float>>(direct))
            check_finite(value);
    }

    const bool indexed = *reference == "IndexToDirect";
    const std::vector<std::int64_t>* indices = nullptr;
    if (indexed) {
        const auto* index_node = firstChildNode(layer, "NormalsIndex");
        if (index_node == nullptr)
            fail("invalid_normal",
                 "Indexed FBX normal layer is missing NormalsIndex data",
                 std::string(path));
        const auto index_values = values(*index_node, &budget, path);
        if (index_values.empty())
            fail("invalid_normal", "FBX NormalsIndex array is empty",
                 std::string(path));
        const auto* index_array = std::get_if<FbxArray>(index_values.front());
        indices = index_array == nullptr
                      ? nullptr
                      : std::get_if<std::vector<std::int64_t>>(index_array);
        if (indices == nullptr)
            fail("invalid_normal",
                 "FBX NormalsIndex data has an unsupported numeric type",
                 std::string(path));
        if (indices->size() > limits.max_indices)
            fail("index_limit", "FBX NormalsIndex count exceeds its limit",
                 std::string(path));
        for (const auto direct_index : *indices) {
            if (direct_index < 0 ||
                static_cast<std::uint64_t>(direct_index) >= direct_count)
                fail("invalid_normal",
                     "FBX NormalsIndex references a missing direct normal",
                     std::string(path));
        }
    }
    return {direct, indices, direct_count, *mapping, indexed};
}

ParsedUvLayer parseUvLayer(const FbxNode& layer, std::string_view path,
                           const FbxConversionLimits& limits, Budget& budget) {
    const auto* uvNode = firstChildNode(layer, "UV");
    const auto* indexNode = firstChildNode(layer, "UVIndex");
    if (uvNode == nullptr || indexNode == nullptr)
        fail("invalid_uv", "FBX UV layer is missing UV or UVIndex data", std::string(path));

    const auto uvValues = values(*uvNode, &budget, path);
    if (uvValues.empty()) fail("invalid_uv", "FBX UV array is empty", std::string(path));
    const auto* direct = std::get_if<FbxArray>(uvValues.front());
    if (direct == nullptr) fail("invalid_uv", "FBX UV data is not an array", std::string(path));
    std::size_t directValues = 0u;
    if (const auto doubles = std::get_if<std::vector<double>>(direct)) directValues = doubles->size();
    else if (const auto floats = std::get_if<std::vector<float>>(direct)) directValues = floats->size();
    else fail("invalid_uv", "FBX UV data has an unsupported numeric type", std::string(path));
    if (directValues % 2u != 0u)
        fail("invalid_uv", "FBX UV data is not pair-aligned", std::string(path));
    const auto directCount = directValues / 2u;
    if (directCount > limits.max_vertices)
        fail("vertex_limit", "FBX UV direct values exceed the vertex limit", std::string(path));
    const auto checkFinite = [&](double value) {
        if (!std::isfinite(value) || value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max()))
            fail("non_finite", "FBX UV value is not representable as a finite float", std::string(path));
    };
    if (const auto doubles = std::get_if<std::vector<double>>(direct)) {
        for (const auto value : *doubles) checkFinite(value);
    } else {
        for (const auto value : *std::get_if<std::vector<float>>(direct)) checkFinite(value);
    }

    const auto indexValues = values(*indexNode, &budget, path);
    if (indexValues.empty()) fail("invalid_uv", "FBX UVIndex array is empty", std::string(path));
    const auto* indexArray = std::get_if<FbxArray>(indexValues.front());
    const auto* indices = indexArray == nullptr ? nullptr : std::get_if<std::vector<std::int64_t>>(indexArray);
    if (indices == nullptr)
        fail("invalid_uv", "FBX UVIndex data has an unsupported numeric type", std::string(path));
    return {direct, indices, directCount};
}

struct UvCorner {
    std::uint32_t position = 0u;
    std::size_t uv = 0u;
    std::size_t polygon_vertex = 0u;
    std::size_t polygon = 0u;
};

struct ExpandedUvGeometry {
    std::vector<float> positions;
    std::vector<float> uvs;
    std::vector<float> normals;
    std::vector<std::uint32_t> source_control_points;
    std::vector<std::uint32_t> triangle_indices;
    std::vector<std::int32_t> triangle_material_slots;
};

double uvComponent(const ParsedUvLayer& layer, std::size_t index) {
    if (const auto doubles = std::get_if<std::vector<double>>(layer.direct)) {
        return (*doubles)[index];
    }
    return static_cast<double>((*std::get_if<std::vector<float>>(layer.direct))[index]);
}

double normalComponent(const ParsedNormalLayer& layer, std::size_t index) {
    if (const auto* doubles = std::get_if<std::vector<double>>(layer.direct))
        return (*doubles)[index];
    return static_cast<double>(
        (*std::get_if<std::vector<float>>(layer.direct))[index]);
}

std::size_t normalDirectIndex(const ParsedNormalLayer& layer,
                              const UvCorner& corner,
                              std::string_view path) {
    std::size_t mapped = 0u;
    switch (layer.mapping) {
    case NormalMapping::polygon_vertex: mapped = corner.polygon_vertex; break;
    case NormalMapping::control_point: mapped = corner.position; break;
    case NormalMapping::polygon: mapped = corner.polygon; break;
    case NormalMapping::all_same: mapped = 0u; break;
    }
    if (layer.indexed) {
        if (mapped >= layer.indices->size())
            fail("invalid_normal",
                 "FBX NormalsIndex does not cover every mapped normal",
                 std::string(path));
        const auto direct = (*layer.indices)[mapped];
        if (direct < 0 || static_cast<std::uint64_t>(direct) >= layer.direct_count)
            fail("invalid_normal",
                 "FBX NormalsIndex references a missing direct normal",
                 std::string(path));
        return static_cast<std::size_t>(direct);
    }
    if (mapped >= layer.direct_count)
        fail("invalid_normal", "FBX normal mapping references missing data",
             std::string(path));
    return mapped;
}

ExpandedUvGeometry geometryWithLayers(
    const FbxNode& geometry, const FbxNode* uv_layer,
    const FbxNode* normal_layer, const FbxNode* material_layer,
    std::string_view path,
    const FbxConversionLimits& limits, Budget& budget,
    const std::vector<float>& controlPositions) {
    const auto parsed_uv = uv_layer == nullptr
                               ? std::optional<ParsedUvLayer>{}
                               : parseUvLayer(*uv_layer, path, limits, budget);
    const auto parsed_normal = normal_layer == nullptr
                                   ? std::optional<ParsedNormalLayer>{}
                                   : parseNormalLayer(*normal_layer, path, limits,
                                                      budget);
    const auto parsed_material =
        material_layer == nullptr
            ? std::optional<ParsedMaterialLayer>{}
            : parseMaterialLayer(*material_layer, path, limits, budget);
    const auto* polygon = firstChildNode(geometry, "PolygonVertexIndex");
    if (polygon == nullptr)
        fail("missing_geometry", "FBX geometry has no PolygonVertexIndex array", std::string(path));
    const auto polygonValues = values(*polygon, &budget, path);
    if (polygonValues.empty())
        fail("missing_geometry", "FBX PolygonVertexIndex property is empty", std::string(path));
    const auto* polygonArray = std::get_if<FbxArray>(polygonValues.front());
    const auto* encoded = polygonArray == nullptr ? nullptr : std::get_if<std::vector<std::int64_t>>(polygonArray);
    if (encoded == nullptr)
        fail("invalid_geometry", "FBX PolygonVertexIndex has an unsupported type", std::string(path));
    if (parsed_uv.has_value() && parsed_uv->indices->size() != encoded->size())
        fail("invalid_uv", "FBX UVIndex count " + std::to_string(parsed_uv->indices->size()) +
                              " does not match polygon vertex count " + std::to_string(encoded->size()), std::string(path));

    const auto vertexCount = controlPositions.size() / 3u;
    if (vertexCount >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "FBX vertex indices exceed their uint32 range", std::string(path));
    const auto maxPolygonVertices = checkedAdd(limits.max_indices / 3u, 2u, path);
    std::size_t polygonSize = 0u;
    std::size_t polygonCount = 0u;
    std::size_t outputVertices = 0u;
    for (std::size_t index = 0u; index < encoded->size(); ++index) {
        const auto value = (*encoded)[index];
        const auto decoded = value < 0 ? ~value : value;
        if (decoded < 0 || static_cast<std::uint64_t>(decoded) >= vertexCount)
            fail("invalid_index", "FBX polygon index references a missing vertex", std::string(path));
        if (parsed_uv.has_value()) {
            const auto uvIndex = (*parsed_uv->indices)[index];
            if (uvIndex < 0 ||
                static_cast<std::uint64_t>(uvIndex) >= parsed_uv->directCount)
                fail("invalid_uv", "FBX UVIndex references a missing direct UV",
                     std::string(path));
        }
        if (polygonSize >= maxPolygonVertices)
            fail("index_limit", "FBX polygon vertex count exceeds its limit", std::string(path));
        ++polygonSize;
        if (value < 0) {
            if (polygonSize < 3u)
                fail("invalid_geometry", "FBX polygon has fewer than three vertices", std::string(path));
            const auto triangles = polygonSize - 2u;
            const auto triangleVertices = checkedMultiply(triangles, 3u, path);
            if (outputVertices > limits.max_indices || triangleVertices > limits.max_indices - outputVertices)
                fail("index_limit", "FBX triangle index count exceeds its limit", std::string(path));
            outputVertices = checkedAdd(outputVertices, triangleVertices, path);
            polygonSize = 0u;
            ++polygonCount;
        }
    }
    if (polygonSize != 0u)
        fail("invalid_geometry", "FBX polygon index stream lacks a terminal index", std::string(path));
    if (outputVertices > limits.max_vertices)
        fail("vertex_limit", "expanded FBX UV geometry exceeds the vertex limit", std::string(path));
    if (outputVertices >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "expanded FBX UV geometry exceeds its uint32 range", std::string(path));
    if (parsed_material.has_value() &&
        parsed_material->mapping == MaterialMapping::polygon &&
        parsed_material->slots->size() != polygonCount)
        fail("invalid_material_layer",
             "FBX ByPolygon material slot count does not match polygon count",
             std::string(path));

    const auto positionValues = checkedMultiply(outputVertices, 3u, path);
    const auto uvValues = checkedMultiply(outputVertices, 2u, path);
    const auto normalValues = checkedMultiply(outputVertices, 3u, path);
    budget.add(checkedMultiply(positionValues, sizeof(float), path), path);
    if (parsed_uv.has_value())
        budget.add(checkedMultiply(uvValues, sizeof(float), path), path);
    if (parsed_normal.has_value())
        budget.add(checkedMultiply(normalValues, sizeof(float), path), path);
    budget.add(checkedMultiply(outputVertices, sizeof(std::uint32_t), path), path);
    budget.add(checkedMultiply(outputVertices, sizeof(std::uint32_t), path), path);
    if (parsed_material.has_value())
        budget.add(checkedMultiply(outputVertices / 3u, sizeof(std::int32_t),
                                   path),
                   path);
    ExpandedUvGeometry result;
    result.positions.reserve(positionValues);
    if (parsed_uv.has_value()) result.uvs.reserve(uvValues);
    if (parsed_normal.has_value()) result.normals.reserve(normalValues);
    result.source_control_points.reserve(outputVertices);
    result.triangle_indices.reserve(outputVertices);
    if (parsed_material.has_value())
        result.triangle_material_slots.reserve(outputVertices / 3u);

    std::vector<UvCorner> polygonCorners;
    const auto appendCorner = [&](const UvCorner corner) {
        const auto positionOffset = checkedMultiply(static_cast<std::size_t>(corner.position), 3u, path);
        const auto outputIndex = result.positions.size() / 3u;
        if (outputIndex >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            fail("id_limit", "expanded FBX UV geometry exceeds its uint32 range", std::string(path));
        result.positions.insert(result.positions.end(), controlPositions.begin() + static_cast<std::ptrdiff_t>(positionOffset),
                                controlPositions.begin() + static_cast<std::ptrdiff_t>(positionOffset + 3u));
        result.source_control_points.push_back(corner.position);
        if (parsed_uv.has_value()) {
            const auto uvOffset = checkedMultiply(corner.uv, 2u, path);
            result.uvs.push_back(
                static_cast<float>(uvComponent(*parsed_uv, uvOffset)));
            result.uvs.push_back(
                -static_cast<float>(uvComponent(*parsed_uv, uvOffset + 1u)));
        }
        if (parsed_normal.has_value()) {
            const auto direct = normalDirectIndex(*parsed_normal, corner, path);
            const auto normalOffset = checkedMultiply(direct, 3u, path);
            result.normals.push_back(static_cast<float>(
                normalComponent(*parsed_normal, normalOffset)));
            result.normals.push_back(static_cast<float>(
                normalComponent(*parsed_normal, normalOffset + 1u)));
            result.normals.push_back(static_cast<float>(
                normalComponent(*parsed_normal, normalOffset + 2u)));
        }
        result.triangle_indices.push_back(static_cast<std::uint32_t>(outputIndex));
    };
    const auto appendTriangle = [&](const UvCorner& first,
                                    const UvCorner& second,
                                    const UvCorner& third,
                                    std::size_t source_polygon) {
        appendCorner(first); appendCorner(second); appendCorner(third);
        if (parsed_material.has_value()) {
            // The native static importer retains the first successful GetAt
            // value after Autodesk rejects later AllSame polygon indices.
            // Broadcast that value without reproducing the stale-stack read.
            const auto slot_index =
                parsed_material->mapping == MaterialMapping::all_same
                    ? 0u
                    : source_polygon;
            result.triangle_material_slots.push_back(static_cast<std::int32_t>(
                (*parsed_material->slots)[slot_index]));
        }
    };
    std::size_t polygon_index = 0u;
    for (std::size_t index = 0u; index < encoded->size(); ++index) {
        const auto value = (*encoded)[index];
        const auto decoded = value < 0 ? ~value : value;
        if (polygonCorners.size() >= maxPolygonVertices)
            fail("index_limit", "FBX polygon vertex count exceeds its limit", std::string(path));
        reserveForAppend(polygonCorners, polygonCorners.size() + 1u, budget, path);
        polygonCorners.push_back(
            {static_cast<std::uint32_t>(decoded),
             parsed_uv.has_value()
                 ? static_cast<std::size_t>((*parsed_uv->indices)[index])
                 : 0u,
             index, polygon_index});
        if (value < 0) {
            for (std::size_t corner = 1u; corner + 1u < polygonCorners.size(); ++corner)
                appendTriangle(polygonCorners[0], polygonCorners[corner],
                               polygonCorners[corner + 1u], polygon_index);
            polygonCorners.clear();
            ++polygon_index;
        }
    }
    if (result.positions.size() != positionValues ||
        (parsed_uv.has_value() && result.uvs.size() != uvValues) ||
        (parsed_normal.has_value() && result.normals.size() != normalValues) ||
        (parsed_material.has_value() &&
         result.triangle_material_slots.size() != outputVertices / 3u) ||
        result.source_control_points.size() != outputVertices ||
        result.triangle_indices.size() != outputVertices)
        fail("invalid_geometry", "expanded FBX UV geometry count changed during conversion", std::string(path));
    return result;
}

std::vector<std::uint32_t> geometryIndices(const FbxNode& geometry, std::string_view path,
                                           std::size_t vertexCount, const FbxConversionLimits& limits,
                                           Budget& budget) {
    const FbxNode* polygon = nullptr;
    for (const auto& child : geometry.children) if (child.name == "PolygonVertexIndex") polygon = &child;
    if (polygon == nullptr) fail("missing_geometry", "FBX geometry has no PolygonVertexIndex array", std::string(path));
    const auto flattened = values(*polygon, &budget, path);
    if (flattened.empty()) fail("missing_geometry", "FBX PolygonVertexIndex property is empty", std::string(path));
    const FbxArray* array = std::get_if<FbxArray>(flattened[0]);
    const auto encoded = array == nullptr ? nullptr : std::get_if<std::vector<std::int64_t>>(array);
    if (encoded == nullptr) fail("invalid_geometry", "FBX PolygonVertexIndex has an unsupported type", std::string(path));
    if (vertexCount >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "FBX vertex indices exceed their uint32 range", std::string(path));
    std::vector<std::uint32_t> result;
    std::vector<std::uint32_t> polygonVertices;
    const auto maxPolygonVertices = checkedAdd(limits.max_indices / 3u, 2u, path);
    for (std::size_t index = 0; index < encoded->size(); ++index) {
        const auto value = (*encoded)[index];
        const bool terminal = value < 0;
        const auto decoded = terminal ? ~value : value;
        if (decoded < 0 || static_cast<std::uint64_t>(decoded) >= vertexCount)
            fail("invalid_index", "FBX polygon index references a missing vertex", std::string(path));
        if (polygonVertices.size() >= maxPolygonVertices)
            fail("index_limit", "FBX polygon vertex count exceeds its limit", std::string(path));
        reserveForAppend(polygonVertices, polygonVertices.size() + 1u, budget, path);
        polygonVertices.push_back(static_cast<std::uint32_t>(decoded));
        if (terminal) {
            if (polygonVertices.size() < 3u) fail("invalid_geometry", "FBX polygon has fewer than three vertices", std::string(path));
            const auto triangles = polygonVertices.size() - 2u;
            if (result.size() > limits.max_indices || triangles > (limits.max_indices - result.size()) / 3u)
                fail("index_limit", "FBX triangle index count exceeds its limit", std::string(path));
            reserveForAppend(result, checkedAdd(result.size(), checkedMultiply(triangles, 3u, path), path), budget, path);
            for (std::size_t corner = 1u; corner + 1u < polygonVertices.size(); ++corner) {
                result.push_back(polygonVertices[0]); result.push_back(polygonVertices[corner]); result.push_back(polygonVertices[corner + 1u]);
            }
            polygonVertices.clear();
        }
    }
    if (!polygonVertices.empty()) fail("invalid_geometry", "FBX polygon index stream lacks a terminal index", std::string(path));
    return result;
}

const FbxArray& requiredArrayChild(const FbxNode& node,
                                   std::string_view child_name,
                                   std::string_view path, Budget& budget) {
    const FbxNode* selected = nullptr;
    for (const auto& child : node.children) {
        if (child.name != child_name) continue;
        if (selected != nullptr)
            fail("invalid_skin", "FBX cluster has duplicate array records",
                 std::string(path));
        selected = &child;
    }
    if (selected == nullptr)
        fail("invalid_skin", "FBX cluster is missing a required array",
             std::string(path));
    const auto flattened = values(*selected, &budget, path);
    if (flattened.size() != 1u)
        fail("invalid_skin", "FBX cluster array record is malformed",
             std::string(path));
    const auto* array = std::get_if<FbxArray>(flattened.front());
    if (array == nullptr)
        fail("invalid_skin", "FBX cluster array has an unsupported type",
             std::string(path));
    return *array;
}

FbxSkinBinding buildSkinBinding(
    std::int64_t skin_id, std::size_t control_point_count,
    const std::vector<std::int64_t>& clusters,
    const std::map<std::int64_t, std::int64_t>& bone_for_cluster,
    const std::vector<ObjectRecord>& records,
    const std::map<std::int64_t, std::size_t>& by_id,
    const FbxConversionLimits& limits, Budget& budget,
    FbxSceneConversion& conversion, DiagnosticBudget& diagnostics,
    std::string_view path) {
    if (clusters.empty())
        fail("invalid_skin", "FBX skin has no clusters", std::string(path));
    if (clusters.size() > limits.max_bones_per_skin)
        fail("skin_bone_limit", "FBX skin bone count exceeds its limit",
             std::string(path));
    if (control_point_count > limits.max_skin_influences)
        fail("skin_influence_limit",
             "FBX skin control-point count exceeds its influence limit",
             std::string(path));

    FbxSkinBinding result;
    result.skin_object_id = skin_id;
    budget.add(checkedMultiply(clusters.size(), sizeof(FbxSkinBone), path),
               path);
    budget.add(checkedMultiply(control_point_count,
                               sizeof(FbxSkinInfluence), path), path);
    result.bones.reserve(clusters.size());
    result.vertex_influences.resize(control_point_count);
    budget.add(control_point_count, path);
    std::vector<std::uint8_t> occupied_slots(control_point_count, 0u);
    std::set<std::string_view> bone_names;
    std::size_t influence_count = 0u;
    bool dropped_influence = false;

    for (std::size_t bone_index = 0u; bone_index < clusters.size();
         ++bone_index) {
        if (bone_index >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()))
            fail("skin_bone_limit",
                 "FBX skin bone index exceeds its 32-bit output field",
                 std::string(path));
        const auto cluster_id = clusters[bone_index];
        const auto cluster_record = by_id.find(cluster_id);
        const auto bone_link = bone_for_cluster.find(cluster_id);
        if (cluster_record == by_id.end() || bone_link == bone_for_cluster.end())
            fail("invalid_skin",
                 "FBX skin cluster has no linked bone Model",
                 std::string(path));
        const auto bone_record = by_id.find(bone_link->second);
        if (bone_record == by_id.end() ||
            records[bone_record->second].node->name != "Model")
            fail("invalid_skin",
                 "FBX skin cluster bone link is not a Model",
                 std::string(path));
        const auto& cluster = *records[cluster_record->second].node;
        const auto cluster_path =
            std::string(path) + "/Cluster/" + std::to_string(cluster_id);
        const auto& index_array = requiredArrayChild(
            cluster, "Indexes", cluster_path, budget);
        const auto& weight_array = requiredArrayChild(
            cluster, "Weights", cluster_path, budget);
        const auto& matrix_array = requiredArrayChild(
            cluster, "TransformLink", cluster_path, budget);
        const auto* indexes =
            std::get_if<std::vector<std::int64_t>>(&index_array);
        if (indexes == nullptr)
            fail("invalid_skin",
                 "FBX cluster Indexes array is not an integer array",
                 cluster_path);
        const auto* double_weights =
            std::get_if<std::vector<double>>(&weight_array);
        const auto* float_weights =
            std::get_if<std::vector<float>>(&weight_array);
        const auto weight_count = double_weights != nullptr
                                      ? double_weights->size()
                                      : float_weights != nullptr
                                            ? float_weights->size()
                                            : std::size_t{0u};
        if ((double_weights == nullptr && float_weights == nullptr) ||
            indexes->size() != weight_count)
            fail("invalid_skin",
                 "FBX cluster Indexes and Weights arrays are not aligned",
                 cluster_path);
        if (indexes->size() > limits.max_skin_influences - influence_count)
            fail("skin_influence_limit",
                 "FBX skin influence count exceeds its limit", cluster_path);
        influence_count += indexes->size();

        Matrix4 link_matrix{};
        const auto* double_matrix =
            std::get_if<std::vector<double>>(&matrix_array);
        const auto* float_matrix =
            std::get_if<std::vector<float>>(&matrix_array);
        const auto matrix_count = double_matrix != nullptr
                                      ? double_matrix->size()
                                      : float_matrix != nullptr
                                            ? float_matrix->size()
                                            : std::size_t{0u};
        if (matrix_count != link_matrix.size())
            fail("skin_matrix_invalid",
                 "FBX cluster TransformLink must contain 16 values",
                 cluster_path);
        for (std::size_t index = 0u; index < link_matrix.size(); ++index) {
            const double value = double_matrix != nullptr
                                     ? (*double_matrix)[index]
                                     : static_cast<double>((*float_matrix)[index]);
            if (!std::isfinite(value) ||
                value < -static_cast<double>(std::numeric_limits<float>::max()) ||
                value > static_cast<double>(std::numeric_limits<float>::max()))
                fail("skin_matrix_invalid",
                     "FBX cluster TransformLink is not a finite float matrix",
                     cluster_path);
            link_matrix[index] = static_cast<float>(value);
        }
        // FBXImporter::loadNode reads only TransformLink here. It copies that
        // matrix without an axis or unit conversion, then inverts it before
        // MeshBuilder::addBone. A Cluster Transform record is intentionally
        // ignored because the recovered native path never reads it.
        const auto inverse_bind = inverseMatrix(link_matrix);
        if (!inverse_bind.has_value())
            fail("skin_matrix_invalid",
                 "FBX cluster TransformLink is singular", cluster_path);

        const auto& bone = records[bone_record->second];
        if (!insertSet(bone_names, std::string_view{bone.name}, budget,
                       cluster_path))
            fail("duplicate_bone_name",
                 "FBX skin contains duplicate bone names", cluster_path);
        budget.add(bone.name.size(), cluster_path);
        result.bones.push_back(
            {bone.id, bone.name, *inverse_bind});

        for (std::size_t influence = 0u; influence < indexes->size();
             ++influence) {
            const auto control_point = (*indexes)[influence];
            if (control_point < 0 ||
                static_cast<std::uint64_t>(control_point) >=
                    control_point_count)
                fail("skin_index_invalid",
                     "FBX skin influence references a missing control point",
                     cluster_path);
            const double source_weight =
                double_weights != nullptr
                    ? (*double_weights)[influence]
                    : static_cast<double>((*float_weights)[influence]);
            if (!std::isfinite(source_weight) || source_weight < 0.0 ||
                source_weight >
                    static_cast<double>(std::numeric_limits<float>::max()))
                fail("skin_weight_invalid",
                     "FBX skin weight is not a non-negative finite float",
                     cluster_path);
            auto& target = result.vertex_influences[
                static_cast<std::size_t>(control_point)];
            auto& occupied = occupied_slots[
                static_cast<std::size_t>(control_point)];
            if (occupied >= target.weights.size()) {
                dropped_influence = true;
                continue;
            }
            const auto slot = static_cast<std::size_t>(occupied++);
            target.weights[slot] = static_cast<float>(source_weight);
            target.bones[slot] = static_cast<std::uint32_t>(bone_index);
        }
    }
    if (dropped_influence)
        addDiagnostic(
            conversion, diagnostics, FbxConversionSeverity::warning,
            "skin_influence_dropped",
            "FBX skin has more than four influences on a control point; later source-order influences were dropped",
            path, false);
    return result;
}

void unsupportedFeatureScan(const FbxDocument& document, FbxSceneConversion& result,
                            DiagnosticBudget& diagnostics) {
    struct Frame { const FbxNode* node = nullptr; std::string path; std::size_t child = 0; };
    std::vector<Frame> stack;
    const auto pushFrame = [&](Frame frame) {
        if (stack.size() == stack.capacity() && diagnostics.output != nullptr) {
            const auto current = stack.capacity();
            const auto next = current == 0u ? 1u : current > std::numeric_limits<std::size_t>::max() / 2u
                                                    ? std::numeric_limits<std::size_t>::max()
                                                    : current * 2u;
            diagnostics.output->add(checkedMultiply(next - current, sizeof(Frame), "unsupported feature scan"),
                                    "unsupported feature scan");
            stack.reserve(next);
        }
        stack.push_back(std::move(frame));
    };
    for (auto iterator = document.roots.rbegin(); iterator != document.roots.rend(); ++iterator) {
        if (iterator->name.size() > diagnostics.limits.max_diagnostic_path_bytes)
            fail("diagnostic_limit", "FBX conversion diagnostic path exceeds its limit", "diagnostics");
        if (diagnostics.output != nullptr) diagnostics.output->add(iterator->name.size(), "diagnostic paths");
        pushFrame({&*iterator, iterator->name, 0u});
    }
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.child == 0u) {
            const auto add = [&](std::string_view code, std::string_view message) {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, code, message, frame.path);
            };
            const auto& node = *frame.node;
            if (node.name == "Image")
                add("unsupported_images",
                    "FBX Image records are not converted");
            if ((node.name == "LayerElementNormal" &&
                 !supportedNormalLayer(node)) ||
                (node.name == "LayerElementMaterial" &&
                 !supportedMaterialLayer(node)) ||
                (node.name == "LayerElementUV" && !supportedUvLayer(node)))
                add("unsupported_layer_mapping", "FBX layer-element mappings are not converted");
        }
        if (frame.child == frame.node->children.size()) { stack.pop_back(); continue; }
        const auto childIndex = frame.child++;
        const auto* child = &frame.node->children[childIndex];
        pushFrame({child, childPath(frame.path, childIndex, diagnostics.limits, diagnostics.output), 0u});
    }
}

void validateDomShape(const FbxDocument& document, const FbxConversionLimits& limits) {
    struct Frame { const FbxNode* node = nullptr; std::size_t depth = 0; std::size_t child = 0; };
    std::vector<Frame> stack;
    const auto depthCapacity = limits.max_depth == std::numeric_limits<std::size_t>::max() ? limits.max_depth : limits.max_depth + 1u;
    stack.reserve(std::min(depthCapacity, std::size_t{1024}));
    std::size_t nodes = 0, properties = 0, propertyValues = 0, stringBytes = 0;
    std::size_t rawProperties = 0, rawBytes = 0;
    const auto enter = [&](const FbxNode* node, std::size_t depth) {
        if (node == nullptr || depth > limits.max_depth) fail("depth_limit", "FBX DOM hierarchy exceeds conversion depth", "DOM");
        if (nodes >= limits.max_nodes) fail("node_limit", "FBX DOM node count exceeds conversion limit", "DOM");
        ++nodes;
        if (node->name.size() > limits.max_string_bytes || stringBytes > limits.max_string_bytes - node->name.size())
            fail("string_limit", "FBX DOM string storage exceeds conversion limit", "DOM");
        stringBytes += node->name.size();
        if (node->properties.size() > limits.max_properties ||
            properties > limits.max_properties - node->properties.size())
            fail("property_limit", "FBX DOM property count exceeds conversion limit", "DOM");
        properties += node->properties.size();
        for (const auto& property : node->properties) {
            if (property.values.size() > limits.max_property_values ||
                propertyValues > limits.max_property_values - property.values.size())
                fail("property_limit", "FBX DOM property value count exceeds conversion limit", "DOM");
            propertyValues += property.values.size();
            for (const auto& value : property.values) {
                const auto* text = std::get_if<std::string>(&value);
                if (text != nullptr) {
                    if (text->size() > limits.max_string_bytes || stringBytes > limits.max_string_bytes - text->size())
                        fail("string_limit", "FBX DOM string storage exceeds conversion limit", "DOM");
                    stringBytes += text->size();
                    continue;
                }
                // Binary R and ASCII Content values are direct byte vectors.
                // FBX b arrays use FbxArray and keep the general DOM budgets.
                const auto* raw =
                    std::get_if<std::vector<std::uint8_t>>(&value);
                if (raw == nullptr) continue;
                if (rawProperties >= limits.max_embedded_images)
                    fail("embedded_image_limit",
                         "FBX raw property count exceeds conversion limit", "DOM");
                ++rawProperties;
                if (raw->size() > limits.max_embedded_image_bytes ||
                    raw->size() > limits.max_embedded_image_total_bytes ||
                    rawBytes > limits.max_embedded_image_total_bytes -
                                   raw->size())
                    fail("embedded_image_limit",
                         "FBX raw property bytes exceed conversion limit", "DOM");
                rawBytes += raw->size();
            }
        }
    };
    for (const auto& root : document.roots) {
        enter(&root, 0u);
        stack.push_back({&root, 0u, 0u});
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.child == frame.node->children.size()) { stack.pop_back(); continue; }
            const auto childIndex = frame.child++;
            const auto* child = &frame.node->children[childIndex];
            if (frame.depth == std::numeric_limits<std::size_t>::max())
                fail("depth_limit", "FBX DOM hierarchy exceeds conversion depth", "DOM");
            const auto childDepth = frame.depth + 1u;
            enter(child, childDepth);
            stack.push_back({child, childDepth, 0u});
        }
    }
}

struct AnimationCurveData {
    std::vector<std::int64_t> times;
    std::vector<float> values;
};

const FbxNode* objectNode(const std::vector<ObjectRecord>& records,
                          const std::map<std::int64_t, std::size_t>& byId,
                          std::int64_t id, std::string_view path) {
    const auto found = byId.find(id);
    if (found == byId.end()) fail("invalid_reference", "FBX animation references an unknown object", std::string(path));
    return records[found->second].node;
}

const FbxArray* childArray(const FbxNode& node, std::string_view name, std::string_view path) {
    const FbxArray* foundArray = nullptr;
    for (const auto& child : node.children) {
        if (child.name != name) continue;
        const auto flattened = values(child, nullptr, path);
        if (flattened.size() != 1u || std::get_if<FbxArray>(flattened.front()) == nullptr)
            fail("invalid_animation", "FBX animation array is malformed", std::string(path));
        if (foundArray != nullptr) fail("invalid_animation", "FBX animation contains duplicate arrays", std::string(path));
        foundArray = std::get_if<FbxArray>(flattened.front());
    }
    return foundArray;
}

std::size_t arraySize(const FbxArray& array) noexcept {
    return std::visit([](const auto& valuesIn) { return valuesIn.size(); }, array);
}

bool arrayNumber(const FbxArray& array, std::size_t index, double& output) noexcept {
    return std::visit([&](const auto& valuesIn) {
        if (index >= valuesIn.size()) return false;
        output = static_cast<double>(valuesIn[index]);
        return std::isfinite(output);
    }, array);
}

bool arrayInteger(const FbxArray& array, std::size_t index,
                  std::int64_t& output) noexcept {
    return std::visit([&](const auto& valuesIn) {
        if (index >= valuesIn.size()) return false;
        using Value = typename std::decay_t<decltype(valuesIn)>::value_type;
        if constexpr (std::is_same_v<Value, std::int64_t>) {
            output = valuesIn[index];
            return true;
        } else if constexpr (std::is_same_v<Value, std::uint8_t>) {
            output = valuesIn[index];
            return true;
        } else {
            const double value = static_cast<double>(valuesIn[index]);
            constexpr double int64ExclusiveUpper = 9223372036854775808.0;
            if (!std::isfinite(value) || std::floor(value) != value ||
                value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                value >= int64ExclusiveUpper)
                return false;
            output = static_cast<std::int64_t>(value);
            return true;
        }
    }, array);
}

bool childInteger(const FbxNode& node, std::string_view name,
                  std::int64_t& output, std::string_view path) {
    bool found = false;
    for (const auto& child : node.children) {
        if (child.name != name) continue;
        if (found) fail("invalid_animation", "FBX animation contains duplicate range values", std::string(path));
        const auto flattened = values(child, nullptr, path);
        if (flattened.size() == 1u && integerValue(flattened.front(), output)) {
            found = true;
            continue;
        }
        if (flattened.size() == 1u) {
            if (const auto* array = std::get_if<FbxArray>(flattened.front())) {
                if (arraySize(*array) == 1u && arrayInteger(*array, 0u, output)) {
                    found = true;
                    continue;
                }
            }
        }
        fail("invalid_animation", "FBX animation range value is malformed", std::string(path));
    }
    return found;
}

AnimationCurveData parseAnimationCurve(const FbxNode& curve, const FbxConversionLimits& limits,
                                       Budget& budget, std::string_view path) {
    const auto* timeArray = childArray(curve, "KeyTime", path);
    const auto* valueArray = childArray(curve, "KeyValueFloat", path);
    if (timeArray == nullptr || valueArray == nullptr)
        fail("invalid_animation", "FBX animation curve has no KeyTime or KeyValueFloat array", std::string(path));
    const auto* timeInts = std::get_if<std::vector<std::int64_t>>(timeArray);
    if (timeInts == nullptr) fail("invalid_animation", "FBX KeyTime must be an integer array", std::string(path));
    const auto& times = *timeInts;
    const auto valueCount = arraySize(*valueArray);
    if (times.empty() || times.size() != valueCount)
        fail("invalid_animation", "FBX animation curve key time/value counts differ", std::string(path));
    if (times.size() > limits.max_animation_keys)
        fail("animation_key_limit", "FBX animation key count exceeds its limit", std::string(path));
    budget.add(checkedMultiply(times.size(), sizeof(std::int64_t), path), path);
    budget.add(checkedMultiply(valueCount, sizeof(float), path), path);
    AnimationCurveData result;
    result.times.reserve(times.size()); result.values.reserve(valueCount);
    std::int64_t previous = 0;
    for (std::size_t index = 0; index < times.size(); ++index) {
        const std::int64_t time = times[index];
        if (index != 0u && time <= previous)
            fail("invalid_animation", "FBX animation key times are not strictly increasing", std::string(path));
        double value = 0.0;
        if (!arrayNumber(*valueArray, index, value))
            fail("non_finite", "FBX animation array contains a non-finite value", std::string(path));
        if (value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max()))
            fail("non_finite", "FBX animation value is outside float range", std::string(path));
        result.times.push_back(time); result.values.push_back(static_cast<float>(value)); previous = time;
    }
    return result;
}

bool explicitlyLinear(const FbxNode& curve, std::size_t keyCount, std::string_view path) {
    if (keyCount < 2u) return true;
    const auto* flags = childArray(curve, "KeyAttrFlags", path);
    if (flags == nullptr) return false;
    if (arraySize(*flags) != keyCount) return false;
    for (std::size_t index = 0; index < keyCount; ++index) {
        std::int64_t signedFlag = 0;
        if (!arrayInteger(*flags, index, signedFlag) || signedFlag < 0) return false;
        const auto flag = static_cast<std::uint64_t>(signedFlag);
        if ((flag & 0x4u) == 0u || (flag & 0x8u) != 0u) return false;
    }
    return true;
}

float sampleAnimationCurve(const AnimationCurveData& curve, std::int64_t time) {
    if (curve.times.size() == 1u || time <= curve.times.front()) return curve.values.front();
    if (time >= curve.times.back()) return curve.values.back();
    const auto upper = std::upper_bound(curve.times.begin(), curve.times.end(), time);
    const auto index = static_cast<std::size_t>(upper - curve.times.begin());
    const auto left = curve.times[index - 1u], right = curve.times[index];
    const auto amount = static_cast<float>(static_cast<double>(time - left) / static_cast<double>(right - left));
    return curve.values[index - 1u] + (curve.values[index] - curve.values[index - 1u]) * amount;
}

struct AnimationBinding {
    std::int64_t model = 0;
    std::string_view property;
    std::array<std::int64_t, 3> curves{0, 0, 0};
};

struct AnimationBase {
    std::array<float, 3> translation{0.0F, 0.0F, 0.0F};
    std::array<float, 3> rotation{0.0F, 0.0F, 0.0F};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
};

bool isNativeAnimationModel(const ObjectRecord& record) {
    if (record.node->name != "Model") return false;
    // FBX SDK 2014's loadAnimationNode accepts eSkeleton, eMesh, and eNull
    // (numeric attribute types 3, 4, and 1). FBX Model records spell these
    // types LimbNode, Mesh, and Null respectively.
    return record.type == "LimbNode" || record.type == "Mesh" || record.type == "Null";
}

std::vector<std::int64_t> nativeAnimationModelOrder(
    const std::vector<ObjectRecord>& records,
    const std::map<std::int64_t, std::size_t>& byId,
    const std::vector<std::size_t>& modelIndexes,
    const std::vector<Link>& links,
    const FbxConversionLimits& limits,
    Budget& budget) {
    std::map<std::int64_t, std::vector<std::int64_t>> children;
    std::set<std::int64_t> parented;
    for (const auto& link : links) {
        if (link.kind != "OO") continue;
        const auto source = byId.at(link.source);
        if (records[source].node->name != "Model") continue;
        if (link.target != 0 && records[byId.at(link.target)].node->name != "Model") continue;
        appendMappedVector(children, link.target, link.source, budget,
                           "animation hierarchy");
        (void)insertSet(parented, link.source, budget, "animation hierarchy parents");
    }

    std::vector<std::int64_t> roots;
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(std::int64_t),
                               "animation hierarchy roots"),
               "animation hierarchy roots");
    roots.reserve(modelIndexes.size());
    if (const auto root = children.find(0); root != children.end())
        roots = root->second;
    for (const auto modelIndex : modelIndexes) {
        const auto modelId = records[modelIndex].id;
        if (!parented.contains(modelId)) roots.push_back(modelId);
    }

    struct Pending { std::int64_t id = 0; std::size_t depth = 0; };
    std::vector<Pending> pending;
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(Pending),
                               "animation hierarchy traversal"),
               "animation hierarchy traversal");
    pending.reserve(modelIndexes.size());
    for (auto iterator = roots.rbegin(); iterator != roots.rend(); ++iterator)
        pending.push_back({*iterator, 0});

    std::vector<std::int64_t> result;
    result.reserve(modelIndexes.size());
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(std::int64_t),
                               "animation selected model IDs"),
               "animation selected model IDs");
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (current.depth > limits.max_depth)
            fail("depth_limit", "FBX animation hierarchy exceeds its limit", "animation");
        const auto& record = records[byId.at(current.id)];
        if (isNativeAnimationModel(record)) {
            result.push_back(current.id);
        }
        const auto child = children.find(current.id);
        if (child == children.end()) continue;
        for (auto iterator = child->second.rbegin(); iterator != child->second.rend(); ++iterator)
            pending.push_back({*iterator, current.depth + 1u});
    }
    return result;
}

AnimationBase animationBase(const FbxNode& model, std::string_view path) {
    AnimationBase base;
    for (const auto& group : model.children) {
        if (group.name != "Properties70" && group.name != "Properties60") continue;
        for (const auto& property : group.children) {
            const auto flattened = values(property, nullptr, path);
            if (flattened.empty() || stringValue(flattened.front()) == nullptr) continue;
            const auto name = *stringValue(flattened.front());
            std::array<float, 3>* target = name == "Lcl Translation" ? &base.translation :
                                            name == "Lcl Rotation" ? &base.rotation :
                                            name == "Lcl Scaling" ? &base.scale : nullptr;
            if (target == nullptr) continue;
            std::size_t found = 0u;
            for (std::size_t index = 1u; index < flattened.size() && found < 3u; ++index) {
                double number = 0.0;
                if (!finiteNumber(flattened[index], number) || number < -static_cast<double>(std::numeric_limits<float>::max()) ||
                    number > static_cast<double>(std::numeric_limits<float>::max()))
                    continue;
                (*target)[found++] = static_cast<float>(number);
            }
            if (found != 3u) fail("invalid_transform", "FBX animated transform has fewer than three values", std::string(path));
        }
    }
    return base;
}

KsAnimationFrame animationFrame(const std::array<float, 3>& translation,
                                const std::array<float, 3>& rotation, const std::array<float, 3>& scale,
                                std::string_view path) {
    const auto radians = [](float degrees) { return degrees * 0.017453292519943295769F; };
    const float x = radians(rotation[0]), y = radians(rotation[1]), z = radians(rotation[2]);
    Matrix4 rx = scene::identity_matrix, ry = scene::identity_matrix, rz = scene::identity_matrix;
    rx[5] = std::cos(x); rx[9] = -std::sin(x); rx[6] = std::sin(x); rx[10] = std::cos(x);
    ry[0] = std::cos(y); ry[8] = std::sin(y); ry[2] = -std::sin(y); ry[10] = std::cos(y);
    rz[0] = std::cos(z); rz[4] = -std::sin(z); rz[1] = std::sin(z); rz[5] = std::cos(z);
    const auto matrix = multiply(multiply(multiply(rz, ry), rx), scene::identity_matrix);
    Matrix4 transformed = matrix;
    transformed[0] *= scale[0]; transformed[1] *= scale[0]; transformed[2] *= scale[0];
    transformed[4] *= scale[1]; transformed[5] *= scale[1]; transformed[6] *= scale[1];
    transformed[8] *= scale[2]; transformed[9] *= scale[2]; transformed[10] *= scale[2];
    transformed[12] = translation[0]; transformed[13] = translation[1]; transformed[14] = translation[2];
    for (const auto value : transformed)
        if (!std::isfinite(value)) fail("non_finite", "FBX animation frame is not finite", std::string(path));
    return decomposeKsAnimationMatrix(transformed);
}

void extractAnimations(const std::vector<ObjectRecord>& records,
                       const std::map<std::int64_t, std::size_t>& byId,
                       const std::vector<std::size_t>& modelIndexes,
                       const std::vector<Link>& links, const FbxConversionLimits& limits,
                       FbxSceneConversion& result, Budget& budget, DiagnosticBudget& diagnostics) {
    if (limits.max_animation_frames < 100u)
        fail("animation_frame_limit", "FBX animation conversion requires 100 output frames", "animation");
    std::vector<std::int64_t> stacks, layers, curves;
    for (const auto& record : records) {
        std::vector<std::int64_t>* target = nullptr;
        std::size_t limit = 0u;
        const char* code = nullptr;
        const char* message = nullptr;
        if (record.node->name == "AnimationStack") {
            target = &stacks;
            limit = limits.max_animation_stacks;
            code = "animation_stack_limit";
            message = "FBX animation stack count exceeds its limit";
        } else if (record.node->name == "AnimationLayer") {
            target = &layers;
            limit = limits.max_animation_layers;
            code = "animation_layer_limit";
            message = "FBX animation layer count exceeds its limit";
        } else if (record.node->name == "AnimationCurve") {
            target = &curves;
            limit = limits.max_animation_curves;
            code = "animation_curve_limit";
            message = "FBX animation curve count exceeds its limit";
        }
        if (target == nullptr) continue;
        if (target->size() >= limit) fail(code, message, "animation");
        reserveForAppend(*target, target->size() + 1u, budget, "animation objects");
        target->push_back(record.id);
    }
    if (stacks.empty()) return;
    const auto clipCapacity = std::min(stacks.size(), limits.max_animation_clips);
    budget.add(checkedMultiply(clipCapacity, sizeof(FbxAnimationClip), "animation clips"), "animation clips");
    result.animations.reserve(clipCapacity);
    const auto animationModelIds = nativeAnimationModelOrder(
        records, byId, modelIndexes, links, limits, budget);

    std::map<std::int64_t, std::vector<std::int64_t>> stackLayers;
    std::map<std::int64_t, std::int64_t> layerForNode;
    std::map<std::int64_t, std::vector<std::pair<std::int64_t, std::string>>> modelForNode;
    std::map<std::int64_t, std::vector<std::pair<std::int64_t, char>>> curveForNode;
    for (const auto& link : links) {
        const std::string_view source = records[byId.at(link.source)].node->name;
        const std::string_view target = link.target == 0
                                            ? std::string_view{}
                                            : std::string_view{
                                                  records[byId.at(link.target)].node->name};
        if (link.kind == "OO" && source == "AnimationLayer" && target == "AnimationStack") {
            const auto existing = stackLayers.find(link.target);
            if (existing != stackLayers.end() &&
                std::find(existing->second.begin(), existing->second.end(), link.source) !=
                    existing->second.end())
                fail("invalid_animation", "FBX animation contains a duplicate layer link",
                     "Connections");
            appendMappedVector(stackLayers, link.target, link.source, budget,
                               "animation stack layers");
        } else if (link.kind == "OO" && source == "AnimationCurveNode" && target == "AnimationLayer") {
            if (layerForNode.contains(link.source))
                fail("invalid_animation",
                     layerForNode.at(link.source) == link.target
                         ? "FBX animation contains a duplicate curve-node layer link"
                         : "FBX animation curve node has multiple layers",
                     "Connections");
            const auto [iterator, inserted] = layerForNode.try_emplace(link.source, link.target);
            (void)iterator;
            if (inserted)
                chargeAssociativeNode(
                    budget,
                    sizeof(std::pair<const std::int64_t, std::int64_t>),
                    "animation layer bindings");
        } else if (link.kind == "OP" && source == "AnimationCurveNode" && target == "Model") {
            if (link.property != "Lcl Translation" && link.property != "Lcl Rotation" && link.property != "Lcl Scaling") {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unsupported_animation_channel",
                              "FBX animation property is outside the supported local transform channels", "Connections");
                continue;
            }
            const auto existing = modelForNode.find(link.source);
            if (existing != modelForNode.end() &&
                std::any_of(existing->second.begin(), existing->second.end(),
                            [&](const auto& value) {
                                return value.first == link.target &&
                                       value.second == link.property;
                            }))
                fail("invalid_animation", "FBX animation contains a duplicate model link",
                     "Connections");
            budget.add(link.property.size(), "animation model properties");
            appendMappedVector(modelForNode, link.source,
                               std::pair<std::int64_t, std::string>{link.target,
                                                                    link.property},
                               budget, "animation model bindings");
        } else if (link.kind == "OP" && source == "AnimationCurve" && target == "AnimationCurveNode") {
            const char axis = link.property.size() == 3u && link.property[0] == 'd' && link.property[1] == '|' ? link.property[2] : '\0';
            if (axis != 'X' && axis != 'Y' && axis != 'Z') {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unsupported_animation_channel",
                              "FBX animation curve axis is not X, Y, or Z", "Connections");
                continue;
            }
            const auto existing = curveForNode.find(link.target);
            if (existing != curveForNode.end() &&
                std::find(existing->second.begin(), existing->second.end(),
                          std::pair<std::int64_t, char>{link.source, axis}) !=
                    existing->second.end())
                fail("invalid_animation", "FBX animation contains a duplicate axis link",
                     "Connections");
            appendMappedVector(curveForNode, link.target,
                               std::pair<std::int64_t, char>{link.source, axis},
                               budget, "animation curve bindings");
        }
    }

    std::size_t totalKeys = 0u;
    for (const auto stackId : stacks) {
        if (result.animations.size() >= limits.max_animation_clips)
            fail("animation_clip_limit", "FBX animation clip count exceeds its limit", "animation");
        const auto stackPath = "Objects/AnimationStack/" + std::to_string(stackId);
        const auto stackRecord = records[byId.at(stackId)];
        std::set<std::int64_t> selectedNodes;
        const auto stackLayer = stackLayers.find(stackId);
        if (stackLayer != stackLayers.end()) {
            for (const auto layerId : stackLayer->second) {
                for (const auto& [nodeId, owningLayer] : layerForNode) {
                    if (owningLayer == layerId)
                        (void)insertSet(selectedNodes, nodeId, budget,
                                        "animation selected nodes");
                }
            }
        }
        std::map<std::pair<std::int64_t, std::string_view>, AnimationBinding> bindings;
        std::map<std::int64_t, AnimationCurveData> curveData;
        std::int64_t minimumTime = std::numeric_limits<std::int64_t>::max();
        std::int64_t maximumTime = std::numeric_limits<std::int64_t>::min();
        bool unsupportedCurve = false;
        std::size_t sourceTrackCount = 0u;
        for (const auto nodeId : selectedNodes) {
            ++sourceTrackCount;
            const auto modelBinding = modelForNode.find(nodeId);
            if (modelBinding == modelForNode.end() || modelBinding->second.empty()) {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unsupported_animation_channel",
                              "FBX curve node is not connected to a Model", stackPath);
                unsupportedCurve = true;
                continue;
            }
            const auto curveBinding = curveForNode.find(nodeId);
            if (curveBinding == curveForNode.end() || curveBinding->second.empty()) {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning,
                              "unsupported_animation_channel",
                              "FBX curve node has no connected axis curves", stackPath);
                unsupportedCurve = true;
                continue;
            }
            for (const auto& [modelId, property] : modelBinding->second) {
                const auto bindingKey =
                    std::pair<std::int64_t, std::string_view>{modelId, property};
                auto [bindingIterator, inserted] = bindings.try_emplace(bindingKey);
                if (inserted) {
                    chargeAssociativeNode(
                        budget,
                        sizeof(std::pair<const std::pair<std::int64_t, std::string_view>,
                                         AnimationBinding>),
                        "animation property bindings");
                }
                auto& binding = bindingIterator->second;
                binding.model = modelId; binding.property = property;
                for (const auto& [curveId, axis] : curveBinding->second) {
                    const std::size_t axisIndex = axis == 'X' ? 0u : axis == 'Y' ? 1u : 2u;
                    if (binding.curves[axisIndex] != 0 && binding.curves[axisIndex] != curveId)
                        fail("invalid_animation", "FBX animation property has duplicate axis curves", stackPath);
                    binding.curves[axisIndex] = curveId;
                    if (!curveData.contains(curveId)) {
                        const auto curvePath = stackPath + "/Curve/" + std::to_string(curveId);
                        const auto* curve = objectNode(records, byId, curveId, curvePath);
                        auto parsed = parseAnimationCurve(*curve, limits, budget, curvePath);
                        if (parsed.times.size() > limits.max_animation_keys - std::min(totalKeys, limits.max_animation_keys))
                            fail("animation_key_limit", "FBX aggregate animation key count exceeds its limit", curvePath);
                        totalKeys += parsed.times.size();
                        if (!explicitlyLinear(*curve, parsed.times.size(), curvePath)) {
                            addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unsupported_animation_interpolation",
                                          "FBX animation curve is not explicitly linear; native evaluator behavior is not reproduced", curvePath);
                            unsupportedCurve = true;
                            continue;
                        }
                        minimumTime = std::min(minimumTime, parsed.times.front());
                        maximumTime = std::max(maximumTime, parsed.times.back());
                        chargeAssociativeNode(
                            budget,
                            sizeof(std::pair<const std::int64_t,
                                             AnimationCurveData>),
                            "animation curve data");
                        curveData.emplace(curveId, std::move(parsed));
                    }
                }
            }
        }
        std::int64_t startNumber = 0, stopNumber = 0;
        const bool hasStart = childInteger(*stackRecord.node, "LocalStart", startNumber, stackPath);
        const bool hasStop = childInteger(*stackRecord.node, "LocalStop", stopNumber, stackPath);
        std::int64_t start = minimumTime, stop = maximumTime;
        if (hasStart || hasStop) {
            if (!hasStart || !hasStop)
                fail("invalid_animation", "FBX animation stack range is invalid", stackPath);
            start = startNumber;
            stop = stopNumber;
        }
        if (start >= stop ||
            (minimumTime == std::numeric_limits<std::int64_t>::max() && !hasStart)) {
            addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unsupported_animation",
                          "FBX animation stack has no positive supported time span", stackPath);
            continue;
        }
        if (unsupportedCurve) {
            // A partially sampled clip would silently replace an unsupported
            // channel with its static value, so do not export it.
            result.complete = false;
            continue;
        }
        if (animationModelIds.empty()) continue;
        struct AnimationTrackGroup {
            std::string_view name;
            std::vector<std::int64_t> modelIds;
        };
        std::vector<AnimationTrackGroup> trackGroups;
        std::map<std::string_view, std::size_t> trackGroupByName;
        budget.add(checkedMultiply(animationModelIds.size(), sizeof(AnimationTrackGroup), stackPath), stackPath);
        trackGroups.reserve(animationModelIds.size());
        for (const auto modelId : animationModelIds) {
            const auto& modelName = records[byId.at(modelId)].name;
            auto group = trackGroupByName.find(modelName);
            if (group == trackGroupByName.end()) {
                if (trackGroups.size() >= limits.max_animation_tracks)
                    fail("animation_track_limit", "FBX animation output track count exceeds its limit", stackPath);
                const auto groupIndex = trackGroups.size();
                chargeAssociativeNode(budget,
                                      sizeof(std::pair<const std::string_view,
                                                       std::size_t>),
                                      "animation track names");
                const auto [inserted, wasInserted] = trackGroupByName.emplace(modelName, groupIndex);
                (void)inserted;
                if (!wasInserted) fail("invalid_animation", "FBX animation track-name grouping is inconsistent", stackPath);
                trackGroups.push_back({modelName, {}});
                group = trackGroupByName.find(modelName);
            }
            auto& groupedModels = trackGroups[group->second].modelIds;
            reserveForAppend(groupedModels, groupedModels.size() + 1u, budget,
                             "animation track-name models");
            groupedModels.push_back(modelId);
        }
        FbxAnimationClip clip;
        budget.add(checkedMultiply(2u, stackRecord.name.size(), stackPath), stackPath);
        clip.name = stackRecord.name;
        const long double span = static_cast<long double>(stop) -
                                 static_cast<long double>(start);
        clip.duration = static_cast<double>(span / 46186158000.0L);
        clip.source_track_count = sourceTrackCount;
        clip.animation.source = clip.name;
        clip.animation.version = 2;
        clip.animation.frameCount = 100u;
        budget.add(checkedMultiply(trackGroups.size(), sizeof(KsAnimationTrack), stackPath), stackPath);
        clip.animation.tracks.reserve(trackGroups.size());
        for (const auto& trackGroup : trackGroups) {
            const auto groupPath = stackPath + "/Track";
            KsAnimationTrack track;
            budget.add(trackGroup.name.size(), groupPath);
            track.name = trackGroup.name;
            const auto frameCount = checkedMultiply(trackGroup.modelIds.size(), std::size_t{100}, groupPath);
            if (frameCount > limits.max_animation_merged_frames)
                fail("animation_frame_limit",
                     "Merged FBX animation-set frames exceed their limit", groupPath);
            budget.add(checkedAdd(8u, checkedMultiply(frameCount, sizeof(KsAnimationFrame), groupPath), groupPath), groupPath);
            track.frames.reserve(frameCount);
            for (const auto modelId : trackGroup.modelIds) {
                const auto modelPath = stackPath + "/Model/" + std::to_string(modelId);
                const auto* model = objectNode(records, byId, modelId, modelPath);
                const auto base = animationBase(*model, modelPath);
                for (std::size_t frameIndex = 0; frameIndex < 100u; ++frameIndex) {
                    const auto time = static_cast<std::int64_t>(
                        static_cast<long double>(start) +
                        span * static_cast<long double>(frameIndex) / 100.0L);
                    std::array<float, 3> translation = base.translation, rotation = base.rotation, scale = base.scale;
                    for (const auto& [key, binding] : bindings) {
                        (void)key;
                        if (binding.model != modelId) continue;
                        auto* target = binding.property == "Lcl Translation" ? &translation : binding.property == "Lcl Rotation" ? &rotation : &scale;
                        for (std::size_t axis = 0; axis < 3u; ++axis) {
                            const auto curveId = binding.curves[axis];
                            const auto found = curveData.find(curveId);
                            if (curveId != 0 && found != curveData.end()) (*target)[axis] = sampleAnimationCurve(found->second, time);
                        }
                    }
                    track.frames.push_back(animationFrame(translation, rotation, scale, modelPath));
                }
            }
            track.animated = false;
            if (track.frames.size() > 1u)
                for (std::size_t frame = 1u; frame < track.frames.size() && !track.animated; ++frame)
                    track.animated = track.frames[frame].quaternion != track.frames.front().quaternion ||
                                     track.frames[frame].position != track.frames.front().position ||
                                     track.frames[frame].scale != track.frames.front().scale;
            clip.animation.tracks.push_back(std::move(track));
        }
        if (!clip.animation.tracks.empty()) {
            clip.animation.frameCount = clip.animation.tracks.front().frames.size();
            result.animations.push_back(std::move(clip));
        }
    }
}

}  // namespace

FbxConversionError::FbxConversionError(FbxConversionDiagnostic diagnostic)
    : std::runtime_error(diagnostic.message), diagnostic_(std::move(diagnostic)) {}

std::optional<std::size_t> fbxNativeFileTextureChannelRank(
    std::string_view channel) noexcept {
    const auto match = std::find(fbx_native_file_texture_channels.begin(),
                                 fbx_native_file_texture_channels.end(), channel);
    if (match == fbx_native_file_texture_channels.end()) return std::nullopt;
    return static_cast<std::size_t>(
        match - fbx_native_file_texture_channels.begin());
}

FbxConversionCapabilityDetail fbxSceneConversionCapability() {
    return {true, true, true, true, true, true, true, false,
            "Static and skinned FBX geometry, geometric transforms, native material scalars, ordered node-material slots, bounded polygon material, normal and UV mappings, external file-texture candidates, embedded Video content, and an explicit-linear local-transform KSANIM bridge are converted; native-pivot evaluation, non-linear animation, and Image records remain unsupported"};
}

FbxSceneConversion convertFbxScene(const FbxDocument& document, FbxConversionLimits limits) {
    FbxSceneConversion result;
    validateDomShape(document, limits);
    Budget budget{0u, limits.max_output_bytes};
    DiagnosticBudget diagnostics{0u, 0u, limits, &budget};
    unsupportedFeatureScan(document, result, diagnostics);
    const auto* objects = rootNamed(document, "Objects");
    if (objects == nullptr) fail("missing_objects", "FBX document has no Objects node", "Objects");
    if (limits.max_nodes == 0u) fail("node_limit", "FBX conversion requires room for its synthetic root", "scene");
    const auto* connections = rootNamed(document, "Connections");
    std::vector<ObjectRecord> records;
    std::map<std::int64_t, std::size_t> byId;
    std::vector<std::size_t> modelIndexes;
    std::vector<std::size_t> materialIndexes;
    std::vector<std::size_t> geometryIndexes;
    const auto deformerBudget =
        limits.max_skin_deformers >
                std::numeric_limits<std::size_t>::max() -
                    limits.max_skin_clusters
            ? std::numeric_limits<std::size_t>::max()
            : limits.max_skin_deformers + limits.max_skin_clusters;
    const auto objectBudget = limits.max_nodes > std::numeric_limits<std::size_t>::max() - limits.max_materials ||
                                      limits.max_nodes + limits.max_materials > std::numeric_limits<std::size_t>::max() - limits.max_meshes ||
                                      limits.max_nodes + limits.max_materials + limits.max_meshes > std::numeric_limits<std::size_t>::max() - deformerBudget
                                  ? std::numeric_limits<std::size_t>::max()
                                  : limits.max_nodes + limits.max_materials + limits.max_meshes + deformerBudget;
    if (objects->children.size() > objectBudget)
        fail("node_limit", "FBX Objects count exceeds conversion limits", "Objects");
    std::size_t modelCount = 0u;
    std::size_t materialCount = 0u;
    std::size_t textureCount = 0u;
    std::size_t geometryCount = 0u;
    std::size_t deformerCount = 0u;
    for (const auto& node : objects->children) {
        if (node.name == "Model") ++modelCount;
        else if (node.name == "Material") ++materialCount;
        else if (node.name == "Texture") ++textureCount;
        else if (node.name == "Geometry") ++geometryCount;
        else if (node.name == "Deformer") ++deformerCount;
    }
    if (modelCount > limits.max_nodes || materialCount > limits.max_materials ||
        textureCount > limits.max_textures || geometryCount > limits.max_meshes ||
        deformerCount > deformerBudget)
        fail("count_limit", "FBX scene object count exceeds conversion limits", "Objects");
    budget.add(checkedMultiply(objects->children.size(), sizeof(ObjectRecord), "records"), "records");
    budget.add(checkedMultiply(modelCount, sizeof(std::size_t), "model indexes"), "model indexes");
    budget.add(checkedMultiply(materialCount, sizeof(std::size_t), "material indexes"), "material indexes");
    budget.add(checkedMultiply(geometryCount, sizeof(std::size_t), "geometry indexes"), "geometry indexes");
    records.reserve(objects->children.size());
    modelIndexes.reserve(modelCount);
    materialIndexes.reserve(materialCount);
    geometryIndexes.reserve(geometryCount);
    for (std::size_t index = 0; index < objects->children.size(); ++index) {
        const auto& node = objects->children[index];
        const auto path = "Objects/" + std::to_string(index);
        ObjectRecord record{&node, objectId(node, path, budget), objectName(node, path, budget), objectType(node, path, budget)};
        if (record.id == 0) fail("invalid_id", "FBX object ID 0 is reserved for the synthetic root", path);
        if (byId.contains(record.id)) fail("duplicate_id", "FBX object IDs must be unique", path);
        chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::size_t>), path);
        byId.emplace(record.id, records.size());
        records.push_back(std::move(record));
        const auto& type = records.back().type;
        if (node.name == "Model") modelIndexes.push_back(records.size() - 1u);
        else if (node.name == "Material") materialIndexes.push_back(records.size() - 1u);
        else if (node.name == "Geometry") geometryIndexes.push_back(records.size() - 1u);
        (void)type;
    }
    if (modelIndexes.size() >= static_cast<std::size_t>(scene::invalid_node_id) ||
        materialIndexes.size() >= static_cast<std::size_t>(scene::invalid_material_id) ||
        geometryIndexes.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "FBX converted scene IDs exceed their uint32 range", "Objects");
    budget.add(sizeof(scene::SceneNode), "scene/root");
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(scene::SceneNode), "scene/nodes"), "scene/nodes");
    budget.add(checkedMultiply(materialIndexes.size(), sizeof(scene::SceneMaterial), "scene/materials"), "scene/materials");
    budget.add(checkedMultiply(geometryIndexes.size(), sizeof(FbxStaticMesh), "scene/meshes"), "scene/meshes");
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(FbxNodeTransform), "scene/transforms"), "scene/transforms");
    budget.add(checkedMultiply(geometryIndexes.size(), sizeof(FbxNodeGeometry), "scene/node geometry"), "scene/node geometry");
    budget.add(checkedMultiply(materialIndexes.size(),
                               sizeof(FbxMaterialParameters),
                               "scene/material parameters"),
               "scene/material parameters");
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(NodeId), "scene/children"), "scene/children");
    result.snapshot.nodes.reserve(modelIndexes.size() + 1u);
    result.snapshot.materials.reserve(materialIndexes.size());
    result.meshes.reserve(geometryIndexes.size());
    result.transforms.reserve(modelIndexes.size());
    result.node_geometry.reserve(geometryIndexes.size());
    result.material_parameters.reserve(materialIndexes.size());
    result.snapshot.nodes.push_back({0u, scene::invalid_node_id, "FBX", scene::NodeKind::node, true, true, false, false, true, 0u, 0.0F, 0.0F, scene::invalid_material_id, {0.0F, 0.0F, 0.0F}, 0.0F, scene::identity_matrix, {}, {}, {}, {}, 0.0F, scene::LocalBoundsSource::unavailable, {}});
    result.snapshot.root = 0u;
    std::map<std::int64_t, std::uint32_t> materialIds;
    for (std::size_t index = 0; index < materialIndexes.size(); ++index) {
        const auto& record = records[materialIndexes[index]];
        std::string_view shaderView = record.type;
        for (const auto& child : record.node->children) if (child.name == "ShadingModel") {
            const auto flattened = values(child, &budget, "scene/materials");
            if (!flattened.empty() && stringValue(flattened[0]) != nullptr) shaderView = *stringValue(flattened[0]);
        }
        budget.add(checkedAdd(record.name.size(), std::string_view{"ksPerPixel"}.size(),
                              "scene/materials"),
                   "scene/materials");
        result.snapshot.materials.push_back(
            {record.name, "ksPerPixel", scene::BlendMode::opaque});
        result.material_parameters.push_back(materialParameters(
            *record.node, shaderView,
            "Objects/Material/" + std::to_string(record.id), budget));
        chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::uint32_t>), "material IDs");
        materialIds.emplace(record.id, static_cast<std::uint32_t>(index));
    }
    std::vector<Link> links;
    if (connections != nullptr) {
        if (connections->children.size() > limits.max_connections) fail("connection_limit", "FBX connection count exceeds its limit", "Connections");
        budget.add(checkedMultiply(connections->children.size(), sizeof(Link), "connections"), "connections");
        links.reserve(connections->children.size());
        for (std::size_t index = 0; index < connections->children.size(); ++index) {
            const auto& node = connections->children[index];
            const auto path = "Connections/" + std::to_string(index);
            const auto flattened = values(node, &budget, path);
            if (node.name != "C" || flattened.size() < 3u || stringValue(flattened[0]) == nullptr)
                fail("invalid_connection", "FBX connection record is malformed", path);
            const auto* kindValue = stringValue(flattened[0]);
            const bool propertyBeforeTarget = *kindValue == "PO";
            const std::size_t targetIndex = propertyBeforeTarget ? 3u : 2u;
            if ((propertyBeforeTarget && flattened.size() != 4u) ||
                (!propertyBeforeTarget && flattened.size() > 4u))
                fail("invalid_connection", "FBX connection record has an invalid value count", path);
            std::int64_t sourceId = 0, targetId = 0;
            if (flattened.size() <= targetIndex || !integerValue(flattened[1], sourceId) ||
                !integerValue(flattened[targetIndex], targetId))
                fail("invalid_reference", "FBX connection IDs are invalid", path);
            const auto* propertyValue = propertyBeforeTarget ? stringValue(flattened[2]) :
                                         flattened.size() > 3u ? stringValue(flattened[3]) : nullptr;
            if ((propertyBeforeTarget && propertyValue == nullptr) ||
                (!propertyBeforeTarget && flattened.size() > 3u && propertyValue == nullptr))
                fail("invalid_connection", "FBX connection property is not a string", path);
            const auto kindBytes = kindValue->size();
            const auto propertyBytes = propertyValue == nullptr ? std::size_t{0u} : propertyValue->size();
            budget.add(checkedAdd(kindBytes, propertyBytes, path), path);
            Link link{*kindValue, sourceId, targetId,
                      propertyValue == nullptr ? std::string() : *propertyValue,
                      index};
            const auto sourceRecord = byId.find(link.source);
            const auto targetRecord = link.target == 0 ? byId.end() : byId.find(link.target);
            const bool missingAnimationCurve =
                *kindValue == "OP" && sourceRecord == byId.end() &&
                targetRecord != byId.end() &&
                records[targetRecord->second].node->name == "AnimationCurveNode" &&
                propertyValue != nullptr && propertyValue->size() == 3u &&
                (*propertyValue)[0] == 'd' && (*propertyValue)[1] == '|' &&
                ((*propertyValue)[2] == 'X' || (*propertyValue)[2] == 'Y' ||
                 (*propertyValue)[2] == 'Z');
            if (missingAnimationCurve) {
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning,
                              "missing_animation_curve",
                              "FBX animation curve link references a missing optional curve; connection ignored",
                              path);
                continue;
            }
            if (sourceRecord == byId.end() ||
                (link.target != 0 && targetRecord == byId.end()))
                fail("invalid_reference", "FBX connection references an unknown object", path);
            links.push_back(std::move(link));
        }
    }
    std::size_t skin_object_count = 0u;
    std::size_t cluster_object_count = 0u;
    for (const auto& record : records) {
        if (record.node->name != "Deformer") continue;
        if (record.type == "Cluster") ++cluster_object_count;
        else if (record.type == "Skin") ++skin_object_count;
        else
            addDiagnostic(result, diagnostics, FbxConversionSeverity::warning,
                          "unsupported_deformer",
                          "FBX deformer type is not converted",
                          "Objects/Deformer/" + std::to_string(record.id));
    }
    if (skin_object_count > limits.max_skin_deformers)
        fail("skin_deformer_limit", "FBX skin deformer count exceeds its limit",
             "Objects/Deformer");
    if (cluster_object_count > limits.max_skin_clusters)
        fail("skin_cluster_limit", "FBX skin cluster count exceeds its limit",
             "Objects/Deformer");

    std::map<std::int64_t, std::vector<std::int64_t>> skinsForGeometry;
    std::map<std::int64_t, std::vector<std::int64_t>> clustersForSkin;
    std::map<std::int64_t, std::int64_t> boneForCluster;
    std::set<std::int64_t> connectedDeformers;
    for (const auto& link : links) {
        if (link.kind != "OO" || link.target == 0) continue;
        const auto& source = records[byId.at(link.source)];
        const auto& target = records[byId.at(link.target)];
        if (source.node->name == "Deformer" && source.type == "Skin" &&
            target.node->name == "Geometry") {
            if (!link.property.empty())
                fail("invalid_skin",
                     "FBX Skin-to-Geometry link has an unexpected property",
                     "Connections");
            auto skins_it = skinsForGeometry.find(target.id);
            if (skins_it == skinsForGeometry.end()) {
                chargeAssociativeNode(
                    budget,
                    sizeof(std::pair<const std::int64_t,
                                     std::vector<std::int64_t>>),
                    "scene/skin assignments");
                skins_it = skinsForGeometry
                               .emplace(target.id,
                                        std::vector<std::int64_t>{})
                               .first;
            }
            auto& skins = skins_it->second;
            if (std::find(skins.begin(), skins.end(), source.id) != skins.end())
                fail("invalid_skin",
                     "FBX Geometry has a duplicate Skin connection",
                     "Connections");
            reserveForAppend(skins, skins.size() + 1u, budget,
                             "scene/skin assignments");
            skins.push_back(source.id);
            (void)insertSet(connectedDeformers, source.id, budget,
                            "scene/connected deformers");
            continue;
        }
        if (source.node->name == "Deformer" && source.type == "Cluster" &&
            target.node->name == "Deformer" && target.type == "Skin") {
            if (!link.property.empty())
                fail("invalid_skin",
                     "FBX Cluster-to-Skin link has an unexpected property",
                     "Connections");
            auto clusters_it = clustersForSkin.find(target.id);
            if (clusters_it == clustersForSkin.end()) {
                chargeAssociativeNode(
                    budget,
                    sizeof(std::pair<const std::int64_t,
                                     std::vector<std::int64_t>>),
                    "scene/skin clusters");
                clusters_it = clustersForSkin
                                  .emplace(target.id,
                                           std::vector<std::int64_t>{})
                                  .first;
            }
            auto& clusters = clusters_it->second;
            if (std::find(clusters.begin(), clusters.end(), source.id) !=
                clusters.end())
                fail("invalid_skin",
                     "FBX Skin has a duplicate Cluster connection",
                     "Connections");
            if (clusters.size() >= limits.max_bones_per_skin)
                fail("skin_bone_limit",
                     "FBX skin bone count exceeds its limit", "Connections");
            reserveForAppend(clusters, clusters.size() + 1u, budget,
                             "scene/skin clusters");
            clusters.push_back(source.id);
            (void)insertSet(connectedDeformers, source.id, budget,
                            "scene/connected deformers");
            continue;
        }
        if (source.node->name == "Model" &&
            target.node->name == "Deformer" && target.type == "Cluster") {
            if (!link.property.empty())
                fail("invalid_skin",
                     "FBX bone-to-Cluster link has an unexpected property",
                     "Connections");
            if (!boneForCluster.emplace(target.id, source.id).second)
                fail("invalid_skin",
                     "FBX skin cluster has multiple linked bone Models",
                     "Connections");
            chargeAssociativeNode(
                budget,
                sizeof(std::pair<const std::int64_t, std::int64_t>),
                "scene/skin bones");
        }
    }
    for (const auto& record : records) {
        if (record.node->name != "Deformer" ||
            (record.type != "Skin" && record.type != "Cluster") ||
            connectedDeformers.contains(record.id))
            continue;
        addDiagnostic(result, diagnostics, FbxConversionSeverity::warning,
                      "unreferenced_deformer",
                      "FBX skin deformer is not connected to supported geometry",
                      "Objects/Deformer/" + std::to_string(record.id));
    }
    std::map<std::int64_t, std::int64_t> videoForTexture;
    for (const auto& link : links) {
        if (link.kind != "OO" || link.target == 0) continue;
        const auto& source = records[byId.at(link.source)];
        const auto& target = records[byId.at(link.target)];
        if (source.node->name != "Video" || target.node->name != "Texture")
            continue;
        const auto [iterator, inserted] =
            videoForTexture.emplace(target.id, source.id);
        if (!inserted)
            fail("invalid_embedded_image",
                 iterator->second == source.id
                     ? "FBX Texture has a duplicate Video connection"
                     : "FBX Texture has multiple embedded Video sources",
                 "Objects/Texture/" + std::to_string(target.id));
        chargeAssociativeNode(
            budget,
            sizeof(std::pair<const std::int64_t, std::int64_t>),
            "scene/embedded video links");
    }

    std::set<std::int64_t> supportedTextures;
    std::map<std::int64_t, std::size_t> imageByVideo;
    std::map<std::int64_t, EmbeddedVideoSource> embeddedSources;
    for (const auto& [textureId, videoId] : videoForTexture) {
        (void)textureId;
        if (embeddedSources.contains(videoId)) continue;
        const auto& video = records[byId.at(videoId)];
        const auto videoPath =
            "Objects/Video/" + std::to_string(video.id);
        auto embedded =
            embeddedVideoSource(video, limits, budget, videoPath);
        chargeAssociativeNode(
            budget,
            sizeof(std::pair<const std::int64_t, EmbeddedVideoSource>),
            "scene/embedded video sources");
        embeddedSources.emplace(videoId, std::move(embedded));
    }
    std::size_t embeddedImageBytes = 0u;
    std::size_t textureReferenceCount = 0u;
    for (std::size_t connectionIndex = 0u; connectionIndex < links.size();
         ++connectionIndex) {
        const auto& link = links[connectionIndex];
        if (link.kind != "OP" || link.property.empty() || link.target == 0) continue;
        if (!fbxNativeFileTextureChannelRank(link.property).has_value()) continue;
        const auto& source = records[byId.at(link.source)];
        const auto& target = records[byId.at(link.target)];
        if (source.node->name != "Texture" || target.node->name != "Material") continue;
        const auto path = "Objects/Texture/" + std::to_string(source.id);
        const auto material = materialIds.find(target.id);
        if (material == materialIds.end())
            fail("invalid_reference", "FBX Texture references a missing Material", path);
        bool supported = false;
        auto basename = fileTextureBasename(source, limits, budget, path);
        if (!basename.empty()) {
            if (textureReferenceCount >= limits.max_texture_references)
                fail("texture_reference_limit",
                     "FBX texture reference count exceeds its limit", path);
            ++textureReferenceCount;
            budget.add(link.property.size(), path);
            reserveForAppend(result.file_texture_candidates,
                             result.file_texture_candidates.size() + 1u,
                             budget, "scene/file texture candidates");
            result.file_texture_candidates.push_back(
                {material->second, source.id, link.property,
                 std::move(basename), link.connection_order});
            supported = true;
        }

        const auto videoLink = videoForTexture.find(source.id);
        if (videoLink != videoForTexture.end()) {
            const auto& video = records[byId.at(videoLink->second)];
            const auto videoPath =
                "Objects/Video/" + std::to_string(video.id);
            const auto& embedded = embeddedSources.at(video.id);
            if (embedded.content != nullptr) {
                if (textureReferenceCount >= limits.max_texture_references)
                    fail("texture_reference_limit",
                         "FBX texture reference count exceeds its limit", path);
                ++textureReferenceCount;
                std::size_t imageIndex = 0u;
                const auto existing = imageByVideo.find(video.id);
                if (existing != imageByVideo.end()) {
                    imageIndex = existing->second;
                } else {
                    if (result.embedded_images.size() >=
                        limits.max_embedded_images)
                        fail("embedded_image_limit",
                             "FBX embedded image count exceeds its limit",
                             videoPath);
                    if (embedded.content->size() >
                            limits.max_embedded_image_total_bytes ||
                        embeddedImageBytes >
                            limits.max_embedded_image_total_bytes -
                                embedded.content->size())
                        fail("embedded_image_limit",
                             "FBX embedded image bytes exceed the aggregate limit",
                             videoPath);
                    embeddedImageBytes += embedded.content->size();
                    budget.add(checkedAdd(embedded.basename.size(),
                                          embedded.content->size(), videoPath),
                               "scene/embedded images");
                    reserveForAppend(result.embedded_images,
                                     result.embedded_images.size() + 1u,
                                     budget, "scene/embedded images");
                    imageIndex = result.embedded_images.size();
                    result.embedded_images.push_back(
                        {video.id, embedded.basename,
                         *embedded.content});
                    chargeAssociativeNode(
                        budget,
                        sizeof(std::pair<const std::int64_t, std::size_t>),
                        "scene/embedded image IDs");
                    imageByVideo.emplace(video.id, imageIndex);
                }
                budget.add(link.property.size(), path);
                reserveForAppend(result.embedded_texture_candidates,
                                 result.embedded_texture_candidates.size() + 1u,
                                 budget,
                                 "scene/embedded texture candidates");
                result.embedded_texture_candidates.push_back(
                    {material->second, source.id, video.id, imageIndex,
                     link.property, link.connection_order});
                result.embedded_image_compatibility = true;
                supported = true;
            }
        }
        if (supported)
            (void)insertSet(supportedTextures, source.id, budget,
                            "scene/supported textures");
    }
    std::sort(result.file_texture_candidates.begin(),
              result.file_texture_candidates.end(),
              [](const FbxMaterialFileTextureCandidate& left,
                 const FbxMaterialFileTextureCandidate& right) {
                  const auto leftKey = std::tuple{
                      left.material,
                      *fbxNativeFileTextureChannelRank(left.channel),
                      left.connection_order};
                  const auto rightKey = std::tuple{
                      right.material,
                      *fbxNativeFileTextureChannelRank(right.channel),
                      right.connection_order};
                  return leftKey < rightKey;
              });
    std::sort(result.embedded_texture_candidates.begin(),
              result.embedded_texture_candidates.end(),
              [](const FbxMaterialEmbeddedTextureCandidate& left,
                 const FbxMaterialEmbeddedTextureCandidate& right) {
                  const auto leftKey = std::tuple{
                      left.material,
                      *fbxNativeFileTextureChannelRank(left.channel),
                      left.connection_order};
                  const auto rightKey = std::tuple{
                      right.material,
                      *fbxNativeFileTextureChannelRank(right.channel),
                      right.connection_order};
                  return leftKey < rightKey;
              });
    for (const auto& record : records) {
        if (record.node->name != "Texture" ||
            supportedTextures.contains(record.id))
            continue;
        addDiagnostic(
            result, diagnostics, FbxConversionSeverity::warning,
            "unsupported_images",
            "FBX texture record has no supported native file connection",
            "Objects/Texture/" + std::to_string(record.id));
    }
    std::map<std::int64_t, std::int64_t> parent;
    std::map<std::int64_t, std::vector<std::int64_t>> childrenByParent;
    std::map<std::int64_t, std::int64_t> geometryForModel;
    std::map<std::int64_t, std::vector<std::int64_t>> materialsForModel;
    std::set<std::int64_t> referencedGeometry;
    for (const auto& link : links) {
        if (link.kind == "PO") {
            const auto source = byId.at(link.source);
            const auto target = link.target == 0 ? nullptr : &records[byId.at(link.target)];
            if (records[source].node->name == "Constraint" && target != nullptr &&
                target->node->name == "Model" && link.property == "Constrained Object")
                continue;
            fail("unsupported_connection", "FBX PO connection is outside the supported Constraint ownership subset", "Connections");
        }
        if (link.kind != "OO" && link.kind != "OP") continue;
        const auto source = byId.at(link.source);
        const auto sourceName = records[source].node->name;
        if (link.kind == "OO" && sourceName == "Model") {
            // Keep both conditional operands as views. A mixed literal/string
            // conditional would materialize a temporary std::string and leave
            // this view dangling before the classification below.
            const std::string_view targetName =
                link.target == 0
                    ? std::string_view{"Root"}
                    : std::string_view{records[byId.at(link.target)].node->name};
            // FBX display layers own models through OO edges, while skin
            // clusters use OO edges from a bone Model to a Deformer. These
            // are membership/attribute edges rather than hierarchy edges.
            if (targetName == "Collection" || targetName == "CollectionExclusive" || targetName == "Deformer") continue;
            if (targetName == "Model" || link.target == 0) {
                if (parent.contains(link.source)) fail("invalid_hierarchy", "FBX model has multiple parents", "Connections");
                chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::int64_t>), "scene/parents");
                parent.emplace(link.source, link.target);
            } else fail("invalid_reference", "FBX Model parent connection targets a non-Model object", "Connections");
        } else if (link.kind == "OO" && sourceName == "Geometry") {
            if (link.target == 0 || records[byId.at(link.target)].node->name != "Model") fail("invalid_reference", "FBX geometry is not connected to a Model", "Connections");
            if (!referencedGeometry.contains(link.source)) {
                chargeAssociativeNode(budget, sizeof(std::int64_t), "scene/referenced geometry");
                referencedGeometry.insert(link.source);
            }
            if (geometryForModel.contains(link.target))
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "multiple_geometry", "Multiple FBX geometries are connected to one Model; first is used", "Connections");
            else {
                chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::int64_t>), "scene/geometry assignments");
                geometryForModel.emplace(link.target, link.source);
            }
        } else if (sourceName == "Material" && link.target != 0 &&
                   records[byId.at(link.target)].node->name == "Model") {
            if (link.kind != "OO")
                fail("unsupported_connection",
                     "FBX node material assignment is not an OO connection",
                     "Connections");
            auto material_it = materialsForModel.find(link.target);
            if (material_it == materialsForModel.end()) {
                chargeAssociativeNode(
                    budget,
                    sizeof(std::pair<const std::int64_t,
                                     std::vector<std::int64_t>>),
                    "scene/material assignments");
                material_it =
                    materialsForModel
                        .emplace(link.target, std::vector<std::int64_t>{})
                        .first;
            }
            auto& model_materials = material_it->second;
            reserveForAppend(model_materials, model_materials.size() + 1u,
                             budget, "scene/material assignments");
            model_materials.push_back(link.source);
        }
    }
    for (const auto modelIndex : modelIndexes) {
        const auto child = records[modelIndex].id;
        const auto parentIt = parent.find(child);
        if (parentIt != parent.end()) {
            auto childIt = childrenByParent.find(parentIt->second);
            if (childIt == childrenByParent.end()) {
                chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::vector<std::int64_t>>), "scene/children map");
                childIt = childrenByParent.emplace(parentIt->second, std::vector<std::int64_t>{}).first;
            }
            auto& children = childIt->second;
            if (children.size() == children.capacity()) {
                const auto current = children.capacity();
                const auto next = current == 0u ? 1u : current > std::numeric_limits<std::size_t>::max() / 2u
                                                        ? std::numeric_limits<std::size_t>::max()
                                                        : current * 2u;
                budget.add(checkedMultiply(next - current, sizeof(std::int64_t), "scene/children map"), "scene/children map");
                children.reserve(next);
            }
            children.push_back(child);
        }
    }
    std::map<std::int64_t, std::uint32_t> meshIds;
    for (const auto geometryIndex : geometryIndexes) {
        const auto& record = records[geometryIndex];
        const auto path = "Objects/Geometry/" + std::to_string(record.id);
        budget.add(record.name.size(), path);
        FbxStaticMesh mesh;
        mesh.object_id = record.id;
        mesh.name = record.name;
        mesh.positions = geometryPositions(*record.node, path, limits, budget);
        const auto control_point_count = mesh.positions.size() / 3u;
        std::optional<FbxSkinBinding> control_point_skin;
        if (const auto skins = skinsForGeometry.find(record.id);
            skins != skinsForGeometry.end()) {
            if (skins->second.size() > 1u)
                addDiagnostic(
                    result, diagnostics, FbxConversionSeverity::warning,
                    "additional_skin_ignored",
                    "FBX Geometry has multiple skin deformers; only the first source-order deformer is used",
                    path, false);
            const auto skin_id = skins->second.front();
            const auto clusters = clustersForSkin.find(skin_id);
            if (clusters == clustersForSkin.end())
                fail("invalid_skin", "FBX skin has no linked clusters", path);
            control_point_skin = buildSkinBinding(
                skin_id, control_point_count, clusters->second,
                boneForCluster, records, byId, limits, budget, result,
                diagnostics, path);
        }
        const auto* uvLayer = firstChildNode(*record.node, "LayerElementUV");
        const auto* normalLayer =
            firstChildNode(*record.node, "LayerElementNormal");
        const FbxNode* materialLayer = nullptr;
        for (const auto& child : record.node->children) {
            if (child.name != "LayerElementMaterial") continue;
            if (materialLayer != nullptr)
                fail("invalid_material_layer",
                     "FBX Geometry contains multiple material layers", path);
            materialLayer = &child;
        }
        const auto* supportedUv =
            uvLayer != nullptr && supportedUvLayer(*uvLayer) ? uvLayer : nullptr;
        const auto* supportedNormal =
            normalLayer != nullptr && supportedNormalLayer(*normalLayer)
                ? normalLayer
                : nullptr;
        const auto* supportedMaterial =
            materialLayer != nullptr && supportedMaterialLayer(*materialLayer)
                ? materialLayer
                : nullptr;
        if (materialLayer == nullptr)
            addDiagnostic(
                result, diagnostics, FbxConversionSeverity::warning,
                "missing_material_layer",
                "FBX Geometry has no material layer; native polygon material behavior cannot be reproduced",
                path);
        if (supportedUv != nullptr || supportedNormal != nullptr ||
            supportedMaterial != nullptr) {
            auto expanded = geometryWithLayers(
                *record.node, supportedUv, supportedNormal, supportedMaterial,
                path, limits, budget, mesh.positions);
            mesh.positions = std::move(expanded.positions);
            mesh.uvs = std::move(expanded.uvs);
            mesh.normals = std::move(expanded.normals);
            mesh.triangle_indices = std::move(expanded.triangle_indices);
            mesh.triangle_material_slots =
                std::move(expanded.triangle_material_slots);
            if (control_point_skin.has_value()) {
                budget.add(checkedMultiply(
                               expanded.source_control_points.size(),
                               sizeof(FbxSkinInfluence), path),
                           path);
                std::vector<FbxSkinInfluence> expanded_influences;
                expanded_influences.reserve(
                    expanded.source_control_points.size());
                for (const auto control_point :
                     expanded.source_control_points) {
                    if (control_point >=
                        control_point_skin->vertex_influences.size())
                        fail("skin_index_invalid",
                             "Expanded FBX vertex references a missing skin control point",
                             path);
                    expanded_influences.push_back(
                        control_point_skin->vertex_influences[control_point]);
                }
                control_point_skin->vertex_influences =
                    std::move(expanded_influences);
            }
        } else {
            mesh.triangle_indices = geometryIndices(*record.node, path, mesh.positions.size() / 3u, limits, budget);
        }
        mesh.skin = std::move(control_point_skin);
        if (mesh.triangle_indices.empty()) fail("invalid_geometry", "FBX geometry has no triangles", path);
        if (result.meshes.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            fail("id_limit", "FBX mesh IDs exceed their uint32 range", path);
        chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::uint32_t>), "mesh IDs");
        meshIds.emplace(record.id, static_cast<std::uint32_t>(result.meshes.size()));
        result.meshes.push_back(std::move(mesh));
    }
    for (const auto geometryIndex : geometryIndexes) {
        const auto id = records[geometryIndex].id;
        if (!referencedGeometry.contains(id))
            addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unreferenced_geometry", "FBX Geometry is not connected to a Model", "Objects/Geometry/" + std::to_string(id));
    }
    std::set<std::int64_t> visiting;
    std::set<std::int64_t> emitted;
    const auto append = [&](auto&& self, std::int64_t modelId, NodeId parentId, Matrix4 parentWorld, std::size_t depth, std::string path) -> void {
        if (depth > limits.max_depth) fail("depth_limit", "FBX model hierarchy exceeds its limit", path);
        if (visiting.contains(modelId)) fail("invalid_hierarchy", "FBX model hierarchy contains a cycle", path);
        chargeAssociativeNode(budget, sizeof(std::int64_t), "scene/visiting models");
        visiting.insert(modelId);
        const auto recordIndex = byId.at(modelId);
        const auto& record = records[recordIndex];
        const auto local = localTransform(*record.node, path, limits, result, diagnostics);
        const auto world = multiply(parentWorld, local);
        finiteMatrix(world, path);
        if (result.snapshot.nodes.size() >= static_cast<std::size_t>(scene::invalid_node_id))
            fail("id_limit", "FBX node IDs exceed their uint32 range", path);
        const auto id = static_cast<NodeId>(result.snapshot.nodes.size());
        scene::NodeKind kind = scene::NodeKind::node;
        scene::MaterialId material = scene::invalid_material_id;
        std::uint32_t meshIndex = 0u;
        std::vector<scene::MaterialId> nodeMaterials;
        if (const auto geometry = geometryForModel.find(modelId); geometry != geometryForModel.end()) {
            const auto mesh = meshIds.find(geometry->second);
            if (mesh == meshIds.end()) fail("invalid_reference", "FBX Model references missing Geometry", path);
            kind = result.meshes[mesh->second].skin.has_value()
                       ? scene::NodeKind::skinned_mesh
                       : scene::NodeKind::mesh;
            meshIndex = mesh->second;
            const auto materialIt = materialsForModel.find(modelId);
            if (materialIt != materialsForModel.end()) {
                budget.add(checkedMultiply(materialIt->second.size(),
                                           sizeof(scene::MaterialId), path),
                           path);
                nodeMaterials.reserve(materialIt->second.size());
                for (const auto materialObject : materialIt->second) {
                    const auto materialIndex = materialIds.find(materialObject);
                    if (materialIndex == materialIds.end())
                        fail("invalid_reference",
                             "FBX Model references missing Material", path);
                    nodeMaterials.push_back(materialIndex->second);
                }
                if (!nodeMaterials.empty()) material = nodeMaterials.front();
            }
        }
        const bool has_geometry = kind == scene::NodeKind::mesh ||
                                  kind == scene::NodeKind::skinned_mesh;
        const auto geometric = has_geometry
                                   ? geometricTransform(*record.node, path)
                                   : scene::identity_matrix;
        const auto meshWorld = has_geometry
                                   ? multiply(world, geometric)
                                   : world;
        finiteMatrix(meshWorld, path);
        budget.add(record.name.size(), path);
        scene::SceneNode node{id, parentId, record.name, kind, modelVisible(*record.node, &budget), modelVisible(*record.node, &budget), has_geometry, false, true, 0u, 0.0F, 0.0F, material, {0.0F, 0.0F, 0.0F}, 0.0F, meshWorld, {}, {}, {}, {}, 0.0F, scene::LocalBoundsSource::unavailable, {}};
        if (has_geometry) {
            const auto& mesh = result.meshes[meshIndex];
            std::array<double, 3> minimum = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
            std::array<double, 3> maximum = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
            std::array<double, 3> localMinimum = minimum;
            std::array<double, 3> localMaximum = maximum;
            const auto transformedPoint = [&](std::size_t index) {
                return std::array<double, 3>{static_cast<double>(meshWorld[0]) * mesh.positions[index] + static_cast<double>(meshWorld[4]) * mesh.positions[index + 1u] + static_cast<double>(meshWorld[8]) * mesh.positions[index + 2u] + static_cast<double>(meshWorld[12]), static_cast<double>(meshWorld[1]) * mesh.positions[index] + static_cast<double>(meshWorld[5]) * mesh.positions[index + 1u] + static_cast<double>(meshWorld[9]) * mesh.positions[index + 2u] + static_cast<double>(meshWorld[13]), static_cast<double>(meshWorld[2]) * mesh.positions[index] + static_cast<double>(meshWorld[6]) * mesh.positions[index + 1u] + static_cast<double>(meshWorld[10]) * mesh.positions[index + 2u] + static_cast<double>(meshWorld[14])};
            };
            for (std::size_t index = 0; index < mesh.positions.size(); index += 3u) {
                const auto transformed = transformedPoint(index);
                for (const auto value : transformed) if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) fail("non_finite", "FBX world position is not representable as float", path);
                for (std::size_t axis = 0; axis < 3u; ++axis) {
                    minimum[axis] = std::min(minimum[axis], transformed[axis]);
                    maximum[axis] = std::max(maximum[axis], transformed[axis]);
                    const double localPosition =
                        static_cast<double>(mesh.positions[index + axis]);
                    localMinimum[axis] =
                        std::min(localMinimum[axis], localPosition);
                    localMaximum[axis] =
                        std::max(localMaximum[axis], localPosition);
                }
            }
            const std::array<double, 3> center = {(minimum[0] + maximum[0]) * 0.5, (minimum[1] + maximum[1]) * 0.5, (minimum[2] + maximum[2]) * 0.5};
            scene::Vector3 localCenter{};
            for (std::size_t axis = 0; axis < 3u; ++axis) {
                if (!std::isfinite(center[axis]) || std::abs(center[axis]) > static_cast<double>(std::numeric_limits<float>::max())) fail("non_finite", "FBX bounds center is not representable as float", path);
                node.bounds_center[axis] = static_cast<float>(center[axis]);
                const double localCenterValue =
                    (localMinimum[axis] + localMaximum[axis]) * 0.5;
                if (!std::isfinite(localCenterValue) ||
                    std::abs(localCenterValue) > static_cast<double>(
                                                     std::numeric_limits<float>::max()))
                    fail("non_finite", "FBX local bounds center is not representable as float", path);
                localCenter[axis] = static_cast<float>(localCenterValue);
            }
            node.local_aabb_center = localCenter;
            double radius = 0.0;
            for (std::size_t index = 0; index < mesh.positions.size(); index += 3u) {
                const auto point = transformedPoint(index);
                const auto dx = point[0] - center[0];
                const auto dy = point[1] - center[1];
                const auto dz = point[2] - center[2];
                radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            if (!std::isfinite(radius) || radius > static_cast<double>(std::numeric_limits<float>::max())) fail("non_finite", "FBX bounds radius is not representable as float", path);
            node.bounds_radius = static_cast<float>(radius);
            result.node_geometry.push_back(
                {id, meshIndex, geometric, std::move(nodeMaterials)});
        }
        result.snapshot.nodes.push_back(std::move(node));
        result.transforms.push_back({id, local, world});
        auto& outputChildren = result.snapshot.nodes[parentId].children;
        if (outputChildren.size() == outputChildren.capacity()) {
            const auto current = outputChildren.capacity();
            const auto next = current == 0u ? 1u : current > std::numeric_limits<std::size_t>::max() / 2u
                                                    ? std::numeric_limits<std::size_t>::max()
                                                    : current * 2u;
            budget.add(checkedMultiply(next - current, sizeof(NodeId), "scene/children"), "scene/children");
            outputChildren.reserve(next);
        }
        outputChildren.push_back(id);
        if (emitted.contains(modelId)) fail("invalid_hierarchy", "FBX Model is emitted more than once", path);
        chargeAssociativeNode(budget, sizeof(std::int64_t), "scene/emitted models");
        emitted.insert(modelId);
        const auto children = childrenByParent.find(modelId);
        if (children != childrenByParent.end())
            for (const auto childId : children->second)
                self(self, childId, id, world, depth + 1u, childPath(path, static_cast<std::size_t>(childId), limits));
        visiting.erase(modelId);
    };
    for (const auto modelIndex : modelIndexes) if (!parent.contains(records[modelIndex].id) || parent.at(records[modelIndex].id) == 0) {
        const auto modelPath = boundedPath("Models/" + std::to_string(records[modelIndex].id), limits);
        budget.add(modelPath.size(), "model paths");
        append(append, records[modelIndex].id, 0u, scene::identity_matrix, 0u, modelPath);
    }
    for (const auto modelIndex : modelIndexes) if (!emitted.contains(records[modelIndex].id)) fail("invalid_hierarchy", "FBX model hierarchy has an unreachable cycle", "Models");
    extractAnimations(records, byId, modelIndexes, links, limits, result, budget, diagnostics);
    if (result.animations.empty()) {
        bool hasAnimationRecord = false;
        std::vector<const FbxNode*> pending;
        for (const auto& root : document.roots) pending.push_back(&root);
        while (!pending.empty()) {
            const auto* node = pending.back(); pending.pop_back();
            if (node->name == "AnimationStack" || node->name == "AnimationLayer" ||
                node->name == "AnimationCurve" || node->name == "AnimationCurveNode") hasAnimationRecord = true;
            for (const auto& child : node->children) pending.push_back(&child);
        }
        if (hasAnimationRecord)
            addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "unsupported_animation",
                          "FBX animation records did not produce a supported KSANIM clip", "animation");
    }
    double sceneRadius = 0.0;
    for (const auto& node : result.snapshot.nodes) {
        const auto centerDistance = std::hypot(std::hypot(static_cast<double>(node.bounds_center[0]), static_cast<double>(node.bounds_center[1])), static_cast<double>(node.bounds_center[2]));
        const auto candidate = centerDistance + static_cast<double>(node.bounds_radius);
        if (!std::isfinite(candidate) || candidate > static_cast<double>(std::numeric_limits<float>::max()))
            fail("non_finite", "FBX scene bounds radius is not representable as float", "scene");
        sceneRadius = std::max(sceneRadius, candidate);
    }
    result.snapshot.bounds_radius = static_cast<float>(sceneRadius);
    return result;
}

}  // namespace apex::formats
