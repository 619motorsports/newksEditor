#include "apex/render/fbx_render_adapter.hpp"

#include "apex/render/decoded_dds_texture.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

constexpr std::size_t max_exact_float_bone_count =
    std::size_t{1U} << std::numeric_limits<float>::digits;

using apex::formats::FbxNodeGeometry;
using apex::formats::FbxStaticMesh;
using apex::formats::Kn5File;
using apex::formats::Kn5Material;
using apex::formats::Kn5MaterialProperty;
using apex::formats::Kn5Node;
using apex::formats::Kn5Texture;
using apex::scene::Matrix4;
using apex::scene::NodeId;

class AdapterFailure final : public std::runtime_error {
public:
    AdapterFailure(FbxRenderAdapterStatus status,
                   FbxRenderAdapterDiagnostic diagnostic)
        : std::runtime_error(diagnostic.message), status_(status),
          diagnostic_(std::move(diagnostic)) {}

    FbxRenderAdapterStatus status_;
    FbxRenderAdapterDiagnostic diagnostic_;
};

[[noreturn]] void fail(FbxRenderAdapterStatus status, std::string code,
                       std::string message, std::string path) {
    throw AdapterFailure(
        status, {std::move(code), std::move(message), std::move(path)});
}

std::size_t checked_add(std::size_t left, std::size_t right,
                        std::string_view path) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        fail(FbxRenderAdapterStatus::resource_limit, "output_limit",
             "FBX render adapter allocation size overflows",
             std::string(path));
    return left + right;
}

std::size_t checked_multiply(std::size_t left, std::size_t right,
                             std::string_view path) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
        fail(FbxRenderAdapterStatus::resource_limit, "output_limit",
             "FBX render adapter allocation size overflows",
             std::string(path));
    return left * right;
}

class OutputBudget final {
public:
    explicit OutputBudget(std::size_t limit) : limit_(limit) {}

    void add(std::size_t bytes, std::string_view path) {
        if (bytes > limit_ - used_)
            fail(FbxRenderAdapterStatus::resource_limit, "output_limit",
                 "FBX render adapter output exceeds its byte limit",
                 std::string(path));
        used_ += bytes;
    }

    [[nodiscard]] std::size_t used() const noexcept { return used_; }

private:
    std::size_t limit_ = 0U;
    std::size_t used_ = 0U;
};

bool finite_matrix(const Matrix4& matrix) {
    return std::all_of(matrix.begin(), matrix.end(),
                       [](float value) { return std::isfinite(value); });
}

bool valid_texture_basename(std::string_view value,
                            std::size_t max_bytes) noexcept {
    if (value.empty() || value.size() > max_bytes)
        return false;
    std::size_t first = 0U;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0)
        --last;
    const auto trimmed = value.substr(first, last - first);
    if (trimmed.empty() || trimmed == "." || trimmed == "..") return false;
    const bool characters_valid =
        std::all_of(value.begin(), value.end(), [&](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return character != '/' && character != '\\' &&
                   character != ':' && byte >= 0x20U && byte != 0x7fU;
        });
    return characters_valid;
}

std::string texture_key(std::string_view value) {
    std::size_t first = 0U;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0)
        --last;
    std::string result;
    result.reserve(last - first);
    for (std::size_t index = first; index < last; ++index) {
        const char character = value[index];
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(byte >= static_cast<unsigned char>('A') &&
                                 byte <= static_cast<unsigned char>('Z')
                             ? static_cast<char>(byte + ('a' - 'A'))
                             : character);
    }
    return result;
}

struct Vector3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

