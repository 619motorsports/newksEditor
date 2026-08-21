#include "apex/formats/fbx_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
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
                                   name == "Lcl Scaling" || name == "Visibility";
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
            if (node.name == "AnimationStack" || node.name == "AnimationLayer" || node.name == "AnimationCurve" || node.name == "AnimationCurveNode") add("unsupported_animation", "FBX animation records are not converted");
            if (node.name == "Texture" || node.name == "Video" || node.name == "Image") add("unsupported_images", "FBX image and texture records are not converted");
            if (node.name == "LayerElementNormal" || node.name == "LayerElementUV" || node.name == "LayerElementMaterial") add("unsupported_layer_mapping", "FBX layer-element mappings are not converted");
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

}  // namespace

FbxConversionError::FbxConversionError(FbxConversionDiagnostic diagnostic)
    : std::runtime_error(diagnostic.message), diagnostic_(std::move(diagnostic)) {}

FbxConversionCapabilityDetail fbxSceneConversionCapability() {
    return {true, true, true, false, false, false, false,
            "Static FBX geometry, transforms, and material assignments are converted; skinning, animation, images, and layer mappings are explicitly unsupported"};
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
            std::int64_t sourceId = 0, targetId = 0;
            if (!integerValue(flattened[1], sourceId) || !integerValue(flattened[2], targetId))
                fail("invalid_reference", "FBX connection IDs are invalid", path);
            const auto* kindValue = stringValue(flattened[0]);
            const auto* propertyValue = flattened.size() > 3u ? stringValue(flattened[3]) : nullptr;
            const auto kindBytes = kindValue->size();
            const auto propertyBytes = propertyValue == nullptr ? std::size_t{0u} : propertyValue->size();
            budget.add(checkedAdd(kindBytes, propertyBytes, path), path);
            Link link{*kindValue, sourceId, targetId, propertyValue == nullptr ? std::string() : *propertyValue};
            if (byId.find(link.source) == byId.end() || (link.target != 0 && byId.find(link.target) == byId.end()))
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
        if (link.kind != "OO" && link.kind != "OP") continue;
        const auto source = byId.at(link.source);
        const auto sourceName = records[source].node->name;
        if (link.kind == "OO" && sourceName == "Model") {
            const auto targetName = link.target == 0 ? std::string("Root") : records[byId.at(link.target)].node->name;
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
        auto mesh = FbxStaticMesh{record.id, record.name, geometryPositions(*record.node, path, limits, budget), {}};
        mesh.triangle_indices = geometryIndices(*record.node, path, mesh.positions.size() / 3u, limits, budget);
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
        budget.add(record.name.size(), path);
        scene::SceneNode node{id, parentId, record.name, kind, modelVisible(*record.node, &budget), modelVisible(*record.node, &budget), kind == scene::NodeKind::mesh, false, true, 0u, 0.0F, 0.0F, material, {0.0F, 0.0F, 0.0F}, 0.0F, world, {}, {}, {}};
        if (kind == scene::NodeKind::mesh) {
            const auto& mesh = result.meshes[meshIndex];
            std::array<double, 3> minimum = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
            std::array<double, 3> maximum = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
            const auto transformedPoint = [&](std::size_t index) {
                return std::array<double, 3>{static_cast<double>(world[0]) * mesh.positions[index] + static_cast<double>(world[4]) * mesh.positions[index + 1u] + static_cast<double>(world[8]) * mesh.positions[index + 2u] + static_cast<double>(world[12]), static_cast<double>(world[1]) * mesh.positions[index] + static_cast<double>(world[5]) * mesh.positions[index + 1u] + static_cast<double>(world[9]) * mesh.positions[index + 2u] + static_cast<double>(world[13]), static_cast<double>(world[2]) * mesh.positions[index] + static_cast<double>(world[6]) * mesh.positions[index + 1u] + static_cast<double>(world[10]) * mesh.positions[index + 2u] + static_cast<double>(world[14])};
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
    for (const auto modelIndex : modelIndexes) if (!parent.contains(records[modelIndex].id)) {
        const auto modelPath = boundedPath("Models/" + std::to_string(records[modelIndex].id), limits);
        budget.add(modelPath.size(), "model paths");
        append(append, records[modelIndex].id, 0u, scene::identity_matrix, 0u, modelPath);
    }
    for (const auto modelIndex : modelIndexes) if (!emitted.contains(records[modelIndex].id)) fail("invalid_hierarchy", "FBX model hierarchy has an unreachable cycle", "Models");
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
