#include "apex/render/skeleton_overlay.hpp"

#include <cmath>
#include <limits>
#include <utility>

namespace apex::render {
namespace {

enum class VisitState : std::uint8_t {
    unseen,
    active,
    complete,
};

struct WalkFrame {
    std::size_t node = 0U;
    std::size_t depth = 0U;
    std::size_t next_child = 0U;
};

[[nodiscard]] SkeletonOverlayResult failure(
    const SkeletonOverlayStatus status, const char* code,
    const char* message) {
    return {status, {code, message}, {}};
}

[[nodiscard]] bool finite_translation(
    const std::array<float, 3U>& translation) noexcept {
    for (const float component : translation) {
        if (!std::isfinite(component)) return false;
    }
    return true;
}

[[nodiscard]] bool valid_kind(const SkeletonOverlayNodeKind kind) noexcept {
    return kind == SkeletonOverlayNodeKind::plain ||
           kind == SkeletonOverlayNodeKind::mesh ||
           kind == SkeletonOverlayNodeKind::skinned_mesh;
}

[[nodiscard]] bool connector_allowed(
    const SkeletonOverlayNodeKind kind) noexcept {
    return kind == SkeletonOverlayNodeKind::plain;
}

[[nodiscard]] bool add_will_overflow(const std::size_t left,
                                     const std::size_t right) noexcept {
    return right > std::numeric_limits<std::size_t>::max() - left;
}

[[nodiscard]] bool multiply_will_overflow(const std::size_t left,
                                          const std::size_t right) noexcept {
    return right != 0U &&
           left > std::numeric_limits<std::size_t>::max() / right;
}

void emit_vertex(std::vector<OverlayLineVertex>& output,
                 const std::array<float, 3U>& position) {
    output.push_back({position, skeleton_overlay_color});
}

void emit_marker(std::vector<OverlayLineVertex>& output,
                 const std::array<float, 3U>& position) {
    std::array<float, 3U> endpoint = position;
    endpoint[0U] = position[0U] + skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint);
    endpoint[0U] = position[0U] - skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint);

    endpoint = position;
    endpoint[1U] = position[1U] + skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint);
    endpoint[1U] = position[1U] - skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint);

    endpoint = position;
    endpoint[2U] = position[2U] + skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint);
    endpoint[2U] = position[2U] - skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint);
}

} // namespace

