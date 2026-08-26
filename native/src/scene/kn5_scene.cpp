#include <apex/scene/kn5_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
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

struct PreviewBoundsAccumulator {
    Vector3 minimum = {std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity()};
    Vector3 maximum = {-std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity()};
    bool has_point = false;

    void include(const Vector3& point) noexcept {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            minimum[axis] = std::min(minimum[axis], point[axis]);
            maximum[axis] = std::max(maximum[axis], point[axis]);
        }
        has_point = true;
    }
};

[[nodiscard]] Kn5PreviewBounds finishPreviewBounds(
    const PreviewBoundsAccumulator& bounds) {
    Kn5PreviewBounds result;
    result.minimum = bounds.minimum;
    result.maximum = bounds.maximum;
    double squared_radius = 0.0;
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const double minimum = static_cast<double>(bounds.minimum[axis]);
        const double maximum = static_cast<double>(bounds.maximum[axis]);
        const double center = minimum + (maximum - minimum) * 0.5;
        if (!std::isfinite(center) ||
            center < -static_cast<double>(std::numeric_limits<float>::max()) ||
            center > static_cast<double>(std::numeric_limits<float>::max())) {
            invalid("scene", "preview bounds center is outside float range");
        }
        result.center[axis] = static_cast<float>(center);
        const double half_extent = (maximum - minimum) * 0.5;
        squared_radius += half_extent * half_extent;
    }
    const double radius = std::sqrt(squared_radius);
    if (!std::isfinite(radius) ||
        radius > static_cast<double>(std::numeric_limits<float>::max())) {
        invalid("scene", "preview bounds radius is outside float range");
    }
    result.radius = static_cast<float>(radius);
    return result;
}

LocalBounds geometryBounds(const formats::Kn5Node& source, std::string_view path) {
    if (source.type == 2) {
        for (const auto value : source.bounds) requireFinite(value, path, "mesh bounds");
        if (source.bounds[3] < 0.0F) invalid(path, "mesh bounds radius is negative");
        return {{source.bounds[0], source.bounds[1], source.bounds[2]}, source.bounds[3]};
    }

    // Skinned records do not carry a serialized sphere. Mesh::updateBoundingSphere
    // and SkinnedMesh::updateBoundingSphere use the arithmetic mean of the
    // positions, followed by the maximum distance from that mean.
    const std::size_t stride = source.vertexStride;
    if (stride == 0 || source.vertices.empty()) return {};
    if (source.vertices.size() % stride != 0) invalid(path, "vertex data is not stride-aligned");
    LocalBounds result;
    const std::size_t vertex_count = source.vertices.size() / stride;
    for (std::size_t offset = 0; offset < source.vertices.size(); offset += stride) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float value = source.vertices[offset + axis];
            requireFinite(value, path, "vertex position");
            result.center[axis] += value;
            requireFinite(result.center[axis], path, "vertex bounds center sum");
        }
    }
    const float inverse_count = 1.0F / static_cast<float>(vertex_count);
    requireFinite(inverse_count, path, "vertex bounds inverse count");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result.center[axis] *= inverse_count;
        requireFinite(result.center[axis], path, "vertex bounds center");
    }
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

class ConversionBudget final {
public:
    explicit ConversionBudget(const Kn5SceneLimits& limits)
        : limit_(limits.max_native_object_bytes), string_limit_(limits.max_string_bytes) {}

    void charge(std::size_t bytes, std::string_view path, std::string_view what) {
        if (bytes > limit_ - used_)
            invalid(path, "scene conversion aggregate allocation budget exceeded while reserving " +
                                std::string(what));
        used_ += bytes;
    }

    void chargeCount(std::size_t count, std::size_t element_bytes,
                     std::string_view path, std::string_view what) {
        if (count != 0U && element_bytes > std::numeric_limits<std::size_t>::max() / count)
            invalid(path, "scene conversion allocation size overflows while reserving " +
                                std::string(what));
        charge(count * element_bytes, path, what);
    }

