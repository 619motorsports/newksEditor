#include "apex/render/skeleton_overlay.hpp"

#include <algorithm>
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

bool same_vertices(
    const std::span<const apex::render::OverlayLineVertex> first,
    const std::span<const apex::render::OverlayLineVertex> second) {
    return first.size() == second.size() &&
           std::equal(first.begin(), first.end(), second.begin(),
                      [](const auto& left, const auto& right) {
                          return left.position == right.position &&
                                 left.color == right.color;
                      });
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
    require(result.ok() && result.vertices.size() == 8U &&
                result.position_sources.size() == result.vertices.size(),
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

void refreshes_world_positions_atomically_without_changing_colors() {
    const std::array<std::uint32_t, 1U> root_children = {1U};
    const std::array<Node, 2U> nodes = {
        node({1.0F, 2.0F, 3.0F}, root_children),
        node({4.0F, 5.0F, 6.0F}),
    };
    auto result = apex::render::build_skeleton_overlay(nodes, 0U);
    require(result.ok(), "refresh fixture builds");
    const auto original = result.vertices;
    std::array<apex::scene::Matrix4, 2U> transforms = {
        apex::scene::identity_matrix, apex::scene::identity_matrix};
    transforms[0U][12U] = 10.0F;
    transforms[0U][13U] = 20.0F;
    transforms[0U][14U] = 30.0F;
    transforms[1U][12U] = 40.0F;
    transforms[1U][13U] = 50.0F;
    transforms[1U][14U] = 60.0F;
    apex::render::Diagnostic diagnostic;
    require(apex::render::update_skeleton_overlay_positions(
                result.vertices, result.position_sources, transforms,
                diagnostic) ==
                apex::render::SkeletonOverlayUpdateStatus::ready &&
                result.vertices[0U].position ==
                    std::array<float, 3U>{10.03F, 20.0F, 30.0F} &&
                result.vertices[6U].position ==
                    std::array<float, 3U>{10.0F, 20.0F, 30.0F} &&
                result.vertices[7U].position ==
                    std::array<float, 3U>{40.0F, 50.0F, 60.0F} &&
                result.vertices[0U].color == original[0U].color &&
                result.vertices[7U].color == original[7U].color,
            "refresh uses current world translations and preserves colors");

    const auto refreshed = result.vertices;
    transforms[1U][0U] = std::numeric_limits<float>::quiet_NaN();
    require(apex::render::update_skeleton_overlay_positions(
                result.vertices, result.position_sources, transforms,
                diagnostic) ==
                apex::render::SkeletonOverlayUpdateStatus::invalid_request &&
                diagnostic.code ==
                    "skeleton_overlay_update_transform_non_finite" &&
                same_vertices(result.vertices, refreshed),
            "non-finite refresh input leaves every vertex unchanged");

    transforms[1U][0U] = 1.0F;
    auto invalid_sources = result.position_sources;
    invalid_sources.back().node = 2U;
    require(apex::render::update_skeleton_overlay_positions(
                result.vertices, invalid_sources, transforms, diagnostic) ==
                apex::render::SkeletonOverlayUpdateStatus::invalid_request &&
                diagnostic.code == "skeleton_overlay_update_source_invalid" &&
                same_vertices(result.vertices, refreshed),
            "out-of-range refresh binding is rejected atomically");
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
    const std::array<std::uint32_t, 4U> root_children = {1U, 2U, 3U, 5U};
    const std::array<std::uint32_t, 1U> plain_children = {4U};
    const std::array<Node, 6U> nodes = {
        node({0.0F, 0.0F, 0.0F}, root_children),
        node({1.0F, 0.0F, 0.0F}, plain_children),
        node({2.0F, 0.0F, 0.0F}, {}, Kind::mesh),
        node({3.0F, 0.0F, 0.0F}, {}, Kind::skinned_mesh),
        node({4.0F, 0.0F, 0.0F}),
        node({5.0F, 0.0F, 0.0F}),
    };
    const auto result = apex::render::build_skeleton_overlay(nodes, 0U);
    require(result.ok() && result.vertices.size() == 18U,
            "mesh connectors are filtered while plain source order remains");
    // Root marker, both eligible root connectors, then the first child marker
    // and connector. Mesh children still recurse, as native intRender does.
    require(result.vertices[6].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                result.vertices[7].position == std::array<float, 3U>{1.0F, 0.0F, 0.0F} &&
                result.vertices[8].position == std::array<float, 3U>{0.0F, 0.0F, 0.0F} &&
                result.vertices[9].position == std::array<float, 3U>{5.0F, 0.0F, 0.0F} &&
                result.vertices[10].position == std::array<float, 3U>{1.03F, 0.0F, 0.0F} &&
                result.vertices[16].position == std::array<float, 3U>{1.0F, 0.0F, 0.0F} &&
                result.vertices[17].position == std::array<float, 3U>{4.0F, 0.0F, 0.0F},
            "all parent connectors precede recursive child geometry");
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
        refreshes_world_positions_atomically_without_changing_colors();
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
