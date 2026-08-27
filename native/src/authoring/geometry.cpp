#include "apex/authoring/geometry.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace apex::authoring {
namespace {

constexpr std::size_t kStride = 11;
constexpr std::size_t kMaxElements = 10'000'000;
const Matrix4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

[[noreturn]] void fail(std::string_view code, std::string message) {
    throw GeometryError(std::string(code), std::move(message));
}

void finite(float value, std::string_view label) {
    if (!std::isfinite(value)) fail("non_finite", std::string(label) + " must be finite");
}

template <std::size_t N>
void finiteArray(const std::array<float, N>& values, std::string_view label) {
    for (const auto value : values) finite(value, label);
}

void finiteVertices(const std::vector<float>& vertices, std::string_view label) {
    // Static KN5 tangent slot 8 may contain a packed UNORM tangent bit
    // pattern. Its float view can be NaN/Inf and must be treated as raw bits.
    for (std::size_t index = 0; index < vertices.size(); ++index)
        if (index % kStride != 8u) finite(vertices[index], label);
}

bool staticMesh(const formats::Kn5Node& node) {
    return (node.type == 2u || (node.type == 0u && node.kind == "mesh")) &&
           node.vertexStride == kStride;
}

void requireStatic(const formats::Kn5Node& node) {
    if (!staticMesh(node)) fail("not_static_mesh", "Geometry authoring requires a static 11-float KN5 mesh");
}

struct Vec3 {
    double x = 0;
    double y = 0;
    double z = 0;
};

Vec3 subtract(const Vec3& left, const Vec3& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 cross(const Vec3& left, const Vec3& right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

double length(const Vec3& value) { return std::hypot(value.x, value.y, value.z); }

Vec3 normalize(const Vec3& value, const Vec3& fallback) {
    const auto magnitude = length(value);
    if (magnitude > 1e-8) return {value.x / magnitude, value.y / magnitude, value.z / magnitude};
    const auto fallbackMagnitude = length(fallback);
    if (fallbackMagnitude > 1e-8)
        return {fallback.x / fallbackMagnitude, fallback.y / fallbackMagnitude, fallback.z / fallbackMagnitude};
    return {0, 1, 0};
}

void finiteVector(const Vec3& value, std::string_view label) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
        fail("non_finite", std::string(label) + " must be finite");
}

Vec3 vertexPosition(const std::vector<float>& vertices, std::size_t index) {
    const auto offset = index * kStride;
    return {vertices[offset], vertices[offset + 1], vertices[offset + 2]};
}

Vec3 triangleCross(const std::vector<float>& vertices, std::uint16_t a,
                   std::uint16_t b, std::uint16_t c) {
    return cross(subtract(vertexPosition(vertices, b), vertexPosition(vertices, a)),
                 subtract(vertexPosition(vertices, c), vertexPosition(vertices, a)));
}

void validateTopology(const std::vector<float>& vertices,
                      const std::vector<std::uint16_t>& indices) {
    if (vertices.size() % kStride != 0u) fail("vertex_stride", "Static vertex data is not divisible by 11 floats");
    finiteVertices(vertices, "Static vertex data");
    if (indices.size() % 3u != 0u) fail("topology", "Static topology needs complete triangles");
    const auto vertexCount = vertices.size() / kStride;
    if (vertexCount > kMaxElements || indices.size() > kMaxElements * 3u)
        fail("count_limit", "Static geometry exceeds the authoring safety limit");
    for (const auto index : indices)
        if (static_cast<std::size_t>(index) >= vertexCount)
            fail("invalid_index", "Topology index exceeds the vertex count");
}

GeometryBounds boundsFor(const std::vector<float>& vertices) {
    if (vertices.empty()) return {};
    GeometryBounds result;
    result.minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
    result.maximum = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()};
    for (std::size_t offset = 0; offset < vertices.size(); offset += kStride) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result.minimum[axis] = std::min(result.minimum[axis], vertices[offset + axis]);
            result.maximum[axis] = std::max(result.maximum[axis], vertices[offset + axis]);
        }
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // Do the midpoint in double precision.  Adding two large float
        // endpoints first can overflow or lose the low bits of the center.
        result.center[axis] = static_cast<float>((static_cast<double>(result.minimum[axis]) +
                                                  static_cast<double>(result.maximum[axis])) * 0.5);
        result.size[axis] = result.maximum[axis] - result.minimum[axis];
    }
    double radius = 0;
    const Vec3 center{result.center[0], result.center[1], result.center[2]};
    for (std::size_t offset = 0; offset < vertices.size(); offset += kStride)
        radius = std::max(radius, length(subtract({vertices[offset], vertices[offset + 1], vertices[offset + 2]}, center)));
    result.radius = static_cast<float>(radius);
    return result;
}

