#include "apex/workspace/workspace_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::workspace {
namespace {

[[nodiscard]] WorkspaceError error(std::string_view code,
                                   std::string_view message) {
    return WorkspaceError("WORKSPACE", "scene", 0U, std::string(code),
                          std::string(message));
}

void add_bytes(std::size_t value, std::size_t& total, std::size_t limit,
               std::string_view code, std::string_view message) {
    if (value > limit - std::min(total, limit)) throw error(code, message);
    total += value;
}

void add_count(std::size_t count, std::size_t element_size, std::size_t& total,
               std::size_t limit, std::string_view message) {
    if (element_size != 0U &&
        count > std::numeric_limits<std::size_t>::max() / element_size) {
        throw error("AGGREGATE_LIMIT", message);
    }
    add_bytes(count * element_size, total, limit, "AGGREGATE_LIMIT", message);
}

void validate_limits(const WorkspaceSceneLimits& limits) {
    if (limits.max_files == 0U) {
        throw error("COUNT_LIMIT", "workspace scene file limit is zero");
    }
    if (limits.max_scene_nodes == 0U) {
        throw error("COUNT_LIMIT", "workspace scene node limit is zero");
    }
    if (limits.max_aggregate_bytes == 0U) {
        throw error("AGGREGATE_LIMIT", "workspace scene aggregate budget is zero");
    }
}

[[nodiscard]] const apex::scene::SceneNode& checked_node(
    const apex::scene::SceneSnapshot& scene, apex::scene::NodeId id) {
    if (id == apex::scene::invalid_node_id ||
        static_cast<std::size_t>(id) >= scene.nodes.size()) {
        throw error("INVALID_SCENE", "workspace root references a missing scene node");
    }
    const auto& node = scene.nodes[static_cast<std::size_t>(id)];
    if (node.id != id) {
        throw error("INVALID_SCENE", "workspace scene node IDs are not dense and stable");
    }
    return node;
}

void validate_lod(const WorkspaceFile& file) {
    if (!file.lod.has_value()) return;
    const auto& lod = *file.lod;
    if (!std::isfinite(lod.inDistance) || !std::isfinite(lod.outDistance) ||
        lod.inDistance < 0.0F || lod.outDistance <= lod.inDistance) {
        throw error("INVALID_LOD", "workspace file has an invalid LOD interval");
    }
}

void validate_request(const WorkspaceLodResolutionRequest& request,
                      const WorkspaceSceneLimits& limits) {
    validate_limits(limits);
    if (request.workspace == nullptr || request.scene == nullptr) {
        throw error("INVALID_REQUEST", "workspace metadata and scene are required");
    }
    if (request.workspace->files.size() > limits.max_files) {
        throw error("COUNT_LIMIT", "workspace file count exceeds its limit");
    }
    if (request.file_root_nodes.size() != request.workspace->files.size()) {
        throw error("INVALID_REQUEST", "workspace file/root mapping is incomplete");
    }
    if (request.scene->nodes.size() > limits.max_scene_nodes) {
        throw error("COUNT_LIMIT", "workspace scene node count exceeds its limit");
    }
    for (float value : request.bounds_center) {
        if (!std::isfinite(value))
            throw error("NON_FINITE_CAMERA", "workspace bounds center must be finite");
    }
    for (float value : request.camera_position) {
        if (!std::isfinite(value))
            throw error("NON_FINITE_CAMERA", "workspace camera position must be finite");
    }
    if (!std::isfinite(request.lod_fov_degrees) ||
        !std::isfinite(request.lod_distance_divisor)) {
        throw error("NON_FINITE_CAMERA", "workspace LOD camera controls must be finite");
    }
}

}  // namespace

