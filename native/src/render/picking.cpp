#include "apex/render/picking.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace apex::render {
namespace {

using Matrix4 = apex::scene::Matrix4;
using Vector3 = apex::scene::Vector3;

constexpr float minimum_distance = 1.0e-5F;
constexpr float minimum_determinant = 1.0e-5F;
constexpr float minimum_matrix_determinant = 1.0e-8F;
constexpr float minimum_length = 1.0e-8F;

[[nodiscard]] bool finite_vector(const Vector3& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](float component) { return std::isfinite(component); });
}

[[nodiscard]] bool finite_matrix(const Matrix4& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](float component) { return std::isfinite(component); });
}

[[nodiscard]] float dot(const Vector3& left, const Vector3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] Vector3 subtract(const Vector3& left, const Vector3& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] Vector3 add(const Vector3& left, const Vector3& right) noexcept {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

[[nodiscard]] Vector3 scale(const Vector3& value, float factor) noexcept {
    return {value[0] * factor, value[1] * factor, value[2] * factor};
}

[[nodiscard]] Vector3 cross(const Vector3& left, const Vector3& right) noexcept {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] float length(const Vector3& value) noexcept {
    return std::sqrt(dot(value, value));
}

[[nodiscard]] Vector3 normalize(const Vector3& value, float value_length) noexcept {
    return scale(value, 1.0F / value_length);
}

[[nodiscard]] PickDiagnostic diagnostic(const char* code,
                                        const char* message) {
    return {code, message};
}

[[nodiscard]] bool finite_ray(const PickRay& ray) noexcept {
    const float direction_length = length(ray.direction);
    return finite_vector(ray.origin) && finite_vector(ray.direction) &&
           std::isfinite(direction_length) && direction_length > minimum_length &&
           (std::isfinite(ray.max_distance) || std::isinf(ray.max_distance)) &&
           ray.max_distance > 0.0F;
}

[[nodiscard]] bool affine_matrix(const Matrix4& value) noexcept {
    return value[3] == 0.0F && value[7] == 0.0F && value[11] == 0.0F &&
           value[15] == 1.0F;
}

[[nodiscard]] Vector3 transform_point(const Matrix4& matrix,
                                      const Vector3& value) noexcept {
    const float x = matrix[0] * value[0] + matrix[4] * value[1] +
                    matrix[8] * value[2] + matrix[12];
    const float y = matrix[1] * value[0] + matrix[5] * value[1] +
                    matrix[9] * value[2] + matrix[13];
    const float z = matrix[2] * value[0] + matrix[6] * value[1] +
                    matrix[10] * value[2] + matrix[14];
    const float w = matrix[3] * value[0] + matrix[7] * value[1] +
                    matrix[11] * value[2] + matrix[15];
    if (w == 0.0F || !std::isfinite(w)) return {NAN, NAN, NAN};
    return {x / w, y / w, z / w};
}

[[nodiscard]] Vector3 transform_vector(const Matrix4& matrix,
                                       const Vector3& value) noexcept {
    return {matrix[0] * value[0] + matrix[4] * value[1] + matrix[8] * value[2],
            matrix[1] * value[0] + matrix[5] * value[1] + matrix[9] * value[2],
            matrix[2] * value[0] + matrix[6] * value[1] + matrix[10] * value[2]};
}

[[nodiscard]] bool multiply_matrix(const Matrix4& left, const Matrix4& right,
                                   Matrix4& output) noexcept {
    output.fill(0.0F);
    for (std::size_t column = 0U; column < 4U; ++column) {
        for (std::size_t row = 0U; row < 4U; ++row) {
            float value = 0.0F;
            for (std::size_t component = 0U; component < 4U; ++component) {
                value += left[component * 4U + row] *
                         right[column * 4U + component];
            }
            if (!std::isfinite(value)) return false;
            output[column * 4U + row] = value;
        }
    }
    return true;
}

// Column-major matrix inversion, matching SceneNode::transform. This is
// local to picking so the render contract does not acquire a matrix utility.
[[nodiscard]] bool invert_matrix(const Matrix4& input, Matrix4& output) noexcept {
    if (!finite_matrix(input)) return false;
    float inverse[16]{};
    inverse[0] = input[5] * input[10] * input[15] - input[5] * input[11] * input[14] -
                 input[9] * input[6] * input[15] + input[9] * input[7] * input[14] +
                 input[13] * input[6] * input[11] - input[13] * input[7] * input[10];
    inverse[4] = -input[4] * input[10] * input[15] + input[4] * input[11] * input[14] +
                 input[8] * input[6] * input[15] - input[8] * input[7] * input[14] -
                 input[12] * input[6] * input[11] + input[12] * input[7] * input[10];
    inverse[8] = input[4] * input[9] * input[15] - input[4] * input[11] * input[13] -
                 input[8] * input[5] * input[15] + input[8] * input[7] * input[13] +
                 input[12] * input[5] * input[11] - input[12] * input[7] * input[9];
    inverse[12] = -input[4] * input[9] * input[14] + input[4] * input[10] * input[13] +
                  input[8] * input[5] * input[14] - input[8] * input[6] * input[13] -
                  input[12] * input[5] * input[10] + input[12] * input[6] * input[9];
    inverse[1] = -input[1] * input[10] * input[15] + input[1] * input[11] * input[14] +
                 input[9] * input[2] * input[15] - input[9] * input[3] * input[14] -
                 input[13] * input[2] * input[11] + input[13] * input[3] * input[10];
    inverse[5] = input[0] * input[10] * input[15] - input[0] * input[11] * input[14] -
                 input[8] * input[2] * input[15] + input[8] * input[3] * input[14] +
                 input[12] * input[2] * input[11] - input[12] * input[3] * input[10];
    inverse[9] = -input[0] * input[9] * input[15] + input[0] * input[11] * input[13] +
                 input[8] * input[1] * input[15] - input[8] * input[3] * input[13] -
                 input[12] * input[1] * input[11] + input[12] * input[3] * input[9];
    inverse[13] = input[0] * input[9] * input[14] - input[0] * input[10] * input[13] -
                  input[8] * input[1] * input[14] + input[8] * input[2] * input[13] +
                  input[12] * input[1] * input[10] - input[12] * input[2] * input[9];
    inverse[2] = input[1] * input[6] * input[15] - input[1] * input[7] * input[14] -
                 input[5] * input[2] * input[15] + input[5] * input[3] * input[14] +
                 input[13] * input[2] * input[7] - input[13] * input[3] * input[6];
    inverse[6] = -input[0] * input[6] * input[15] + input[0] * input[7] * input[14] +
                 input[4] * input[2] * input[15] - input[4] * input[3] * input[14] -
                 input[12] * input[2] * input[7] + input[12] * input[3] * input[6];
    inverse[10] = input[0] * input[5] * input[15] - input[0] * input[7] * input[13] -
                  input[4] * input[1] * input[15] + input[4] * input[3] * input[13] +
                  input[12] * input[1] * input[7] - input[12] * input[3] * input[5];
    inverse[14] = -input[0] * input[5] * input[14] + input[0] * input[6] * input[13] +
                  input[4] * input[1] * input[14] - input[4] * input[2] * input[13] -
                  input[12] * input[1] * input[6] + input[12] * input[2] * input[5];
    inverse[3] = -input[1] * input[6] * input[11] + input[1] * input[7] * input[10] +
                 input[5] * input[2] * input[11] - input[5] * input[3] * input[10] -
                 input[9] * input[2] * input[7] + input[9] * input[3] * input[6];
    inverse[7] = input[0] * input[6] * input[11] - input[0] * input[7] * input[10] -
                 input[4] * input[2] * input[11] + input[4] * input[3] * input[10] +
                 input[8] * input[2] * input[7] - input[8] * input[3] * input[6];
    inverse[11] = -input[0] * input[5] * input[11] + input[0] * input[7] * input[9] +
                  input[4] * input[1] * input[11] - input[4] * input[3] * input[9] -
                  input[8] * input[1] * input[7] + input[8] * input[3] * input[5];
    inverse[15] = input[0] * input[5] * input[10] - input[0] * input[6] * input[9] -
                  input[4] * input[1] * input[10] + input[4] * input[2] * input[9] +
                  input[8] * input[1] * input[6] - input[8] * input[2] * input[5];
    const float determinant = input[0] * inverse[0] + input[1] * inverse[4] +
                              input[2] * inverse[8] + input[3] * inverse[12];
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= minimum_matrix_determinant)
        return false;
    const float scale_factor = 1.0F / determinant;
    for (std::size_t index = 0U; index < 16U; ++index) {
        output[index] = inverse[index] * scale_factor;
        if (!std::isfinite(output[index])) return false;
    }
    return true;
}

