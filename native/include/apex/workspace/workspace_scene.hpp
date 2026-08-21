#pragma once

#include "apex/scene/scene.hpp"
#include "apex/workspace/workspace.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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

struct WorkspacePreviewResolutionRequest {
    const apex::scene::SceneSnapshot* scene = nullptr;
    // Null keeps authored cockpit state. A value selects the first exact
    // COCKPIT_HR/COCKPIT_LR pair, matching the production F3 path.
    std::optional<bool> cockpit_high_visible;
    // Null keeps authored rim state. A value replaces every exact RIM_* and
    // RIM_BLUR_* root, matching the production F1 path.
    std::optional<bool> blurred_rims_visible;
    bool driver_cockpit = false;
    std::span<const std::string> driver_hidden_names{};
};

struct WorkspacePreviewResolution {
    bool cockpit_available = false;
    apex::scene::NodeId cockpit_high_root = apex::scene::invalid_node_id;
    apex::scene::NodeId cockpit_low_root = apex::scene::invalid_node_id;
    std::size_t cockpit_high_nodes = 0U;
    std::size_t cockpit_low_nodes = 0U;
    bool rim_blur_available = false;
    apex::scene::NodeId first_regular_rim = apex::scene::invalid_node_id;
    std::size_t regular_rim_nodes = 0U;
    std::size_t blurred_rim_nodes = 0U;
    std::size_t driver_hidden_requested = 0U;
    std::size_t driver_hidden_matched = 0U;
    std::vector<apex::scene::NodeActivityOverride> activity_overrides;
    // Driver cockpit-hidden roots remain suppressed when show-hidden is
    // active. Keep these separate from workspace LOD exclusions.
    std::vector<apex::scene::NodeId> suppressed_root_nodes;
};

/** Resolve source-exact cockpit, rim, and driver preview state. */
[[nodiscard]] WorkspacePreviewResolution resolveWorkspacePreview(
    const WorkspacePreviewResolutionRequest& request,
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

inline WorkspacePreviewResolution resolve_workspace_preview(
    const WorkspacePreviewResolutionRequest& request,
    WorkspaceSceneLimits limits = {}) {
    return resolveWorkspacePreview(request, limits);
}

}  // namespace apex::workspace
