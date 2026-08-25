#include "apex/app/workspace_selection.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace apex::app {
namespace {

WorkspaceSelectionResult fail(WorkspaceSelectionStatus status, std::string code,
                              std::string message,
                              scene::NodeId node = scene::invalid_node_id) {
  WorkspaceSelectionResult result;
  result.status = status;
  result.diagnostic = {std::move(code), std::move(message), node};
  return result;
}

bool charge(std::size_t count, std::size_t element_size, std::size_t &total,
            const WorkspaceSelectionLimits &limits) noexcept {
  if (element_size != 0U &&
      count > std::numeric_limits<std::size_t>::max() / element_size) {
    return false;
  }
  const auto bytes = count * element_size;
  if (total > limits.max_aggregate_bytes ||
      bytes > limits.max_aggregate_bytes - total) {
    return false;
  }
  total += bytes;
  return true;
}

std::string_view trim_ascii(std::string_view value) noexcept {
  const auto whitespace = [](char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == '\v';
  };
  while (!value.empty() && whitespace(value.front()))
    value.remove_prefix(1U);
  while (!value.empty() && whitespace(value.back()))
    value.remove_suffix(1U);
  return value;
}

bool ascii(std::string_view value) noexcept {
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return character < 0x80U; });
}

unsigned char lower_ascii(unsigned char character) noexcept {
  if (character >= static_cast<unsigned char>('A') &&
      character <= static_cast<unsigned char>('Z')) {
    return static_cast<unsigned char>(character + ('a' - 'A'));
  }
  return character;
}

bool contains_ascii_case_insensitive(std::string_view value,
                                     std::string_view query) noexcept {
  if (query.empty())
    return true;
  if (query.size() > value.size())
    return false;
  for (std::size_t first = 0U; first <= value.size() - query.size(); ++first) {
    bool matches = true;
    for (std::size_t offset = 0U; offset < query.size(); ++offset) {
      if (lower_ascii(static_cast<unsigned char>(value[first + offset])) !=
          lower_ascii(static_cast<unsigned char>(query[offset]))) {
        matches = false;
        break;
      }
    }
    if (matches)
      return true;
  }
  return false;
}

bool geometry(scene::NodeKind kind) noexcept {
  return kind == scene::NodeKind::mesh || kind == scene::NodeKind::skinned_mesh;
}

struct StackEntry {
  scene::NodeId node = scene::invalid_node_id;
  std::size_t depth = 0U;
};

} // namespace