[[nodiscard]] TrianglePickResult triangle_failure(PickStatus status,
                                                   const char* code,
                                                   const char* message) {
    return {status, diagnostic(code, message), std::nullopt};
}

[[nodiscard]] TrianglePickResult intersect_local(const PickRay& ray,
                                                 const Vector3& first,
                                                 const Vector3& second,
                                                 const Vector3& third) noexcept {
    if (!finite_ray(ray) || !finite_vector(first) || !finite_vector(second) ||
        !finite_vector(third))
        return triangle_failure(PickStatus::invalid_request, "pick_input_non_finite",
                                "Ray and triangle inputs must be finite");
    const Vector3 edge_one = subtract(second, first);
    const Vector3 edge_two = subtract(third, first);
    const Vector3 p_vector = cross(ray.direction, edge_two);
    const float determinant = dot(edge_one, p_vector);
    if (!std::isfinite(determinant) || std::abs(determinant) < minimum_determinant)
        return {PickStatus::miss, {}, std::nullopt};
    const float inverse_determinant = 1.0F / determinant;
    const Vector3 offset = subtract(ray.origin, first);
    const float first_barycentric = dot(offset, p_vector) * inverse_determinant;
    if (first_barycentric < 0.0F || first_barycentric > 1.0F)
        return {PickStatus::miss, {}, std::nullopt};
    const Vector3 q_vector = cross(offset, edge_one);
    const float second_barycentric = dot(ray.direction, q_vector) * inverse_determinant;
    if (second_barycentric < 0.0F ||
        first_barycentric + second_barycentric > 1.0F)
        return {PickStatus::miss, {}, std::nullopt};
    const float distance = dot(edge_two, q_vector) * inverse_determinant;
    if (!std::isfinite(distance) || distance <= minimum_distance ||
        distance > ray.max_distance)
        return {PickStatus::miss, {}, std::nullopt};
    const float third_barycentric = 1.0F - first_barycentric - second_barycentric;
    const Vector3 position = add(ray.origin, scale(ray.direction, distance));
    if (!finite_vector(position)) return {PickStatus::miss, {}, std::nullopt};
    PickHit hit;
    hit.distance = distance;
    hit.position = position;
    hit.mesh_position = position;
    hit.barycentric = {third_barycentric, first_barycentric, second_barycentric};
    return {PickStatus::hit, {}, hit};
}

}  // namespace