WorkspaceSceneBinding bindWorkspaceScene(apex::scene::SceneSnapshot& scene,
                                         const WorkspaceMetadata& workspace,
                                         WorkspaceSceneLimits limits) {
    try {
        validate_limits(limits);
        if (workspace.files.size() > limits.max_files) {
            throw error("COUNT_LIMIT", "workspace file count exceeds scene binding limit");
        }
        if (scene.nodes.size() > limits.max_scene_nodes) {
            throw error("COUNT_LIMIT", "workspace scene node count exceeds its limit");
        }
        const auto& root = checked_node(scene, scene.root);
        if (root.parent != apex::scene::invalid_node_id ||
            root.children.size() != workspace.files.size()) {
            throw error("INVALID_SCENE", "workspace files do not match merged scene roots");
        }

        std::size_t string_bytes = workspace.kind.size();
        if (string_bytes > limits.max_string_bytes) {
            throw error("STRING_LIMIT", "workspace scene labels exceed string budget");
        }
        std::size_t aggregate_bytes = 0U;
        add_count(workspace.files.size(), sizeof(apex::scene::NodeId), aggregate_bytes,
                  limits.max_aggregate_bytes,
                  "workspace root map exceeds aggregate budget");
        add_count(scene.nodes.size(), sizeof(bool), aggregate_bytes,
                  limits.max_aggregate_bytes,
                  "workspace scene validation exceeds aggregate budget");
        add_count(workspace.files.size(),
                  sizeof(std::pair<std::string, std::string>), aggregate_bytes,
                  limits.max_aggregate_bytes,
                  "workspace scene label records exceed aggregate budget");

        for (std::size_t index = 0U; index < workspace.files.size(); ++index) {
            const auto id = root.children[index];
            const auto& node = checked_node(scene, id);
            const auto& file = workspace.files[index];
            if (node.parent != scene.root || node.kind != apex::scene::NodeKind::node ||
                node.name != file.name) {
                throw error("INVALID_SCENE",
                            "workspace file order does not match merged scene wrappers");
            }
            validate_lod(file);
            add_bytes(file.name.size(), string_bytes, limits.max_string_bytes,
                      "STRING_LIMIT", "workspace scene labels exceed string budget");
            add_bytes(file.auxiliary.size(), string_bytes, limits.max_string_bytes,
                      "STRING_LIMIT", "workspace scene labels exceed string budget");
        }
        add_bytes(string_bytes, aggregate_bytes, limits.max_aggregate_bytes,
                  "AGGREGATE_LIMIT", "workspace scene labels exceed aggregate budget");

        std::vector<bool> seen(scene.nodes.size(), false);
        std::vector<std::pair<std::string, std::string>> labels;
        labels.reserve(workspace.files.size());
        WorkspaceSceneBinding binding;
        binding.file_root_nodes.reserve(workspace.files.size());
        for (std::size_t index = 0U; index < workspace.files.size(); ++index) {
            const auto id = root.children[index];
            if (seen[static_cast<std::size_t>(id)]) {
                throw error("INVALID_SCENE", "workspace scene wrapper is referenced more than once");
            }
            seen[static_cast<std::size_t>(id)] = true;
            binding.file_root_nodes.push_back(id);
            labels.emplace_back(workspace.files[index].name,
                                workspace.files[index].auxiliary);
        }

        std::string workspace_kind = workspace.kind;
        for (std::size_t index = 0U; index < labels.size(); ++index) {
            auto& node = scene.nodes[static_cast<std::size_t>(binding.file_root_nodes[index])];
            node.workspace_file.swap(labels[index].first);
            node.workspace_auxiliary.swap(labels[index].second);
        }
        scene.workspace_kind.swap(workspace_kind);
        return binding;
    } catch (const WorkspaceError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw error("ALLOCATION_FAILED",
                    "workspace scene binding allocation failed within its budget");
    }
}

WorkspaceLodResolution resolveWorkspaceLod(
    const WorkspaceLodResolutionRequest& request, WorkspaceSceneLimits limits) {
    try {
        validate_request(request, limits);
        const auto& workspace = *request.workspace;
        const auto& scene = *request.scene;

        std::size_t aggregate_bytes = 0U;
        add_count(workspace.files.size(),
                  2U * sizeof(apex::scene::NodeId) + sizeof(std::uint32_t) +
                      sizeof(bool),
                  aggregate_bytes, limits.max_aggregate_bytes,
                  "workspace LOD resolution exceeds aggregate budget");
        add_count(scene.nodes.size(), sizeof(bool), aggregate_bytes,
                  limits.max_aggregate_bytes,
                  "workspace LOD validation exceeds aggregate budget");

        std::vector<bool> seen(scene.nodes.size(), false);
        const auto& root = checked_node(scene, scene.root);
        if (root.parent != apex::scene::invalid_node_id ||
            root.children.size() != workspace.files.size()) {
            throw error("INVALID_SCENE", "workspace LOD roots do not match the scene root");
        }
        for (std::size_t index = 0U; index < workspace.files.size(); ++index) {
            const auto id = request.file_root_nodes[index];
            const auto& node = checked_node(scene, id);
            if (seen[static_cast<std::size_t>(id)] || node.parent != scene.root ||
                root.children[index] != id ||
                node.workspace_file != workspace.files[index].name) {
                throw error("INVALID_SCENE", "workspace LOD root mapping is inconsistent");
            }
            seen[static_cast<std::size_t>(id)] = true;
            validate_lod(workspace.files[index]);
        }

        const double dx = static_cast<double>(request.camera_position[0]) -
                          static_cast<double>(request.bounds_center[0]);
        const double dy = static_cast<double>(request.camera_position[1]) -
                          static_cast<double>(request.bounds_center[1]);
        const double dz = static_cast<double>(request.camera_position[2]) -
                          static_cast<double>(request.bounds_center[2]);
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) ||
            distance > static_cast<double>(std::numeric_limits<float>::max())) {
            throw error("NON_FINITE_CAMERA", "workspace camera distance is outside float range");
        }

        WorkspaceLodResolution result;
        result.selected_index = request.selected_index;
        result.camera_distance = static_cast<float>(distance);
        result.effective_distance = carLodDistance(
            result.camera_distance, request.lod_fov_degrees,
            request.lod_distance_divisor, request.track_camera);
        result.lod_fov_degrees = request.lod_fov_degrees;
        result.track_camera = request.track_camera;
        if (!std::isfinite(result.effective_distance)) {
            throw error("NON_FINITE_CAMERA", "effective workspace LOD distance is not finite");
        }
        result.active_indices.reserve(workspace.files.size());
        result.excluded_root_nodes.reserve(workspace.files.size());
        for (std::size_t index = 0U; index < workspace.files.size(); ++index) {
            const auto& file = workspace.files[index];
            if (!file.lod.has_value()) continue;
            if (carLodVisible(&*file.lod, result.effective_distance,
                              request.selected_index)) {
                result.active_indices.push_back(file.lod->index);
            } else {
                result.excluded_root_nodes.push_back(request.file_root_nodes[index]);
            }
        }
        std::sort(result.active_indices.begin(), result.active_indices.end());
        result.active_indices.erase(
            std::unique(result.active_indices.begin(), result.active_indices.end()),
            result.active_indices.end());
        return result;
    } catch (const WorkspaceError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw error("ALLOCATION_FAILED",
                    "workspace LOD resolution allocation failed within its budget");
    }
}

}  // namespace apex::workspace
