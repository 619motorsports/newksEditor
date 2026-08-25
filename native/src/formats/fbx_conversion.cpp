#include "apex/formats/fbx_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
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
};

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
                   std::string_view message, std::string_view path) {
    result.complete = false;
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

struct ParsedUvLayer {
    const FbxArray* direct = nullptr;
    const std::vector<std::int64_t>* indices = nullptr;
    std::size_t directCount = 0u;
};

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
};

struct ExpandedUvGeometry {
    std::vector<float> positions;
    std::vector<float> uvs;
    std::vector<std::uint32_t> triangle_indices;
};

double uvComponent(const ParsedUvLayer& layer, std::size_t index) {
    if (const auto doubles = std::get_if<std::vector<double>>(layer.direct)) {
        return (*doubles)[index];
    }
    return static_cast<double>((*std::get_if<std::vector<float>>(layer.direct))[index]);
}

ExpandedUvGeometry geometryWithUvs(const FbxNode& geometry, const FbxNode& layer,
                                   std::string_view path, const FbxConversionLimits& limits,
                                   Budget& budget, const std::vector<float>& controlPositions) {
    const auto parsed = parseUvLayer(layer, path, limits, budget);
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
    if (parsed.indices->size() != encoded->size())
        fail("invalid_uv", "FBX UVIndex count " + std::to_string(parsed.indices->size()) +
                              " does not match polygon vertex count " + std::to_string(encoded->size()), std::string(path));

    const auto vertexCount = controlPositions.size() / 3u;
    if (vertexCount >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "FBX vertex indices exceed their uint32 range", std::string(path));
    const auto maxPolygonVertices = checkedAdd(limits.max_indices / 3u, 2u, path);
    std::size_t polygonSize = 0u;
    std::size_t outputVertices = 0u;
    for (std::size_t index = 0u; index < encoded->size(); ++index) {
        const auto value = (*encoded)[index];
        const auto decoded = value < 0 ? ~value : value;
        if (decoded < 0 || static_cast<std::uint64_t>(decoded) >= vertexCount)
            fail("invalid_index", "FBX polygon index references a missing vertex", std::string(path));
        const auto uvIndex = (*parsed.indices)[index];
        if (uvIndex < 0 || static_cast<std::uint64_t>(uvIndex) >= parsed.directCount)
            fail("invalid_uv", "FBX UVIndex references a missing direct UV", std::string(path));
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
        }
    }
    if (polygonSize != 0u)
        fail("invalid_geometry", "FBX polygon index stream lacks a terminal index", std::string(path));
    if (outputVertices > limits.max_vertices)
        fail("vertex_limit", "expanded FBX UV geometry exceeds the vertex limit", std::string(path));
    if (outputVertices >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        fail("id_limit", "expanded FBX UV geometry exceeds its uint32 range", std::string(path));

    const auto positionValues = checkedMultiply(outputVertices, 3u, path);
    const auto uvValues = checkedMultiply(outputVertices, 2u, path);
    budget.add(checkedMultiply(positionValues, sizeof(float), path), path);
    budget.add(checkedMultiply(uvValues, sizeof(float), path), path);
    budget.add(checkedMultiply(outputVertices, sizeof(std::uint32_t), path), path);
    ExpandedUvGeometry result;
    result.positions.reserve(positionValues);
    result.uvs.reserve(uvValues);
    result.triangle_indices.reserve(outputVertices);

    std::vector<UvCorner> polygonCorners;
    const auto appendCorner = [&](const UvCorner corner) {
        const auto positionOffset = checkedMultiply(static_cast<std::size_t>(corner.position), 3u, path);
        const auto uvOffset = checkedMultiply(corner.uv, 2u, path);
        const auto outputIndex = result.positions.size() / 3u;
        if (outputIndex >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            fail("id_limit", "expanded FBX UV geometry exceeds its uint32 range", std::string(path));
        result.positions.insert(result.positions.end(), controlPositions.begin() + static_cast<std::ptrdiff_t>(positionOffset),
                                controlPositions.begin() + static_cast<std::ptrdiff_t>(positionOffset + 3u));
        result.uvs.push_back(static_cast<float>(uvComponent(parsed, uvOffset)));
        result.uvs.push_back(-static_cast<float>(uvComponent(parsed, uvOffset + 1u)));
        result.triangle_indices.push_back(static_cast<std::uint32_t>(outputIndex));
    };
    const auto appendTriangle = [&](const UvCorner& first, const UvCorner& second, const UvCorner& third) {
        appendCorner(first); appendCorner(second); appendCorner(third);
    };
    for (std::size_t index = 0u; index < encoded->size(); ++index) {
        const auto value = (*encoded)[index];
        const auto decoded = value < 0 ? ~value : value;
        if (polygonCorners.size() >= maxPolygonVertices)
            fail("index_limit", "FBX polygon vertex count exceeds its limit", std::string(path));
        reserveForAppend(polygonCorners, polygonCorners.size() + 1u, budget, path);
        polygonCorners.push_back({static_cast<std::uint32_t>(decoded), static_cast<std::size_t>((*parsed.indices)[index])});
        if (value < 0) {
            for (std::size_t corner = 1u; corner + 1u < polygonCorners.size(); ++corner)
                appendTriangle(polygonCorners[0], polygonCorners[corner], polygonCorners[corner + 1u]);
            polygonCorners.clear();
        }
    }
    if (result.positions.size() != positionValues || result.uvs.size() != uvValues ||
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
            if (node.name == "Texture" || node.name == "Video" || node.name == "Image") add("unsupported_images", "FBX image and texture records are not converted");
            if (node.name == "LayerElementNormal" || node.name == "LayerElementMaterial" ||
                (node.name == "LayerElementUV" && !supportedUvLayer(node)))
                add("unsupported_layer_mapping", "FBX layer-element mappings are not converted");
            if (node.name == "Deformer" || node.name == "Skin" || node.name == "Cluster") add("unsupported_skinning", "FBX skinning and deformers are not converted");
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
                if (text == nullptr) continue;
                if (text->size() > limits.max_string_bytes || stringBytes > limits.max_string_bytes - text->size())
                    fail("string_limit", "FBX DOM string storage exceeds conversion limit", "DOM");
                stringBytes += text->size();
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
                        static_cast<long double>(start) + span * frameIndex / 100.0L);
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

FbxConversionCapabilityDetail fbxSceneConversionCapability() {
    return {true, true, true, false, true, false, false,
            "Static FBX geometry, transforms, material assignments, one bounded UV mapping, and an explicit-linear local-transform KSANIM bridge are converted; native-pivot evaluation, non-linear animation, skinning, and images remain unsupported"};
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
    const auto objectBudget = limits.max_nodes > std::numeric_limits<std::size_t>::max() - limits.max_materials ||
                                      limits.max_nodes + limits.max_materials > std::numeric_limits<std::size_t>::max() - limits.max_meshes
                                  ? std::numeric_limits<std::size_t>::max()
                                  : limits.max_nodes + limits.max_materials + limits.max_meshes;
    if (objects->children.size() > objectBudget)
        fail("node_limit", "FBX Objects count exceeds conversion limits", "Objects");
    std::size_t modelCount = 0u;
    std::size_t materialCount = 0u;
    std::size_t geometryCount = 0u;
    for (const auto& node : objects->children) {
        if (node.name == "Model") ++modelCount;
        else if (node.name == "Material") ++materialCount;
        else if (node.name == "Geometry") ++geometryCount;
    }
    if (modelCount > limits.max_nodes || materialCount > limits.max_materials || geometryCount > limits.max_meshes)
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
        else if (node.name == "Deformer") result.complete = false;
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
    budget.add(checkedMultiply(modelIndexes.size(), sizeof(NodeId), "scene/children"), "scene/children");
    result.snapshot.nodes.reserve(modelIndexes.size() + 1u);
    result.snapshot.materials.reserve(materialIndexes.size());
    result.meshes.reserve(geometryIndexes.size());
    result.transforms.reserve(modelIndexes.size());
    result.node_geometry.reserve(geometryIndexes.size());
    result.snapshot.nodes.push_back({0u, scene::invalid_node_id, "FBX", scene::NodeKind::node, true, true, false, false, true, 0u, 0.0F, 0.0F, scene::invalid_material_id, {0.0F, 0.0F, 0.0F}, 0.0F, scene::identity_matrix, {}, {}, {}});
    result.snapshot.root = 0u;
    std::map<std::int64_t, std::uint32_t> materialIds;
    for (std::size_t index = 0; index < materialIndexes.size(); ++index) {
        const auto& record = records[materialIndexes[index]];
        std::string_view shaderView = record.type;
        for (const auto& child : record.node->children) if (child.name == "ShadingModel") {
            const auto flattened = values(child, &budget, "scene/materials");
            if (!flattened.empty() && stringValue(flattened[0]) != nullptr) shaderView = *stringValue(flattened[0]);
        }
        budget.add(checkedAdd(record.name.size(), shaderView.size(), "scene/materials"), "scene/materials");
        std::string shader(shaderView);
        result.snapshot.materials.push_back({record.name, shader, scene::BlendMode::opaque});
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
            Link link{*kindValue, sourceId, targetId, propertyValue == nullptr ? std::string() : *propertyValue};
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
    std::map<std::int64_t, std::int64_t> parent;
    std::map<std::int64_t, std::vector<std::int64_t>> childrenByParent;
    std::map<std::int64_t, std::int64_t> geometryForModel;
    std::map<std::int64_t, std::int64_t> materialForModel;
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
        } else if ((link.kind == "OO" || link.kind == "OP") && sourceName == "Material") {
            if (link.target == 0 || records[byId.at(link.target)].node->name != "Model") continue;
            if (materialForModel.contains(link.target))
                addDiagnostic(result, diagnostics, FbxConversionSeverity::warning, "multiple_materials", "Multiple FBX materials are connected to one Model; first is used", "Connections");
            else {
                chargeAssociativeNode(budget, sizeof(std::pair<const std::int64_t, std::int64_t>), "scene/material assignments");
                materialForModel.emplace(link.target, link.source);
            }
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
        const auto* uvLayer = firstChildNode(*record.node, "LayerElementUV");
        if (uvLayer != nullptr && supportedUvLayer(*uvLayer)) {
            auto expanded = geometryWithUvs(*record.node, *uvLayer, path, limits, budget, mesh.positions);
            mesh.positions = std::move(expanded.positions);
            mesh.uvs = std::move(expanded.uvs);
            mesh.triangle_indices = std::move(expanded.triangle_indices);
        } else {
            mesh.triangle_indices = geometryIndices(*record.node, path, mesh.positions.size() / 3u, limits, budget);
        }
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
        if (const auto geometry = geometryForModel.find(modelId); geometry != geometryForModel.end()) {
            const auto mesh = meshIds.find(geometry->second);
            if (mesh == meshIds.end()) fail("invalid_reference", "FBX Model references missing Geometry", path);
            kind = scene::NodeKind::mesh; meshIndex = mesh->second;
            const auto materialIt = materialForModel.find(modelId);
            if (materialIt != materialForModel.end()) {
                const auto materialIndex = materialIds.find(materialIt->second);
                if (materialIndex == materialIds.end()) fail("invalid_reference", "FBX Model references missing Material", path);
                material = materialIndex->second;
            }
        }
        const auto meshWorld = kind == scene::NodeKind::mesh
                                   ? multiply(world, geometricTransform(*record.node, path))
                                   : world;
        finiteMatrix(meshWorld, path);
        budget.add(record.name.size(), path);
        scene::SceneNode node{id, parentId, record.name, kind, modelVisible(*record.node, &budget), modelVisible(*record.node, &budget), kind == scene::NodeKind::mesh, false, true, 0u, 0.0F, 0.0F, material, {0.0F, 0.0F, 0.0F}, 0.0F, meshWorld, {}, {}, {}};
        if (kind == scene::NodeKind::mesh) {
            const auto& mesh = result.meshes[meshIndex];
            std::array<double, 3> minimum = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
            std::array<double, 3> maximum = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
            const auto transformedPoint = [&](std::size_t index) {
                return std::array<double, 3>{static_cast<double>(meshWorld[0]) * mesh.positions[index] + static_cast<double>(meshWorld[4]) * mesh.positions[index + 1u] + static_cast<double>(meshWorld[8]) * mesh.positions[index + 2u] + static_cast<double>(meshWorld[12]), static_cast<double>(meshWorld[1]) * mesh.positions[index] + static_cast<double>(meshWorld[5]) * mesh.positions[index + 1u] + static_cast<double>(meshWorld[9]) * mesh.positions[index + 2u] + static_cast<double>(meshWorld[13]), static_cast<double>(meshWorld[2]) * mesh.positions[index] + static_cast<double>(meshWorld[6]) * mesh.positions[index + 1u] + static_cast<double>(meshWorld[10]) * mesh.positions[index + 2u] + static_cast<double>(meshWorld[14])};
            };
            for (std::size_t index = 0; index < mesh.positions.size(); index += 3u) {
                const auto transformed = transformedPoint(index);
                for (const auto value : transformed) if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) fail("non_finite", "FBX world position is not representable as float", path);
                for (std::size_t axis = 0; axis < 3u; ++axis) { minimum[axis] = std::min(minimum[axis], transformed[axis]); maximum[axis] = std::max(maximum[axis], transformed[axis]); }
            }
            const std::array<double, 3> center = {(minimum[0] + maximum[0]) * 0.5, (minimum[1] + maximum[1]) * 0.5, (minimum[2] + maximum[2]) * 0.5};
            for (std::size_t axis = 0; axis < 3u; ++axis) {
                if (!std::isfinite(center[axis]) || std::abs(center[axis]) > static_cast<double>(std::numeric_limits<float>::max())) fail("non_finite", "FBX bounds center is not representable as float", path);
                node.bounds_center[axis] = static_cast<float>(center[axis]);
            }
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
            result.node_geometry.push_back({id, meshIndex});
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