ScreenRayResult build_screen_ray(const CameraFrame& camera, float pixel_x,
                                 float pixel_y, std::uint32_t viewport_width,
                                 std::uint32_t viewport_height) {
    if (!finite_vector(camera.position) || !finite_vector(camera.forward) ||
        !finite_vector(camera.up) || !std::isfinite(camera.fov_radians) ||
        !std::isfinite(pixel_x) || !std::isfinite(pixel_y))
        return {std::nullopt,
                diagnostic("pick_camera_non_finite", "Camera and screen inputs must be finite")};
    if (viewport_width == 0U || viewport_height == 0U)
        return {std::nullopt,
                diagnostic("pick_viewport_invalid", "Picking requires a non-zero viewport")};
    if (!(camera.fov_radians > 0.0F && camera.fov_radians < 3.14159265358979323846F))
        return {std::nullopt,
                diagnostic("pick_camera_range_invalid", "Camera FOV is invalid")};
    const float forward_length = length(camera.forward);
    const float up_length = length(camera.up);
    if (!(forward_length > minimum_length) || !(up_length > minimum_length))
        return {std::nullopt,
                diagnostic("pick_camera_basis_invalid", "Camera basis vectors must be non-zero")};
    const Vector3 forward = normalize(camera.forward, forward_length);
    const Vector3 up = normalize(camera.up, up_length);
    const Vector3 right_source = cross(forward, up);
    const float right_length = length(right_source);
    if (!(right_length > minimum_length))
        return {std::nullopt,
                diagnostic("pick_camera_basis_invalid", "Camera basis vectors must not be parallel")};
    const Vector3 right = normalize(right_source, right_length);
    const Vector3 corrected_up = cross(right, forward);
    const float tangent = std::tan(camera.fov_radians * 0.5F);
    const float aspect = static_cast<float>(viewport_width) /
                         static_cast<float>(viewport_height);
    const float normalized_x = 2.0F * pixel_x /
                                   static_cast<float>(viewport_width) -
                               1.0F;
    const float normalized_y = 1.0F - 2.0F * pixel_y /
                                         static_cast<float>(viewport_height);
    const Vector3 unnormalized = add(
        forward,
        add(scale(right, normalized_x * tangent * aspect),
            scale(corrected_up, normalized_y * tangent)));
    const float direction_length = length(unnormalized);
    if (!std::isfinite(direction_length) || !(direction_length > minimum_length))
        return {std::nullopt,
                diagnostic("pick_ray_invalid", "Screen coordinates produced an invalid ray")};
    const Vector3 direction = normalize(unnormalized, direction_length);
    return {PickRay{camera.position, direction,
                    installed_editor_pick_initial_distance}, {}};
}

