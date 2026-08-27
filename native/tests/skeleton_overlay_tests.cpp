#include "apex/render/skeleton_overlay.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

using Node = apex::render::SkeletonOverlayNode;
using Kind = apex::render::SkeletonOverlayNodeKind;

Node node(const std::array<float, 3U> position,
          const std::span<const std::uint32_t> children = {},
          const Kind kind = Kind::plain) {
    return {position, children, kind, 0U};
}

void exact_native_marker_and_connector_vertices() {
    const std::array<std::uint32_t, 1U> root_children = {1U};
    const std::array<Node, 2U> nodes = {
        node({1.0F, 2.0F, 3.0F}, root_children),
        node({4.0F, 5.0F, 6.0F}),
    };
    const auto result = apex::render::build_skeleton_overlay(nodes, 0U);
    require(result.ok() && result.vertices.size() == 8U,
            "native marker plus connector has eight vertices");
    const auto white = apex::render::skeleton_overlay_marker_color;
    const auto magenta = apex::render::skeleton_overlay_connector_color;
    const auto& vertices = result.vertices;
    require(vertices[0].position ==
                    std::array<float, 3U>{1.03F, 2.0F, 3.0F} &&
                vertices[1].position ==
                    std::array<float, 3U>{0.97F, 2.0F, 3.0F} &&
                vertices[2].position ==
                    std::array<float, 3U>{1.0F, 2.03F, 3.0F} &&
                vertices[3].position ==
                    std::array<float, 3U>{1.0F, 1.97F, 3.0F} &&
                vertices[4].position ==
                    std::array<float, 3U>{1.0F, 2.0F, 3.03F} &&
                vertices[5].position ==
                    std::array<float, 3U>{1.0F, 2.0F, 2.97F} &&
                vertices[6].position ==
                    std::array<float, 3U>{1.0F, 2.0F, 3.0F} &&
                vertices[7].position ==
                    std::array<float, 3U>{4.0F, 5.0F, 6.0F},
            "marker and connector preserve recovered coordinates and order");
    for (std::size_t index = 0U; index < 6U; ++index)
        require(vertices[index].color == white,
                "skeleton markers use the recovered white color");
    require(vertices[6U].color == magenta &&
                vertices[7U].color == magenta,
            "unselected skeleton connectors use the recovered magenta color");
}

void applies_recovered_selected_connector_color() {
    const std::array<std::uint32_t, 1U> root_children = {1U};
    const std::array<std::uint32_t, 1U> child_children = {2U};
    const std::array<Node, 3U> nodes = {
        node({0.0F, 0.0F, 0.0F}, root_children),
        node({1.0F, 0.0F, 0.0F}, child_children),
        node({2.0F, 0.0F, 0.0F}),
    };
    const auto result =
        apex::render::build_skeleton_overlay(nodes, 0U, 1U);
    require(result.ok() && result.vertices.size() == 16U,
            "selected skeleton fixture emits two markers and connectors");
    require(result.vertices[6U].color ==
                    apex::render::skeleton_overlay_connector_color &&
                result.vertices[7U].color ==
                    apex::render::skeleton_overlay_connector_color,
            "connectors from an unselected current node remain magenta");
    require(result.vertices[14U].color ==
                    apex::render::skeleton_overlay_selected_connector_color &&
                result.vertices[15U].color ==
                    apex::render::skeleton_overlay_selected_connector_color,
            "connectors from the selected current node are yellow");
    for (const std::size_t index : {0U, 1U, 2U, 3U, 4U, 5U,
                                    8U, 9U, 10U, 11U, 12U, 13U})
        require(result.vertices[index].color ==
                    apex::render::skeleton_overlay_marker_color,
                "selection does not change the white marker color");

    const auto invalid = apex::render::build_skeleton_overlay(nodes, 0U, 3U);
    require(!invalid.ok() &&
                invalid.diagnostic.code ==
                    "skeleton_overlay_selected_node_invalid" &&
                invalid.vertices.empty(),
            "an invalid selected node is rejected before output allocation");
}

void preserves_child_order_and_filters_mesh_connectors() {
    const std::array<std::uint32_t, 3U> root_children = {1U, 2U, 3U};
    const std::array<std::uint32_t, 1U> plain_children = {4U};
    const std::array<Node, 5U> nodes = {
        node({0.0F, 0.0F, 0.0F}, root_children),
        node({1.0F, 0.0F, 0.0F}, plain_children),
        node({2.0F, 0.0F, 0.0F}, {}, Kind::mesh),
        node({3.0F, 0.0F, 0.0F}, {}, Kind::skinned_mesh),
        node({4.0F, 0.0F, 0.0F}),
    };
    const auto result = apex::render::build_skeleton_overlay(nodes, 0U);
    require(result.ok() && result.vertices.size() == 16U,
            "mesh connector is filtered while plain source order remains");
    // Root marker, root->plain, root->mesh omitted, then plain marker,
    // plain->leaf. The mesh child still recurses, as native intRender does.
    require(result.vertices[6].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                result.vertices[7].position == std::array<float, 3U>{1.0F, 0.0F, 0.0F} &&
                result.vertices[8].position == std::array<float, 3U>{1.03F, 0.0F, 0.0F} &&
                result.vertices[14].position == std::array<float, 3U>{1.0F, 0.0F, 0.0F} &&
                result.vertices[15].position == std::array<float, 3U>{4.0F, 0.0F, 0.0F},
            "source-order markers and connectors are preserved");
}

