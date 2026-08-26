#include "apex/render/draw_packet.hpp"
#include "apex/render/kn5_scene_node_map.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace apex::render {
namespace {

using Matrix = apex::scene::Matrix4;

bool finite_matrix(const Matrix& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

std::string canonical_resource_slot(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    std::string result(value.substr(begin, end - begin));
    for (char& character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

Matrix multiply_matrix(const Matrix& left, const Matrix& right) {
    Matrix output{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double value = 0.0;
            for (std::size_t index = 0; index < 4; ++index)
                value += static_cast<double>(left[index * 4 + row]) * right[column * 4 + index];
            if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() ||
                value > std::numeric_limits<float>::max())
                throw DrawPacketError("NON_FINITE_MATRIX", "matrix multiplication produced a non-finite value");
            output[column * 4 + row] = static_cast<float>(value);
        }
    }
    return output;
}

std::optional<Matrix> invert_matrix(const Matrix& input) {
    const double a00 = input[0], a01 = input[1], a02 = input[2], a03 = input[3];
    const double a10 = input[4], a11 = input[5], a12 = input[6], a13 = input[7];
    const double a20 = input[8], a21 = input[9], a22 = input[10], a23 = input[11];
    const double a30 = input[12], a31 = input[13], a32 = input[14], a33 = input[15];
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
    const double determinant = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return std::nullopt;
    const double d = 1.0 / determinant;
    const std::array<double, 16> values = {
        (a11 * b11 - a12 * b10 + a13 * b09) * d,
        (-a01 * b11 + a02 * b10 - a03 * b09) * d,
        (a31 * b05 - a32 * b04 + a33 * b03) * d,
        (-a21 * b05 + a22 * b04 - a23 * b03) * d,
        (-a10 * b11 + a12 * b08 - a13 * b07) * d,
        (a00 * b11 - a02 * b08 + a03 * b07) * d,
        (-a30 * b05 + a32 * b02 - a33 * b01) * d,
        (a20 * b05 - a22 * b02 + a23 * b01) * d,
        (a10 * b10 - a11 * b08 + a13 * b06) * d,
        (-a00 * b10 + a01 * b08 - a03 * b06) * d,
        (a30 * b04 - a31 * b02 + a33 * b00) * d,
        (-a20 * b04 + a21 * b02 - a23 * b00) * d,
        (-a10 * b09 + a11 * b07 - a12 * b06) * d,
        (a00 * b09 - a01 * b07 + a02 * b06) * d,
        (-a30 * b03 + a31 * b01 - a32 * b00) * d,
        (a20 * b03 - a21 * b01 + a22 * b00) * d};
    Matrix output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        if (!std::isfinite(values[index]) || values[index] < -std::numeric_limits<float>::max() ||
            values[index] > std::numeric_limits<float>::max()) return std::nullopt;
        output[index] = static_cast<float>(values[index]);
    }
    return output;
}

std::array<double, 3> transform_point(const Matrix& matrix, const std::array<double, 3>& value) {
    return {
        static_cast<double>(matrix[0]) * value[0] + static_cast<double>(matrix[4]) * value[1] +
            static_cast<double>(matrix[8]) * value[2] + matrix[12],
        static_cast<double>(matrix[1]) * value[0] + static_cast<double>(matrix[5]) * value[1] +
            static_cast<double>(matrix[9]) * value[2] + matrix[13],
        static_cast<double>(matrix[2]) * value[0] + static_cast<double>(matrix[6]) * value[1] +
            static_cast<double>(matrix[10]) * value[2] + matrix[14]};
}

std::array<double, 3> transform_direction(const Matrix& matrix, const std::array<double, 3>& value) {
    return {
        static_cast<double>(matrix[0]) * value[0] + static_cast<double>(matrix[4]) * value[1] +
            static_cast<double>(matrix[8]) * value[2],
        static_cast<double>(matrix[1]) * value[0] + static_cast<double>(matrix[5]) * value[1] +
            static_cast<double>(matrix[9]) * value[2],
        static_cast<double>(matrix[2]) * value[0] + static_cast<double>(matrix[6]) * value[1] +
            static_cast<double>(matrix[10]) * value[2]};
}

std::array<double, 3> normalized(std::array<double, 3> value) {
    const double length = std::hypot(std::hypot(value[0], value[1]), value[2]);
    if (!std::isfinite(length) || length <= 1e-12) return {0.0, 0.0, 0.0};
    for (double& component : value) component /= length;
    return value;
}

bool checked_add(std::size_t left, std::size_t right, std::size_t maximum, std::size_t& output) {
    if (right > maximum || left > maximum - right) return false;
    output = left + right;
    return true;
}

bool checked_mul(std::size_t left, std::size_t right, std::size_t maximum, std::size_t& output) {
    if (left != 0 && right > maximum / left) return false;
    output = left * right;
    return true;
}

void add_diagnostic(DrawPacketBuildResult& result, const DrawPacketLimits& limits,
                   DrawPacketDiagnostic::Severity severity, std::string code,
                   std::string message, apex::scene::NodeId node = apex::scene::invalid_node_id,
                   apex::scene::MaterialId material = apex::scene::invalid_material_id) {
    if (result.diagnostics.size() >= limits.max_diagnostics) {
        result.limit_exceeded = true;
        result.supported = false;
        return;
    }
    std::size_t bytes = 0;
    if (!checked_add(code.size(), message.size(), limits.max_diagnostic_bytes, bytes) ||
        !checked_add(result.diagnostic_bytes, bytes, limits.max_diagnostic_bytes, result.diagnostic_bytes)) {
        result.limit_exceeded = true;
        result.supported = false;
        return;
    }
    result.diagnostics.push_back({severity, std::move(code), std::move(message), node, material});
    if (severity == DrawPacketDiagnostic::Severity::error) result.supported = false;
}

void add_unsupported(DrawPacketBuildResult& result, const DrawPacketLimits& limits,
                     std::string code, std::string description,
                     apex::scene::NodeId node = apex::scene::invalid_node_id,
                     apex::scene::MaterialId material = apex::scene::invalid_material_id) {
    if (result.unsupported_effects.size() >= limits.max_unsupported_effects) {
        result.limit_exceeded = true;
        result.supported = false;
        return;
    }
    std::size_t bytes = 0;
    if (!checked_add(code.size(), description.size(), limits.max_unsupported_effect_bytes, bytes) ||
        !checked_add(result.unsupported_effect_bytes, bytes, limits.max_unsupported_effect_bytes,
                     result.unsupported_effect_bytes)) {
        result.limit_exceeded = true;
        result.supported = false;
        return;
    }
    result.unsupported_effects.push_back({std::move(code), std::move(description), node, material});
}

bool node_order_less(const RenderItem& first, const RenderItem& second) noexcept {
    if (first.transparent != second.transparent) return !first.transparent;
    if (first.layer != second.layer) return first.layer < second.layer;
    if (first.transparent && first.distance != second.distance) return first.distance > second.distance;
    return false;
}

}  // namespace