TrianglePickResult intersect_pick_triangle(const PickRay& ray, const Vector3& first,
                                           const Vector3& second,
                                           const Vector3& third) {
    TrianglePickResult result = intersect_local(ray, first, second, third);
    if (result.ok()) result.hit->indices = {0U, 1U, 2U};
    return result;
}

TrianglePickResult pick_mesh(const PickRay& world_ray, const PickMeshView& mesh,
                             const Matrix4& mesh_to_world,
                             const PickMeshLimits& limits) {
    if (!finite_ray(world_ray) || mesh.vertex_stride < 3U ||
        mesh.vertex_stride > 4096U || !finite_matrix(mesh_to_world) ||
        !affine_matrix(mesh_to_world))
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_request_invalid",
                                "Mesh ray, stride, or transform is invalid");
    const bool has_16 = !mesh.indices16.empty();
    const bool has_32 = !mesh.indices32.empty();
    if (has_16 == has_32 || mesh.vertices.empty())
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_indices_invalid",
                                "A mesh requires exactly one non-empty index span");
    const std::size_t index_count = has_16 ? mesh.indices16.size() : mesh.indices32.size();
    if (limits.max_vertices == 0U || limits.max_indices == 0U ||
        index_count > limits.max_indices)
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_limit_exceeded",
                                "Mesh input exceeds the configured pick limit");
    if (index_count % 3U != 0U)
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_indices_invalid",
                                "Mesh indices must contain complete triangles");
    if (mesh.vertices.size() % mesh.vertex_stride != 0U)
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_vertices_invalid",
                                "Mesh vertex storage is not a complete stride stream");
    const std::size_t vertex_count = mesh.vertices.size() / mesh.vertex_stride;
    if (vertex_count == 0U || vertex_count > limits.max_vertices ||
        vertex_count > std::numeric_limits<std::uint32_t>::max())
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_vertices_invalid",
                                "Mesh vertex storage is not a complete bounded stream");
    for (const float value : mesh.vertices)
        if (!std::isfinite(value))
            return triangle_failure(PickStatus::invalid_request, "pick_mesh_vertex_non_finite",
                                    "Mesh vertex data must be finite");
    auto index_at = [&](std::size_t offset) -> std::uint32_t {
        return has_16 ? static_cast<std::uint32_t>(mesh.indices16[offset])
                      : mesh.indices32[offset];
    };
    for (std::size_t offset = 0U; offset < index_count; ++offset) {
        if (index_at(offset) >= vertex_count)
            return triangle_failure(PickStatus::invalid_request, "pick_mesh_index_out_of_range",
                                    "A mesh index is outside the vertex stream");
    }
    Matrix4 world_to_mesh{};
    if (!invert_matrix(mesh_to_world, world_to_mesh))
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_transform_invalid",
                                "Mesh transform is singular or non-finite");
    const Vector3 local_origin = transform_point(world_to_mesh, world_ray.origin);
    const Vector3 local_direction_raw = transform_vector(world_to_mesh, world_ray.direction);
    const float local_direction_length = length(local_direction_raw);
    if (!finite_vector(local_origin) || !finite_vector(local_direction_raw) ||
        !(local_direction_length > minimum_length))
        return triangle_failure(PickStatus::invalid_request, "pick_mesh_transform_invalid",
                                "Mesh transform produced an invalid local ray");
    const PickRay local_ray{local_origin, local_direction_raw,
                            world_ray.max_distance};
    for (std::size_t triangle = 0U; triangle < index_count / 3U; ++triangle) {
        const std::size_t offset = triangle * 3U;
        const std::array<std::uint32_t, 3U> indices = {
            index_at(offset), index_at(offset + 1U), index_at(offset + 2U)};
        const auto vertex = [&](std::uint32_t index) {
            const std::size_t base = static_cast<std::size_t>(index) * mesh.vertex_stride;
            return Vector3{mesh.vertices[base], mesh.vertices[base + 1U],
                           mesh.vertices[base + 2U]};
        };
        TrianglePickResult local_hit =
            intersect_local(local_ray, vertex(indices[0]), vertex(indices[1]), vertex(indices[2]));
        if (!local_hit.ok()) continue;
        const Vector3 world_position = transform_point(mesh_to_world, local_hit.hit->position);
        if (!finite_vector(world_position))
            continue;
        local_hit.hit->triangle_index = static_cast<std::uint32_t>(triangle);
        local_hit.hit->indices = indices;
        local_hit.hit->position = world_position;
        // The inverse-transformed direction remains unnormalized. Local t is
        // therefore the original world-ray parameter used by RayPicker.
        return local_hit;
    }
    return {PickStatus::miss, {}, std::nullopt};
}

