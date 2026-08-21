#include <apex/scene/kn5_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace apex::scene {
namespace {

[[noreturn]] void invalid(std::string_view path, std::string_view message) {
    throw Kn5SceneError("KN5 scene " + std::string(path) + ": " + std::string(message));
}

void requireFinite(float value, std::string_view path, std::string_view field) {
    if (!std::isfinite(value)) invalid(path, std::string(field) + " is not finite");
}

void requireFinite(const formats::Kn5Matrix4& matrix, std::string_view path,
                   std::string_view field) {
    for (const auto value : matrix) requireFinite(value, path, field);
}

Matrix4 multiply(const Matrix4& left, const Matrix4& right, std::string_view path) {
    Matrix4 output{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t index = 0; index < 4; ++index)
                output[column * 4 + row] += left[index * 4 + row] * right[column * 4 + index];
            requireFinite(output[column * 4 + row], path, "world transform");
        }
    }
    return output;
}

Vector3 transformPoint(const Matrix4& matrix, const Vector3& point, std::string_view path) {
    Vector3 output = {
        matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
        matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
        matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14]};
    for (const auto value : output) requireFinite(value, path, "world bounds center");
    return output;
}

float maxScale(const Matrix4& matrix, std::string_view path) {
    float result = 0.0F;
    for (std::size_t column = 0; column < 3; ++column) {
        const float length = std::sqrt(matrix[column * 4] * matrix[column * 4] +
                                       matrix[column * 4 + 1] * matrix[column * 4 + 1] +
                                       matrix[column * 4 + 2] * matrix[column * 4 + 2]);
        requireFinite(length, path, "world bounds scale");
        result = std::max(result, length);
    }
    return result;
}

struct LocalBounds {
    Vector3 center = {0.0F, 0.0F, 0.0F};
    float radius = 0.0F;
};

LocalBounds geometryBounds(const formats::Kn5Node& source, std::string_view path) {
    if (source.type == 2) {
        for (const auto value : source.bounds) requireFinite(value, path, "mesh bounds");
        if (source.bounds[3] < 0.0F) invalid(path, "mesh bounds radius is negative");
        return {{source.bounds[0], source.bounds[1], source.bounds[2]}, source.bounds[3]};
    }

    // Skinned records do not carry a serialized sphere. Derive a conservative
    // local sphere from the position AABB for scene framing and diagnostics.
    const std::size_t stride = source.vertexStride;
    if (stride == 0 || source.vertices.empty()) return {};
    if (source.vertices.size() % stride != 0) invalid(path, "vertex data is not stride-aligned");
    Vector3 minimum = {std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity()};
    Vector3 maximum = {-std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity()};
    for (std::size_t offset = 0; offset < source.vertices.size(); offset += stride) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float value = source.vertices[offset + axis];
            requireFinite(value, path, "vertex position");
            minimum[axis] = std::min(minimum[axis], value);
            maximum[axis] = std::max(maximum[axis], value);
        }
    }
    LocalBounds result;
    for (std::size_t axis = 0; axis < 3; ++axis) result.center[axis] = (minimum[axis] + maximum[axis]) * 0.5F;
    for (std::size_t offset = 0; offset < source.vertices.size(); offset += stride) {
        float squared = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float delta = source.vertices[offset + axis] - result.center[axis];
            squared += delta * delta;
        }
        const float distance = std::sqrt(squared);
        requireFinite(distance, path, "vertex bounds radius");
        result.radius = std::max(result.radius, distance);
    }
    return result;
}

NodeKind nodeKind(std::uint32_t type, std::string_view path) {
    switch (type) {
        case 1: return NodeKind::node;
        case 2: return NodeKind::mesh;
        case 3: return NodeKind::skinned_mesh;
        default: invalid(path, "unsupported node type");
    }
}