void recalculateNormals(std::vector<float>& vertices, const std::vector<std::uint16_t>& indices) {
    const auto vertexCount = vertices.size() / kStride;
    std::vector<Vec3> sums(vertexCount);
    for (std::size_t offset = 0; offset < indices.size(); offset += 3u) {
        const auto normal = triangleCross(vertices, indices[offset], indices[offset + 1], indices[offset + 2]);
        for (std::size_t index = offset; index < offset + 3u; ++index) {
            auto& sum = sums[indices[index]];
            sum.x += normal.x; sum.y += normal.y; sum.z += normal.z;
        }
    }
    for (std::size_t index = 0; index < vertexCount; ++index) {
        const auto offset = index * kStride;
        const Vec3 fallback{vertices[offset + 3], vertices[offset + 4], vertices[offset + 5]};
        const auto normal = normalize(sums[index], fallback);
        vertices[offset + 3] = static_cast<float>(normal.x);
        vertices[offset + 4] = static_cast<float>(normal.y);
        vertices[offset + 5] = static_cast<float>(normal.z);
    }
}

struct NormalTransform {
    std::array<double, 9> values{};
    double determinant = 0;
};

NormalTransform inverseTranspose3(const Matrix4& transform) {
    const double a = transform[0], b = transform[4], c = transform[8];
    const double d = transform[1], e = transform[5], f = transform[9];
    const double g = transform[2], h = transform[6], i = transform[10];
    const double A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    const double determinant = a * A + b * B + c * C;
    if (std::abs(determinant) < 1e-8) fail("collapsed_transform", "Geometry scale cannot collapse an axis");
    const std::array<double, 9> inverse = {
        A / determinant, (c * h - b * i) / determinant, (b * f - c * e) / determinant,
        B / determinant, (a * i - c * g) / determinant, (c * d - a * f) / determinant,
        C / determinant, (b * g - a * h) / determinant, (a * e - b * d) / determinant};
    return {{inverse[0], inverse[3], inverse[6], inverse[1], inverse[4], inverse[7],
             inverse[2], inverse[5], inverse[8]}, determinant};
}

Vec3 direction(const Matrix4& transform, const Vec3& value) {
    return {static_cast<double>(transform[0]) * value.x + static_cast<double>(transform[4]) * value.y + static_cast<double>(transform[8]) * value.z,
            static_cast<double>(transform[1]) * value.x + static_cast<double>(transform[5]) * value.y + static_cast<double>(transform[9]) * value.z,
            static_cast<double>(transform[2]) * value.x + static_cast<double>(transform[6]) * value.y + static_cast<double>(transform[10]) * value.z};
}

Vec3 normalDirection(const NormalTransform& transform, const Vec3& value) {
    return normalize({transform.values[0] * value.x + transform.values[1] * value.y + transform.values[2] * value.z,
                      transform.values[3] * value.x + transform.values[4] * value.y + transform.values[5] * value.z,
                      transform.values[6] * value.x + transform.values[7] * value.y + transform.values[8] * value.z},
                     {0, 1, 0});
}

std::string pathForChild(std::string_view path, std::size_t index) {
    return path == "root" ? std::to_string(index) : std::string(path) + "/" + std::to_string(index);
}

formats::Kn5Node* nodeAtPath(formats::Kn5Node& root, std::string_view path) {
    if (path == "root") return &root;
    if (path.empty()) return nullptr;
    formats::Kn5Node* node = &root;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (part.empty()) return nullptr;
        std::size_t index = 0;
        for (const auto character : part) {
            if (character < '0' || character > '9') return nullptr;
            const auto digit = static_cast<std::size_t>(character - '0');
            if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10u) return nullptr;
            index = index * 10u + digit;
        }
        if (index >= node->children.size()) return nullptr;
        node = &node->children[index];
        if (slash == std::string_view::npos) break;
        start = slash + 1u;
    }
    return node;
}