Vector3 subtract(Vector3 left, Vector3 right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vector3 cross(Vector3 left, Vector3 right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

Vector3 normalize_if_nonzero(Vector3 value) {
    const float length =
        std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length != 0.0F) {
        value.x /= length;
        value.y /= length;
        value.z /= length;
    }
    return value;
}

bool finite(Vector3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool zero(Vector3 value) {
    return value.x == 0.0F && value.y == 0.0F && value.z == 0.0F;
}

Vector3 transform_position(const Matrix4& matrix, Vector3 value,
                           std::string_view path) {
    const std::array<double, 3U> transformed = {
        static_cast<double>(matrix[0]) * value.x +
            static_cast<double>(matrix[4]) * value.y +
            static_cast<double>(matrix[8]) * value.z + matrix[12],
        static_cast<double>(matrix[1]) * value.x +
            static_cast<double>(matrix[5]) * value.y +
            static_cast<double>(matrix[9]) * value.z + matrix[13],
        static_cast<double>(matrix[2]) * value.x +
            static_cast<double>(matrix[6]) * value.y +
            static_cast<double>(matrix[10]) * value.z + matrix[14],
    };
    for (const auto component : transformed) {
        if (!std::isfinite(component) ||
            component < -static_cast<double>(
                            std::numeric_limits<float>::max()) ||
            component > static_cast<double>(
                            std::numeric_limits<float>::max()))
            fail(FbxRenderAdapterStatus::unsupported, "non_finite_position",
                 "Geometric FBX position is not a finite float",
                 std::string(path));
    }
    return {static_cast<float>(transformed[0]),
            static_cast<float>(transformed[1]),
            static_cast<float>(transformed[2])};
}

Vector3 transform_normal(const Matrix4& matrix, Vector3 value,
                         std::string_view path) {
    // The recovered importer uses the geometric matrix's upper-left 3x3. It
    // does not use an inverse-transpose matrix.
    const std::array<double, 3U> transformed = {
        static_cast<double>(matrix[0]) * value.x +
            static_cast<double>(matrix[4]) * value.y +
            static_cast<double>(matrix[8]) * value.z,
        static_cast<double>(matrix[1]) * value.x +
            static_cast<double>(matrix[5]) * value.y +
            static_cast<double>(matrix[9]) * value.z,
        static_cast<double>(matrix[2]) * value.x +
            static_cast<double>(matrix[6]) * value.y +
            static_cast<double>(matrix[10]) * value.z,
    };
    for (const auto component : transformed) {
        if (!std::isfinite(component) ||
            component < -static_cast<double>(
                            std::numeric_limits<float>::max()) ||
            component > static_cast<double>(
                            std::numeric_limits<float>::max()))
            fail(FbxRenderAdapterStatus::unsupported, "non_finite_normal",
                 "Geometric FBX normal is not a finite float",
                 std::string(path));
    }
    auto result = Vector3{static_cast<float>(transformed[0]),
                          static_cast<float>(transformed[1]),
                          static_cast<float>(transformed[2])};
    result = normalize_if_nonzero(result);
    if (!finite(result))
        fail(FbxRenderAdapterStatus::unsupported, "non_finite_normal",
             "Normalized FBX normal is not finite", std::string(path));
    return result;
}

Vector3 position_at(const std::vector<float>& vertices, std::size_t vertex,
                    std::size_t stride) {
    const auto offset = vertex * stride;
    return {vertices[offset], vertices[offset + 1U], vertices[offset + 2U]};
}

Vector3 normal_at(const std::vector<float>& vertices, std::size_t vertex,
                  std::size_t stride) {
    const auto offset = vertex * stride + 3U;
    return {vertices[offset], vertices[offset + 1U], vertices[offset + 2U]};
}

void set_tangent(std::vector<float>& vertices, std::size_t vertex,
                 Vector3 tangent, std::size_t stride) {
    const auto offset = vertex * stride + 8U;
    vertices[offset] = tangent.x;
    vertices[offset + 1U] = tangent.y;
    vertices[offset + 2U] = tangent.z;
}

void generate_native_tangents(std::vector<float>& vertices,
                              std::size_t stride) {
    constexpr float fallback = 0.5773502691896258F;
    const auto vertex_count = vertices.size() / stride;
    for (std::size_t first = 0U; first < vertex_count; first += 3U) {
        const auto second = first + 1U;
        const auto third = first + 2U;
        const auto p0 = position_at(vertices, first, stride);
        const auto p1 = position_at(vertices, second, stride);
        const auto p2 = position_at(vertices, third, stride);
        const auto e1 = subtract(p1, p0);
        const auto e2 = subtract(p2, p0);
        const auto uv0 = first * stride + 6U;
        const auto uv1 = second * stride + 6U;
        const auto uv2 = third * stride + 6U;
        const float duv1x = vertices[uv1] - vertices[uv0];
        const float duv1y = vertices[uv1 + 1U] - vertices[uv0 + 1U];
        const float duv2x = vertices[uv2] - vertices[uv0];
        const float duv2y = vertices[uv2 + 1U] - vertices[uv0 + 1U];
        const Vector3 a = {e1.x * duv2y - e2.x * duv1y,
                           e1.y * duv2y - e2.y * duv1y,
                           e1.z * duv2y - e2.z * duv1y};
        const Vector3 b = {e2.x * duv1x - e1.x * duv2x,
                           e2.y * duv1x - e1.y * duv2x,
                           e2.z * duv1x - e1.z * duv2x};
        for (const auto vertex : {first, second, third}) {
            const auto normal = normal_at(vertices, vertex, stride);
            auto projected = normalize_if_nonzero(cross(normal, a));
            auto tangent = zero(projected) ? cross(b, normal)
                                           : cross(projected, normal);
            tangent = normalize_if_nonzero(tangent);
            if (!finite(tangent)) tangent = {1.0F, 0.0F, 0.0F};
            else if (zero(tangent)) tangent = {fallback, fallback, fallback};
            set_tangent(vertices, vertex, tangent, stride);
        }
    }
}

std::array<float, 4U> native_bounds(const std::vector<float>& vertices,
                                    std::size_t stride,
                                    std::string_view path) {
    const auto vertex_count = vertices.size() / stride;
    if (vertex_count == 0U)
        fail(FbxRenderAdapterStatus::invalid_request, "empty_batch",
             "FBX material batch has no vertices", std::string(path));
    Vector3 center{};
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
        const auto position = position_at(vertices, vertex, stride);
        center.x += position.x;
        center.y += position.y;
        center.z += position.z;
        if (!finite(center))
            fail(FbxRenderAdapterStatus::unsupported, "non_finite_bounds",
                 "FBX mesh bounds sum is not finite", std::string(path));
    }
    const float inverse = 1.0F / static_cast<float>(vertex_count);
    center.x *= inverse;
    center.y *= inverse;
    center.z *= inverse;
    if (!finite(center))
        fail(FbxRenderAdapterStatus::unsupported, "non_finite_bounds",
             "FBX mesh bounds center is not finite", std::string(path));
    float radius = 0.0F;
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
        const auto delta = subtract(position_at(vertices, vertex, stride), center);
        const float distance =
            std::sqrt(delta.x * delta.x + delta.y * delta.y +
                      delta.z * delta.z);
        if (!std::isfinite(distance))
            fail(FbxRenderAdapterStatus::unsupported, "non_finite_bounds",
                 "FBX mesh bounds radius is not finite", std::string(path));
        radius = std::max(radius, distance);
    }
    return {center.x, center.y, center.z, radius};
}

class Builder final {
public:
    Builder(const apex::formats::FbxSceneConversion& conversion,
            const FbxExternalTextureAuthorityResult* textures,
            std::string source, FbxRenderAdapterLimits limits)
        : conversion_(conversion), textures_(textures), source_(std::move(source)),
          limits_(std::move(limits)), budget_(limits_.max_output_bytes) {}

    FbxRenderAdapterResult build() {
        validate_limits();
        validate_conversion();
        build_materials_and_textures();

        model_.source = source_;
        model_.magic = "sc6969";
        model_.version = 6U;
        model_.root = make_wrapper(conversion_.snapshot.root, 0U, true);
        model_.root.transform = apex::scene::identity_matrix;
        if (std::any_of(emitted_.begin(), emitted_.end(),
                        [](bool value) { return !value; }))
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_hierarchy",
                 "FBX source hierarchy contains an unreachable node", "nodes");

        if (output_meshes_ == 0U) {
            staged_ = true;
            add_diagnostic("missing_geometry",
                           "The canonical FBX model has no static mesh",
                           "geometry");
        }

        if (!conversion_.complete) {
            staged_ = true;
            add_diagnostic(
                "incomplete_fbx_conversion",
                "The FBX conversion contains unsupported source behavior; the canonical model remains staged",
                "conversion");
        }

        auto converted = apex::scene::convertKn5Scene(model_, limits_.scene);
        FbxRenderAdapterResult result;
        result.status = staged_ ? FbxRenderAdapterStatus::staged
                                : FbxRenderAdapterStatus::ready;
        result.accounted_model_bytes = budget_.used();
        result.model = std::move(model_);
        result.scene = std::move(converted);
        result.diagnostics = std::move(diagnostics_);
        return result;
    }

