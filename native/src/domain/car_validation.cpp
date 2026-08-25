#include "apex/domain/car_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace apex::domain {
namespace {

using Node = apex::formats::Kn5Node;
using Matrix = apex::formats::Kn5Matrix4;
using Point = std::array<float, 3>;

constexpr std::array<std::string_view, 14> kRequired = {
    "SUSP_LF", "SUSP_LR", "SUSP_RF", "SUSP_RR", "WHEEL_LF", "WHEEL_LR",
    "WHEEL_RF", "WHEEL_RR", "COCKPIT_LR", "STEER_LR", "DISC_LF", "DISC_LR",
    "DISC_RF", "DISC_RR"};
constexpr std::array<std::string_view, 4> kCorners = {"LF", "LR", "RF", "RR"};

struct Visit {
    const Node* node = nullptr;
    std::size_t depth = 0;
    Matrix world{};
};

struct BoundsAccumulator {
    Point minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity()};
    Point maximum = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()};
    bool has_values = false;

    void include(Point value) noexcept {
        has_values = true;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], value[axis]);
            maximum[axis] = std::max(maximum[axis], value[axis]);
        }
    }

    [[nodiscard]] std::optional<CarBounds> finish() const {
        if (!has_values) return std::nullopt;
        CarBounds result{minimum, maximum, {}, {}};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double minimum_value = static_cast<double>(minimum[axis]);
            const double maximum_value = static_cast<double>(maximum[axis]);
            const double size = maximum_value - minimum_value;
            const double center = minimum_value + size * 0.5;
            if (!std::isfinite(size) || !std::isfinite(center) ||
                size > static_cast<double>(std::numeric_limits<float>::max()) ||
                std::abs(center) > static_cast<double>(std::numeric_limits<float>::max()))
                return std::nullopt;
            result.size[axis] = static_cast<float>(size);
            result.center[axis] = static_cast<float>(center);
        }
        return result;
    }
};

void finding(CarValidationReport& report, const CarValidationLimits& limits,
             CarFindingSeverity severity, std::string_view code,
             std::string_view message, std::string_view node = {}) {
    if (severity == CarFindingSeverity::error) ++report.errors;
    else ++report.warnings;
    if (report.findings.size() >= limits.maxFindings) return;
    report.findings.push_back({severity, std::string(code), std::string(message), std::string(node)});
}

void lod_finding(CarLodValidationReport& report, const CarValidationLimits& limits,
                 CarFindingSeverity severity, std::string_view code,
                 std::string message, std::string_view file = {},
                 std::string_view node = {}) {
    if (severity == CarFindingSeverity::error) ++report.errors;
    else ++report.warnings;
    if (report.findings.size() >= limits.maxFindings) return;
    report.findings.push_back({severity, std::string(code), std::move(message),
                               std::string(file), std::string(node)});
}

[[nodiscard]] std::string upper_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z'))
            character = static_cast<char>(byte - ('a' - 'A'));
    }
    return result;
}

[[nodiscard]] bool finite_matrix(const Matrix& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

[[nodiscard]] Matrix multiply(const Matrix& left, const Matrix& right) noexcept {
    Matrix result{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t index = 0; index < 4; ++index)
                result[column * 4 + row] += left[index * 4 + row] * right[column * 4 + index];
    return result;
}

[[nodiscard]] bool finite_point(Point value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float component) { return std::isfinite(component); });
}

[[nodiscard]] Point point(const Matrix& matrix, float x, float y, float z) noexcept {
    return {matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
            matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
            matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]};
}

[[nodiscard]] std::size_t vertex_count(const Node& node) noexcept {
    return node.vertexStride == 0 ? 0 : node.vertices.size() / node.vertexStride;
}

[[nodiscard]] bool valid_kind(const Node& node) noexcept {
    return (node.type == 1U && node.kind == "node") ||
           (node.type == 2U && node.kind == "mesh") ||
           (node.type == 3U && node.kind == "skinnedMesh");
}

struct InputCheck {
    bool safe = true;
    std::vector<Visit> visits;
};