    void chargeString(std::size_t bytes, std::string_view path, std::string_view what) {
        if (bytes > string_limit_)
            invalid(path, std::string(what) + " exceeds the configured string limit");
        if (bytes == std::numeric_limits<std::size_t>::max())
            invalid(path, "scene conversion string allocation size overflows");
        charge(bytes + 1U, path, what);
    }

    void chargePath(std::size_t bytes, std::string_view path) {
        chargeString(bytes, path, "traversal path scratch");
    }

private:
    std::size_t limit_ = 0U;
    std::size_t string_limit_ = 0U;
    std::size_t used_ = 0U;
};

struct ConversionCounts {
    std::size_t nodes = 0U;
    std::size_t geometry = 0U;
};

std::size_t decimalDigits(std::size_t value) noexcept {
    std::size_t digits = 1U;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

std::string makeChildPath(const std::string& path, std::size_t index,
                          ConversionBudget& budget) {
    if (path.size() > std::numeric_limits<std::size_t>::max() - 2U - decimalDigits(index))
        invalid(path, "scene traversal path size overflows");
    const std::size_t child_size = path.size() + 1U + decimalDigits(index);
    if (child_size == std::numeric_limits<std::size_t>::max())
        invalid(path, "scene traversal path allocation size overflows");
    budget.chargePath(child_size, path);
    budget.charge(child_size + 1U, path, "traversal path temporary");
    return path + "/" + std::to_string(index);
}

void preflightNode(const formats::Kn5Node& source, std::size_t depth,
                   std::string path, const Kn5SceneLimits& limits,
                   std::size_t material_count, ConversionBudget& budget,
                   ConversionCounts& counts, bool path_charged) {
    if (!path_charged) budget.chargePath(path.size(), path);
    if (depth > limits.max_depth) invalid(path, "hierarchy is too deep");
    if (counts.nodes >= limits.max_nodes ||
        counts.nodes >= static_cast<std::size_t>(invalid_node_id))
        invalid(path, "node count exceeds conversion limit");

    const NodeKind kind = nodeKind(source.type, path);
    budget.chargeCount(1U, sizeof(SceneNode), path, "scene nodes");
    budget.chargeString(source.name.size(), path, "node name");
    budget.chargeCount(source.children.size(), sizeof(NodeId), path, "node child links");
    ++counts.nodes;

    if (kind == NodeKind::node) {
        requireFinite(source.transform, path, "node transform");
    } else {
        if (source.vertexStride != (kind == NodeKind::mesh ? 11U : 19U))
            invalid(path, "unexpected vertex stride");
        if (source.vertices.size() % source.vertexStride != 0)
            invalid(path, "vertex data is not stride-aligned");
        if (source.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
            source.vertices.size() / source.vertexStride > std::numeric_limits<std::uint32_t>::max())
            invalid(path, "geometry count exceeds 32-bit scene metadata");
        const auto vertex_count = source.vertices.size() / source.vertexStride;
        for (const auto value : source.vertices) requireFinite(value, path, "vertex data");
        for (const auto index : source.indices)
            if (static_cast<std::size_t>(index) >= vertex_count)
                invalid(path, "index references a missing vertex");
        if (source.bones.size() > std::numeric_limits<std::uint32_t>::max())
            invalid(path, "bone count exceeds 32-bit scene metadata");
        for (const auto& bone : source.bones)
            requireFinite(bone.transform, path, "bone transform");
        if (source.materialId >= material_count)
            invalid(path, "material reference is outside the material table");
        requireFinite(source.lodIn, path, "LOD in");
        requireFinite(source.lodOut, path, "LOD out");
        (void)geometryBounds(source, path);
        budget.chargeCount(1U, sizeof(Kn5GeometryMetadata), path,
                           "geometry metadata");
        ++counts.geometry;
    }

    for (std::size_t index = 0; index < source.children.size(); ++index) {
        if (depth == std::numeric_limits<std::size_t>::max())
            invalid(path, "hierarchy depth overflows");
        std::string child_path = makeChildPath(path, index, budget);
        preflightNode(source.children[index], depth + 1U, std::move(child_path), limits,
                      material_count, budget, counts, true);
    }
}

void appendNode(const formats::Kn5Node& source, NodeId parent, const Matrix4& parentWorld,
                std::size_t depth, std::string path, const Kn5SceneLimits& limits,
                bool parent_active, PreviewBoundsAccumulator& preview_bounds,
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
    const bool branch_active = parent_active && source.active;

    SceneNode node;
    node.name = source.name;
    node.children.reserve(source.children.size());
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
        node.local_bounds_center = bounds.center;
        node.local_bounds_radius = bounds.radius;
        node.local_bounds_source = kind == NodeKind::mesh
                                       ? LocalBoundsSource::kn5_serialized
                                       : LocalBoundsSource::kn5_vertex_mean;
        node.bounds_center = transformPoint(world, bounds.center, path);
        node.bounds_radius = bounds.radius * maxScale(world, path);
        requireFinite(node.bounds_radius, path, "world bounds radius");

        if (branch_active && source.visible && source.renderable) {
            for (std::size_t offset = 0U; offset < source.vertices.size();
                 offset += source.vertexStride) {
                const Vector3 position = {source.vertices[offset],
                                          source.vertices[offset + 1U],
                                          source.vertices[offset + 2U]};
                preview_bounds.include(transformPoint(world, position, path));
            }
        }

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
                   path + "/" + std::to_string(index), limits, branch_active,
                   preview_bounds, result);
    }
}

}  // namespace