void rejectNumericPathAlias(std::string_view path) {
    if (path == "root") return;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        const bool numeric = !part.empty() && std::all_of(part.begin(), part.end(),
                                                           [](char value) { return value >= '0' && value <= '9'; });
        if (numeric && part.size() > 1u && part.front() == '0')
            fail("invalid_path", "Geometry path contains a non-canonical index");
        if (slash == std::string_view::npos) break;
        start = slash + 1u;
    }
}

void capture(const formats::Kn5Node& node, std::string_view path, GeometryBaselines& result,
             std::size_t depth, std::size_t& nodeCount) {
    if (depth > 1024u) fail("depth_limit", "Scene hierarchy is too deep");
    if (nodeCount == kMaxElements) fail("count_limit", "Scene node count exceeds the authoring safety limit");
    ++nodeCount;
    if (staticMesh(node)) result.emplace(std::string(path), GeometryBaseline{node.vertices, node.indices, node.bounds});
    for (std::size_t index = 0; index < node.children.size(); ++index)
        capture(node.children[index], pathForChild(path, index), result, depth + 1u, nodeCount);
}

void addCaptureBytes(std::size_t& total, std::size_t value, std::size_t limit) {
    if (value > std::numeric_limits<std::size_t>::max() - total || value > limit - std::min(limit, total))
        fail("output_limit", "Geometry baselines exceed the configured output limit");
    total += value;
}

std::size_t captureBytes(std::size_t count, std::size_t elementSize) {
    if (count != 0u && elementSize > std::numeric_limits<std::size_t>::max() / count)
        fail("size_overflow", "Geometry baseline size overflows");
    return count * elementSize;
}

void preflightCapture(const formats::Kn5Node& node, std::size_t depth, std::size_t& nodeCount,
                      std::size_t& bytes, std::size_t limit) {
    if (depth > 1024u) fail("depth_limit", "Scene hierarchy is too deep");
    if (nodeCount == kMaxElements) fail("count_limit", "Scene node count exceeds the authoring safety limit");
    ++nodeCount;
    if (staticMesh(node)) {
        validateTopology(node.vertices, node.indices);
        addCaptureBytes(bytes, captureBytes(node.vertices.size(), sizeof(float)), limit);
        addCaptureBytes(bytes, captureBytes(node.indices.size(), sizeof(std::uint16_t)), limit);
        addCaptureBytes(bytes, captureBytes(node.bounds.size(), sizeof(float)), limit);
    }
    for (const auto& child : node.children)
        preflightCapture(child, depth + 1u, nodeCount, bytes, limit);
}

void warning(std::vector<std::string>* warnings, std::string message) {
    if (warnings) warnings->push_back(std::move(message));
}

} // namespace

GeometryBounds static_geometry_bounds(const formats::Kn5Node& node) {
    requireStatic(node);
    validateTopology(node.vertices, node.indices);
    const auto result = boundsFor(node.vertices);
    finiteArray(result.minimum, "geometry bounds"); finiteArray(result.maximum, "geometry bounds");
    finiteArray(result.center, "geometry bounds"); finiteArray(result.size, "geometry bounds"); finite(result.radius, "geometry bounds");
    return result;
}

StaticGeometryMetrics static_geometry_metrics(const formats::Kn5Node& node) {
    requireStatic(node);
    validateTopology(node.vertices, node.indices);
    return {static_geometry_bounds(node), node.vertices.size() / kStride, node.indices.size() / 3u};
}

GeometryBaselines capture_static_geometry_baselines(const formats::Kn5Node& root, GeometryLimits limits) {
    std::size_t preflightNodeCount = 0;
    std::size_t bytes = 0;
    preflightCapture(root, 0, preflightNodeCount, bytes, limits.maxOutputBytes);
    GeometryBaselines result;
    std::size_t nodeCount = 0;
    capture(root, "root", result, 0, nodeCount);
    return result;
}