[[nodiscard]] InputCheck validate_input(const apex::formats::Kn5File& model,
                                        CarValidationReport& report,
                                        const CarValidationLimits& limits,
                                        bool retain_visits) {
    InputCheck result;
    if (model.materials.size() > limits.maxMaterials) {
        finding(report, limits, CarFindingSeverity::error, "MATERIAL_LIMIT", "material count exceeds validation limit");
        result.safe = false;
    }
    if (model.textures.size() > limits.maxTextures) {
        finding(report, limits, CarFindingSeverity::error, "TEXTURE_LIMIT", "texture count exceeds validation limit");
        result.safe = false;
    }
    report.materials = model.materials.size();
    report.textures = model.textures.size();
    if (!result.safe) return result;
    const auto check_string = [&](std::string_view value, std::string_view what) {
        if (value.size() > limits.maxStringBytes) {
            finding(report, limits, CarFindingSeverity::error, "STRING_LIMIT", std::string(what) + " exceeds validation limit");
            result.safe = false;
        }
    };
    for (const auto& texture : model.textures) check_string(texture.name, "texture name");
    std::size_t property_total = 0;
    std::size_t resource_total = 0;
    bool nested_materials_bounded = true;
    for (const auto& material : model.materials) {
        if (material.properties.size() > limits.maxMaterialProperties ||
            property_total > limits.maxMaterialProperties - material.properties.size()) {
            finding(report, limits, CarFindingSeverity::error, "MATERIAL_PROPERTY_LIMIT", "aggregate material property count exceeds validation limit");
            nested_materials_bounded = false;
        } else property_total += material.properties.size();
        if (material.resources.size() > limits.maxMaterialResources ||
            resource_total > limits.maxMaterialResources - material.resources.size()) {
            finding(report, limits, CarFindingSeverity::error, "MATERIAL_RESOURCE_LIMIT", "aggregate material resource count exceeds validation limit");
            nested_materials_bounded = false;
        } else resource_total += material.resources.size();
    }
    report.material_properties = property_total;
    report.material_resources = resource_total;
    for (const auto& material : model.materials) {
        check_string(material.name, "material name");
        check_string(material.shader, "material shader");
        if (!nested_materials_bounded) continue;
        for (const auto& property : material.properties) {
            check_string(property.name, "material property name");
            if (!std::isfinite(property.value) ||
                !std::all_of(property.value2.begin(), property.value2.end(), [](float value) { return std::isfinite(value); }) ||
                !std::all_of(property.value3.begin(), property.value3.end(), [](float value) { return std::isfinite(value); }) ||
                !std::all_of(property.value4.begin(), property.value4.end(), [](float value) { return std::isfinite(value); })) {
                finding(report, limits, CarFindingSeverity::error, "NON_FINITE", "material property contains a non-finite value");
                result.safe = false;
            }
        }
        for (const auto& resource : material.resources) {
            check_string(resource.slot, "material resource slot");
            check_string(resource.texture, "material resource texture");
        }
    }

    std::size_t bone_total = 0;
    if (limits.maxNodes == 0) {
        finding(report, limits, CarFindingSeverity::error, "NODE_LIMIT", "KN5 node count exceeds validation limit");
        result.safe = false;
        return result;
    }
    std::size_t scheduled_nodes = 1;
    std::vector<Visit> stack;
    stack.push_back({&model.root, 0, apex::formats::Kn5Matrix4{
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}});
    while (!stack.empty()) {
        const Visit current = stack.back();
        stack.pop_back();
        if (current.depth > limits.maxDepth) {
            finding(report, limits, CarFindingSeverity::error, "DEPTH_LIMIT", "KN5 hierarchy exceeds validation depth limit");
            result.safe = false;
            break;
        }
        if (report.nodes >= limits.maxNodes) {
            finding(report, limits, CarFindingSeverity::error, "NODE_LIMIT", "KN5 node count exceeds validation limit");
            result.safe = false;
            break;
        }
        ++report.nodes;
        const Node& node = *current.node;
        check_string(node.name, "node name");
        if (!valid_kind(node)) {
            finding(report, limits, CarFindingSeverity::error, "NODE_IDENTITY", "KN5 node type and kind do not agree", node.name);
            result.safe = false;
        }
        if (!finite_matrix(node.transform)) {
            finding(report, limits, CarFindingSeverity::error, "NON_FINITE_MATRIX", "KN5 node transform is not finite", node.name);
            result.safe = false;
        }
        const auto world = multiply(current.world, node.transform);
        if (!finite_matrix(world)) {
            finding(report, limits, CarFindingSeverity::error, "NON_FINITE_WORLD", "KN5 world transform is not finite", node.name);
            result.safe = false;
        }
        if (node.type == 2U || node.type == 3U) {
            if (report.meshes >= limits.maxMeshes) {
                finding(report, limits, CarFindingSeverity::error, "MESH_LIMIT", "KN5 mesh count exceeds validation limit", node.name);
                result.safe = false;
            } else ++report.meshes;
            if (node.vertexStride < 3U || node.vertices.size() % node.vertexStride != 0U) {
                finding(report, limits, CarFindingSeverity::error, "INVALID_VERTEX_LAYOUT", "mesh vertex data does not contain complete positions", node.name);
                result.safe = false;
            }
            const auto count = vertex_count(node);
            if (count > limits.maxVertices || report.vertices > limits.maxVertices - count) {
                finding(report, limits, CarFindingSeverity::error, "VERTEX_LIMIT", "mesh vertex count exceeds validation limit", node.name);
                result.safe = false;
            } else report.vertices += count;
            if (node.indices.size() % 3U != 0U) {
                finding(report, limits, CarFindingSeverity::error, "INVALID_INDEX_COUNT", "mesh index count is not a triangle multiple", node.name);
                result.safe = false;
            }
            if (node.indices.size() > limits.maxIndices || report.indices > limits.maxIndices - node.indices.size()) {
                finding(report, limits, CarFindingSeverity::error, "INDEX_LIMIT", "mesh index count exceeds validation limit", node.name);
                result.safe = false;
            } else report.indices += node.indices.size();
            for (const auto value : node.vertices) if (!std::isfinite(value)) {
                finding(report, limits, CarFindingSeverity::error, "NON_FINITE_VERTEX", "mesh vertex data contains a non-finite value", node.name);
                result.safe = false;
                break;
            }
            for (const auto index : node.indices) if (static_cast<std::size_t>(index) >= count) {
                finding(report, limits, CarFindingSeverity::error, "INVALID_INDEX", "mesh index is outside the vertex array", node.name);
                result.safe = false;
                break;
            }
            if (node.materialId >= model.materials.size()) {
                finding(report, limits, CarFindingSeverity::error, "INVALID_MATERIAL_INDEX", "mesh material index is outside the material table", node.name);
                result.safe = false;
            }
            const bool bones_bounded = node.bones.size() <= limits.maxBones &&
                                       bone_total <= limits.maxBones - node.bones.size();
            if (!bones_bounded) {
                finding(report, limits, CarFindingSeverity::error, "BONE_LIMIT", "aggregate mesh bone count exceeds validation limit", node.name);
                result.safe = false;
            } else {
                bone_total += node.bones.size();
                report.bones = bone_total;
                for (const auto& bone : node.bones) {
                    check_string(bone.name, "bone name");
                    if (!finite_matrix(bone.transform)) {
                        finding(report, limits, CarFindingSeverity::error, "NON_FINITE_BONE", "bone transform is not finite", node.name);
                        result.safe = false;
                    }
                }
            }
        }
        if (retain_visits) result.visits.push_back({&node, current.depth, world});
        if (node.children.size() > limits.maxNodes - scheduled_nodes) {
            finding(report, limits, CarFindingSeverity::error, "NODE_SCHEDULE_LIMIT", "scheduled KN5 node count exceeds validation limit", node.name);
            result.safe = false;
            break;
        }
        if (current.depth >= limits.maxDepth && !node.children.empty()) {
            finding(report, limits, CarFindingSeverity::error, "DEPTH_LIMIT", "KN5 hierarchy exceeds validation depth limit", node.name);
            result.safe = false;
            break;
        }
        scheduled_nodes += node.children.size();
        for (auto iterator = node.children.rbegin(); iterator != node.children.rend(); ++iterator)
            stack.push_back({&*iterator, current.depth + 1U, world});
    }
    report.triangles = report.indices / 3U;
    if (retain_visits && result.visits.size() > limits.maxNodes) {
        finding(report, limits, CarFindingSeverity::error, "NODE_LIMIT", "retained hierarchy exceeds validation limit");
        result.safe = false;
    }
    return result;
}