Kn5ScenePickResult pick_kn5_scene(const PickRay& world_ray,
                                  const formats::Kn5Node& root,
                                  const Kn5ScenePickLimits& limits) {
    Kn5ScenePickResult result;
    if (!finite_ray(world_ray) || limits.max_nodes == 0U ||
        limits.max_depth == 0U || limits.max_triangles == 0U ||
        limits.mesh.max_vertices == 0U || limits.mesh.max_indices == 0U) {
        result.status = PickStatus::invalid_request;
        result.diagnostic = diagnostic(
            "pick_scene_request_invalid",
            "Scene ray and pick limits must be valid");
        return result;
    }
    struct Entry {
        const formats::Kn5Node* node = nullptr;
        Matrix4 parent_to_world = apex::scene::identity_matrix;
        std::size_t depth = 0U;
    };
    try {
        std::vector<Entry> stack;
        stack.push_back({&root, apex::scene::identity_matrix, 0U});
        std::size_t visited = 0U;
        std::size_t triangle_count = 0U;
        float best_distance = installed_editor_pick_initial_distance;
        while (!stack.empty()) {
            const Entry entry = stack.back();
            stack.pop_back();
            if (entry.node == nullptr || ++visited > limits.max_nodes ||
                entry.depth > limits.max_depth) {
                result.status = PickStatus::invalid_request;
                result.diagnostic = diagnostic(
                    "pick_scene_limit_exceeded",
                    "Scene traversal exceeds the configured pick limit");
                result.hit.reset();
                return result;
            }
            const formats::Kn5Node& node = *entry.node;
            if (!node.active) continue;

            Matrix4 node_to_world = entry.parent_to_world;
            if (node.type == 1U) {
                if (!finite_matrix(node.transform) ||
                    !affine_matrix(node.transform) ||
                    !multiply_matrix(entry.parent_to_world, node.transform,
                                     node_to_world)) {
                    result.status = PickStatus::invalid_request;
                    result.diagnostic = diagnostic(
                        "pick_scene_transform_invalid",
                        "An active KN5 node transform is invalid");
                    result.hit.reset();
                    return result;
                }
            } else if (node.type == 2U) {
                if (node.vertexStride != 11U || node.indices.size() % 3U != 0U) {
                    result.status = PickStatus::invalid_request;
                    result.diagnostic = diagnostic(
                        "pick_scene_geometry_invalid",
                        "An active KN5 mesh has invalid geometry layout");
                    result.hit.reset();
                    return result;
                }
                const std::size_t mesh_triangles = node.indices.size() / 3U;
                if (mesh_triangles > limits.max_triangles - triangle_count) {
                    result.status = PickStatus::invalid_request;
                    result.diagnostic = diagnostic(
                        "pick_scene_limit_exceeded",
                        "Scene triangles exceed the configured pick limit");
                    result.hit.reset();
                    return result;
                }
                triangle_count += mesh_triangles;
                const TrianglePickResult mesh_result = pick_mesh(
                    world_ray,
                    {node.vertices, node.vertexStride, node.indices, {}},
                    node_to_world, limits.mesh);
                if (mesh_result.status == PickStatus::invalid_request) {
                    result.status = PickStatus::invalid_request;
                    result.diagnostic = mesh_result.diagnostic;
                    result.hit.reset();
                    return result;
                }
                if (mesh_result.ok() &&
                    mesh_result.hit->distance < best_distance) {
                    best_distance = mesh_result.hit->distance;
                    result.status = PickStatus::hit;
                    result.hit = Kn5ScenePickHit{
                        *mesh_result.hit, visited - 1U,
                        mesh_result.hit->mesh_position};
                }
            } else if (node.type == 3U) {
                result.skipped_skinned_mesh = true;
            } else {
                result.status = PickStatus::invalid_request;
                result.diagnostic = diagnostic(
                    "pick_scene_node_invalid",
                    "An active KN5 node has an unsupported type");
                result.hit.reset();
                return result;
            }

            if (stack.size() > limits.max_nodes - visited ||
                entry.depth == std::numeric_limits<std::size_t>::max()) {
                result.status = PickStatus::invalid_request;
                result.diagnostic = diagnostic(
                    "pick_scene_limit_exceeded",
                    "Scene children exceed the configured pick limit");
                result.hit.reset();
                return result;
            }
            const std::size_t scheduled = visited + stack.size();
            if (node.children.size() > limits.max_nodes - scheduled) {
                result.status = PickStatus::invalid_request;
                result.diagnostic = diagnostic(
                    "pick_scene_limit_exceeded",
                    "Scene children exceed the configured pick limit");
                result.hit.reset();
                return result;
            }
            for (std::size_t child = node.children.size(); child > 0U; --child) {
                stack.push_back({&node.children[child - 1U], node_to_world,
                                 entry.depth + 1U});
            }
        }
        if (result.status == PickStatus::miss && result.skipped_skinned_mesh) {
            result.diagnostic = diagnostic(
                "pick_skinned_unsupported",
                "Installed-editor picking skips skinned meshes");
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.status = PickStatus::invalid_request;
        result.diagnostic = diagnostic(
            "pick_scene_allocation_failed",
            "Scene picking could not allocate bounded traversal storage");
        result.hit.reset();
        return result;
    }
}

const char* pick_status_name(PickStatus status) noexcept {
    switch (status) {
    case PickStatus::hit: return "hit";
    case PickStatus::miss: return "miss";
    case PickStatus::invalid_request: return "invalid_request";
    }
    return "unknown";
}

}  // namespace apex::render