private:
    void validate_limits() const {
        if (limits_.max_materials == 0U || limits_.max_textures == 0U ||
            limits_.max_embedded_images == 0U ||
            limits_.max_embedded_candidates == 0U ||
            limits_.max_nodes == 0U || limits_.max_meshes == 0U ||
            limits_.max_batches_per_geometry == 0U ||
            limits_.max_vertices == 0U || limits_.max_indices == 0U ||
            limits_.max_bones_per_mesh == 0U ||
            limits_.max_vertices_per_mesh == 0U ||
            limits_.max_vertices_per_mesh > 65'536U ||
            limits_.max_depth == 0U || limits_.max_name_bytes == 0U ||
            limits_.max_diagnostics == 0U ||
            limits_.max_diagnostic_bytes == 0U ||
            limits_.max_output_bytes == 0U ||
            limits_.max_total_embedded_source_bytes == 0U ||
            limits_.max_total_embedded_decoded_bytes == 0U ||
            limits_.embedded_decode.maxInputBytes == 0U ||
            limits_.embedded_decode.maxOutputBytes == 0U)
            fail(FbxRenderAdapterStatus::invalid_request, "invalid_limits",
                 "FBX render adapter limits are invalid", "limits");
    }

    void validate_name(std::string_view name, std::string_view path) const {
        if (name.size() > limits_.max_name_bytes)
            fail(FbxRenderAdapterStatus::resource_limit, "name_limit",
                 "FBX render adapter name exceeds its byte limit",
                 std::string(path));
    }

    void validate_conversion() {
        if (source_.size() > limits_.max_name_bytes)
            fail(FbxRenderAdapterStatus::resource_limit, "name_limit",
                 "FBX source name exceeds its byte limit", "source");
        if (conversion_.snapshot.materials.size() > limits_.max_materials ||
            conversion_.snapshot.materials.size() >=
                static_cast<std::size_t>(apex::scene::invalid_material_id))
            fail(FbxRenderAdapterStatus::resource_limit, "material_limit",
                 "FBX material count exceeds the adapter limit", "materials");
        if (conversion_.embedded_images.size() >
                limits_.max_embedded_images ||
            conversion_.embedded_texture_candidates.size() >
                limits_.max_embedded_candidates)
            fail(FbxRenderAdapterStatus::resource_limit,
                 "embedded_texture_count_limit",
                 "FBX embedded texture metadata exceeds the adapter limit",
                 "textures/embedded");
        if (conversion_.snapshot.materials.size() !=
            conversion_.material_parameters.size())
            fail(FbxRenderAdapterStatus::invalid_request,
                 "material_parameter_mismatch",
                 "FBX material parameters do not match the material table",
                 "materials");
        if (conversion_.snapshot.nodes.empty() ||
            conversion_.snapshot.root >= conversion_.snapshot.nodes.size())
            fail(FbxRenderAdapterStatus::invalid_request, "invalid_hierarchy",
                 "FBX conversion has no valid scene root", "nodes");
        if (conversion_.snapshot.nodes.size() > limits_.max_nodes)
            fail(FbxRenderAdapterStatus::resource_limit, "node_limit",
                 "FBX source node count exceeds the adapter limit", "nodes");
        if (conversion_.snapshot.nodes[conversion_.snapshot.root].parent !=
            apex::scene::invalid_node_id)
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_hierarchy",
                 "FBX source root has a parent", "nodes");
        for (std::size_t index = 0U;
             index < conversion_.snapshot.nodes.size(); ++index) {
            const auto& node = conversion_.snapshot.nodes[index];
            if (node.id != index)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_hierarchy",
                     "FBX source node IDs are not dense and ordered",
                     "nodes/" + std::to_string(index));
            validate_name(node.name, "nodes/" + std::to_string(index));
            for (const auto child : node.children) {
                if (child >= conversion_.snapshot.nodes.size() ||
                    conversion_.snapshot.nodes[child].parent != node.id)
                    fail(FbxRenderAdapterStatus::invalid_request,
                         "invalid_hierarchy",
                         "FBX source child link is invalid",
                         "nodes/" + std::to_string(index));
            }
        }
        for (const auto& transform : conversion_.transforms) {
            if (transform.node >= conversion_.snapshot.nodes.size() ||
                transform.node == conversion_.snapshot.root ||
                locals_.contains(transform.node) ||
                !finite_matrix(transform.local))
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_transform",
                     "FBX node transform table is malformed", "transforms");
            budget_.add(sizeof(std::pair<const NodeId, Matrix4>), "transforms");
            locals_.emplace(transform.node, transform.local);
        }
        for (std::size_t index = 0U;
             index < conversion_.snapshot.nodes.size(); ++index) {
            if (index == conversion_.snapshot.root) continue;
            if (!locals_.contains(static_cast<NodeId>(index)))
                fail(FbxRenderAdapterStatus::invalid_request,
                     "missing_transform",
                     "FBX node has no retained local transform",
                     "nodes/" + std::to_string(index));
        }
        for (const auto& geometry : conversion_.node_geometry) {
            if (geometry.node >= conversion_.snapshot.nodes.size() ||
                geometry.mesh >= conversion_.meshes.size() ||
                geometries_.contains(geometry.node) ||
                !finite_matrix(geometry.geometric))
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_geometry_mapping",
                     "FBX node-to-geometry table is malformed",
                     "node_geometry");
            for (const auto material : geometry.materials)
                if (material >= conversion_.snapshot.materials.size())
                    fail(FbxRenderAdapterStatus::invalid_request,
                         "invalid_material_reference",
                         "FBX node material slot references a missing material",
                         "node_geometry");
            budget_.add(sizeof(std::pair<const NodeId,
                                         const FbxNodeGeometry*>),
                        "node_geometry");
            geometries_.emplace(geometry.node, &geometry);
        }
    }

    void add_property(Kn5Material& material, std::string name, float value,
                      std::string_view path) {
        if (!std::isfinite(value))
            fail(FbxRenderAdapterStatus::invalid_request,
                 "non_finite_material",
                 "FBX material property is not finite", std::string(path));
        budget_.add(checked_add(sizeof(Kn5MaterialProperty), name.size(), path),
                    path);
        material.properties.push_back({std::move(name), value, {}, {}, {}});
    }

    void build_materials_and_textures() {
        const auto material_count = conversion_.snapshot.materials.size();
        budget_.add(checked_multiply(material_count, sizeof(Kn5Material),
                                     "materials"),
                    "materials");
        model_.materials.reserve(material_count);
        material_has_texture_.assign(material_count, false);
        for (std::size_t index = 0U; index < material_count; ++index) {
            const auto& source = conversion_.snapshot.materials[index];
            validate_name(source.name, "materials/" + std::to_string(index));
            validate_name(source.shader, "materials/" + std::to_string(index));
            budget_.add(checked_add(source.name.size(), source.shader.size(),
                                    "materials"),
                        "materials");
            Kn5Material material;
            material.name = source.name;
            material.shader = "ksPerPixel";
            const auto& parameters = conversion_.material_parameters[index];
            if (parameters.ambient_color.has_value())
                add_property(material, "ksAmbient",
                             (*parameters.ambient_color)[0], "materials");
            if (parameters.diffuse_color.has_value())
                add_property(material, "ksDiffuse",
                             (*parameters.diffuse_color)[0], "materials");
            if (parameters.specular_color.has_value())
                add_property(material, "ksSpecular",
                             (*parameters.specular_color)[0], "materials");
            float exponent = parameters.shininess.value_or(1.0F);
            if (exponent < 1.0F) exponent = 10.0F;
            add_property(material, "ksSpecularEXP", exponent, "materials");
            model_.materials.push_back(std::move(material));
        }

        // Embedded Video content is a compatibility path used by the WebGL
        // loader. Recovered ksEditor behavior remains represented separately
        // by file_texture_candidates and the external authority below.
        std::map<std::string,
                 std::tuple<bool, std::size_t, std::size_t>> by_name;
        budget_.add(
            checked_multiply(material_count, sizeof(std::size_t),
                             "textures/embedded metadata"),
            "textures/embedded metadata");
        budget_.add(
            checked_multiply(conversion_.embedded_texture_candidates.size(),
                             sizeof(std::size_t),
                             "textures/embedded metadata"),
            "textures/embedded metadata");
        budget_.add(
            checked_multiply(conversion_.embedded_images.size(), sizeof(bool),
                             "textures/embedded metadata"),
            "textures/embedded metadata");
        std::vector<std::size_t> selected_embedded(
            material_count, std::numeric_limits<std::size_t>::max());
        std::vector<std::size_t> embedded_order(
            conversion_.embedded_texture_candidates.size());
        for (std::size_t index = 0U; index < embedded_order.size(); ++index)
            embedded_order[index] = index;
        std::sort(embedded_order.begin(), embedded_order.end(),
                  [&](std::size_t left_index, std::size_t right_index) {
                      const auto& left = conversion_.embedded_texture_candidates
                          [left_index];
                      const auto& right = conversion_.embedded_texture_candidates
                          [right_index];
                      return std::tuple{
                                 left.material,
                                 apex::formats::fbxNativeFileTextureChannelRank(
                                     left.channel)
                                     .value_or(std::numeric_limits<std::size_t>::max()),
                                 left.connection_order, left.texture_object_id,
                                 left_index} <
                             std::tuple{
                                 right.material,
                                 apex::formats::fbxNativeFileTextureChannelRank(
                                     right.channel)
                                     .value_or(std::numeric_limits<std::size_t>::max()),
                                 right.connection_order, right.texture_object_id,
                                 right_index};
                  });

        std::set<std::int64_t> video_ids;
        std::vector<bool> image_referenced(
            conversion_.embedded_images.size(), false);
        std::size_t total_source_bytes = 0U;
        bool embedded_payloads_valid = true;
        for (std::size_t image_index = 0U;
             image_index < conversion_.embedded_images.size(); ++image_index) {
            const auto& image = conversion_.embedded_images[image_index];
            const auto path =
                "textures/embedded/" + std::to_string(image_index);
            if (image.video_object_id <= 0 ||
                !video_ids.insert(image.video_object_id).second)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_embedded_image_identity",
                     "FBX embedded image identity is invalid", path);
            budget_.add(sizeof(std::int64_t),
                        "textures/embedded video identities");
            if (!valid_texture_basename(image.basename,
                                        limits_.max_name_bytes))
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_name",
                     "FBX embedded texture basename is unsafe", path);
            if (image.content.empty() ||
                image.content.size() >
                    std::numeric_limits<std::uint32_t>::max() ||
                image.content.size() > limits_.embedded_decode.maxInputBytes ||
                image.content.size() >
                    limits_.max_total_embedded_source_bytes -
                        total_source_bytes)
                fail(FbxRenderAdapterStatus::resource_limit,
                     "embedded_texture_source_limit",
                     "FBX embedded texture source bytes exceed their limit",
                     path);
            total_source_bytes += image.content.size();
        }

        std::optional<std::pair<apex::scene::MaterialId,
                                std::pair<std::size_t, std::size_t>>>
            previous_embedded_order;
        for (const auto candidate_index : embedded_order) {
            const auto& candidate = conversion_.embedded_texture_candidates
                [candidate_index];
            const auto rank =
                apex::formats::fbxNativeFileTextureChannelRank(
                    candidate.channel);
            if (candidate.material >= material_count ||
                candidate.texture_object_id <= 0 ||
                candidate.video_object_id <= 0 || !rank.has_value() ||
                candidate.embedded_image_index >=
                    conversion_.embedded_images.size() ||
                conversion_.embedded_images[candidate.embedded_image_index]
                        .video_object_id != candidate.video_object_id)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_embedded_texture_candidate",
                     "FBX embedded texture candidate is malformed",
                     "textures/embedded/candidates/" +
                         std::to_string(candidate_index));
            const auto source_order = std::pair{*rank,
                                                 candidate.connection_order};
            if (previous_embedded_order.has_value() &&
                previous_embedded_order->first == candidate.material &&
                previous_embedded_order->second == source_order)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "duplicate_embedded_texture_order",
                     "FBX embedded texture candidates duplicate one source order",
                     "textures/embedded/candidates/" +
                         std::to_string(candidate_index));
            previous_embedded_order =
                std::pair{candidate.material, source_order};
            image_referenced[candidate.embedded_image_index] = true;
            if (selected_embedded[candidate.material] ==
                std::numeric_limits<std::size_t>::max())
                selected_embedded[candidate.material] = candidate_index;
        }
        if (!std::all_of(image_referenced.begin(), image_referenced.end(),
                         [](bool value) { return value; }))
            fail(FbxRenderAdapterStatus::invalid_request,
                 "unreferenced_embedded_image",
                 "FBX embedded image table contains an unreferenced payload",
                 "textures/embedded");

        std::size_t total_decoded_bytes = 0U;
        for (std::size_t image_index = 0U;
             image_index < conversion_.embedded_images.size(); ++image_index) {
            const auto& image = conversion_.embedded_images[image_index];
            auto decode_limits = limits_.embedded_decode;
            decode_limits.maxInputBytes = std::min(
                decode_limits.maxInputBytes, image.content.size());
            const auto remaining_decoded =
                limits_.max_total_embedded_decoded_bytes -
                total_decoded_bytes;
            if (remaining_decoded == 0U)
                fail(FbxRenderAdapterStatus::resource_limit,
                     "embedded_texture_decoded_limit",
                     "Decoded FBX embedded texture bytes exceed their limit",
                     "textures/embedded/" + std::to_string(image_index));
            decode_limits.maxOutputBytes = std::min(
                decode_limits.maxOutputBytes,
                remaining_decoded);
            const auto planned = plan_decoded_texture_payload(
                image.content, image.basename, decode_limits);
            if (!planned.ok()) {
                if (planned.diagnostic.code == "input_too_large" ||
                    planned.diagnostic.code == "output_too_large" ||
                    planned.diagnostic.code == "allocation_failed")
                    fail(FbxRenderAdapterStatus::resource_limit,
                         "embedded_texture_decode_" +
                             planned.diagnostic.code,
                         planned.diagnostic.message,
                         "textures/embedded/" +
                             std::to_string(image_index));
                embedded_payloads_valid = false;
                staged_ = true;
                add_diagnostic(
                    "embedded_texture_decode_" + planned.diagnostic.code,
                    planned.diagnostic.message,
                    "textures/embedded/" + std::to_string(image_index));
                continue;
            }
            for (const auto& level : planned.plan.levels) {
                if (level.pixels.size() >
                    limits_.max_total_embedded_decoded_bytes -
                        total_decoded_bytes)
                    fail(FbxRenderAdapterStatus::resource_limit,
                         "embedded_texture_decoded_limit",
                         "Decoded FBX embedded texture bytes exceed their limit",
                         "textures/embedded/" +
                             std::to_string(image_index));
                total_decoded_bytes += level.pixels.size();
            }
        }

        if (embedded_payloads_valid) {
            std::map<std::size_t, std::size_t> emitted_images;
            for (std::size_t material = 0U; material < material_count;
                 ++material) {
                const auto candidate_index = selected_embedded[material];
                if (candidate_index == std::numeric_limits<std::size_t>::max())
                    continue;
                const auto& candidate =
                    conversion_.embedded_texture_candidates[candidate_index];
                const auto image_index = candidate.embedded_image_index;
                const auto& image = conversion_.embedded_images[image_index];
                std::size_t texture_index = 0U;
                const auto existing_image = emitted_images.find(image_index);
                if (existing_image != emitted_images.end()) {
                    texture_index = existing_image->second;
                } else {
                    const auto canonical_name = texture_key(image.basename);
                    const auto collision = by_name.find(canonical_name);
                    if (collision != by_name.end())
                        fail(FbxRenderAdapterStatus::invalid_request,
                             "texture_name_collision",
                             "Distinct FBX texture payloads use the same basename",
                             "textures/embedded/" +
                                 std::to_string(image_index));
                    if (model_.textures.size() >= limits_.max_textures)
                        fail(FbxRenderAdapterStatus::resource_limit,
                             "texture_limit",
                             "FBX texture count exceeds the adapter limit",
                             "textures");
                    budget_.add(
                        checked_add(sizeof(Kn5Texture), image.basename.size(),
                                    "textures"),
                        "textures");
                    budget_.add(image.content.size(), "textures");
                    Kn5Texture texture;
                    texture.active = true;
                    texture.name = image.basename;
                    texture.size = static_cast<std::uint32_t>(
                        image.content.size());
                    texture.data = image.content;
                    texture_index = model_.textures.size();
                    model_.textures.push_back(std::move(texture));
                    by_name.emplace(canonical_name,
                                    std::tuple{true, image_index,
                                               texture_index});
                    budget_.add(
                        sizeof(std::pair<
                            const std::string,
                            std::tuple<bool, std::size_t, std::size_t>>) +
                            canonical_name.size(),
                        "textures/embedded names");
                    emitted_images.emplace(image_index, texture_index);
                    budget_.add(
                        sizeof(std::pair<const std::size_t, std::size_t>),
                        "textures/embedded emitted images");
                }
                material_has_texture_[material] = true;
                const auto& emitted_name = model_.textures[texture_index].name;
                budget_.add(sizeof(apex::formats::Kn5MaterialResource) +
                                std::string_view{"txDiffuse"}.size() +
                                emitted_name.size(),
                            "materials/resources");
                model_.materials[material].resources.push_back(
                    {"txDiffuse", 0U, emitted_name});
            }
        }
        if (!conversion_.embedded_texture_candidates.empty())
            add_diagnostic(
                "embedded_image_compatibility",
                "FBX Video.Content uses WebGL compatibility behavior, not recovered ksEditor image loading",
                "textures/embedded");

        if (textures_ == nullptr) return;
        if (!textures_->ok() ||
            textures_->material_selection_indices.size() != material_count ||
            textures_->selections.size() > material_count ||
            textures_->authority.request_resource_indices.size() !=
                textures_->selections.size())
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_texture_authority",
                 "FBX texture authority result is incomplete", "textures");

        std::vector<bool> selection_seen(textures_->selections.size(), false);
        for (std::size_t material = 0U; material < material_count; ++material) {
            const auto selection_index =
                textures_->material_selection_indices[material];
            if (selection_index == invalid_fbx_texture_selection) continue;
            if (material_has_texture_[material])
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_authority",
                     "External FBX texture authority shadows embedded content",
                     "textures/materials/" + std::to_string(material));
            if (selection_index >= textures_->selections.size())
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_selection",
                     "FBX material texture selection is outside its table",
                     "textures/materials/" + std::to_string(material));
            const auto& selection = textures_->selections[selection_index];
            if (selection_seen[selection_index] ||
                selection.material_index != material ||
                selection.candidate_index >=
                    conversion_.file_texture_candidates.size() ||
                selection.texture_object_id <= 0 ||
                !apex::formats::fbxNativeFileTextureChannelRank(
                     selection.channel).has_value() ||
                selection.authority_resource_index ==
                    invalid_external_texture_resource ||
                selection.authority_resource_index >=
                    textures_->authority.resources.size() ||
                textures_->authority.request_resource_indices[selection_index] !=
                    selection.authority_resource_index)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_selection",
                     "FBX material texture selection is malformed",
                     "textures/materials/" + std::to_string(material));
            selection_seen[selection_index] = true;
            const auto& candidate = conversion_.file_texture_candidates
                [selection.candidate_index];
            if (candidate.material != material ||
                candidate.texture_object_id != selection.texture_object_id ||
                candidate.channel != selection.channel ||
                candidate.basename != selection.basename)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_selection",
                     "FBX texture selection does not match its source candidate",
                     "textures/materials/" + std::to_string(material));
            if (!valid_texture_basename(selection.basename,
                                        limits_.max_name_bytes))
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_name",
                     "FBX selected texture basename is unsafe",
                     "textures/materials/" + std::to_string(material));
            const auto& resource = textures_->authority.resources
                [selection.authority_resource_index];
            if (resource.source_bytes.empty() ||
                resource.source_bytes.size() >
                    std::numeric_limits<std::uint32_t>::max())
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_texture_payload",
                     "FBX selected texture payload is empty or too large",
                     "textures/materials/" + std::to_string(material));

            std::size_t texture_index = 0U;
            const auto canonical_name = texture_key(selection.basename);
            const auto existing = by_name.find(canonical_name);
            if (existing != by_name.end()) {
                if (std::get<0>(existing->second) ||
                    std::get<1>(existing->second) !=
                        selection.authority_resource_index)
                    fail(FbxRenderAdapterStatus::invalid_request,
                         "texture_name_collision",
                         "Distinct FBX texture payloads use the same basename",
                         "textures/materials/" + std::to_string(material));
                texture_index = std::get<2>(existing->second);
            } else {
                if (model_.textures.size() >= limits_.max_textures)
                    fail(FbxRenderAdapterStatus::resource_limit,
                         "texture_limit",
                         "FBX texture count exceeds the adapter limit",
                         "textures");
                budget_.add(
                    checked_add(sizeof(Kn5Texture), selection.basename.size(),
                                "textures"),
                    "textures");
                budget_.add(resource.source_bytes.size(), "textures");
                Kn5Texture texture;
                texture.active = true;
                texture.name = selection.basename;
                texture.size =
                    static_cast<std::uint32_t>(resource.source_bytes.size());
                texture.data = resource.source_bytes;
                texture_index = model_.textures.size();
                model_.textures.push_back(std::move(texture));
                budget_.add(sizeof(std::pair<
                                    const std::string,
                                    std::pair<std::size_t, std::size_t>>) +
                                selection.basename.size(),
                            "textures");
                by_name.emplace(canonical_name,
                                std::tuple{false,
                                           selection.authority_resource_index,
                                           texture_index});
            }
            material_has_texture_[material] = true;
            const auto& emitted_name = model_.textures[texture_index].name;
            budget_.add(sizeof(apex::formats::Kn5MaterialResource) +
                            std::string_view{"txDiffuse"}.size() +
                            emitted_name.size(),
                        "materials/resources");
            model_.materials[material].resources.push_back(
                {"txDiffuse", 0U, emitted_name});
        }
        if (!std::all_of(selection_seen.begin(), selection_seen.end(),
                         [](bool seen) { return seen; }))
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_texture_authority",
                 "FBX texture authority contains an unreferenced selection",
                 "textures");
    }

    std::uint32_t fallback_material() {
        if (fallback_material_.has_value()) return *fallback_material_;
        if (model_.materials.size() >= limits_.max_materials ||
            model_.materials.size() >=
                std::numeric_limits<std::uint32_t>::max())
            fail(FbxRenderAdapterStatus::resource_limit, "material_limit",
                 "FBX fallback material exceeds the adapter limit",
                 "materials/fallback");
        constexpr std::string_view name = "FBX_DEFAULT";
        budget_.add(sizeof(Kn5Material) + name.size() +
                        std::string_view{"ksPerPixel"}.size(),
                    "materials/fallback");
        Kn5Material material;
        material.name = std::string(name);
        material.shader = "ksPerPixel";
        add_property(material, "ksSpecularEXP", 1.0F,
                     "materials/fallback");
        fallback_material_ =
            static_cast<std::uint32_t>(model_.materials.size());
        model_.materials.push_back(std::move(material));
        material_has_texture_.push_back(false);
        return *fallback_material_;
    }

    void add_diagnostic(std::string code, std::string message,
                        std::string path) {
        if (diagnostics_.size() >= limits_.max_diagnostics)
            fail(FbxRenderAdapterStatus::resource_limit, "diagnostic_limit",
                 "FBX render adapter diagnostic count exceeds its limit",
                 "diagnostics");
        const auto bytes = checked_add(
            checked_add(code.size(), message.size(), "diagnostics"),
            path.size(), "diagnostics");
        if (bytes > limits_.max_diagnostic_bytes - diagnostic_bytes_)
            fail(FbxRenderAdapterStatus::resource_limit, "diagnostic_limit",
                 "FBX render adapter diagnostic bytes exceed their limit",
                 "diagnostics");
        diagnostic_bytes_ += bytes;
        diagnostics_.push_back(
            {std::move(code), std::move(message), std::move(path)});
    }

    std::uint32_t skinned_material(std::uint32_t base,
                                   std::string_view path) {
        const auto found = skinned_materials_.find(base);
        if (found != skinned_materials_.end()) return found->second;
        if (base >= model_.materials.size())
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_material_reference",
                 "FBX skinned material base is missing", std::string(path));
        if (model_.materials.size() >= limits_.max_materials ||
            model_.materials.size() >=
                static_cast<std::size_t>(apex::scene::invalid_material_id))
            fail(FbxRenderAdapterStatus::resource_limit, "material_limit",
                 "FBX skinned material exceeds the adapter limit",
                 std::string(path));
        const auto& source = model_.materials[base];
        std::size_t bytes = sizeof(Kn5Material) + source.name.size() +
                            std::string_view{"ksSkinnedMesh"}.size();
        for (const auto& property : source.properties)
            bytes = checked_add(bytes,
                                sizeof(Kn5MaterialProperty) +
                                    property.name.size(),
                                path);
        for (const auto& resource : source.resources)
            bytes = checked_add(bytes,
                                sizeof(apex::formats::Kn5MaterialResource) +
                                    resource.slot.size() +
                                    resource.texture.size(),
                                path);
        budget_.add(bytes, path);
        // The recovered batch finalizer selects ksSkinnedMesh. Keep a static
        // use of the same source material unchanged in the canonical model.
        Kn5Material clone = source;
        clone.shader = "ksSkinnedMesh";
        const auto result = static_cast<std::uint32_t>(model_.materials.size());
        model_.materials.push_back(std::move(clone));
        material_has_texture_.push_back(material_has_texture_[base]);
        budget_.add(sizeof(std::pair<const std::uint32_t, std::uint32_t>),
                    path);
        skinned_materials_.emplace(base, result);
        return result;
    }

    std::uint32_t resolve_material(const FbxNodeGeometry& mapping,
                                   std::int32_t raw_slot,
                                   std::size_t batch_count,
                                   bool skinned,
                                   std::string_view path) {
        // MeshBuilder's single-batch finalizer always asks the provider for
        // local slot zero. Multiple batches use their raw signed slot.
        const std::int64_t local_slot =
            batch_count == 1U ? 0 : static_cast<std::int64_t>(raw_slot);
        if (local_slot >= 0 &&
            static_cast<std::uint64_t>(local_slot) < mapping.materials.size()) {
            const auto material =
                mapping.materials[static_cast<std::size_t>(local_slot)];
            if (material >= model_.materials.size())
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_material_reference",
                     "FBX material slot references a missing canonical material",
                     "node_geometry");
            return skinned ? skinned_material(material, path) : material;
        }
        const auto material = fallback_material();
        return skinned ? skinned_material(material, path) : material;
    }

    void append_source_vertex(std::vector<float>& output,
                              const FbxStaticMesh& mesh,
                              const Matrix4& geometric,
                              std::uint32_t source_vertex,
                              std::string_view path) {
        const auto vertex_count = mesh.positions.size() / 3U;
        if (source_vertex >= vertex_count)
            fail(FbxRenderAdapterStatus::invalid_request, "invalid_index",
                 "FBX triangle references a missing vertex",
                 std::string(path));
        const auto position_offset =
            static_cast<std::size_t>(source_vertex) * 3U;
        const auto position = transform_position(
            geometric,
            {mesh.positions[position_offset],
             mesh.positions[position_offset + 1U],
             mesh.positions[position_offset + 2U]},
            path);
        Vector3 normal{};
        if (!mesh.normals.empty()) {
            normal = transform_normal(
                geometric,
                {mesh.normals[position_offset],
                 mesh.normals[position_offset + 1U],
                 mesh.normals[position_offset + 2U]},
                path);
        }
        float u = 0.0F;
        float v = 0.0F;
        if (!mesh.uvs.empty()) {
            const auto uv_offset =
                static_cast<std::size_t>(source_vertex) * 2U;
            u = mesh.uvs[uv_offset];
            v = mesh.uvs[uv_offset + 1U];
        }
        if (!std::isfinite(u) || !std::isfinite(v))
            fail(FbxRenderAdapterStatus::invalid_request, "non_finite_uv",
                 "FBX UV is not finite", std::string(path));
        output.insert(output.end(), {position.x, position.y, position.z,
                                     normal.x, normal.y, normal.z, u, v,
                                     0.0F, 0.0F, 0.0F});
        if (mesh.skin.has_value()) {
            if (source_vertex >= mesh.skin->vertex_influences.size())
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_skin_layout",
                     "FBX skin influence table is not vertex-aligned",
                     std::string(path));
            const auto& influence =
                mesh.skin->vertex_influences[source_vertex];
            output.insert(output.end(), influence.weights.begin(),
                          influence.weights.end());
            for (const auto bone : influence.bones)
                output.push_back(static_cast<float>(bone));
        }
    }

    Kn5Node make_batch(const apex::scene::SceneNode& source_node,
                       const FbxNodeGeometry& mapping,
                       const FbxStaticMesh& mesh, std::int32_t raw_slot,
                       std::size_t batch_index, std::size_t batch_count,
                       std::span<const std::size_t> triangles,
                       bool exact_slots,
                       std::string_view path) {
        const auto triangle_count = triangles.size();
        const auto vertex_count = checked_multiply(triangle_count, 3U, path);
        if (vertex_count > limits_.max_vertices_per_mesh ||
            vertex_count > 65'536U)
            fail(FbxRenderAdapterStatus::unsupported, "kn5_index_limit",
                 "FBX material batch exceeds the safe KN5 16-bit vertex limit",
                 std::string(path));
        if (vertex_count > limits_.max_vertices ||
            output_vertices_ > limits_.max_vertices - vertex_count ||
            vertex_count > limits_.max_indices ||
            output_indices_ > limits_.max_indices - vertex_count)
            fail(FbxRenderAdapterStatus::resource_limit, "geometry_limit",
                 "FBX canonical geometry exceeds the adapter count limits",
                 std::string(path));
        output_vertices_ += vertex_count;
        output_indices_ += vertex_count;
        const bool skinned = mesh.skin.has_value();
        const std::size_t stride = skinned ? 19U : 11U;
        budget_.add(checked_multiply(
                        checked_multiply(vertex_count, stride, path),
                        sizeof(float), path),
                    path);
        budget_.add(checked_multiply(vertex_count, sizeof(std::uint16_t), path),
                    path);

        Kn5Node output;
        output.type = skinned ? 3U : 2U;
        output.kind = skinned ? "skinnedMesh" : "mesh";
        output.name = source_node.name;
        if (batch_count > 1U)
            output.name += "_SUB" + std::to_string(batch_index);
        validate_name(output.name, path);
        budget_.add(output.name.size(), path);
        output.active = true;
        output.transform = apex::scene::identity_matrix;
        output.castShadows = true;
        output.visible = true;
        output.vertexStride = stride;
        output.materialId = resolve_material(mapping, raw_slot, batch_count,
                                             skinned, path);
        output.layer = 0U;
        output.lodIn = 0.0F;
        output.lodOut = 0.0F;
        output.vertices.reserve(vertex_count * stride);
        output.indices.reserve(vertex_count);
        if (skinned) {
            budget_.add(checked_multiply(mesh.skin->bones.size(),
                                         sizeof(apex::formats::Kn5Bone), path),
                        path);
            output.bones.reserve(mesh.skin->bones.size());
            for (const auto& bone : mesh.skin->bones) {
                budget_.add(bone.name.size(), path);
                output.bones.push_back({bone.name, bone.inverse_bind});
            }
        }

        std::size_t output_vertex = 0U;
        const auto source_triangles = mesh.triangle_indices.size() / 3U;
        for (const auto triangle : triangles) {
            if (triangle >= source_triangles)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_batch_triangle",
                     "FBX material batch references a missing triangle",
                     std::string(path));
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                append_source_vertex(output.vertices, mesh, mapping.geometric,
                                     mesh.triangle_indices[triangle * 3U + corner],
                                     path);
                output.indices.push_back(
                    static_cast<std::uint16_t>(output_vertex));
                ++output_vertex;
            }
        }
        if (output_vertex != vertex_count ||
            output.vertices.size() != vertex_count * stride)
            fail(FbxRenderAdapterStatus::invalid_request,
                 "batch_count_changed",
                 "FBX material batch count changed during conversion",
                 std::string(path));
        generate_native_tangents(output.vertices, stride);
        output.bounds = native_bounds(output.vertices, stride, path);
        const bool has_texture =
            output.materialId < material_has_texture_.size() &&
            material_has_texture_[output.materialId];
        output.renderable = true;
        if (!conversion_.complete || !exact_slots || !has_texture) {
            staged_ = true;
            add_diagnostic(
                !exact_slots ? "missing_material_slots"
                             : !has_texture ? "missing_diffuse_texture"
                                            : "incomplete_fbx_conversion",
                !exact_slots
                    ? "FBX geometry has no exact polygon material slots"
                    : !has_texture
                          ? "FBX material has no owned native diffuse texture"
                          : "FBX conversion is incomplete",
                std::string(path));
        }
        return output;
    }

    void append_geometry(Kn5Node& wrapper,
                         const apex::scene::SceneNode& source_node,
                         const FbxNodeGeometry& mapping,
                         std::string_view path) {
        if (mapping.mesh >= conversion_.meshes.size())
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_geometry_mapping",
                 "FBX geometry mapping references a missing mesh",
                 std::string(path));
        const auto& mesh = conversion_.meshes[mapping.mesh];
        if (mesh.positions.empty() || mesh.positions.size() % 3U != 0U ||
            mesh.triangle_indices.empty() ||
            mesh.triangle_indices.size() % 3U != 0U)
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_geometry",
                 "FBX mesh position or triangle data is malformed",
                 std::string(path));
        const auto vertex_count = mesh.positions.size() / 3U;
        const auto expected_uv_count =
            checked_multiply(vertex_count, 2U, path);
        if ((!mesh.normals.empty() && mesh.normals.size() != mesh.positions.size()) ||
            (!mesh.uvs.empty() && mesh.uvs.size() != expected_uv_count))
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_geometry_layout",
                 "FBX mesh attribute arrays are not vertex-aligned",
                 std::string(path));
        if (mesh.skin.has_value()) {
            if (mesh.skin->bones.empty() ||
                mesh.skin->bones.size() > limits_.max_bones_per_mesh ||
                mesh.skin->bones.size() > max_exact_float_bone_count ||
                mesh.skin->vertex_influences.size() != vertex_count)
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_skin_layout",
                     "FBX skin bones or influences are malformed",
                     std::string(path));
            std::set<std::string_view> bone_names;
            for (const auto& bone : mesh.skin->bones) {
                validate_name(bone.name, path);
                const auto [name, inserted] = bone_names.insert(bone.name);
                (void)name;
                if (bone.name.empty() || !finite_matrix(bone.inverse_bind) ||
                    !inserted)
                    fail(FbxRenderAdapterStatus::invalid_request,
                         "invalid_skin_bone",
                         "FBX skin bone name or inverse-bind matrix is invalid",
                         std::string(path));
                budget_.add(sizeof(std::string_view) +
                                3U * sizeof(void*),
                            path);
            }
            for (const auto& influence : mesh.skin->vertex_influences) {
                for (std::size_t slot = 0U; slot < influence.weights.size();
                     ++slot) {
                    if (!std::isfinite(influence.weights[slot]) ||
                        influence.weights[slot] < 0.0F ||
                        influence.bones[slot] >= mesh.skin->bones.size())
                        fail(FbxRenderAdapterStatus::invalid_request,
                             "invalid_skin_influence",
                             "FBX skin influence is not finite or references a missing bone",
                             std::string(path));
                }
            }
        }
        const auto triangle_count = mesh.triangle_indices.size() / 3U;
        const bool exact_slots = !mesh.triangle_material_slots.empty();
        if (exact_slots && mesh.triangle_material_slots.size() != triangle_count)
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_material_slots",
                 "FBX material slot count does not match triangle count",
                 std::string(path));
        for (const auto index : mesh.triangle_indices)
            if (index >= vertex_count)
                fail(FbxRenderAdapterStatus::invalid_request, "invalid_index",
                     "FBX triangle references a missing vertex",
                     std::string(path));

        std::vector<std::int32_t> batch_slots;
        std::vector<std::size_t> batch_counts;
        std::map<std::int32_t, std::size_t> batch_by_slot;
        for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
            const auto slot = exact_slots
                                  ? mesh.triangle_material_slots[triangle]
                                  : std::int32_t{0};
            auto found = batch_by_slot.find(slot);
            if (found == batch_by_slot.end()) {
                if (batch_slots.size() >= limits_.max_batches_per_geometry)
                    fail(FbxRenderAdapterStatus::resource_limit,
                         "batch_limit",
                         "FBX material batch count exceeds its geometry limit",
                         std::string(path));
                budget_.add(sizeof(std::int32_t) + sizeof(std::size_t) +
                                sizeof(std::pair<const std::int32_t,
                                                 std::size_t>),
                            path);
                const auto batch = batch_slots.size();
                batch_slots.push_back(slot);
                batch_counts.push_back(1U);
                batch_by_slot.emplace(slot, batch);
            } else {
                ++batch_counts[found->second];
            }
        }
        if (batch_slots.size() > limits_.max_meshes ||
            output_meshes_ > limits_.max_meshes - batch_slots.size())
            fail(FbxRenderAdapterStatus::resource_limit, "mesh_limit",
                 "FBX canonical mesh count exceeds the adapter limit",
                 std::string(path));
        output_meshes_ += batch_slots.size();
        if (batch_slots.size() > limits_.max_nodes ||
            output_nodes_ > limits_.max_nodes - batch_slots.size())
            fail(FbxRenderAdapterStatus::resource_limit, "node_limit",
                 "FBX canonical node count exceeds the adapter limit",
                 std::string(path));
        output_nodes_ += batch_slots.size();
        budget_.add(checked_multiply(batch_slots.size(), sizeof(Kn5Node), path),
                    path);
        budget_.add(checked_multiply(batch_slots.size(),
                                     sizeof(std::vector<std::size_t>), path),
                    path);
        budget_.add(checked_multiply(triangle_count, sizeof(std::size_t), path),
                    path);
        std::vector<std::vector<std::size_t>> batch_triangles;
        batch_triangles.reserve(batch_slots.size());
        for (const auto count : batch_counts) {
            batch_triangles.emplace_back();
            batch_triangles.back().reserve(count);
        }
        for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
            const auto slot = exact_slots
                                  ? mesh.triangle_material_slots[triangle]
                                  : std::int32_t{0};
            batch_triangles[batch_by_slot.at(slot)].push_back(triangle);
        }
        wrapper.children.reserve(
            checked_add(wrapper.children.size(), batch_slots.size(), path));
        for (std::size_t batch = 0U; batch < batch_slots.size(); ++batch) {
            wrapper.children.push_back(make_batch(
                source_node, mapping, mesh, batch_slots[batch], batch,
                batch_slots.size(), batch_triangles[batch], exact_slots,
                std::string(path) + "/batch/" + std::to_string(batch)));
        }
    }

    Kn5Node make_wrapper(NodeId node_id, std::size_t depth, bool root) {
        if (depth > limits_.max_depth)
            fail(FbxRenderAdapterStatus::resource_limit, "depth_limit",
                 "FBX hierarchy exceeds the adapter depth limit", "nodes");
        if (node_id >= conversion_.snapshot.nodes.size())
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_hierarchy", "FBX node ID is outside its table",
                 "nodes");
        if (output_nodes_ >= limits_.max_nodes)
            fail(FbxRenderAdapterStatus::resource_limit, "node_limit",
                 "FBX canonical node count exceeds the adapter limit",
                 "nodes");
        ++output_nodes_;
        budget_.add(sizeof(Kn5Node), "nodes");
        const auto& source_node = conversion_.snapshot.nodes[node_id];
        if (emitted_.size() != conversion_.snapshot.nodes.size())
            emitted_.assign(conversion_.snapshot.nodes.size(), false);
        if (emitted_[node_id])
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_hierarchy",
                 "FBX source node is referenced more than once", "nodes");
        emitted_[node_id] = true;
        Kn5Node wrapper;
        wrapper.type = 1U;
        wrapper.kind = "node";
        wrapper.name = source_node.name;
        wrapper.active = true;
        wrapper.visible = true;
        wrapper.transform =
            root ? apex::scene::identity_matrix : locals_.at(node_id);
        budget_.add(wrapper.name.size(), "nodes");

        const auto geometry = geometries_.find(node_id);
        const bool source_has_geometry =
            source_node.kind == apex::scene::NodeKind::mesh ||
            source_node.kind == apex::scene::NodeKind::skinned_mesh;
        if ((geometry != geometries_.end()) != source_has_geometry)
            fail(FbxRenderAdapterStatus::invalid_request,
                 "invalid_geometry_mapping",
                 "FBX mesh node and geometry mapping do not match",
                 "nodes/" + std::to_string(node_id));
        if (geometry != geometries_.end())
            if (conversion_.meshes[geometry->second->mesh].skin.has_value() !=
                (source_node.kind == apex::scene::NodeKind::skinned_mesh))
                fail(FbxRenderAdapterStatus::invalid_request,
                     "invalid_skin_mapping",
                     "FBX skinned node and mesh binding do not match",
                     "nodes/" + std::to_string(node_id));
        if (geometry != geometries_.end())
            append_geometry(wrapper, source_node, *geometry->second,
                            "nodes/" + std::to_string(node_id));
        budget_.add(checked_multiply(source_node.children.size(), sizeof(Kn5Node),
                                     "nodes"),
                    "nodes");
        wrapper.children.reserve(checked_add(wrapper.children.size(),
                                             source_node.children.size(),
                                             "nodes"));
        for (const auto child : source_node.children)
            wrapper.children.push_back(make_wrapper(child, depth + 1U, false));
        return wrapper;
    }

    const apex::formats::FbxSceneConversion& conversion_;
    const FbxExternalTextureAuthorityResult* textures_ = nullptr;
    std::string source_;
    FbxRenderAdapterLimits limits_;
    OutputBudget budget_;
    Kn5File model_;
    std::map<NodeId, Matrix4> locals_;
    std::map<NodeId, const FbxNodeGeometry*> geometries_;
    std::vector<bool> material_has_texture_;
    std::map<std::uint32_t, std::uint32_t> skinned_materials_;
    std::vector<bool> emitted_;
    std::optional<std::uint32_t> fallback_material_;
    std::vector<FbxRenderAdapterDiagnostic> diagnostics_;
    std::size_t diagnostic_bytes_ = 0U;
    std::size_t output_nodes_ = 0U;
    std::size_t output_meshes_ = 0U;
    std::size_t output_vertices_ = 0U;
    std::size_t output_indices_ = 0U;
    bool staged_ = false;
};

} // namespace