formats::Kn5Node repair_static_topology(const formats::Kn5Node& node, const GeometryEdit& edit,
                                        const std::vector<float>& baseline_vertices,
                                        const std::vector<std::uint16_t>& baseline_indices) {
    requireStatic(node);
    validateTopology(baseline_vertices, baseline_indices);
    formats::Kn5Node result = node;
    result.vertices = baseline_vertices;
    result.indices = baseline_indices;
    if (edit.remove_degenerate) {
        const auto scaleBounds = boundsFor(result.vertices);
        const auto diagonal = std::sqrt(static_cast<double>(scaleBounds.size[0]) * scaleBounds.size[0] +
                                         static_cast<double>(scaleBounds.size[1]) * scaleBounds.size[1] +
                                         static_cast<double>(scaleBounds.size[2]) * scaleBounds.size[2]);
        const auto scale = std::max(1.0, diagonal);
        const auto minimumDoubleArea = std::numeric_limits<double>::epsilon() * 64.0 * scale * scale;
        std::vector<std::uint16_t> retained;
        retained.reserve(result.indices.size());
        for (std::size_t offset = 0; offset < result.indices.size(); offset += 3u) {
            const auto a = result.indices[offset], b = result.indices[offset + 1], c = result.indices[offset + 2];
            if (a == b || b == c || a == c || length(triangleCross(result.vertices, a, b, c)) <= minimumDoubleArea) continue;
            retained.insert(retained.end(), {a, b, c});
        }
        result.indices = std::move(retained);
    }
    if (edit.reverse_winding) {
        for (std::size_t offset = 0; offset < result.indices.size(); offset += 3u)
            std::swap(result.indices[offset + 1], result.indices[offset + 2]);
    }
    if (edit.recalculate_normals) recalculateNormals(result.vertices, result.indices);
    else if (edit.reverse_winding) {
        for (std::size_t offset = 0; offset < result.vertices.size(); offset += kStride)
            for (std::size_t axis = 3; axis < 6; ++axis) result.vertices[offset + axis] = -result.vertices[offset + axis];
    }
    return result;
}

GeometryTransformResult transform_static_geometry(const formats::Kn5Node& node, const Matrix4& transform,
                                                  const std::vector<float>& baseline_vertices) {
    requireStatic(node);
    if (baseline_vertices.size() != node.vertices.size())
        fail("baseline_mismatch", "Geometry baseline does not match the mesh");
    validateTopology(baseline_vertices, node.indices);
    for (const auto value : transform) finite(value, "Geometry transform");
    const auto normal = inverseTranspose3(transform);
    const auto baselineBounds = boundsFor(baseline_vertices);
    const Vec3 pivot{baselineBounds.center[0], baselineBounds.center[1], baselineBounds.center[2]};
    auto vertices = baseline_vertices;
    for (std::size_t offset = 0; offset < vertices.size(); offset += kStride) {
        const auto relative = subtract(vertexPosition(baseline_vertices, offset / kStride), pivot);
        const auto moved = direction(transform, relative);
        finiteVector(moved, "transformed position");
        vertices[offset] = static_cast<float>(moved.x + pivot.x + transform[12]);
        vertices[offset + 1] = static_cast<float>(moved.y + pivot.y + transform[13]);
        vertices[offset + 2] = static_cast<float>(moved.z + pivot.z + transform[14]);
        const Vec3 sourceNormal{baseline_vertices[offset + 3], baseline_vertices[offset + 4], baseline_vertices[offset + 5]};
        const auto transformedNormal = normalDirection(normal, sourceNormal);
        finiteVector(transformedNormal, "transformed normal");
        vertices[offset + 3] = static_cast<float>(transformedNormal.x);
        vertices[offset + 4] = static_cast<float>(transformedNormal.y);
        vertices[offset + 5] = static_cast<float>(transformedNormal.z);

        const Vec3 plainTangent{baseline_vertices[offset + 8], baseline_vertices[offset + 9], baseline_vertices[offset + 10]};
        const auto tangentLength = length(plainTangent);
        const bool plain = std::isfinite(baseline_vertices[offset + 8]) &&
                           std::isfinite(baseline_vertices[offset + 9]) &&
                           std::isfinite(baseline_vertices[offset + 10]) && tangentLength > 0.5 && tangentLength < 1.5;
        std::uint32_t packed = std::bit_cast<std::uint32_t>(baseline_vertices[offset + 8]);
        Vec3 tangent = plain ? plainTangent
                             : Vec3{((packed & 0xffu) / 255.0) * 2.0 - 1.0,
                                    (((packed >> 8u) & 0xffu) / 255.0) * 2.0 - 1.0,
                                    (((packed >> 16u) & 0xffu) / 255.0) * 2.0 - 1.0};
        const auto movedTangent = direction(transform, tangent);
        finiteVector(movedTangent, "transformed tangent");
        const auto dot = movedTangent.x * transformedNormal.x + movedTangent.y * transformedNormal.y + movedTangent.z * transformedNormal.z;
        const auto orthogonal = normalize({movedTangent.x - transformedNormal.x * dot,
                                            movedTangent.y - transformedNormal.y * dot,
                                            movedTangent.z - transformedNormal.z * dot}, {1, 0, 0});
        finiteVector(orthogonal, "orthogonal tangent");
        if (plain) {
            vertices[offset + 8] = static_cast<float>(orthogonal.x);
            vertices[offset + 9] = static_cast<float>(orthogonal.y);
            vertices[offset + 10] = static_cast<float>(orthogonal.z);
        } else {
            const auto channel = [](double value) -> std::uint32_t {
                return static_cast<std::uint32_t>(std::clamp(std::lround((value + 1.0) * 0.5 * 255.0), 0L, 255L));
            };
            packed = (packed & 0xff000000u) | channel(orthogonal.x) |
                     (channel(orthogonal.y) << 8u) | (channel(orthogonal.z) << 16u);
            vertices[offset + 8] = std::bit_cast<float>(packed);
        }
    }
    const auto metrics = boundsFor(vertices);
    finiteVertices(vertices, "transformed geometry");
    finiteArray(metrics.minimum, "transformed bounds"); finiteArray(metrics.maximum, "transformed bounds");
    finiteArray(metrics.center, "transformed bounds"); finiteArray(metrics.size, "transformed bounds"); finite(metrics.radius, "transformed bounds");
    return {vertices, {metrics.center[0], metrics.center[1], metrics.center[2], metrics.radius}, metrics,
            normal.determinant < 0};
}