DrawPacketError::DrawPacketError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

std::vector<float> skin_vertices_reference(
    std::span<const float> vertices,
    std::span<const Matrix> palettes,
    const Matrix& mesh_world,
    const DrawPacketLimits& limits) {
    if (!finite_matrix(mesh_world)) throw DrawPacketError("NON_FINITE_MATRIX", "skinned mesh world matrix is not finite");
    std::size_t max_vertex_values = 0;
    if (!checked_mul(limits.max_vertices, 19U, std::numeric_limits<std::size_t>::max(), max_vertex_values) ||
        palettes.size() > limits.max_bones || vertices.size() > max_vertex_values)
        throw DrawPacketError("SKIN_LIMIT", "skinned mesh exceeds CPU skinning limits");
    if (vertices.size() % 19U != 0) throw DrawPacketError("INVALID_SKIN_LAYOUT", "skinned vertices are not stride aligned");
    std::size_t output_bytes = 0;
    if (!checked_mul(vertices.size(), sizeof(float), limits.max_cpu_skin_bytes, output_bytes))
        throw DrawPacketError("SKIN_BYTE_LIMIT", "CPU skinned output exceeds byte budget");
    for (const Matrix& palette : palettes)
        if (!finite_matrix(palette)) throw DrawPacketError("NON_FINITE_MATRIX", "bone palette is not finite");
    for (const float value : vertices)
        if (!std::isfinite(value)) throw DrawPacketError("NON_FINITE_VERTEX", "skinned vertex data is not finite");
    const auto inverse_mesh = invert_matrix(mesh_world);
    if (!inverse_mesh.has_value()) throw DrawPacketError("NON_INVERTIBLE_MATRIX", "skinned mesh world transform is not invertible");
    std::vector<float> output(vertices.begin(), vertices.end());
    for (std::size_t offset = 0; offset < vertices.size(); offset += 19U) {
        std::array<double, 3> source_position = {vertices[offset], vertices[offset + 1], vertices[offset + 2]};
        std::array<double, 3> source_normal = {vertices[offset + 3], vertices[offset + 4], vertices[offset + 5]};
        std::array<double, 3> source_tangent = {vertices[offset + 8], vertices[offset + 9], vertices[offset + 10]};
        std::array<double, 3> world_position{}, world_normal{}, world_tangent{};
        double total = 0.0;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const float weight = vertices[offset + 11U + influence];
            const float index_value = vertices[offset + 15U + influence];
            if (!std::isfinite(weight) || !std::isfinite(index_value) || index_value < 0.0F ||
                std::trunc(index_value) != index_value ||
                static_cast<double>(index_value) >= static_cast<double>(palettes.size()))
                throw DrawPacketError("INVALID_SKIN_INFLUENCE", "skinned vertex has an invalid finite bone influence");
            if (!(weight > 0.0F)) continue;
            const Matrix& palette = palettes[static_cast<std::size_t>(index_value)];
            const auto position = transform_point(palette, source_position);
            const auto normal = transform_direction(palette, source_normal);
            const auto tangent = transform_direction(palette, source_tangent);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                world_position[axis] += position[axis] * weight;
                world_normal[axis] += normal[axis] * weight;
                world_tangent[axis] += tangent[axis] * weight;
            }
            total += weight;
        }
        if (!std::isfinite(total)) throw DrawPacketError("NON_FINITE_WEIGHT", "skinned weight accumulation is not finite");
        if (total <= 1e-8) continue;
        if (std::abs(total - 1.0) > 1e-5) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                world_position[axis] /= total;
                world_normal[axis] /= total;
                world_tangent[axis] /= total;
            }
        }
        const auto local_position = transform_point(*inverse_mesh, world_position);
        const auto local_normal = normalized(transform_direction(*inverse_mesh, world_normal));
        const auto local_tangent = normalized(transform_direction(*inverse_mesh, world_tangent));
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(local_position[axis]) || !std::isfinite(local_normal[axis]) || !std::isfinite(local_tangent[axis]))
                throw DrawPacketError("NON_FINITE_SKIN_RESULT", "CPU skinning produced a non-finite vertex");
            if (std::abs(local_position[axis]) > std::numeric_limits<float>::max() ||
                std::abs(local_normal[axis]) > std::numeric_limits<float>::max() ||
                std::abs(local_tangent[axis]) > std::numeric_limits<float>::max())
                throw DrawPacketError("NON_FINITE_SKIN_RESULT", "CPU skinning result exceeds float range");
            output[offset + axis] = static_cast<float>(local_position[axis]);
            output[offset + 3U + axis] = static_cast<float>(local_normal[axis]);
            output[offset + 8U + axis] = static_cast<float>(local_tangent[axis]);
        }
    }
    return output;
}

