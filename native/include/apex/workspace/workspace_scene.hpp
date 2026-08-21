#pragma once

#include "apex/scene/scene.hpp"
#include "apex/workspace/workspace.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace apex::workspace {

struct WorkspaceSceneLimits {
    std::size_t max_files = 100'000U;
    std::size_t max_scene_nodes = 1'000'000U;
    std::size_t max_string_bytes = 4U * 1024U * 1024U;
    std::size_t max_aggregate_bytes = 64U * 1024U * 1024U;
};

/** Stable source-order map from workspace files to merged scene wrappers. */
struct WorkspaceSceneBinding {
    std::vector<apex::scene::NodeId> file_root_nodes;
};

/**
 * Validate a merged workspace scene and attach inherited file/auxiliary labels.
 * The snapshot is unchanged if validation or allocation fails.
 */
[[nodiscard]] WorkspaceSceneBinding bindWorkspaceScene(
    apex::scene::SceneSnapshot& scene, const WorkspaceMetadata& workspace,
    WorkspaceSceneLimits limits = {});

struct WorkspaceLodResolutionRequest {
    const WorkspaceMetadata* workspace = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    std::span<const apex::scene::NodeId> file_root_nodes{};
    // These values must come from the production preview AABB center and the
    // active camera. SceneSnapshot::bounds_radius is not an equivalent source.
    apex::scene::Vector3 bounds_center{};
    apex::scene::Vector3 camera_position{};
    std::optional<std::uint32_t> selected_index;
    float lod_fov_degrees = 45.0F;
    float lod_distance_divisor = 1.0F;
    bool track_camera = false;
};

struct WorkspaceLodResolution {
    std::optional<std::uint32_t> selected_index;
    float camera_distance = 0.0F;
    float effective_distance = 0.0F;
    float lod_fov_degrees = 45.0F;
    bool track_camera = false;
    std::vector<std::uint32_t> active_indices;
    std::vector<apex::scene::NodeId> excluded_root_nodes;
};

/** Resolve exact half-open workspace LOD ranges before generic render planning. */
[[nodiscard]] WorkspaceLodResolution resolveWorkspaceLod(
    const WorkspaceLodResolutionRequest& request,
    WorkspaceSceneLimits limits = {});

inline WorkspaceSceneBinding bind_workspace_scene(
    apex::scene::SceneSnapshot& scene, const WorkspaceMetadata& workspace,
    WorkspaceSceneLimits limits = {}) {
    return bindWorkspaceScene(scene, workspace, limits);
}

inline WorkspaceLodResolution resolve_workspace_lod(
    const WorkspaceLodResolutionRequest& request,
    WorkspaceSceneLimits limits = {}) {
    return resolveWorkspaceLod(request, limits);
}

}  // namespace apex::workspace