std::size_t apply_geometry_edits(formats::Kn5Node& root, const std::map<std::string, GeometryEdit>& edits,
                                 const GeometryBaselines* baselines, std::vector<std::string>* warnings) {
    if (edits.empty()) return 0;
    for (const auto& [path, edit] : edits) {
        (void)edit;
        rejectNumericPathAlias(path);
    }
    if (baselines)
        for (const auto& [path, baseline] : *baselines) {
            rejectNumericPathAlias(path);
            validateTopology(baseline.vertices, baseline.indices);
        }
    // Work on a full copy.  Baseline restoration and each successful edit are
    // committed together, so a failed validation cannot leave the caller's
    // hierarchy partly restored or partly edited.
    auto candidateRoot = root;
    if (baselines) {
        for (const auto& [path, baseline] : *baselines) {
            auto* node = nodeAtPath(candidateRoot, path);
            if (!node) continue;
            node->vertices = baseline.vertices;
            node->indices = baseline.indices;
            if (baseline.bounds) node->bounds = *baseline.bounds;
        }
    }
    std::size_t applied = 0;
    bool failed = false;
    for (const auto& [path, edit] : edits) {
        auto* node = nodeAtPath(candidateRoot, path);
        if (!node) { warning(warnings, path + ": geometry node was not found"); failed = true; continue; }
        if (node->type == 3u || node->kind == "skinnedMesh") {
            warning(warnings, path + ": " + node->name + " uses skinned bind-pose geometry and was not changed");
            failed = true;
            continue;
        }
        if (!staticMesh(*node)) {
            warning(warnings, path + ": " + node->name + " is not editable static KN5 geometry");
            failed = true;
            continue;
        }
        try {
            const auto baseline = baselines ? baselines->find(path) : GeometryBaselines::const_iterator{};
            const auto& vertices = baselines && baseline != baselines->end() ? baseline->second.vertices : node->vertices;
            const auto& indices = baselines && baseline != baselines->end() ? baseline->second.indices : node->indices;
            auto candidate = repair_static_topology(*node, edit, vertices, indices);
            if (edit.transform || edit.recalculate_normals) {
                const auto transformed = transform_static_geometry(candidate, edit.transform.value_or(kIdentity), candidate.vertices);
                candidate.vertices = transformed.vertices;
                candidate.bounds = transformed.bounds;
                if (transformed.mirrored) {
                    for (std::size_t offset = 0; offset < candidate.indices.size(); offset += 3u)
                        std::swap(candidate.indices[offset + 1], candidate.indices[offset + 2]);
                }
            }
            *node = std::move(candidate);
            ++applied;
        } catch (const GeometryError& error) {
            warning(warnings, path + ": " + error.what());
            failed = true;
        }
    }
    if (failed) return 0;
    root = std::move(candidateRoot);
    return applied;
}

} // namespace apex::authoring