Kn5SceneError::Kn5SceneError(std::string message)
    : std::runtime_error(std::move(message)) {}

Kn5SceneConversion convertKn5Scene(const formats::Kn5File& model,
                                    const Kn5SceneLimits& limits) {
    try {
        if (limits.max_materials == 0 || model.materials.size() > limits.max_materials)
            invalid("materials", "material count exceeds conversion limit");
        if (model.materials.size() >= static_cast<std::size_t>(invalid_material_id))
            invalid("materials", "material count exceeds scene ID range");
        if (limits.max_nodes == 0)
            invalid("nodes", "node count exceeds conversion limit");
        if (limits.max_native_object_bytes == 0U)
            invalid("scene", "scene conversion aggregate allocation budget is zero");

        ConversionBudget budget(limits);
        budget.chargeCount(model.materials.size(), sizeof(SceneMaterial), "materials",
                           "scene materials");
        for (const auto& source : model.materials) {
            if (source.blendMode > 2)
                invalid("materials", "unsupported material blend mode");
            budget.chargeString(source.name.size(), "materials", "material name");
            budget.chargeString(source.shader.size(), "materials", "shader name");
        }
        ConversionCounts counts;
        preflightNode(model.root, 0U, "root", limits, model.materials.size(), budget,
                      counts, false);

        Kn5SceneConversion result;
        result.snapshot.materials.reserve(model.materials.size());
        for (const auto& source : model.materials) {
            result.snapshot.materials.push_back({
                source.name,
                source.shader,
                static_cast<BlendMode>(source.blendMode)});
        }
        result.snapshot.nodes.reserve(counts.nodes);
        result.geometry.reserve(counts.geometry);
        PreviewBoundsAccumulator preview_bounds;
        appendNode(model.root, invalid_node_id, identity_matrix, 0, "root", limits,
                   true, preview_bounds, result);
        if (preview_bounds.has_point)
            result.preview_bounds = finishPreviewBounds(preview_bounds);

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
    } catch (const Kn5SceneError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw Kn5SceneError(
            "KN5 scene conversion allocation failed within the configured aggregate budget");
    }
}

SceneSnapshot convertKn5ToScene(const formats::Kn5File& model,
                                const Kn5SceneLimits& limits) {
    return convertKn5Scene(model, limits).snapshot;
}

}  // namespace apex::scene
