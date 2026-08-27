#include "apex/render/picking.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;
using Vector3 = apex::scene::Vector3;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void close(float actual, float expected, std::string_view message) {
    if (std::abs(actual - expected) > 1.0e-4F)
        throw std::runtime_error(std::string(message));
}

CameraFrame camera() {
    CameraFrameRequest request;
    request.eye = {0.0F, 0.0F, 5.0F};
    request.target = {0.0F, 0.0F, 0.0F};
    request.near_plane = 0.1F;
    request.far_plane = 20.0F;
    const auto result = build_camera_frame(request);
    require(result.ok(), "picking camera fixture builds");
    return *result.frame;
}

void builds_center_and_corner_rays() {
    const CameraFrame frame = camera();
    const auto center = build_screen_ray(frame, 50.0F, 50.0F, 100U, 100U);
    require(center.ok(), "center screen ray builds");
    close(center.ray->origin[2], 5.0F, "center ray begins at camera");
    close(center.ray->direction[0], 0.0F, "center ray has no horizontal offset");
    close(center.ray->direction[1], 0.0F, "center ray has no vertical offset");
    close(center.ray->direction[2], -1.0F, "center ray points forward");

    const auto corner = build_screen_ray(frame, 0.0F, 0.0F, 100U, 100U);
    require(corner.ok() && corner.ray->direction[0] < 0.0F &&
                corner.ray->direction[1] > 0.0F,
            "upper-left pixel maps to left and up world ray");
    require(center.ray->max_distance == installed_editor_pick_initial_distance,
            "screen ray uses recovered traversal distance initializer");

    CameraFrame backend_frame = frame;
    backend_frame.clip_space = CameraClipSpace::vulkan;
    const auto vulkan = build_screen_ray(
        backend_frame, 0.0F, 0.0F, 100U, 100U);
    backend_frame.clip_space = CameraClipSpace::d3d12;
    const auto d3d12 = build_screen_ray(
        backend_frame, 0.0F, 0.0F, 100U, 100U);
    require(vulkan.ok() && d3d12.ok() &&
                vulkan.ray->direction == d3d12.ray->direction,
            "screen ray is backend-neutral");
    require(intersect_pick_triangle(
                *center.ray, {-1.0F, -1.0F, -200.0F},
                {1.0F, -1.0F, -200.0F}, {0.0F, 1.0F, -200.0F})
                .ok(),
            "screen ray does not clip at the camera far plane");
}

void rejects_invalid_screen_inputs() {
    CameraFrame frame = camera();
    frame.fov_radians = std::numeric_limits<float>::quiet_NaN();
    const auto bad_camera = build_screen_ray(frame, 1.0F, 1.0F, 10U, 10U);
    require(!bad_camera.ok() && bad_camera.diagnostic.code == "pick_camera_non_finite",
            "non-finite camera is rejected");
    frame = camera();
    const auto bad_viewport = build_screen_ray(frame, 1.0F, 1.0F, 0U, 10U);
    require(!bad_viewport.ok() && bad_viewport.diagnostic.code == "pick_viewport_invalid",
            "zero viewport is rejected");
}