FbxRenderAdapterResult build_fbx_render_scene(
    const apex::formats::FbxSceneConversion& conversion,
    const FbxExternalTextureAuthorityResult* textures, std::string source,
    FbxRenderAdapterLimits limits) {
    try {
        return Builder(conversion, textures, std::move(source),
                       std::move(limits))
            .build();
    } catch (const AdapterFailure& error) {
        FbxRenderAdapterResult result;
        result.status = error.status_;
        result.diagnostics.push_back(error.diagnostic_);
        return result;
    } catch (const std::bad_alloc&) {
        FbxRenderAdapterResult result;
        result.status = FbxRenderAdapterStatus::resource_limit;
        result.diagnostics.push_back(
            {"allocation_failed",
             "FBX render adapter allocation failed", "adapter"});
        return result;
    } catch (const std::length_error&) {
        FbxRenderAdapterResult result;
        result.status = FbxRenderAdapterStatus::resource_limit;
        result.diagnostics.push_back(
            {"allocation_failed",
             "FBX render adapter allocation failed", "adapter"});
        return result;
    } catch (const std::exception& error) {
        FbxRenderAdapterResult result;
        result.status = FbxRenderAdapterStatus::invalid_request;
        result.diagnostics.push_back(
            {"canonical_scene_failed", error.what(), "scene"});
        return result;
    }
}

} // namespace apex::render
