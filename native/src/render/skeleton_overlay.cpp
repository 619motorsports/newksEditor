#include "apex/render/skeleton_overlay.hpp"

#include <algorithm>
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
    return {status, {code, message}, {}, {}};
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

void emit_vertex(SkeletonOverlayResult& output,
                 const std::array<float, 3U>& position,
                 const std::array<float, 3U>& color,
                 const std::uint32_t node,
                 const std::array<float, 3U>& offset) {
    output.vertices.push_back({position, color});
    output.position_sources.push_back({node, offset});
}

void emit_marker(SkeletonOverlayResult& output,
                 const std::uint32_t node,
                 const std::array<float, 3U>& position) {
    std::array<float, 3U> endpoint = position;
    endpoint[0U] = position[0U] + skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint, skeleton_overlay_marker_color, node,
                {skeleton_overlay_marker_half_extent, 0.0F, 0.0F});
    endpoint[0U] = position[0U] - skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint, skeleton_overlay_marker_color, node,
                {-skeleton_overlay_marker_half_extent, 0.0F, 0.0F});

    endpoint = position;
    endpoint[1U] = position[1U] + skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint, skeleton_overlay_marker_color, node,
                {0.0F, skeleton_overlay_marker_half_extent, 0.0F});
    endpoint[1U] = position[1U] - skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint, skeleton_overlay_marker_color, node,
                {0.0F, -skeleton_overlay_marker_half_extent, 0.0F});

    endpoint = position;
    endpoint[2U] = position[2U] + skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint, skeleton_overlay_marker_color, node,
                {0.0F, 0.0F, skeleton_overlay_marker_half_extent});
    endpoint[2U] = position[2U] - skeleton_overlay_marker_half_extent;
    emit_vertex(output, endpoint, skeleton_overlay_marker_color, node,
                {0.0F, 0.0F, -skeleton_overlay_marker_half_extent});
}

SkeletonOverlayResult build_skeleton_overlay_impl(
    const std::span<const SkeletonOverlayNode> nodes,
    const std::uint32_t root,
    const std::uint32_t selected_node,
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
    if (selected_node != invalid_skeleton_overlay_node &&
        selected_node >= nodes.size()) {
        return failure(SkeletonOverlayStatus::invalid_request,
                       "skeleton_overlay_selected_node_invalid",
                       "The selected skeleton node must identify a supplied node");
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
    result.position_sources.reserve(marker_vertices + connector_vertices);
    while (!stack.empty()) {
        WalkFrame& frame = stack.back();
        const SkeletonOverlayNode& node = nodes[frame.node];
        if (frame.next_child == 0U && !node.children.empty()) {
            emit_marker(result, static_cast<std::uint32_t>(frame.node),
                        node.world_translation);
        }
        if (frame.next_child == node.children.size()) {
            stack.pop_back();
            continue;
        }

        const std::uint32_t child_id = node.children[frame.next_child++];
        const SkeletonOverlayNode& child = nodes[child_id];
        if (connector_allowed(child.kind)) {
            const auto& connector_color =
                frame.node == selected_node
                    ? skeleton_overlay_selected_connector_color
                    : skeleton_overlay_connector_color;
            emit_vertex(result, node.world_translation, connector_color,
                        static_cast<std::uint32_t>(frame.node), {});
            emit_vertex(result, child.world_translation, connector_color,
                        child_id, {});
        }
        // All children recurse, including mesh and skinned-mesh nodes. Only
        // their connector was filtered above, matching the native walk.
        stack.push_back({static_cast<std::size_t>(child_id), frame.depth + 1U,
                         0U});
    }
    return result;
}

} // namespace

SkeletonOverlayResult build_skeleton_overlay(
    const std::span<const SkeletonOverlayNode> nodes,
    const std::uint32_t root,
    const SkeletonOverlayLimits limits) {
    return build_skeleton_overlay_impl(
        nodes, root, invalid_skeleton_overlay_node, limits);
}

SkeletonOverlayResult build_skeleton_overlay(
    const std::span<const SkeletonOverlayNode> nodes,
    const std::uint32_t root,
    const std::uint32_t selected_node,
    const SkeletonOverlayLimits limits) {
    return build_skeleton_overlay_impl(nodes, root, selected_node, limits);
}

SkeletonOverlayUpdateStatus update_skeleton_overlay_positions(
    const std::span<OverlayLineVertex> vertices,
    const std::span<const SkeletonOverlayResult::PositionSource>
        position_sources,
    const std::span<const apex::scene::Matrix4> world_transforms,
    Diagnostic& diagnostic) noexcept {
    if ((vertices.data() == nullptr && !vertices.empty()) ||
        (position_sources.data() == nullptr && !position_sources.empty()) ||
        (world_transforms.data() == nullptr && !world_transforms.empty())) {
        diagnostic = {
            "skeleton_overlay_update_span_null",
            "A non-empty skeleton refresh span must have storage"};
        return SkeletonOverlayUpdateStatus::invalid_request;
    }
    if (vertices.size() != position_sources.size()) {
        diagnostic = {
            "skeleton_overlay_update_size_mismatch",
            "Skeleton vertices and position sources must have equal counts"};
        return SkeletonOverlayUpdateStatus::invalid_request;
    }
    if (world_transforms.empty()) {
        diagnostic = {
            "skeleton_overlay_update_transforms_empty",
            "Skeleton position refresh requires world transforms"};
        return SkeletonOverlayUpdateStatus::invalid_request;
    }
    for (const auto& transform : world_transforms) {
        if (!std::all_of(transform.begin(), transform.end(), [](float value) {
                return std::isfinite(value);
            })) {
            diagnostic = {
                "skeleton_overlay_update_transform_non_finite",
                "Skeleton world transforms must contain only finite values"};
            return SkeletonOverlayUpdateStatus::invalid_request;
        }
    }
    for (const auto& source : position_sources) {
        if (static_cast<std::size_t>(source.node) >= world_transforms.size()) {
            diagnostic = {
                "skeleton_overlay_update_source_invalid",
                "A skeleton position source falls outside the transform span"};
            return SkeletonOverlayUpdateStatus::invalid_request;
        }
        const auto& transform = world_transforms[source.node];
        for (std::size_t component = 0U; component < 3U; ++component) {
            if (!std::isfinite(transform[12U + component] +
                               source.offset[component])) {
                diagnostic = {
                    "skeleton_overlay_update_position_non_finite",
                    "A refreshed skeleton position is not finite"};
                return SkeletonOverlayUpdateStatus::invalid_request;
            }
        }
    }
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        const auto& source = position_sources[index];
        const auto& transform = world_transforms[source.node];
        for (std::size_t component = 0U; component < 3U; ++component) {
            vertices[index].position[component] =
                transform[12U + component] + source.offset[component];
        }
    }
    diagnostic = {};
    return SkeletonOverlayUpdateStatus::ready;
}

} // namespace apex::render