void appendNode(const formats::Kn5Node& source, NodeId parent, const Matrix4& parentWorld,
                std::size_t depth, std::string path, const Kn5SceneLimits& limits,
                Kn5SceneConversion& result) {
    if (depth > limits.max_depth) invalid(path, "hierarchy is too deep");
    if (result.snapshot.nodes.size() >= limits.max_nodes ||
        result.snapshot.nodes.size() >= static_cast<std::size_t>(invalid_node_id))
        invalid(path, "node count exceeds conversion limit");

    const NodeKind kind = nodeKind(source.type, path);
    Matrix4 local = identity_matrix;
    if (kind == NodeKind::node) {
        requireFinite(source.transform, path, "node transform");
        local = source.transform;
    }
    const Matrix4 world = multiply(parentWorld, local, path);

    SceneNode node;
    node.name = source.name;
    node.kind = kind;
    node.active = source.active;
    node.transform = world;
    node.parent = parent;
    if (kind != NodeKind::node) {
        if (source.vertexStride != (kind == NodeKind::mesh ? 11U : 19U))
            invalid(path, "unexpected vertex stride");
        if (source.vertices.size() % source.vertexStride != 0)
            invalid(path, "vertex data is not stride-aligned");
        if (source.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
            source.vertices.size() / source.vertexStride > std::numeric_limits<std::uint32_t>::max())
            invalid(path, "geometry count exceeds 32-bit scene metadata");
        const auto vertexCount = source.vertices.size() / source.vertexStride;
        for (const auto value : source.vertices) requireFinite(value, path, "vertex data");
        for (const auto index : source.indices)
            if (static_cast<std::size_t>(index) >= vertexCount) invalid(path, "index references a missing vertex");
        if (source.bones.size() > std::numeric_limits<std::uint32_t>::max())
            invalid(path, "bone count exceeds 32-bit scene metadata");
        for (const auto& bone : source.bones) {
            requireFinite(bone.transform, path, "bone transform");
        }
        if (source.materialId >= result.snapshot.materials.size())
            invalid(path, "material reference is outside the material table");
        node.visible = source.visible;
        node.renderable = source.renderable;
        node.transparent = source.transparent;
        node.cast_shadows = source.castShadows;
        node.layer = source.layer;
        node.lod_in = source.lodIn;
        node.lod_out = source.lodOut;
        requireFinite(node.lod_in, path, "LOD in");
        requireFinite(node.lod_out, path, "LOD out");
        node.material = static_cast<MaterialId>(source.materialId);

        const auto bounds = geometryBounds(source, path);
        node.bounds_center = transformPoint(world, bounds.center, path);
        node.bounds_radius = bounds.radius * maxScale(world, path);
        requireFinite(node.bounds_radius, path, "world bounds radius");

        result.geometry.push_back({
            static_cast<NodeId>(result.snapshot.nodes.size()),
            static_cast<std::uint32_t>(vertexCount),
            static_cast<std::uint32_t>(source.indices.size()),
            static_cast<std::uint32_t>(source.indices.size() / 3),
            static_cast<std::uint32_t>(source.vertexStride),
            static_cast<std::uint32_t>(source.bones.size()),
            kind == NodeKind::skinned_mesh});
    }

    const NodeId id = static_cast<NodeId>(result.snapshot.nodes.size());
    node.id = id;
    result.snapshot.nodes.push_back(std::move(node));
    if (parent == invalid_node_id) {
        if (result.snapshot.root != invalid_node_id) invalid(path, "multiple scene roots");
        result.snapshot.root = id;
    } else {
        if (static_cast<std::size_t>(parent) >= result.snapshot.nodes.size() - 1)
            invalid(path, "parent ID is outside the converted scene");
        result.snapshot.nodes[static_cast<std::size_t>(parent)].children.push_back(id);
    }

    for (std::size_t index = 0; index < source.children.size(); ++index) {
        appendNode(source.children[index], id, world, depth + 1,
                   path + "/" + std::to_string(index), limits, result);
    }
}

}  // namespace

Kn5SceneError::Kn5SceneError(std::string message)
    : std::runtime_error(std::move(message)) {}

Kn5SceneConversion convertKn5Scene(const formats::Kn5File& model,
                                    const Kn5SceneLimits& limits) {
    Kn5SceneConversion result;
    if (limits.max_materials == 0 || model.materials.size() > limits.max_materials)
        invalid("materials", "material count exceeds conversion limit");
    if (model.materials.size() >= static_cast<std::size_t>(invalid_material_id))
        invalid("materials", "material count exceeds scene ID range");
    result.snapshot.materials.reserve(model.materials.size());
    for (const auto& source : model.materials) {
        if (source.blendMode > 2) invalid("materials", "unsupported material blend mode");
        result.snapshot.materials.push_back({
            source.name,
            source.shader,
            static_cast<BlendMode>(source.blendMode)});
    }
    result.snapshot.nodes.reserve(1);
    if (limits.max_nodes == 0) invalid("nodes", "node count exceeds conversion limit");
    appendNode(model.root, invalid_node_id, identity_matrix, 0, "root", limits, result);

    for (const auto& node : result.snapshot.nodes) {
        const float distance = std::sqrt(node.bounds_center[0] * node.bounds_center[0] +
                                         node.bounds_center[1] * node.bounds_center[1] +
                                         node.bounds_center[2] * node.bounds_center[2]);
        requireFinite(distance, "scene", "bounds distance");
        const float extent = distance + node.bounds_radius;
        requireFinite(extent, "scene", "bounds extent");
        result.snapshot.bounds_radius = std::max(result.snapshot.bounds_radius, extent);
    }
    return result;
}

SceneSnapshot convertKn5ToScene(const formats::Kn5File& model,
                                const Kn5SceneLimits& limits) {
    return convertKn5Scene(model, limits).snapshot;
}

}  // namespace apex::scene
