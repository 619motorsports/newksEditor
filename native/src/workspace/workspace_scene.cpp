#include "apex/workspace/workspace_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <set>
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

[[nodiscard]] std::string_view trim_ascii(std::string_view value) noexcept {
    std::size_t begin = 0U;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' ||
            value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           (value[end - 1U] == ' ' || value[end - 1U] == '\t' ||
            value[end - 1U] == '\r' || value[end - 1U] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

[[nodiscard]] char upper_ascii(char value) noexcept {
    return value >= 'a' && value <= 'z'
               ? static_cast<char>(value - 'a' + 'A')
               : value;
}

struct CaseInsensitiveLess {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view left,
                                  std::string_view right) const noexcept {
        const std::size_t count = std::min(left.size(), right.size());
        for (std::size_t index = 0U; index < count; ++index) {
            const char left_value = upper_ascii(left[index]);
            const char right_value = upper_ascii(right[index]);
            if (left_value != right_value) return left_value < right_value;
        }
        return left.size() < right.size();
    }
};

enum class RimRole { none, regular, blurred };

struct RimMatch {
    RimRole role = RimRole::none;
    std::size_t corner = 0U;
};

[[nodiscard]] RimMatch rim_match(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 4U> regular = {
        "RIM_LF", "RIM_RF", "RIM_LR", "RIM_RR"};
    constexpr std::array<std::string_view, 4U> blurred = {
        "RIM_BLUR_LF", "RIM_BLUR_RF", "RIM_BLUR_LR", "RIM_BLUR_RR"};
    for (std::size_t index = 0U; index < regular.size(); ++index) {
        if (name == regular[index]) return {RimRole::regular, index};
        if (name == blurred[index]) return {RimRole::blurred, index};
    }
    return {};
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

WorkspacePreviewResolution resolveWorkspacePreview(
    const WorkspacePreviewResolutionRequest& request,
    WorkspaceSceneLimits limits) {
    try {
        validate_limits(limits);
        if (request.scene == nullptr) {
            throw error("INVALID_REQUEST", "workspace preview scene is required");
        }
        const auto& scene = *request.scene;
        if (scene.nodes.size() > limits.max_scene_nodes) {
            throw error("COUNT_LIMIT", "workspace preview node count exceeds its limit");
        }
        if (request.driver_hidden_names.size() > limits.max_files) {
            throw error("COUNT_LIMIT", "driver hidden-name count exceeds its limit");
        }
        if (scene.root == apex::scene::invalid_node_id ||
            static_cast<std::size_t>(scene.root) >= scene.nodes.size()) {
            throw error("INVALID_SCENE", "workspace preview scene root is missing");
        }

        struct PreviewWalkState {
            apex::scene::NodeId node = apex::scene::invalid_node_id;
            std::string_view auxiliary;
            bool suppressed = false;
        };

        std::size_t aggregate_bytes = 0U;
        add_count(scene.nodes.size(),
                  sizeof(bool) + sizeof(PreviewWalkState) + 3U * sizeof(apex::scene::NodeId) +
                      sizeof(apex::scene::NodeActivityOverride),
                  aggregate_bytes, limits.max_aggregate_bytes,
                  "workspace preview resolution exceeds aggregate budget");
        add_count(request.driver_hidden_names.size(),
                  2U * (sizeof(std::string) + 3U * sizeof(void*)), aggregate_bytes,
                  limits.max_aggregate_bytes,
                  "driver hidden-name state exceeds aggregate budget");

        std::size_t string_bytes = 0U;
        std::set<std::string, CaseInsensitiveLess> hidden_names;
        for (const std::string& raw_name : request.driver_hidden_names) {
            const std::string_view name = trim_ascii(raw_name);
            if (name.empty()) continue;
            if (name.size() > limits.max_string_bytes) {
                throw error("STRING_LIMIT", "driver hidden name exceeds string budget");
            }
            add_bytes(name.size(), string_bytes, limits.max_string_bytes, "STRING_LIMIT",
                      "driver hidden names exceed string budget");
            hidden_names.emplace(name);
        }
        add_count(string_bytes, 2U, aggregate_bytes, limits.max_aggregate_bytes,
                  "driver hidden names exceed aggregate budget");

        WorkspacePreviewResolution result;
        result.driver_hidden_requested = hidden_names.size();
        std::set<std::string, CaseInsensitiveLess> matched_hidden_names;
        std::array<apex::scene::NodeId, 4U> first_regular_rims = {
            apex::scene::invalid_node_id, apex::scene::invalid_node_id,
            apex::scene::invalid_node_id, apex::scene::invalid_node_id};
        std::vector<apex::scene::NodeId> regular_rims;
        std::vector<apex::scene::NodeId> blurred_rims;
        regular_rims.reserve(scene.nodes.size());
        blurred_rims.reserve(scene.nodes.size());
        result.activity_overrides.reserve(scene.nodes.size());
        result.suppressed_root_nodes.reserve(scene.nodes.size());

        std::vector<bool> visited(scene.nodes.size(), false);
        std::vector<PreviewWalkState> stack;
        stack.reserve(scene.nodes.size());
        stack.push_back({scene.root, {}, false});
        std::size_t visited_count = 0U;
        while (!stack.empty()) {
            const PreviewWalkState state = stack.back();
            stack.pop_back();
            const auto& node = checked_node(scene, state.node);
            const std::size_t node_index = static_cast<std::size_t>(node.id);
            if (visited[node_index]) {
                throw error("INVALID_SCENE",
                            "workspace preview scene contains a repeated node edge");
            }
            visited[node_index] = true;
            ++visited_count;

            if (node.name == "COCKPIT_HR") {
                ++result.cockpit_high_nodes;
                if (result.cockpit_high_root == apex::scene::invalid_node_id) {
                    result.cockpit_high_root = node.id;
                }
            } else if (node.name == "COCKPIT_LR") {
                ++result.cockpit_low_nodes;
                if (result.cockpit_low_root == apex::scene::invalid_node_id) {
                    result.cockpit_low_root = node.id;
                }
            }

            const RimMatch rim = rim_match(node.name);
            if (rim.role == RimRole::regular) {
                regular_rims.push_back(node.id);
                if (first_regular_rims[rim.corner] == apex::scene::invalid_node_id) {
                    first_regular_rims[rim.corner] = node.id;
                }
            } else if (rim.role == RimRole::blurred) {
                blurred_rims.push_back(node.id);
            }

            const std::string_view auxiliary = node.workspace_auxiliary.empty()
                                                   ? state.auxiliary
                                                   : std::string_view(node.workspace_auxiliary);
            bool suppressed = state.suppressed;
            if (request.driver_cockpit && auxiliary == "driver") {
                const auto match = hidden_names.find(std::string_view(node.name));
                if (match != hidden_names.end()) {
                    matched_hidden_names.insert(*match);
                    if (!suppressed) result.suppressed_root_nodes.push_back(node.id);
                    suppressed = true;
                }
            }

            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
                const auto& child_node = checked_node(scene, *child);
                if (child_node.parent != node.id) {
                    throw error("INVALID_SCENE",
                                "workspace preview parent and child edges disagree");
                }
                stack.push_back({*child, auxiliary, suppressed});
            }
        }
        if (visited_count != scene.nodes.size() ||
            scene.nodes[static_cast<std::size_t>(scene.root)].parent !=
                apex::scene::invalid_node_id) {
            throw error("INVALID_SCENE",
                        "workspace preview scene is disconnected or has an invalid root");
        }

        result.cockpit_available = result.cockpit_high_root != apex::scene::invalid_node_id &&
                                   result.cockpit_low_root != apex::scene::invalid_node_id;
        if (request.cockpit_high_visible.has_value() && result.cockpit_available) {
            result.activity_overrides.push_back(
                {result.cockpit_high_root, *request.cockpit_high_visible});
            result.activity_overrides.push_back(
                {result.cockpit_low_root, !*request.cockpit_high_visible});
        }

        result.regular_rim_nodes = regular_rims.size();
        result.blurred_rim_nodes = blurred_rims.size();
        for (const apex::scene::NodeId id : first_regular_rims) {
            if (id != apex::scene::invalid_node_id) {
                result.first_regular_rim = id;
                break;
            }
        }
        result.rim_blur_available = result.first_regular_rim != apex::scene::invalid_node_id;
        if (request.blurred_rims_visible.has_value() && result.rim_blur_available) {
            for (const apex::scene::NodeId id : regular_rims) {
                result.activity_overrides.push_back({id, !*request.blurred_rims_visible});
            }
            for (const apex::scene::NodeId id : blurred_rims) {
                result.activity_overrides.push_back({id, *request.blurred_rims_visible});
            }
        }
        result.driver_hidden_matched = matched_hidden_names.size();
        return result;
    } catch (const WorkspaceError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw error("ALLOCATION_FAILED",
                    "workspace preview resolution allocation failed within its budget");
    }
}

}  // namespace apex::workspace