[[nodiscard]] const Visit* first_named(const std::vector<Visit>& visits, std::string_view name) {
    const auto folded = upper_ascii(name);
    for (const auto& visit : visits) if (upper_ascii(visit.node->name) == folded) return &visit;
    return nullptr;
}

[[nodiscard]] float distance(Point left, Point right) noexcept {
    const auto x = left[0] - right[0], y = left[1] - right[1], z = left[2] - right[2];
    return std::sqrt(x * x + y * y + z * z);
}

[[nodiscard]] Point translation(const Visit* visit) noexcept {
    return {visit->world[12], visit->world[13], visit->world[14]};
}

[[nodiscard]] std::vector<Visit> collect_lod_visits(const Node& root) {
    constexpr Matrix identity = {
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::vector<Visit> visits;
    std::vector<Visit> stack;
    stack.push_back({&root, 0U, identity});
    while (!stack.empty()) {
        const Visit current = stack.back();
        stack.pop_back();
        const Matrix world = multiply(current.world, current.node->transform);
        visits.push_back({current.node, current.depth, world});
        for (auto iterator = current.node->children.rbegin();
             iterator != current.node->children.rend(); ++iterator)
            stack.push_back({&*iterator, current.depth + 1U, world});
    }
    return visits;
}

[[nodiscard]] std::string joined_names(const std::vector<std::string>& names,
                                       std::size_t maximum) {
    std::ostringstream output;
    const std::size_t count = std::min(names.size(), maximum);
    for (std::size_t index = 0U; index < count; ++index) {
        if (index != 0U) output << ", ";
        output << names[index];
    }
    if (names.size() > count) output << " (+" << names.size() - count << ')';
    return output.str();
}

[[nodiscard]] float transform_deviation(const std::vector<Visit>& visits) noexcept {
    constexpr apex::formats::Kn5Matrix4 identity = {
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    float maximum = 0.0F;
    for (const auto& visit : visits)
        for (std::size_t index = 0; index < 16; ++index)
            maximum = std::max(maximum, std::abs(visit.node->transform[index] - identity[index]));
    return maximum;
}

[[nodiscard]] std::optional<CarBounds> mesh_bounds(const std::vector<Visit>& visits,
                                                    CarValidationReport& report,
                                                    const CarValidationLimits& limits) {
    BoundsAccumulator bounds;
    for (const auto& visit : visits) {
        if (visit.node->type != 2U && visit.node->type != 3U) continue;
        const auto count = vertex_count(*visit.node);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = index * visit.node->vertexStride;
            const auto value = point(visit.world, visit.node->vertices[offset],
                                     visit.node->vertices[offset + 1U], visit.node->vertices[offset + 2U]);
            if (!finite_point(value)) {
                finding(report, limits, CarFindingSeverity::error, "NON_FINITE_BOUNDS", "transformed mesh position is not finite", visit.node->name);
                continue;
            }
            bounds.include(value);
        }
    }
    const auto result = bounds.finish();
    if (!result && bounds.has_values)
        finding(report, limits, CarFindingSeverity::error, "NON_FINITE_BOUNDS", "derived mesh bounds are not finite");
    return result;
}

struct VertexKey {
    std::int64_t x = 0, y = 0, z = 0;
    friend bool operator==(const VertexKey&, const VertexKey&) = default;
};
struct VertexHash {
    std::size_t operator()(const VertexKey& value) const noexcept {
        const auto mix = [](std::uint64_t input) { input ^= input >> 30U; input *= 0xbf58476d1ce4e5b9ULL; input ^= input >> 27U; input *= 0x94d049bb133111ebULL; return input ^ (input >> 31U); };
        return static_cast<std::size_t>(mix(static_cast<std::uint64_t>(value.x)) ^ mix(static_cast<std::uint64_t>(value.y) << 1U) ^ mix(static_cast<std::uint64_t>(value.z) << 2U));
    }
};
struct EdgeKey {
    std::size_t a = 0, b = 0;
    friend bool operator==(const EdgeKey&, const EdgeKey&) = default;
};
struct EdgeHash {
    std::size_t operator()(const EdgeKey& value) const noexcept {
        return std::hash<std::size_t>{}(value.a) ^ (std::hash<std::size_t>{}(value.b) << 1U);
    }
};

[[nodiscard]] CarTopologySummary topology(const std::vector<Visit>& visits,
                                           CarValidationReport& report,
                                           const CarValidationLimits& limits) {
    constexpr long double epsilon = 1.0e-5L;
    std::unordered_map<VertexKey, std::size_t, VertexHash> welded;
    std::unordered_map<EdgeKey, std::uint8_t, EdgeHash> edges;
    CarTopologySummary result;
    for (const auto& visit : visits) {
        if (visit.node->type != 2U && visit.node->type != 3U) continue;
        const auto count = vertex_count(*visit.node);
        std::vector<std::size_t> ids;
        ids.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = index * visit.node->vertexStride;
            const auto value = point(visit.world, visit.node->vertices[offset], visit.node->vertices[offset + 1U], visit.node->vertices[offset + 2U]);
            if (!finite_point(value)) continue;
            const auto quantize = [](float component, std::int64_t& output) {
                const long double scaled = static_cast<long double>(component) / epsilon;
                // Keep a substantial margin below INT64_MAX. This remains
                // portable on implementations where long double has the
                // same precision as double and cannot represent INT64_MAX
                // exactly.
                constexpr long double safe_limit = 9.0e18L;
                // llround has undefined/domain-error behavior outside the
                // int64 range. Check a conservative rounding envelope first.
                if (!std::isfinite(scaled) || scaled < -safe_limit || scaled > safe_limit) return false;
                output = static_cast<std::int64_t>(std::llround(scaled));
                return true;
            };
            VertexKey key;
            if (!quantize(value[0], key.x) || !quantize(value[1], key.y) || !quantize(value[2], key.z)) {
                finding(report, limits, CarFindingSeverity::error, "TOPOLOGY_COORDINATE_RANGE", "finite mesh coordinate cannot be represented by the topology weld key");
                return result;
            }
            const auto found = welded.find(key);
            if (found == welded.end()) {
                if (welded.size() >= limits.maxTopologyVertices) {
                    finding(report, limits, CarFindingSeverity::error, "TOPOLOGY_VERTEX_LIMIT", "welded topology vertex count exceeds validation limit");
                    return result;
                }
                const auto [iterator, inserted] = welded.emplace(key, welded.size());
                (void)inserted;
                ids.push_back(iterator->second);
            } else ids.push_back(found->second);
        }
        for (std::size_t index = 0; index + 2U < visit.node->indices.size(); index += 3U) {
            const auto ia = visit.node->indices[index], ib = visit.node->indices[index + 1U], ic = visit.node->indices[index + 2U];
            if (ia >= ids.size() || ib >= ids.size() || ic >= ids.size()) continue;
            const auto a = ids[ia], b = ids[ib], c = ids[ic];
            if (a == b || b == c || a == c) { ++result.degenerate_triangles; continue; }
            const auto add_edge = [&](std::size_t left, std::size_t right) {
                if (left > right) std::swap(left, right);
                const EdgeKey key{left, right};
                const auto found = edges.find(key);
                if (found == edges.end() && edges.size() >= limits.maxTopologyEdges) {
                    finding(report, limits, CarFindingSeverity::error, "TOPOLOGY_EDGE_LIMIT", "topology edge count exceeds validation limit");
                    return false;
                }
                auto iterator = found;
                if (iterator == edges.end()) {
                    iterator = edges.emplace(key, std::uint8_t{0U}).first;
                }
                if (iterator->second < std::numeric_limits<std::uint8_t>::max()) ++iterator->second;
                return true;
            };
            if (!add_edge(a, b) || !add_edge(b, c) || !add_edge(c, a)) return result;
        }
    }
    result.welded_vertices = welded.size();
    result.edges = edges.size();
    for (const auto& [edge, count] : edges) {
        (void)edge;
        if (count == 1U) ++result.boundary_edges;
        if (count > 2U) ++result.non_manifold_edges;
    }
    result.closed = !edges.empty() && result.boundary_edges == 0 && result.non_manifold_edges == 0;
    return result;
}

}  // namespace