void rejects_malformed_and_nonfinite_input_atomically() {
    const std::array<std::uint32_t, 1U> truncated_child = {4U};
    const std::array<Node, 1U> truncated = {
        node({0.0F, 0.0F, 0.0F}, truncated_child),
    };
    auto result = apex::render::build_skeleton_overlay(truncated, 0U);
    require(!result.ok() && result.status == apex::render::SkeletonOverlayStatus::input_truncated &&
                result.vertices.empty(),
            "out-of-range child is rejected without partial output");

    std::array<Node, 1U> nonfinite = {node({0.0F, 0.0F, 0.0F})};
    nonfinite[0].world_translation[1U] =
        std::numeric_limits<float>::quiet_NaN();
    result = apex::render::build_skeleton_overlay(nonfinite, 0U);
    require(!result.ok() &&
                result.status == apex::render::SkeletonOverlayStatus::non_finite_translation &&
                result.vertices.empty(),
            "non-finite translation is rejected without partial output");

    const std::array<Node, 1U> declared_truncated = {
        {node({0.0F, 0.0F, 0.0F}).world_translation, {}, Kind::plain, 2U},
    };
    result = apex::render::build_skeleton_overlay(declared_truncated, 0U);
    require(!result.ok() && result.status == apex::render::SkeletonOverlayStatus::input_truncated &&
                result.diagnostic.code == "skeleton_overlay_children_truncated" &&
                result.vertices.empty(),
            "declared child truncation is rejected atomically");
}

void rejects_cycles_duplicates_depth_and_counts() {
    const std::array<std::uint32_t, 1U> to_child = {1U};
    const std::array<std::uint32_t, 1U> to_root = {0U};
    const std::array<Node, 2U> cycle = {
        node({0.0F, 0.0F, 0.0F}, to_child),
        node({1.0F, 0.0F, 0.0F}, to_root),
    };
    auto result = apex::render::build_skeleton_overlay(cycle, 0U);
    require(result.status == apex::render::SkeletonOverlayStatus::cycle_detected &&
                result.vertices.empty(),
            "cycle is rejected before geometry allocation");

    const std::array<std::uint32_t, 2U> duplicate_children = {1U, 1U};
    const std::array<Node, 2U> duplicate = {
        node({0.0F, 0.0F, 0.0F}, duplicate_children),
        node({1.0F, 0.0F, 0.0F}),
    };
    result = apex::render::build_skeleton_overlay(duplicate, 0U);
    require(result.status == apex::render::SkeletonOverlayStatus::duplicate_node &&
                result.vertices.empty(),
            "duplicate node reference is rejected atomically");

    apex::render::SkeletonOverlayLimits limits;
    limits.max_depth = 0U;
    result = apex::render::build_skeleton_overlay(cycle, 0U, limits);
    require(result.status == apex::render::SkeletonOverlayStatus::depth_limit_exceeded &&
                result.vertices.empty(),
            "depth limit is enforced before output");

    limits = {};
    limits.max_nodes = 1U;
    result = apex::render::build_skeleton_overlay(duplicate, 0U, limits);
    require(result.status == apex::render::SkeletonOverlayStatus::node_count_exceeded &&
                result.vertices.empty(),
            "node count limit is enforced");

    limits = {};
    limits.max_edges = 0U;
    const std::array<Node, 2U> valid = {
        node({0.0F, 0.0F, 0.0F}, to_child),
        node({1.0F, 0.0F, 0.0F}),
    };
    result = apex::render::build_skeleton_overlay(valid, 0U, limits);
    require(result.status == apex::render::SkeletonOverlayStatus::edge_count_exceeded &&
                result.vertices.empty(),
            "edge count limit is enforced");

    limits = {};
    limits.max_vertices = 7U;
    result = apex::render::build_skeleton_overlay(valid, 0U, limits);
    require(result.status == apex::render::SkeletonOverlayStatus::vertex_count_exceeded &&
                result.vertices.empty(),
            "vertex count limit is enforced atomically");
}

} // namespace

int main() {
    try {
        exact_native_marker_and_connector_vertices();
        applies_recovered_selected_connector_color();
        preserves_child_order_and_filters_mesh_connectors();
        rejects_malformed_and_nonfinite_input_atomically();
        rejects_cycles_duplicates_depth_and_counts();
        std::cout << "skeleton overlay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "skeleton overlay tests failed: " << error.what() << '\n';
        return 1;
    }
}