std::vector<float> skin_vertices_reference(
    const apex::formats::Kn5Node& mesh,
    const std::map<std::string, Matrix>& bone_world_by_name,
    const Matrix& mesh_world,
    const DrawPacketLimits& limits) {
    if (mesh.kind != "skinnedMesh" || mesh.vertexStride != 19)
        throw DrawPacketError("INVALID_SKIN_LAYOUT", "expected a 19-float skinned KN5 mesh");
    if (mesh.bones.size() > limits.max_bones)
        throw DrawPacketError("SKIN_LIMIT", "skinned mesh exceeds CPU skinning limits");
    std::vector<Matrix> palettes;
    palettes.reserve(mesh.bones.size());
    for (const auto& bone : mesh.bones) {
        if (bone.name.size() > limits.max_string_bytes)
            throw DrawPacketError("STRING_LIMIT", "skinned mesh bone name exceeds draw-packet limits");
        if (!finite_matrix(bone.transform)) throw DrawPacketError("NON_FINITE_MATRIX", "bone transform is not finite");
        const auto found = bone_world_by_name.find(bone.name);
        if (found == bone_world_by_name.end()) throw DrawPacketError("MISSING_BONE", "skinned mesh bone has no world transform");
        if (!finite_matrix(found->second)) throw DrawPacketError("NON_FINITE_MATRIX", "bone world transform is not finite");
        palettes.push_back(multiply_matrix(found->second, bone.transform));
    }
    return skin_vertices_reference(std::span<const float>(mesh.vertices),
                                   std::span<const Matrix>(palettes), mesh_world, limits);
}