WorkspaceSelectionResult
resolve_workspace_selection(const scene::SceneSnapshot &snapshot,
                            const WorkspaceSelectionRequest &request,
                            WorkspaceSelectionLimits limits) {
  const auto query = trim_ascii(request.query);
  if (query.size() > limits.max_query_bytes) {
    return fail(WorkspaceSelectionStatus::limit_exceeded,
                "workspace_selection_query_limit",
                "The hierarchy query exceeds its byte limit");
  }
  if (!ascii(query)) {
    return fail(WorkspaceSelectionStatus::unsupported,
                "workspace_selection_query_unicode_unsupported",
                "The native hierarchy search supports only ASCII queries");
  }

  const auto node_count = snapshot.nodes.size();
  if (node_count == 0U || snapshot.root == scene::invalid_node_id ||
      static_cast<std::size_t>(snapshot.root) >= node_count) {
    return fail(WorkspaceSelectionStatus::invalid_scene,
                "workspace_selection_root_invalid",
                "The scene does not contain one valid root node");
  }
  if (node_count > limits.max_nodes ||
      node_count >= static_cast<std::size_t>(scene::invalid_node_id)) {
    return fail(WorkspaceSelectionStatus::limit_exceeded,
                "workspace_selection_node_limit",
                "The scene node count exceeds the hierarchy search limit");
  }

  std::size_t aggregate_bytes = 0U;
  if (!charge(node_count, sizeof(bool), aggregate_bytes, limits) ||
      !charge(1U, sizeof(StackEntry), aggregate_bytes, limits)) {
    return fail(WorkspaceSelectionStatus::limit_exceeded,
                "workspace_selection_aggregate_limit",
                "Hierarchy validation exceeds its aggregate byte limit");
  }

  std::vector<bool> visited(node_count, false);
  std::vector<StackEntry> stack;
  stack.push_back({snapshot.root, 0U});
  std::size_t charged_stack_entries = 1U;
  WorkspaceSelectionResult output;
  output.status = WorkspaceSelectionStatus::ready;
  std::size_t visited_count = 0U;

  while (!stack.empty()) {
    const auto entry = stack.back();
    stack.pop_back();
    if (entry.depth > limits.max_depth) {
      return fail(WorkspaceSelectionStatus::limit_exceeded,
                  "workspace_selection_depth_limit",
                  "The scene hierarchy depth exceeds the search limit",
                  entry.node);
    }
    if (entry.node == scene::invalid_node_id ||
        static_cast<std::size_t>(entry.node) >= node_count) {
      return fail(WorkspaceSelectionStatus::invalid_scene,
                  "workspace_selection_child_invalid",
                  "The scene hierarchy contains an invalid child node",
                  entry.node);
    }
    const auto index = static_cast<std::size_t>(entry.node);
    const auto &node = snapshot.nodes[index];
    if (node.id != entry.node) {
      return fail(WorkspaceSelectionStatus::invalid_scene,
                  "workspace_selection_identity_invalid",
                  "A scene node ID does not match its table position",
                  entry.node);
    }
    if (visited[index]) {
      return fail(WorkspaceSelectionStatus::invalid_scene,
                  "workspace_selection_topology_invalid",
                  "The scene hierarchy contains a cycle or duplicate child",
                  entry.node);
    }
    visited[index] = true;
    ++visited_count;
    if (node.name.size() > limits.max_name_bytes) {
      return fail(WorkspaceSelectionStatus::limit_exceeded,
                  "workspace_selection_name_limit",
                  "A scene node name exceeds the hierarchy search limit",
                  entry.node);
    }
    const bool is_root = entry.node == snapshot.root;
    if ((is_root && node.parent != scene::invalid_node_id) ||
        (!is_root && node.parent == scene::invalid_node_id)) {
      return fail(WorkspaceSelectionStatus::invalid_scene,
                  "workspace_selection_parent_invalid",
                  "A scene node has an invalid parent relationship",
                  entry.node);
    }

    if (request.collect_matches &&
        contains_ascii_case_insensitive(node.name, query)) {
      if (output.matches.size() >= limits.max_results) {
        return fail(WorkspaceSelectionStatus::limit_exceeded,
                    "workspace_selection_result_limit",
                    "The hierarchy search result count exceeds its limit",
                    entry.node);
      }
      if (!charge(1U, sizeof(WorkspaceHierarchyMatch), aggregate_bytes,
                  limits) ||
          !charge(node.name.size(), 1U, aggregate_bytes, limits)) {
        return fail(
            WorkspaceSelectionStatus::limit_exceeded,
            "workspace_selection_aggregate_limit",
            "Hierarchy search results exceed their aggregate byte limit",
            entry.node);
      }
      output.matches.push_back(
          {node.id, node.name, node.kind, query.empty() ? entry.depth : 0U});
    }

    for (auto child = node.children.rbegin(); child != node.children.rend();
         ++child) {
      if (*child == scene::invalid_node_id ||
          static_cast<std::size_t>(*child) >= node_count) {
        return fail(WorkspaceSelectionStatus::invalid_scene,
                    "workspace_selection_child_invalid",
                    "The scene hierarchy contains an invalid child node",
                    *child);
      }
      const auto &child_node = snapshot.nodes[static_cast<std::size_t>(*child)];
      if (child_node.parent != node.id) {
        return fail(WorkspaceSelectionStatus::invalid_scene,
                    "workspace_selection_parent_invalid",
                    "A scene child does not reference its hierarchy parent",
                    *child);
      }
      if (stack.size() == charged_stack_entries) {
        if (!charge(1U, sizeof(StackEntry), aggregate_bytes, limits)) {
          return fail(WorkspaceSelectionStatus::limit_exceeded,
                      "workspace_selection_aggregate_limit",
                      "Hierarchy traversal exceeds its aggregate byte limit",
                      *child);
        }
        ++charged_stack_entries;
      }
      stack.push_back({*child, entry.depth + 1U});
    }
  }

  if (visited_count != node_count) {
    return fail(WorkspaceSelectionStatus::invalid_scene,
                "workspace_selection_unreachable_node",
                "The scene contains a node outside its root hierarchy");
  }

  if (request.selected_node != scene::invalid_node_id) {
    if (static_cast<std::size_t>(request.selected_node) >= node_count) {
      return fail(WorkspaceSelectionStatus::invalid_selection,
                  "workspace_selection_node_invalid",
                  "The selected scene node does not exist",
                  request.selected_node);
    }
    const auto &selected =
        snapshot.nodes[static_cast<std::size_t>(request.selected_node)];
    if (request.isolate_selected && !geometry(selected.kind)) {
      return fail(WorkspaceSelectionStatus::invalid_selection,
                  "workspace_selection_isolation_invalid",
                  "Only a mesh or skinned mesh can be isolated",
                  request.selected_node);
    }
  } else if (request.isolate_selected) {
    return fail(WorkspaceSelectionStatus::invalid_selection,
                "workspace_selection_isolation_invalid",
                "Isolation requires one selected mesh");
  }

  output.state = {request.selected_node, request.isolate_selected,
                  request.show_hidden, request.wireframe};
  return output;
}

const char *
workspace_selection_status_name(WorkspaceSelectionStatus status) noexcept {
  switch (status) {
  case WorkspaceSelectionStatus::ready:
    return "ready";
  case WorkspaceSelectionStatus::invalid_scene:
    return "invalid_scene";
  case WorkspaceSelectionStatus::invalid_selection:
    return "invalid_selection";
  case WorkspaceSelectionStatus::unsupported:
    return "unsupported";
  case WorkspaceSelectionStatus::limit_exceeded:
    return "limit_exceeded";
  }
  return "invalid_scene";
}

const char *workspace_selection_node_kind_name(scene::NodeKind kind) noexcept {
  switch (kind) {
  case scene::NodeKind::node:
    return "node";
  case scene::NodeKind::mesh:
    return "mesh";
  case scene::NodeKind::skinned_mesh:
    return "skinnedMesh";
  }
  return "node";
}

} // namespace apex::app
