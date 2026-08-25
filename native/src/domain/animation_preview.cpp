#include "apex/domain/animation_preview.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace apex::domain {
namespace {

[[noreturn]] void fail(std::string_view source, std::string_view code,
                       std::string_view message) {
  throw core::ParseError("ANIMATION_PREVIEW", std::string(source), 0U,
                         std::string(code), std::string(message));
}

void charge(std::size_t count, std::size_t element_size, std::size_t &total,
            const AnimationPreviewLimits &limits, std::string_view source,
            std::string_view message) {
  if (element_size != 0U &&
      count > std::numeric_limits<std::size_t>::max() / element_size) {
    fail(source, "AGGREGATE_LIMIT", message);
  }
  const std::size_t bytes = count * element_size;
  if (bytes > limits.max_aggregate_bytes -
                  std::min(total, limits.max_aggregate_bytes)) {
    fail(source, "AGGREGATE_LIMIT", message);
  }
  total += bytes;
}

void add_diagnostic(AnimationPreviewApplication &result,
                    const AnimationPreviewLimits &limits,
                    std::size_t &aggregate_bytes,
                    AnimationPreviewDiagnosticSeverity severity,
                    std::string_view code, std::string_view message) {
  if (result.diagnostics.size() >= limits.max_diagnostics) {
    fail(result.source, "DIAGNOSTIC_LIMIT",
         "animation preview diagnostic output exceeds its limit");
  }
  charge(1U, sizeof(AnimationPreviewDiagnostic), aggregate_bytes, limits,
         result.source,
         "animation preview diagnostics exceed their aggregate budget");
  charge(code.size() + message.size() + result.source.size(), 1U,
         aggregate_bytes, limits, result.source,
         "animation preview diagnostics exceed their aggregate budget");
  result.diagnostics.push_back(
      {severity, std::string(code), std::string(message), result.source});
}

[[nodiscard]] bool finite_matrix(const formats::Kn5Matrix4 &matrix) noexcept {
  return std::all_of(matrix.begin(), matrix.end(),
                     [](float value) { return std::isfinite(value); });
}

struct SelectedTrack {
  std::string name;
  const formats::KsAnimationTrack *track = nullptr;
  formats::Kn5Matrix4 matrix{};
  bool matched = false;
};

struct PendingTransform {
  formats::Kn5Node *node = nullptr;
  formats::Kn5Matrix4 matrix{};
};

} // namespace

bool AnimationPreviewApplication::has_errors() const noexcept {
  return std::any_of(
      diagnostics.begin(), diagnostics.end(), [](const auto &item) {
        return item.severity == AnimationPreviewDiagnosticSeverity::error;
      });
}