SkeletonOverlayResult build_skeleton_overlay(
    const std::span<const SkeletonOverlayNode> nodes,
    const std::uint32_t root,
    const SkeletonOverlayLimits limits) {
    if (nodes.data() == nullptr && !nodes.empty()) {
        return failure(SkeletonOverlayStatus::invalid_request,
                       "skeleton_overlay_nodes_span_null",
                       "A non-empty skeleton node span must have storage");
    }
    if (limits.declared_node_count != 0U &&
        limits.declared_node_count != nodes.size()) {
        return failure(SkeletonOverlayStatus::input_truncated,
                       "skeleton_overlay_nodes_truncated",
                       "The skeleton node span does not contain its declared count");
    }
    if (nodes.empty() || root >= nodes.size()) {
        return failure(SkeletonOverlayStatus::invalid_request,
                       "skeleton_overlay_root_invalid",
                       "The skeleton root must identify a supplied node");
    }
    if (nodes.size() > limits.max_nodes) {
        return failure(SkeletonOverlayStatus::node_count_exceeded,
                       "skeleton_overlay_node_limit",
                       "The skeleton node count exceeds the caller limit");
    }

    std::size_t edge_count = 0U;
    for (const SkeletonOverlayNode& node : nodes) {
        if (!finite_translation(node.world_translation)) {
            return failure(SkeletonOverlayStatus::non_finite_translation,
                           "skeleton_overlay_translation_non_finite",
                           "Skeleton world translations must contain only finite values");
        }
        if (!valid_kind(node.kind)) {
            return failure(SkeletonOverlayStatus::invalid_request,
                           "skeleton_overlay_node_kind_invalid",
                           "Skeleton node kind is not recognized");
        }
        if (node.children.data() == nullptr && !node.children.empty()) {
            return failure(SkeletonOverlayStatus::invalid_request,
                           "skeleton_overlay_children_span_null",
                           "A non-empty child span must have storage");
        }
        if (node.declared_child_count != 0U &&
            node.declared_child_count != node.children.size()) {
            return failure(SkeletonOverlayStatus::input_truncated,
                           "skeleton_overlay_children_truncated",
                           "A skeleton child span does not contain its declared count");
        }
        if (add_will_overflow(edge_count, node.children.size()) ||
            edge_count + node.children.size() > limits.max_edges) {
            return failure(SkeletonOverlayStatus::edge_count_exceeded,
                           "skeleton_overlay_edge_limit",
                           "The skeleton edge count exceeds the caller limit");
        }
        edge_count += node.children.size();
        for (const std::uint32_t child : node.children) {
            if (child >= nodes.size()) {
                return failure(SkeletonOverlayStatus::input_truncated,
                               "skeleton_overlay_child_index_truncated",
                               "A skeleton child index falls outside the supplied node span");
            }
        }
    }

    // This preflight walk is deliberately separate from output generation.
    // It rejects cycles and repeated references before reserve()/push_back()
    // can allocate the render geometry.
    std::vector<VisitState> states(nodes.size(), VisitState::unseen);
    std::vector<WalkFrame> stack;
    stack.reserve(nodes.size());
    states[root] = VisitState::active;
    stack.push_back({root, 0U, 0U});

    std::size_t connector_count = 0U;
    std::size_t marker_count = nodes[root].children.empty() ? 0U : 1U;
    while (!stack.empty()) {
        WalkFrame& frame = stack.back();
        const SkeletonOverlayNode& node = nodes[frame.node];
        if (frame.next_child == node.children.size()) {
            states[frame.node] = VisitState::complete;
            stack.pop_back();
            continue;
        }

        const std::uint32_t child_id = node.children[frame.next_child++];
        const std::size_t child = static_cast<std::size_t>(child_id);
        if (states[child] == VisitState::active) {
            return failure(SkeletonOverlayStatus::cycle_detected,
                           "skeleton_overlay_cycle",
                           "The skeleton hierarchy contains a cycle");
        }
        if (states[child] == VisitState::complete) {
            return failure(SkeletonOverlayStatus::duplicate_node,
                           "skeleton_overlay_duplicate_node",
                           "A skeleton node is referenced more than once");
        }
        if (frame.depth >= limits.max_depth) {
            return failure(SkeletonOverlayStatus::depth_limit_exceeded,
                           "skeleton_overlay_depth_limit",
                           "The skeleton hierarchy exceeds the caller depth limit");
        }
        if (nodes[child].children.size() != 0U) ++marker_count;
        if (connector_allowed(nodes[child].kind)) ++connector_count;
        states[child] = VisitState::active;
        stack.push_back({child, frame.depth + 1U, 0U});
    }

    if (multiply_will_overflow(marker_count,
                               skeleton_overlay_marker_vertex_count) ||
        multiply_will_overflow(connector_count, 2U)) {
        return failure(SkeletonOverlayStatus::vertex_count_exceeded,
                       "skeleton_overlay_vertex_count_overflow",
                       "The skeleton vertex count cannot be represented");
    }
    const std::size_t marker_vertices =
        marker_count * skeleton_overlay_marker_vertex_count;
    const std::size_t connector_vertices = connector_count * 2U;
    if (add_will_overflow(marker_vertices, connector_vertices) ||
        marker_vertices + connector_vertices > limits.max_vertices) {
        return failure(SkeletonOverlayStatus::vertex_count_exceeded,
                       "skeleton_overlay_vertex_limit",
                       "The skeleton vertex count exceeds the caller limit");
    }

    // The preflight proved a single source-ordered tree. Replay it to emit
    // each node marker, then all of that node's connectors, then descendants,
    // matching the native intRender/connector-helper call order.
    stack.clear();
    stack.push_back({root, 0U, 0U});
    SkeletonOverlayResult result;
    result.status = SkeletonOverlayStatus::ready;
    result.vertices.reserve(marker_vertices + connector_vertices);
    while (!stack.empty()) {
        WalkFrame& frame = stack.back();
        const SkeletonOverlayNode& node = nodes[frame.node];
        if (frame.next_child == 0U && !node.children.empty()) {
            emit_marker(result.vertices, node.world_translation);
        }
        if (frame.next_child == node.children.size()) {
            stack.pop_back();
            continue;
        }

        const std::uint32_t child_id = node.children[frame.next_child++];
        const SkeletonOverlayNode& child = nodes[child_id];
        if (connector_allowed(child.kind)) {
            emit_vertex(result.vertices, node.world_translation);
            emit_vertex(result.vertices, child.world_translation);
        }
        // All children recurse, including mesh and skinned-mesh nodes. Only
        // their connector was filtered above, matching the native walk.
        stack.push_back({static_cast<std::size_t>(child_id), frame.depth + 1U,
                         0U});
    }
    return result;
}

} // namespace apex::render
