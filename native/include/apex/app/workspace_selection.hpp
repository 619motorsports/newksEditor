#pragma once

#include "apex/scene/scene.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace apex::app {

struct WorkspaceSelectionLimits {
  std::size_t max_nodes = 2'000'000U;
  std::size_t max_depth = 1'024U;
  std::size_t max_query_bytes = 4U * 1024U;
  std::size_t max_name_bytes = 1U * 1024U * 1024U;
  std::size_t max_results = 100'000U;
  std::size_t max_aggregate_bytes = 64U * 1024U * 1024U;
};

enum class WorkspaceSelectionStatus {
  ready,
  invalid_scene,
  invalid_selection,
  unsupported,
  limit_exceeded,
};

struct WorkspaceSelectionDiagnostic {
  std::string code;
  std::string message;
  scene::NodeId node = scene::invalid_node_id;
};

struct WorkspaceHierarchyMatch {
  scene::NodeId node = scene::invalid_node_id;
  std::string name;
  scene::NodeKind kind = scene::NodeKind::node;
  std::size_t depth = 0U;
};

struct WorkspaceSelectionRequest {
  std::string_view query;
  bool collect_matches = true;
  scene::NodeId selected_node = scene::invalid_node_id;
  bool isolate_selected = false;
  bool show_hidden = false;
  bool wireframe = false;
};

struct WorkspaceSelectionState {
  scene::NodeId selected_node = scene::invalid_node_id;
  bool isolate_selected = false;
  bool show_hidden = false;
  bool wireframe = false;
};

struct WorkspaceSelectionResult {
  WorkspaceSelectionStatus status = WorkspaceSelectionStatus::invalid_scene;
  WorkspaceSelectionDiagnostic diagnostic;
  std::vector<WorkspaceHierarchyMatch> matches;
  WorkspaceSelectionState state;

  [[nodiscard]] bool ok() const noexcept {
    return status == WorkspaceSelectionStatus::ready;
  }
};

/**
 * Resolve hierarchy search and preview selection without changing the scene.
 *
 * An empty query returns all nodes with their hierarchy depths. A nonempty
 * query uses the browser's trimmed ASCII case-insensitive substring rule and
 * returns matching rows at depth zero. Non-ASCII queries are explicit
 * unsupported input because this portable layer has no Unicode case-folding
 * dependency.
 */
[[nodiscard]] WorkspaceSelectionResult
resolve_workspace_selection(const scene::SceneSnapshot &scene,
                            const WorkspaceSelectionRequest &request,
                            WorkspaceSelectionLimits limits = {});

[[nodiscard]] const char *
workspace_selection_status_name(WorkspaceSelectionStatus status) noexcept;
[[nodiscard]] const char *
workspace_selection_node_kind_name(scene::NodeKind kind) noexcept;

} // namespace apex::app