AnimationPreviewApplication
apply_animation_preview(formats::Kn5File &model,
                        const formats::KsAnimation &animation, float position,
                        AnimationPreviewLimits limits) {
  AnimationPreviewApplication result;
  result.position = position;
  result.tracks = animation.tracks.size();
  if (!std::isfinite(position)) {
    fail(animation.source, "NON_FINITE_POSITION",
         "animation preview position must be finite");
  }
  if (animation.tracks.size() > limits.max_tracks) {
    fail(animation.source, "TRACK_LIMIT",
         "animation preview track count exceeds its limit");
  }

  std::size_t aggregate_bytes = 0U;
  charge(animation.source.size(), 1U, aggregate_bytes, limits, animation.source,
         "animation preview source exceeds its aggregate budget");
  result.source = animation.source;
  charge(animation.tracks.size(), sizeof(SelectedTrack), aggregate_bytes,
         limits, animation.source,
         "animation preview track table exceeds its aggregate budget");

  std::vector<SelectedTrack> selected;
  selected.reserve(animation.tracks.size());
  std::map<std::string, std::size_t, std::less<>> by_name;
  for (const auto &track : animation.tracks) {
    if (!track.animated)
      continue;
    ++result.animated_tracks;
    if (track.name.empty()) {
      fail(animation.source, "INVALID_TRACK_NAME",
           "animated KSANIM track name is empty");
    }
    if (track.name.size() > limits.max_track_name_bytes) {
      fail(animation.source, "TRACK_NAME_LIMIT",
           "animation preview track name exceeds its limit");
    }
    charge(track.name.size(), 1U, aggregate_bytes, limits, animation.source,
           "animation preview track names exceed their aggregate budget");
    const auto found = by_name.find(track.name);
    if (found == by_name.end()) {
      charge(sizeof(std::pair<std::string, std::size_t>) + 3U * sizeof(void *) +
                 track.name.size(),
             1U, aggregate_bytes, limits, animation.source,
             "animation preview lookup table exceeds its aggregate budget");
      by_name.emplace(track.name, selected.size());
      selected.push_back({track.name, &track, {}, false});
    } else {
      selected[found->second].track = &track;
      add_diagnostic(result, limits, aggregate_bytes,
                     AnimationPreviewDiagnosticSeverity::warning,
                     "DUPLICATE_ANIMATED_TRACK",
                     "later animated duplicate track wins");
    }
  }
  result.unique_animated_tracks = selected.size();
  for (auto &item : selected) {
    const auto sampled = formats::sampleKsAnimationTrack(*item.track, position);
    if (!sampled.has_value()) {
      fail(animation.source, "EMPTY_ANIMATED_TRACK",
           "animated KSANIM track contains no frames");
    }
    if (!finite_matrix(*sampled)) {
      fail(animation.source, "NON_FINITE_TRANSFORM",
           "sampled animation transform is not finite");
    }
    item.matrix = *sampled;
  }

  struct StackFrame {
    formats::Kn5Node *node = nullptr;
    std::size_t next_child = 0U;
    std::size_t depth = 0U;
  };
  std::vector<StackFrame> stack;
  charge(1U, sizeof(StackFrame), aggregate_bytes, limits, animation.source,
         "animation preview traversal stack exceeds its aggregate budget");
  stack.push_back({&model.root, 0U, 0U});
  std::size_t charged_stack_entries = 1U;
  std::vector<PendingTransform> pending;
  std::size_t node_count = 0U;
  while (!stack.empty()) {
    auto &frame = stack.back();
    if (frame.next_child == 0U) {
      if (frame.depth > limits.max_depth) {
        fail(model.source, "DEPTH_LIMIT",
             "animation preview hierarchy depth exceeds its limit");
      }
      if (node_count >= limits.max_nodes) {
        fail(model.source, "NODE_LIMIT",
             "animation preview hierarchy node count exceeds its limit");
      }
      ++node_count;
      if (frame.node->kind == "node") {
        const auto found = by_name.find(frame.node->name);
        if (found != by_name.end()) {
          auto &track = selected[found->second];
          charge(
              1U, sizeof(PendingTransform), aggregate_bytes, limits,
              animation.source,
              "animation preview transform table exceeds its aggregate budget");
          pending.push_back({frame.node, track.matrix});
          track.matched = true;
        }
      }
    }
    if (frame.next_child < frame.node->children.size()) {
      auto *child = &frame.node->children[frame.next_child];
      ++frame.next_child;
      if (stack.size() == charged_stack_entries) {
        charge(
            1U, sizeof(StackFrame), aggregate_bytes, limits, animation.source,
            "animation preview traversal stack exceeds its aggregate budget");
        ++charged_stack_entries;
      }
      stack.push_back({child, 0U, frame.depth + 1U});
    } else {
      stack.pop_back();
    }
  }

  bool unmatched_limit_reported = false;
  for (const auto &track : selected) {
    if (track.matched) {
      ++result.matched_tracks;
      continue;
    }
    if (result.unmatched_tracks.size() < limits.max_unmatched_tracks) {
      charge(sizeof(std::string) + track.name.size(), 1U, aggregate_bytes,
             limits, animation.source,
             "unmatched animation tracks exceed their aggregate budget");
      result.unmatched_tracks.push_back(track.name);
    } else if (!unmatched_limit_reported) {
      add_diagnostic(result, limits, aggregate_bytes,
                     AnimationPreviewDiagnosticSeverity::warning,
                     "UNMATCHED_OUTPUT_LIMIT",
                     "unmatched animation track output was truncated");
      unmatched_limit_reported = true;
    }
  }

  for (const auto &item : pending)
    item.node->transform = item.matrix;
  result.matched_nodes = pending.size();
  return result;
}

} // namespace apex::domain