DrawPacketBuildResult build_draw_packets(
    const apex::formats::Kn5File& model, const apex::scene::SceneSnapshot& scene,
    const RenderPlan& plan, const DrawPacketOptions& options, const DrawPacketLimits& limits) {
    DrawPacketBuildResult result;
    add_unsupported(result, limits, "shader_execution_staged", "Stock shader execution and pixel effects are not implemented in this backend-neutral packet stage.");
    add_unsupported(result, limits, "texture_sampling_staged", "Texture loading and sampling are represented by resource slots only.");
    if (scene.nodes.size() > limits.max_scene_nodes || scene.materials.size() > limits.max_scene_materials ||
        model.materials.size() > limits.max_scene_materials || model.textures.size() > limits.max_scene_textures ||
        model.textures.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        plan.items.size() > limits.max_packets ||
        (options.include_shadow_casters &&
         (plan.shadow_only_items.size() > limits.max_packets ||
          plan.shadow_only_items.size() >
              limits.max_packets - plan.items.size()))) {
        add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INPUT_LIMIT",
                       "scene, material, or render-plan input exceeds draw-packet limits");
        result.limit_exceeded = true;
        return result;
    }

    const Kn5SceneNodeMapLimits map_limits{limits.max_scene_nodes, limits.max_scene_nodes,
                                            limits.max_scene_nodes, limits.max_string_bytes};
    const Kn5SceneNodeMapResult node_map = map_kn5_scene_nodes(model.root, scene, map_limits);
    if (!node_map.ok()) {
        add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error,
                       node_map.diagnostic.code, node_map.diagnostic.message,
                       node_map.diagnostic.node);
        result.limit_exceeded = node_map.diagnostic.limit_exceeded;
        return result;
    }
    const std::vector<const apex::formats::Kn5Node*>& raw_nodes = node_map.source_nodes;

    using TextureScopeKey = std::pair<std::optional<std::size_t>, std::string>;
    std::map<TextureScopeKey, std::optional<std::uint32_t>> texture_indices;
    for (std::size_t index = 0; index < model.textures.size(); ++index) {
        const auto& texture = model.textures[index];
        if (texture.name.size() > limits.max_string_bytes) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "STRING_LIMIT",
                           "KN5 texture name exceeds draw-packet limits");
            result.limit_exceeded = true;
            return result;
        }
        const std::string key = canonical_resource_slot(texture.name);
        if (key.empty()) continue;
        const auto [found, inserted] = texture_indices.emplace(
            TextureScopeKey{texture.workspaceFileIndex, key}, static_cast<std::uint32_t>(index));
        if (!inserted) found->second.reset();
    }

    std::map<std::string, Matrix> bone_world_by_name;
    for (const auto& node : scene.nodes) {
        if (!finite_matrix(node.transform)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "NON_FINITE_MATRIX", "scene node transform is not finite", node.id, node.material);
            return result;
        }
        if (node.name.size() > limits.max_string_bytes) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "STRING_LIMIT", "scene node name exceeds draw-packet limits", node.id, node.material);
            return result;
        }
        if (bone_world_by_name.contains(node.name)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::warning, "DUPLICATE_BONE_NAME",
                           "duplicate scene node name uses the first world transform for JS-compatible bone lookup",
                           node.id, node.material);
        } else {
            bone_world_by_name.emplace(node.name, node.transform);
        }
    }

    std::vector<RenderItem> items = plan.items;
    for (const auto& item : items) {
        if (item.node == apex::scene::invalid_node_id || item.material == apex::scene::invalid_material_id ||
            !std::isfinite(item.distance)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INVALID_RENDER_ITEM",
                           "render plan contains an invalid node, material, or distance", item.node, item.material);
            return result;
        }
    }
    std::stable_sort(items.begin(), items.end(), node_order_less);
    std::vector<std::size_t> prepared_source_orders;
    prepared_source_orders.reserve(items.size() +
                                   (options.include_shadow_casters
                                        ? plan.shadow_only_items.size()
                                        : 0U));
    const std::size_t color_item_count = items.size();
    if (options.include_shadow_casters) {
        std::vector<bool> retained_nodes(scene.nodes.size(), false);
        for (const auto& item : items) {
            if (item.node != apex::scene::invalid_node_id &&
                static_cast<std::size_t>(item.node) < retained_nodes.size()) {
                retained_nodes[static_cast<std::size_t>(item.node)] = true;
            }
        }
        for (const auto& item : plan.shadow_only_items) {
            if (item.node == apex::scene::invalid_node_id ||
                static_cast<std::size_t>(item.node) >= retained_nodes.size() ||
                retained_nodes[static_cast<std::size_t>(item.node)] ||
                item.material == apex::scene::invalid_material_id ||
                !std::isfinite(item.distance) || !item.casts_shadows) {
                add_diagnostic(
                    result, limits, DrawPacketDiagnostic::Severity::error,
                    "INVALID_SHADOW_ONLY_RENDER_ITEM",
                    "The shadow-only plan contains an invalid or duplicate mesh",
                    item.node, item.material);
                return result;
            }
            retained_nodes[static_cast<std::size_t>(item.node)] = true;
            items.push_back(item);
        }
    }
    for (std::size_t order = 0; order < items.size(); ++order) {
        const auto& item = items[order];
        const bool shadow_only = order >= color_item_count;
        const auto* scene_node = scene.find_node(item.node);
        if (scene_node == nullptr || static_cast<std::size_t>(scene_node->id) >= raw_nodes.size()) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INVALID_NODE_REFERENCE", "render item references no scene node", item.node, item.material);
            continue;
        }
        const auto* raw = raw_nodes[static_cast<std::size_t>(scene_node->id)];
        if ((raw->type != 2U && raw->type != 3U) ||
            (raw->type == 2U && raw->kind != "mesh") ||
            (raw->type == 3U && raw->kind != "skinnedMesh")) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "NOT_MESH", "render item does not reference a static or skinned mesh", item.node, item.material);
            continue;
        }
        if (raw->materialId >= model.materials.size() || item.material != raw->materialId ||
            scene_node->material != raw->materialId || item.material >= scene.materials.size()) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "MATERIAL_REFERENCE", "mesh material reference is outside the validated material tables", item.node, item.material);
            continue;
        }
        const auto& source_material = model.materials[raw->materialId];
        const auto& scene_material = scene.materials[item.material];
        if (scene_material.name != source_material.name || scene_material.shader != source_material.shader ||
            static_cast<std::uint32_t>(scene_material.blend_mode) != source_material.blendMode) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "SCENE_MATERIAL_IDENTITY",
                           "scene material does not match the source KN5 material", item.node, item.material);
            continue;
        }
        std::set<std::string> resource_slots;
        std::size_t bytes = 0;
        std::size_t part = 0;
        std::size_t new_total_bytes = 0;
        MaterialRenderProfile profile;
        if (source_material.name.size() > limits.max_string_bytes ||
            source_material.shader.size() > limits.max_string_bytes ||
            source_material.properties.size() > limits.max_material_properties) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "MATERIAL_LIMIT",
                           "material strings or properties exceed draw-packet limits", item.node, item.material);
            result.limit_exceeded = true;
            continue;
        }
        bool invalid_property = false;
        for (const auto& property : source_material.properties) {
            if (property.name.size() > limits.max_string_bytes || !std::isfinite(property.value) ||
                std::any_of(property.value2.begin(), property.value2.end(), [](float value) { return !std::isfinite(value); }) ||
                std::any_of(property.value3.begin(), property.value3.end(), [](float value) { return !std::isfinite(value); }) ||
                std::any_of(property.value4.begin(), property.value4.end(), [](float value) { return !std::isfinite(value); })) {
                invalid_property = true;
                break;
            }
        }
        if (invalid_property) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INVALID_MATERIAL_PROPERTY",
                           "material property is not finite or exceeds string limits", item.node, item.material);
            continue;
        }
        if (source_material.depthMode > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INVALID_MATERIAL_STATE",
                           "material depth mode cannot be represented by the profile resolver", item.node, item.material);
            continue;
        }
        if (source_material.blendMode > 2U || raw->vertexStride != (raw->type == 2U ? 11U : 19U) ||
            raw->vertices.size() % raw->vertexStride != 0 || raw->indices.size() % 3U != 0) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INVALID_GEOMETRY", "mesh geometry layout is invalid", item.node, item.material);
            continue;
        }
        const std::size_t vertex_count = raw->vertices.size() / raw->vertexStride;
        if (vertex_count > limits.max_vertices || raw->indices.size() > limits.max_indices ||
            vertex_count > std::numeric_limits<std::uint32_t>::max() || raw->indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "GEOMETRY_LIMIT", "mesh geometry exceeds draw-packet limits", item.node, item.material);
            result.limit_exceeded = true;
            continue;
        }
        for (const auto value : raw->vertices) {
            if (!std::isfinite(value)) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "NON_FINITE_VERTEX", "mesh vertex data is not finite", item.node, item.material);
                goto next_item;
            }
        }
        for (const auto index : raw->indices) {
            if (static_cast<std::size_t>(index) >= vertex_count) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "INDEX_REFERENCE", "mesh index references a missing vertex", item.node, item.material);
                goto next_item;
            }
        }
        if (source_material.resources.size() > limits.max_resources_per_packet) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "RESOURCE_LIMIT", "material resource slots exceed draw-packet limits", item.node, item.material);
            result.limit_exceeded = true;
            goto next_item;
        }
        for (const auto& resource : source_material.resources) {
            if (resource.slot.size() > limits.max_string_bytes || resource.texture.size() > limits.max_string_bytes) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "STRING_LIMIT", "material resource string exceeds draw-packet limits", item.node, item.material);
                goto next_item;
            }
            if (!resource.texture.empty()) {
                const std::string key = canonical_resource_slot(resource.texture);
                const auto found = texture_indices.find({source_material.workspaceFileIndex, key});
                if (key.empty() || found == texture_indices.end() || !found->second.has_value()) {
                    add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "RESOURCE_REFERENCE",
                                   "material resource texture name does not resolve to exactly one KN5 texture",
                                   item.node, item.material);
                    goto next_item;
                }
            }
            if (canonical_resource_slot(resource.slot).empty() ||
                !resource_slots.insert(canonical_resource_slot(resource.slot)).second) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "RESOURCE_REFERENCE",
                               "material resource slot is empty or duplicated", item.node, item.material);
                goto next_item;
            }
            if (resource.texture.empty())
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::warning, "MISSING_TEXTURE",
                               "material resource slot has no texture name", item.node, item.material);
        }
        if (raw->type == 3U && raw->bones.size() > limits.max_bones) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "BONE_LIMIT",
                           "skinned bone palette exceeds draw-packet limits", item.node, item.material);
            result.limit_exceeded = true;
            goto next_item;
        }
        if (raw->type == 3U) {
            bool invalid_bone = false;
            for (const auto& bone : raw->bones) {
                const auto found = bone_world_by_name.find(bone.name);
                if (bone.name.size() > limits.max_string_bytes || found == bone_world_by_name.end() ||
                    !finite_matrix(bone.transform) || !finite_matrix(found->second)) {
                    invalid_bone = true;
                    break;
                }
                try {
                    (void)multiply_matrix(found->second, bone.transform);
                } catch (const DrawPacketError&) {
                    invalid_bone = true;
                    break;
                }
            }
            if (invalid_bone) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "BONE_REFERENCE",
                               "skinned bone has no finite world transform", item.node, item.material);
                goto next_item;
            }
        }
        {
            MaterialInput input{source_material.shader, static_cast<int>(source_material.blendMode), static_cast<int>(source_material.depthMode)};
            NodeMaterialInput node_input{item.transparent};
            profile = resolve_material_render_profile(input, node_input);
            if (profile.stock == nullptr) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::warning, "UNKNOWN_SHADER",
                               "material shader is outside the stock shader catalog", item.node, item.material);
                result.supported = false;
                add_unsupported(result, limits, "unknown_shader",
                                "Stock shader execution is unavailable for shader " + source_material.shader,
                                item.node, item.material);
            } else {
                const std::string_view expected_layout = raw->type == 3U ? "skinned" : "mesh";
                if (profile.stock->vertex_layout != expected_layout) {
                    add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "UNSUPPORTED_LAYOUT",
                                   "stock shader vertex layout is incompatible with the mesh node", item.node, item.material);
                    add_unsupported(result, limits, "unsupported_vertex_layout",
                                    "Stock shader requires vertex layout " + std::string(profile.stock->vertex_layout),
                                    item.node, item.material);
                    goto next_item;
                }
            }
        }
        bytes = 0;
        part = 0;
        if (!checked_mul(raw->vertices.size(), sizeof(float), limits.max_packet_bytes, bytes) ||
            !checked_mul(raw->indices.size(), sizeof(std::uint16_t), limits.max_packet_bytes - std::min(bytes, limits.max_packet_bytes), part) ||
            !checked_add(bytes, part, limits.max_packet_bytes, bytes) ||
            !checked_mul(raw->type == 3U ? raw->bones.size() : 0U, sizeof(Matrix),
                         limits.max_packet_bytes - std::min(bytes, limits.max_packet_bytes), part) ||
            !checked_add(bytes, part, limits.max_packet_bytes, bytes)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "PACKET_BYTE_LIMIT",
                           "draw packet exceeds per-packet byte budget", item.node, item.material);
            result.limit_exceeded = true;
            goto next_item;
        }
        if (!checked_add(bytes, sizeof(DrawPacket), limits.max_packet_bytes, bytes) ||
            !checked_mul(source_material.resources.size(), sizeof(DrawResourceSlot),
                         limits.max_packet_bytes - std::min(bytes, limits.max_packet_bytes), part) ||
            !checked_add(bytes, part, limits.max_packet_bytes, bytes) ||
            !checked_add(bytes, profile.shader.size(), limits.max_packet_bytes, bytes) ||
            !checked_add(bytes, profile.blend_source.size(), limits.max_packet_bytes, bytes) ||
            !checked_add(bytes, profile.blend.size(), limits.max_packet_bytes, bytes) ||
            !checked_add(bytes, profile.blend_mode.size(), limits.max_packet_bytes, bytes) ||
            !checked_add(bytes, profile.depth_mode.size(), limits.max_packet_bytes, bytes) ||
            !checked_add(bytes, profile.cull_source.size(), limits.max_packet_bytes, bytes)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "PACKET_BYTE_LIMIT",
                           "draw packet metadata exceeds per-packet byte budget", item.node, item.material);
            result.limit_exceeded = true;
            goto next_item;
        }
        for (const auto& resource : source_material.resources) {
            std::size_t resolved_texture_bytes = 0;
            if (!resource.texture.empty()) {
                const auto found = texture_indices.find(
                    {source_material.workspaceFileIndex, canonical_resource_slot(resource.texture)});
                // Every non-empty resource was resolved during validation.
                resolved_texture_bytes = model.textures[*found->second].name.size();
            }
            if (!checked_add(bytes, resource.slot.size(), limits.max_packet_bytes, bytes) ||
                !checked_add(bytes, resolved_texture_bytes, limits.max_packet_bytes, bytes)) {
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "PACKET_BYTE_LIMIT",
                               "draw packet resource strings exceed byte budget", item.node, item.material);
                result.limit_exceeded = true;
                goto next_item;
            }
        }
        if (!checked_add(result.total_bytes, bytes, limits.max_total_bytes, new_total_bytes)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "TOTAL_BYTE_LIMIT",
                           "draw packet output exceeds total byte budget", item.node, item.material);
            result.limit_exceeded = true;
            goto next_item;
        }
        if (result.packets.size() >= limits.max_packets) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "PACKET_LIMIT",
                           "draw packet count exceeds its limit", item.node, item.material);
            result.limit_exceeded = true;
            goto next_item;
        }
        {
            DrawPacket packet;
            packet.node = item.node;
            packet.material = item.material;
            packet.primitive = raw->type == 3U ? DrawPrimitiveKind::skinned_mesh : DrawPrimitiveKind::static_mesh;
            packet.vertex_count = static_cast<std::uint32_t>(vertex_count);
            packet.index_count = static_cast<std::uint32_t>(raw->indices.size());
            packet.vertex_stride_floats = static_cast<std::uint32_t>(raw->vertexStride);
            packet.order = result.packets.size();
            packet.distance = item.distance;
            packet.layer = item.layer;
            packet.transparency_overridden = item.transparency_overridden;
            packet.world_matrix = scene_node->transform;
            packet.material_profile = profile;
            const bool packet_transparent = item.transparent || profile.transparent;
            if (packet_transparent && profile.depth_write)
                add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::warning,
                               "TRANSPARENT_DEPTH_WRITE_DISABLED",
                               "transparent packet cannot write depth; depth write was disabled",
                               item.node, item.material);
            packet.flags = {packet_transparent, profile.blend_enabled, profile.alpha_to_coverage,
                            profile.depth_test, profile.depth_write && !packet_transparent, options.wireframe,
                            item.node == options.selected_node,
                            item.casts_shadows && options.include_shadow_casters};
            packet.shadow_only = shadow_only;
            packet.resources.reserve(source_material.resources.size());
            for (const auto& resource : source_material.resources) {
                std::uint32_t texture_index = invalid_draw_texture_index;
                std::string texture_name;
                if (!resource.texture.empty()) {
                    const auto found = texture_indices.find(
                        {source_material.workspaceFileIndex, canonical_resource_slot(resource.texture)});
                    // Every non-empty resource was resolved during validation.
                    texture_index = *found->second;
                    texture_name = model.textures[texture_index].name;
                }
                packet.resources.push_back({resource.slot, resource.textureId, texture_index,
                                            std::move(texture_name)});
            }
            if (packet.primitive == DrawPrimitiveKind::skinned_mesh) {
                packet.bone_palette.reserve(raw->bones.size());
                for (const auto& bone : raw->bones) {
                    const auto found = bone_world_by_name.find(bone.name);
                    if (found == bone_world_by_name.end() || !finite_matrix(bone.transform)) {
                        add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "BONE_REFERENCE", "skinned bone has no finite world transform", item.node, item.material);
                        goto next_item;
                    }
                    try {
                        packet.bone_palette.push_back(multiply_matrix(found->second, bone.transform));
                    } catch (const DrawPacketError&) {
                        add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error, "NON_FINITE_MATRIX", "skinned bone palette is not finite", item.node, item.material);
                        goto next_item;
                    }
                }
                add_unsupported(result, limits, "skinning_execution_staged",
                                "Bone palettes are validated and supplied, but shader skinning execution is staged.",
                                item.node, item.material);
            }
            result.total_bytes = new_total_bytes;
            result.packets.push_back(std::move(packet));
            prepared_source_orders.push_back(item.source_order);
        }
    next_item:
        continue;
    }
    if (!result.supported) return result;
    if (prepared_source_orders.size() != result.packets.size()) {
        add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error,
                       "INVALID_SOURCE_ORDER",
                       "Prepared packet source-order metadata is incomplete");
        return result;
    }
    const bool has_source_order = std::any_of(
        prepared_source_orders.begin(), prepared_source_orders.end(),
        [](std::size_t order) {
            return order != invalid_render_item_source_order;
        });
    if (has_source_order) {
        if (std::any_of(prepared_source_orders.begin(),
                        prepared_source_orders.end(), [](std::size_t order) {
                            return order == invalid_render_item_source_order;
                        })) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error,
                           "INVALID_SOURCE_ORDER",
                           "Prepared packet source-order metadata is partial");
            return result;
        }
        std::size_t order_bytes = 0U;
        std::size_t charged_total = 0U;
        if (result.packets.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max()) ||
            !checked_mul(result.packets.size(), sizeof(std::uint32_t),
                         limits.max_total_bytes, order_bytes) ||
            !checked_add(result.total_bytes, order_bytes,
                         limits.max_total_bytes, charged_total)) {
            add_diagnostic(result, limits, DrawPacketDiagnostic::Severity::error,
                           "TOTAL_BYTE_LIMIT",
                           "Shadow packet order exceeds the draw-packet byte budget");
            result.limit_exceeded = true;
            return result;
        }
        result.shadow_packet_order.resize(result.packets.size());
        std::iota(result.shadow_packet_order.begin(),
                  result.shadow_packet_order.end(), 0U);
        std::stable_sort(
            result.shadow_packet_order.begin(), result.shadow_packet_order.end(),
            [&](std::uint32_t first, std::uint32_t second) {
                return prepared_source_orders[first] <
                       prepared_source_orders[second];
            });
        for (std::size_t index = 1U;
             index < result.shadow_packet_order.size(); ++index) {
            const auto previous = result.shadow_packet_order[index - 1U];
            const auto current = result.shadow_packet_order[index];
            if (prepared_source_orders[previous] ==
                prepared_source_orders[current]) {
                result.shadow_packet_order.clear();
                add_diagnostic(
                    result, limits, DrawPacketDiagnostic::Severity::error,
                    "INVALID_SOURCE_ORDER",
                    "Prepared packets contain duplicate source-order metadata");
                return result;
            }
        }
        result.total_bytes = charged_total;
    }
    return result;
}

}  // namespace apex::render