void triangle_boundaries_and_distance_match_contract() {
    const PickRay ray{{0.25F, 0.25F, 1.0F}, {0.0F, 0.0F, -1.0F}, 5.0F};
    const auto boundary = intersect_pick_triangle(
        ray, {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    require(boundary.ok(), "barycentric interior hit accepted");
    close(boundary.hit->distance, 1.0F, "triangle distance");
    close(boundary.hit->barycentric[0] + boundary.hit->barycentric[1] +
              boundary.hit->barycentric[2],
          1.0F, "barycentric coordinates sum to one");
    const PickRay edge_ray{{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.0F}, 5.0F};
    require(intersect_pick_triangle(edge_ray, {0.0F, 0.0F, 0.0F},
                                    {1.0F, 0.0F, 0.0F},
                                    {0.0F, 1.0F, 0.0F})
                .ok(),
            "barycentric boundary equality is accepted");
    const PickRay near_ray{{0.25F, 0.25F, 1.0e-5F}, {0.0F, 0.0F, -1.0F}, 5.0F};
    require(intersect_pick_triangle(near_ray, {0.0F, 0.0F, 0.0F},
                                    {1.0F, 0.0F, 0.0F},
                                    {0.0F, 1.0F, 0.0F})
                .status == PickStatus::miss,
            "threshold triangle distance is rejected");

    const PickRay small_determinant{{0.0F, 0.0F, 1.0F},
                                    {0.0F, 0.0F, -1.0F}, 5.0F};
    require(intersect_pick_triangle(small_determinant,
                                    {0.0F, 0.0F, 0.0F},
                                    {0.002F, 0.0F, 0.0F},
                                    {0.0F, 0.002F, 0.0F})
                .status == PickStatus::miss,
            "small triangle determinant is rejected");
}

void mesh_uses_index_order_and_transform() {
    const std::array<float, 18U> vertices = {
        0.0F, 0.0F, 0.0F,  1.0F, 0.0F, 0.0F,  0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, -1.0F, 1.0F, 0.0F, -1.0F, 0.0F, 1.0F, -1.0F};
    const std::array<std::uint16_t, 6U> indices = {3U, 4U, 5U, 0U, 1U, 2U};
    apex::scene::Matrix4 transform = apex::scene::identity_matrix;
    transform[14] = -2.0F;
    const PickMeshView mesh{vertices, 3U, indices, {}};
    const PickRay ray{{0.25F, 0.25F, 1.0F}, {0.0F, 0.0F, -1.0F}, 10.0F};
    const auto result = pick_mesh(ray, mesh, transform);
    require(result.ok() && result.hit->triangle_index == 0U &&
                result.hit->indices == std::array<std::uint32_t, 3U>{3U, 4U, 5U},
            "mesh returns first hit in index order");
    close(result.hit->position[2], -3.0F, "mesh transform reaches world hit");
    close(result.hit->distance, 4.0F, "mesh world distance");
}

apex::formats::Kn5Node static_triangle(float z) {
    apex::formats::Kn5Node node;
    node.type = 2U;
    node.kind = "mesh";
    node.active = true;
    node.vertexStride = 11U;
    for (const Vector3 position :
         std::array<Vector3, 3U>{{{0.0F, 0.0F, z},
                                  {1.0F, 0.0F, z},
                                  {0.0F, 1.0F, z}}}) {
        node.vertices.insert(node.vertices.end(), position.begin(),
                             position.end());
        node.vertices.insert(node.vertices.end(), 8U, 0.0F);
    }
    node.indices = {0U, 1U, 2U};
    return node;
}

void scene_pick_preserves_native_traversal_and_local_callback() {
    apex::formats::Kn5Node root;
    root.type = 1U;
    root.kind = "node";
    root.active = true;
    root.transform = apex::scene::identity_matrix;

    apex::formats::Kn5Node scaled;
    scaled.type = 1U;
    scaled.kind = "node";
    scaled.active = true;
    scaled.transform = apex::scene::identity_matrix;
    scaled.transform[0] = 2.0F;
    scaled.children.push_back(static_triangle(0.0F));
    root.children.push_back(std::move(scaled));

    const PickRay ray{{0.5F, 0.25F, 1.0F}, {0.0F, 0.0F, -1.0F},
                      installed_editor_pick_initial_distance};
    const auto hit = pick_kn5_scene(ray, root);
    require(hit.ok(), "active static scene triangle is picked");
    close(hit.hit->triangle.position[0], 0.5F,
          "generic hit retains world position");
    close(hit.hit->callback_position[0], 0.25F,
          "native callback retains mesh-local position");

    root.children[0].active = false;
    require(pick_kn5_scene(ray, root).status == PickStatus::miss,
            "inactive node prunes its mesh descendants");
}

void scene_pick_skips_skinned_and_rejects_malformed_static_meshes() {
    apex::formats::Kn5Node root;
    root.type = 1U;
    root.kind = "node";
    root.active = true;
    root.transform = apex::scene::identity_matrix;
    auto skinned = static_triangle(0.0F);
    skinned.type = 3U;
    skinned.kind = "skinnedMesh";
    skinned.vertexStride = 19U;
    root.children.push_back(std::move(skinned));
    const PickRay ray{{0.25F, 0.25F, 1.0F}, {0.0F, 0.0F, -1.0F},
                      installed_editor_pick_initial_distance};
    const auto skipped = pick_kn5_scene(ray, root);
    require(skipped.status == PickStatus::miss &&
                skipped.skipped_skinned_mesh &&
                skipped.diagnostic.code == "pick_skinned_unsupported",
            "skinned mesh is explicitly skipped");

    root.children[0] = static_triangle(0.0F);
    root.children[0].indices.push_back(0U);
    const auto malformed = pick_kn5_scene(ray, root);
    require(malformed.status == PickStatus::invalid_request &&
                malformed.diagnostic.code == "pick_scene_geometry_invalid",
            "malformed direct static mesh is rejected");
}

void rejects_malformed_mesh_indices_before_traversal() {
    const std::array<float, 9U> vertices = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                            0.0F, 1.0F, 0.0F};
    const std::array<std::uint32_t, 3U> out_of_range = {0U, 1U, 4U};
    const std::array<std::uint32_t, 2U> incomplete = {0U, 1U};
    const PickRay ray{{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.0F}, 10.0F};
    const auto bad_index = pick_mesh(ray, {vertices, 3U, {}, out_of_range});
    require(bad_index.status == PickStatus::invalid_request &&
                bad_index.diagnostic.code == "pick_mesh_index_out_of_range",
            "out-of-range index is rejected before traversal");
    const auto bad_count = pick_mesh(ray, {vertices, 3U, {}, incomplete});
    require(bad_count.status == PickStatus::invalid_request &&
                bad_count.diagnostic.code == "pick_mesh_indices_invalid",
            "incomplete triangle is rejected");

    PickMeshLimits small_limit;
    small_limit.max_indices = 2U;
    const std::array<std::uint32_t, 3U> valid = {0U, 1U, 2U};
    const auto limited = pick_mesh(ray, {vertices, 3U, {}, valid},
                                   apex::scene::identity_matrix, small_limit);
    require(limited.status == PickStatus::invalid_request &&
                limited.diagnostic.code == "pick_mesh_limit_exceeded",
            "mesh index limit is enforced before traversal");
}

}  // namespace

int main() {
    try {
        builds_center_and_corner_rays();
        rejects_invalid_screen_inputs();
        triangle_boundaries_and_distance_match_contract();
        mesh_uses_index_order_and_transform();
        scene_pick_preserves_native_traversal_and_local_callback();
        scene_pick_skips_skinned_and_rejects_malformed_static_meshes();
        rejects_malformed_mesh_indices_before_traversal();
        std::cout << "picking tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "picking tests failed: " << error.what() << '\n';
        return 1;
    }
}
