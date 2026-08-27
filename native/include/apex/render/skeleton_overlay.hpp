#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace apex::render {

inline constexpr float skeleton_overlay_marker_half_extent = 0.03F;
inline constexpr std::array<float, 3U> skeleton_overlay_marker_color = {
    1.0F, 1.0F, 1.0F};
inline constexpr std::array<float, 3U> skeleton_overlay_connector_color = {
    1.0F, 0.0F, 1.0F};
inline constexpr std::array<float, 3U>
    skeleton_overlay_selected_connector_color = {1.0F, 1.0F, 0.0F};
inline constexpr std::uint32_t invalid_skeleton_overlay_node =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t skeleton_overlay_marker_vertex_count = 6U;

// The native renderer emits markers for nodes that have children. Mesh and
// skinned-mesh children still participate in the recursive walk, but they do
// not receive a parent-to-child connector (the recovered RTTI checks at
// SkeletonRenderer's connector helper filter those two types).
enum class SkeletonOverlayNodeKind : std::uint8_t {
    plain,
    mesh,
    skinned_mesh,
};

struct SkeletonOverlayNode {
    std::array<float, 3U> world_translation{};
    std::span<const std::uint32_t> children;
    SkeletonOverlayNodeKind kind = SkeletonOverlayNodeKind::plain;
    // A non-zero declaration lets a caller prove that an upstream child
    // array was not truncated before it reached this renderer contract.
    std::size_t declared_child_count = 0U;
};

struct SkeletonOverlayLimits {
    std::size_t max_nodes = 4'096U;
    std::size_t max_edges = 16'384U;
    std::size_t max_depth = 1'024U;
    std::size_t max_vertices = max_overlay_line_vertices;
    // A non-zero declaration lets a caller prove that the node span itself
    // was not truncated.
    std::size_t declared_node_count = 0U;
};

enum class SkeletonOverlayStatus : std::uint8_t {
    ready,
    invalid_request,
    input_truncated,
    non_finite_translation,
    cycle_detected,
    duplicate_node,
    depth_limit_exceeded,
    node_count_exceeded,
    edge_count_exceeded,
    vertex_count_exceeded,
};

struct SkeletonOverlayResult {
    SkeletonOverlayStatus status = SkeletonOverlayStatus::invalid_request;
    Diagnostic diagnostic;
    std::vector<OverlayLineVertex> vertices;

    [[nodiscard]] bool ok() const noexcept {
        return status == SkeletonOverlayStatus::ready;
    }
};

// Build the source-order line list used by the original skeleton overlay.
// Validation completes before the output vector is allocated, and every
// failure returns an empty vertex list. The input remains owned by the
// caller; this function only consumes the supplied spans.
[[nodiscard]] SkeletonOverlayResult build_skeleton_overlay(
    std::span<const SkeletonOverlayNode> nodes,
    std::uint32_t root,
    SkeletonOverlayLimits limits = {});

// This overload applies the recovered selected-node connector color. The
// current node controls the color of all connectors to its plain children.
[[nodiscard]] SkeletonOverlayResult build_skeleton_overlay(
    std::span<const SkeletonOverlayNode> nodes,
    std::uint32_t root,
    std::uint32_t selected_node,
    SkeletonOverlayLimits limits = {});

} // namespace apex::render