CarValidationReport audit_car_hierarchy(const apex::formats::Kn5File& model,
                                        CarValidationLimits limits) {
    CarValidationReport report;
    const auto checked = validate_input(model, report, limits, true);
    if (!checked.safe) return report;
    report.required_nodes_present = 0;
    for (const auto name : kRequired) {
        const auto* visit = first_named(checked.visits, name);
        if (visit == nullptr) {
            finding(report, limits, CarFindingSeverity::warning, "REQUIRED_NODE_MISSING", std::string(name) + " is missing from LOD 0");
        } else {
            ++report.required_nodes_present;
            if (visit->node->kind != "node")
                finding(report, limits, CarFindingSeverity::error, "REQUIRED_NODE_KIND", std::string(name) + " must be a hierarchy node", visit->node->name);
        }
    }
    std::map<std::string, std::size_t> names;
    for (const auto& visit : checked.visits) {
        if (visit.node->kind != "node") continue;
        const auto key = upper_ascii(visit.node->name);
        if (!key.empty()) ++names[key];
    }
    for (const auto& [name, count] : names) if (count > 1U)
        finding(report, limits, CarFindingSeverity::warning, "DUPLICATE_NODE_NAME", "hierarchy contains duplicate node name " + name, name);
    for (const auto name : {std::string_view("COCKPIT_HR"), std::string_view("STEER_HR")})
        if (first_named(checked.visits, name) == nullptr)
            finding(report, limits, CarFindingSeverity::warning, "RECOMMENDED_NODE_MISSING", std::string(name) + " is recommended for LOD 0");
    for (const auto corner : kCorners) {
        const auto* wheel = first_named(checked.visits, "WHEEL_" + std::string(corner));
        for (const auto prefix : {std::string_view("SUSP_"), std::string_view("DISC_"), std::string_view("RIM_")}) {
            const auto* related = first_named(checked.visits, std::string(prefix) + std::string(corner));
            if (wheel != nullptr && related != nullptr) {
                const auto separation = distance(translation(wheel), translation(related));
                if (separation > 0.05F)
                    finding(report, limits, CarFindingSeverity::warning, "PIVOT_OFFSET", std::string(prefix) + std::string(corner) + " pivot is separated from its wheel", related->node->name);
            }
        }
    }
    const auto* left_front = first_named(checked.visits, "WHEEL_LF");
    const auto* left_rear = first_named(checked.visits, "WHEEL_LR");
    const auto* right_front = first_named(checked.visits, "WHEEL_RF");
    const auto* right_rear = first_named(checked.visits, "WHEEL_RR");
    if (left_front && left_rear && right_front && right_rear) {
        const auto left = (translation(left_front)[0] + translation(left_rear)[0]) * 0.5F;
        const auto right = (translation(right_front)[0] + translation(right_rear)[0]) * 0.5F;
        const auto front = (translation(left_front)[2] + translation(right_front)[2]) * 0.5F;
        const auto rear = (translation(left_rear)[2] + translation(right_rear)[2]) * 0.5F;
        if (left <= right) finding(report, limits, CarFindingSeverity::error, "WHEEL_AXIS", "wheel pivots do not use the expected left/right X orientation");
        if (front <= rear) finding(report, limits, CarFindingSeverity::error, "WHEEL_AXIS", "wheel pivots do not use +Z as the front direction");
    }
    finding(report, limits, CarFindingSeverity::warning, "LOD_METADATA_STAGED", "KN5 input has no workspace LOD metadata; this foundation audits the parsed hierarchy as LOD 0");
    return report;
}

