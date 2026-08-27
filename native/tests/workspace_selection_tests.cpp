#include "apex/app/workspace_selection.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

apex::scene::SceneNode node(std::string name, apex::scene::NodeKind kind) {
  apex::scene::SceneNode result;
  result.name = std::move(name);
  result.kind = kind;
  return result;
}

apex::scene::SceneSnapshot fixture() {
  apex::scene::SceneSnapshot scene;
  const auto root = scene.add_node(node("ROOT", apex::scene::NodeKind::node));
  (void)scene.add_node(node("Door", apex::scene::NodeKind::mesh), root);
  const auto group =
      scene.add_node(node("GROUP", apex::scene::NodeKind::node), root);
  (void)scene.add_node(node("LEFT_DOOR", apex::scene::NodeKind::skinned_mesh),
                       group);
  (void)scene.add_node(node("Door", apex::scene::NodeKind::mesh), root);
  return scene;
}

void searches_in_browser_preorder_without_collapsing_duplicates() {
  const auto scene = fixture();
  apex::app::WorkspaceSelectionRequest request;
  request.query = "  dOoR\t";
  const auto result = apex::app::resolve_workspace_selection(scene, request);
  require(result.ok() && result.matches.size() == 3U,
          "case-insensitive hierarchy search");
  require(result.matches[0].node == 1U && result.matches[0].name == "Door" &&
              result.matches[1].node == 3U &&
              result.matches[1].name == "LEFT_DOOR" &&
              result.matches[2].node == 4U && result.matches[2].name == "Door",
          "preorder search and duplicate-name preservation");
  require(result.matches[0].depth == 0U && result.matches[1].depth == 0U &&
              result.matches[2].depth == 0U,
          "filtered browser rows use depth zero");

  request.query = {};
  const auto all = apex::app::resolve_workspace_selection(scene, request);
  require(all.ok() && all.matches.size() == 5U && all.matches[0].node == 0U &&
              all.matches[0].depth == 0U && all.matches[1].node == 1U &&
              all.matches[1].depth == 1U && all.matches[2].node == 2U &&
              all.matches[2].depth == 1U && all.matches[3].node == 3U &&
              all.matches[3].depth == 2U && all.matches[4].node == 4U &&
              all.matches[4].depth == 1U,
          "empty query preserves hierarchy depth and preorder");
}

void validates_selection_and_maps_existing_render_options() {
  const auto scene = fixture();
  apex::app::WorkspaceSelectionRequest request;
  request.query = "door";
  request.selected_node = 3U;
  request.isolate_selected = true;
  request.show_hidden = true;
  request.wireframe = true;
  const auto result = apex::app::resolve_workspace_selection(scene, request);
  require(result.ok() && result.state.selected_node == 3U &&
              result.state.isolate_selected && result.state.show_hidden &&
              result.state.wireframe,
          "mesh selection state");

  request.selected_node = 2U;
  request.isolate_selected = false;
  require(apex::app::resolve_workspace_selection(scene, request).ok(),
          "a hierarchy group can be selected without isolation");
  request.isolate_selected = true;
  const auto group = apex::app::resolve_workspace_selection(scene, request);
  require(!group.ok() &&
              group.status ==
                  apex::app::WorkspaceSelectionStatus::invalid_selection &&
              group.matches.empty() &&
              group.diagnostic.code == "workspace_selection_isolation_invalid",
          "a hierarchy group cannot be isolated");

  request.selected_node = 999U;
  request.isolate_selected = false;
  const auto missing = apex::app::resolve_workspace_selection(scene, request);
  require(!missing.ok() && missing.matches.empty() &&
              missing.diagnostic.code == "workspace_selection_node_invalid",
          "an invalid selected node exposes no search result");

  request.selected_node = apex::scene::invalid_node_id;
  request.isolate_selected = true;
  const auto detached = apex::app::resolve_workspace_selection(scene, request);
  require(!detached.ok() && detached.matches.empty() &&
              detached.diagnostic.code ==
                  "workspace_selection_isolation_invalid",
          "detached isolation is rejected atomically");
}

void rejects_malformed_topology_and_all_bounded_outputs() {
  const auto source = fixture();
  auto malformed = source;
  malformed.nodes[3].parent = 0U;
  auto result = apex::app::resolve_workspace_selection(malformed, {});
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_parent_invalid",
          "child-parent mismatch rejection");

  malformed = source;
  malformed.nodes[0].children.push_back(1U);
  result = apex::app::resolve_workspace_selection(malformed, {});
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_topology_invalid",
          "duplicate child rejection");

  malformed = source;
  malformed.nodes.push_back(node("ORPHAN", apex::scene::NodeKind::node));
  malformed.nodes.back().id = 5U;
  result = apex::app::resolve_workspace_selection(malformed, {});
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_unreachable_node",
          "unreachable node rejection");

  malformed = source;
  malformed.nodes[1].id = 4U;
  result = apex::app::resolve_workspace_selection(malformed, {});
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_identity_invalid",
          "node-table identity rejection");

  auto limits = apex::app::WorkspaceSelectionLimits{};
  limits.max_nodes = 4U;
  result = apex::app::resolve_workspace_selection(source, {}, limits);
  require(!result.ok() &&
              result.diagnostic.code == "workspace_selection_node_limit",
          "node limit");

  limits = {};
  limits.max_depth = 1U;
  result = apex::app::resolve_workspace_selection(source, {}, limits);
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_depth_limit",
          "depth limit and no partial output");

  limits = {};
  limits.max_results = 2U;
  result = apex::app::resolve_workspace_selection(source, {}, limits);
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_result_limit",
          "result limit and no partial output");

  limits = {};
  limits.max_name_bytes = 3U;
  result = apex::app::resolve_workspace_selection(source, {}, limits);
  require(!result.ok() && result.matches.empty() &&
              result.diagnostic.code == "workspace_selection_name_limit",
          "name limit and no partial output");

  limits = {};
  limits.max_query_bytes = 2U;
  apex::app::WorkspaceSelectionRequest request;
  request.query = "door";
  result = apex::app::resolve_workspace_selection(source, request, limits);
  require(!result.ok() &&
              result.diagnostic.code == "workspace_selection_query_limit",
          "query limit");

  limits = {};
  limits.max_aggregate_bytes = 1U;
  result = apex::app::resolve_workspace_selection(source, {}, limits);
  require(!result.ok() &&
              result.diagnostic.code == "workspace_selection_aggregate_limit",
          "aggregate limit");

  request = {};
  request.query = "D\xC3\x96R";
  result = apex::app::resolve_workspace_selection(source, request);
  require(!result.ok() &&
              result.status ==
                  apex::app::WorkspaceSelectionStatus::unsupported &&
              result.diagnostic.code ==
                  "workspace_selection_query_unicode_unsupported",
          "Unicode case-folding boundary is explicit");

  require(source.nodes[1].name == "Door" &&
              source.nodes[0].children ==
                  std::vector<apex::scene::NodeId>({1U, 2U, 4U}),
          "selection and failed searches do not mutate the scene");
}

} // namespace

int main() {
  try {
    searches_in_browser_preorder_without_collapsing_duplicates();
    validates_selection_and_maps_existing_render_options();
    rejects_malformed_topology_and_all_bounded_outputs();
    std::cout << "workspace selection tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "workspace selection tests failed: " << error.what() << '\n';
    return 1;
  }
}