CarLodValidationReport audit_car_lod_hierarchies(
    const apex::formats::Kn5File& merged_model,
    std::span<const CarLodHierarchyInput> lods,
    CarValidationLimits limits) {
    CarLodValidationReport report;
    if (lods.empty()) {
        lod_finding(report, limits, CarFindingSeverity::error, "LOD_INPUT_EMPTY",
                    "A car LOD audit needs at least one hierarchy root");
        return report;
    }
    if (lods.size() > limits.maxLods) {
        lod_finding(report, limits, CarFindingSeverity::error, "LOD_COUNT_LIMIT",
                    "Car LOD count exceeds the validation limit");
        return report;
    }

    CarValidationReport input_report;
    const auto checked = validate_input(merged_model, input_report, limits, false);
    if (!checked.safe) {
        report.errors = input_report.errors;
        report.warnings = input_report.warnings;
        report.findings.reserve(input_report.findings.size());
        for (const auto& source : input_report.findings)
            report.findings.push_back({source.severity, source.code, source.message,
                                       {}, source.node});
        return report;
    }

    struct LodState {
        CarLodHierarchyInput input;
        std::vector<Visit> visits;
    };
    std::vector<LodState> states;
    states.reserve(lods.size());
    report.lods.reserve(lods.size());
    std::set<std::size_t> root_indices;
    std::set<std::uint32_t> lod_indices;
    std::size_t total_lod_nodes = 0U;

    for (const auto& input : lods) {
        if (input.file.empty() || input.file.size() > limits.maxStringBytes ||
            input.file.find('\0') != std::string_view::npos) {
            lod_finding(report, limits, CarFindingSeverity::error,
                        "LOD_FILE_INVALID",
                        "Car LOD file name is empty, too large, or contains NUL");
            continue;
        }
        if (input.root_child_index >= merged_model.root.children.size()) {
            lod_finding(report, limits, CarFindingSeverity::error,
                        "LOD_ROOT_INVALID",
                        "Car LOD root index is outside the merged hierarchy",
                        input.file);
            continue;
        }
        if (!root_indices.insert(input.root_child_index).second) {
            lod_finding(report, limits, CarFindingSeverity::error,
                        "LOD_ROOT_DUPLICATE",
                        "Car LOD inputs contain a duplicate hierarchy root",
                        input.file);
            continue;
        }
        if (!lod_indices.insert(input.index).second) {
            lod_finding(report, limits, CarFindingSeverity::error,
                        "LOD_INDEX_DUPLICATE",
                        "Car LOD inputs contain a duplicate LOD index",
                        input.file);
            continue;
        }

        auto visits = collect_lod_visits(
            merged_model.root.children[input.root_child_index]);
        if (visits.size() > limits.maxNodes ||
            total_lod_nodes > limits.maxNodes - visits.size()) {
            lod_finding(report, limits, CarFindingSeverity::error,
                        "LOD_NODE_LIMIT",
                        "Aggregate car LOD hierarchy nodes exceed the validation limit",
                        input.file);
            continue;
        }
        total_lod_nodes += visits.size();

        std::map<std::string, std::size_t> all_names;
        std::map<std::string, std::size_t> hierarchy_names;
        for (const auto& visit : visits) {
            const auto key = upper_ascii(visit.node->name);
            if (key.empty()) continue;
            ++all_names[key];
            if (visit.node->kind == "node") ++hierarchy_names[key];
        }
        CarLodHierarchySummary summary;
        summary.index = input.index;
        summary.file = input.file;
        summary.nodes = visits.size();
        summary.unique_names = all_names.size();

        for (const auto name : kRequired) {
            const auto* visit = first_named(visits, name);
            if (visit == nullptr) {
                lod_finding(report, limits, CarFindingSeverity::warning,
                            "REQUIRED_NODE_MISSING",
                            std::string(name) + " is missing from LOD " +
                                std::to_string(input.index),
                            input.file);
                continue;
            }
            ++summary.required_nodes_present;
            if (visit->node->kind != "node")
                lod_finding(report, limits, CarFindingSeverity::error,
                            "REQUIRED_NODE_KIND",
                            std::string(name) + " must be a hierarchy node",
                            input.file, visit->node->name);
        }

        std::vector<std::string> required_duplicates;
        std::vector<std::string> other_duplicates;
        for (const auto& [name, count] : hierarchy_names) {
            if (count <= 1U) continue;
            const bool required = std::find(kRequired.begin(), kRequired.end(),
                                            std::string_view(name)) != kRequired.end();
            (required ? required_duplicates : other_duplicates).push_back(name);
        }
        if (!required_duplicates.empty())
            lod_finding(report, limits, CarFindingSeverity::warning,
                        "REQUIRED_NODE_DUPLICATE",
                        "LOD " + std::to_string(input.index) +
                            " duplicates SDK-required hierarchy nodes: " +
                            joined_names(required_duplicates, required_duplicates.size()),
                        input.file);
        if (!other_duplicates.empty())
            lod_finding(report, limits, CarFindingSeverity::warning,
                        "DUPLICATE_NODE_NAME",
                        "LOD " + std::to_string(input.index) +
                            " has duplicate hierarchy-node names: " +
                            joined_names(other_duplicates, 12U),
                        input.file);

        for (const auto name : {std::string_view("COCKPIT_HR"),
                                std::string_view("STEER_HR")}) {
            const bool present = first_named(visits, name) != nullptr;
            if (input.index == 0U && !present)
                lod_finding(report, limits, CarFindingSeverity::warning,
                            "RECOMMENDED_NODE_MISSING",
                            std::string(name) + " is recommended for LOD 0",
                            input.file);
            if (input.index != 0U && present)
                lod_finding(report, limits, CarFindingSeverity::warning,
                            "HIGH_DETAIL_NODE_IN_REDUCED_LOD",
                            std::string(name) + " must only be present in LOD 0",
                            input.file, name);
        }

        for (const auto corner : kCorners) {
            const auto* wheel = first_named(visits, "WHEEL_" + std::string(corner));
            for (const auto prefix : {std::string_view("SUSP_"),
                                      std::string_view("DISC_"),
                                      std::string_view("RIM_")}) {
                const auto* related = first_named(
                    visits, std::string(prefix) + std::string(corner));
                if (wheel != nullptr && related != nullptr &&
                    distance(translation(wheel), translation(related)) > 0.05F)
                    lod_finding(report, limits, CarFindingSeverity::warning,
                                "PIVOT_OFFSET",
                                std::string(prefix) + std::string(corner) +
                                    " pivot is separated from its wheel",
                                input.file, related->node->name);
            }
        }

        const auto* left_front = first_named(visits, "WHEEL_LF");
        const auto* left_rear = first_named(visits, "WHEEL_LR");
        const auto* right_front = first_named(visits, "WHEEL_RF");
        const auto* right_rear = first_named(visits, "WHEEL_RR");
        if (left_front != nullptr && left_rear != nullptr &&
            right_front != nullptr && right_rear != nullptr) {
            const float left =
                (translation(left_front)[0] + translation(left_rear)[0]) * 0.5F;
            const float right =
                (translation(right_front)[0] + translation(right_rear)[0]) * 0.5F;
            const float front =
                (translation(left_front)[2] + translation(right_front)[2]) * 0.5F;
            const float rear =
                (translation(left_rear)[2] + translation(right_rear)[2]) * 0.5F;
            if (left <= right)
                lod_finding(report, limits, CarFindingSeverity::error,
                            "WHEEL_AXIS",
                            "Wheel pivots do not use the expected left/right X orientation",
                            input.file);
            if (front <= rear)
                lod_finding(report, limits, CarFindingSeverity::error,
                            "WHEEL_AXIS",
                            "Wheel pivots do not use +Z as the front direction",
                            input.file);
        }

        report.lods.push_back(std::move(summary));
        states.push_back({input, std::move(visits)});
    }

    if (states.size() > 1U) {
        const auto reference = std::find_if(
            states.begin(), states.end(),
            [](const LodState& state) { return state.input.index == 0U; });
        const LodState& reference_state =
            reference == states.end() ? states.front() : *reference;
        for (const auto& state : states) {
            if (&state == &reference_state) continue;
            for (const auto name : {std::string_view("WHEEL_LF"),
                                    std::string_view("WHEEL_LR"),
                                    std::string_view("WHEEL_RF"),
                                    std::string_view("WHEEL_RR")}) {
                const auto* expected = first_named(reference_state.visits, name);
                const auto* actual = first_named(state.visits, name);
                if (expected != nullptr && actual != nullptr &&
                    distance(translation(expected), translation(actual)) > 0.05F)
                    lod_finding(report, limits, CarFindingSeverity::warning,
                                "LOD_PIVOT_MISMATCH",
                                std::string(name) + " differs from LOD 0",
                                state.input.file, actual->node->name);
            }
        }
    }
    return report;
}

CarValidationReport audit_car_collider(const apex::formats::Kn5File& collider,
                                       const apex::formats::Kn5File* visual_model,
                                       CarValidationLimits limits) {
    CarValidationReport report;
    const auto checked = validate_input(collider, report, limits, true);
    if (!checked.safe) return report;
    report.bounds = mesh_bounds(checked.visits, report, limits);
    report.pivot_deviation = transform_deviation(checked.visits);
    std::unordered_set<std::uint32_t> used_materials;
    for (const auto& visit : checked.visits) {
        if (visit.node->type != 2U && visit.node->type != 3U) continue;
        used_materials.insert(visit.node->materialId);
        if (visit.node->type == 3U)
            finding(report, limits, CarFindingSeverity::error, "SKINNED_COLLIDER", "collider geometry must not be skinned", visit.node->name);
    }
    if (!checked.visits.empty() && report.meshes == 0)
        finding(report, limits, CarFindingSeverity::error, "NO_COLLIDER_MESH", "collider KN5 has no collision mesh");
    if (!collider.textures.empty())
        finding(report, limits, CarFindingSeverity::error, "COLLIDER_TEXTURES", "collider contains textures; SDK collision exports must contain none");
    for (const auto material_id : used_materials) {
        const auto& material = collider.materials[material_id];
        if (upper_ascii(material.shader) != "GL")
            finding(report, limits, CarFindingSeverity::error, "COLLIDER_SHADER", "used collider material does not use the GL collision shader", material.name);
    }
    if (report.triangles > 60U)
        finding(report, limits, CarFindingSeverity::warning, "COLLIDER_TRIANGLE_GUIDELINE", "collider exceeds the SDK 40-60 triangle guideline");
    report.topology = topology(checked.visits, report, limits);
    if (report.meshes != 0U && !report.topology.closed)
        finding(report, limits, CarFindingSeverity::error, "COLLIDER_OPEN", "collider is not a closed manifold");
    if (report.topology.degenerate_triangles != 0U)
        finding(report, limits, CarFindingSeverity::warning, "DEGENERATE_TRIANGLES", "collider contains degenerate triangles");
    if (report.pivot_deviation > 1.0e-5F)
        finding(report, limits, CarFindingSeverity::warning, "COLLIDER_TRANSFORM", "collider hierarchy is transformed; verify pivot and wheel-axis orientation");
    if (report.bounds) {
        for (const auto size : report.bounds->size) if (size < 0.01F || size > 20.0F) {
            finding(report, limits, CarFindingSeverity::error, "COLLIDER_DIMENSIONS", "collider dimensions are outside the plausible 0.01-20 m range");
            break;
        }
    }
    if (visual_model != nullptr) {
        CarValidationReport visual_report;
        const auto visual_checked = validate_input(*visual_model, visual_report, limits, true);
        if (!visual_checked.safe) {
            finding(report, limits, CarFindingSeverity::warning, "VISUAL_MODEL_INVALID", "visual model could not be used for collider bounds comparison");
        } else if (report.bounds) {
            const auto visual_bounds = mesh_bounds(visual_checked.visits, visual_report, limits);
            if (!visual_bounds) {
                finding(report, limits, CarFindingSeverity::warning, "VISUAL_MODEL_INVALID", "visual model bounds are not finite and cannot be used for collider comparison");
            } else {
                for (const auto axis : {0U, 2U}) if (report.bounds->min[axis] < visual_bounds->min[axis] - 0.05F || report.bounds->max[axis] > visual_bounds->max[axis] + 0.05F) {
                    finding(report, limits, CarFindingSeverity::warning, "COLLIDER_OUTSIDE_VISUAL", "collider extends outside visual model bounds");
                    break;
                }
                if (report.bounds->min[1] < visual_bounds->min[1] - 0.05F)
                    finding(report, limits, CarFindingSeverity::warning, "COLLIDER_BELOW_VISUAL", "collider extends below the visual model bounds");
            }
        }
    }
    return report;
}

}  // namespace apex::domain
