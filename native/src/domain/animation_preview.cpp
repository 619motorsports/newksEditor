#include "apex/domain/animation_preview.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

template <typename Value>
void reserve_for_append(std::vector<Value> &values, std::size_t count_limit,
                        std::size_t &aggregate_bytes,
                        const AnimationPreviewLimits &limits,
                        std::string_view source, std::string_view limit_code,
                        std::string_view message) {
  if (values.size() < values.capacity())
    return;
  if (values.size() >= count_limit)
    fail(source, limit_code, message);
  const std::size_t remaining = count_limit - values.size();
  const std::size_t growth =
      values.capacity() == 0U ? 1U : std::min(values.capacity(), remaining);
  const std::size_t next_capacity = values.size() + growth;
  // Charge the complete new allocation. The previous allocation remains
  // charged, which bounds the temporary overlap during vector reallocation.
  charge(next_capacity, sizeof(Value), aggregate_bytes, limits, source,
         message);
  values.reserve(next_capacity);
}

template <typename Result>
void add_diagnostic(Result &result, const AnimationPreviewLimits &limits,
                    std::size_t &aggregate_bytes,
                    AnimationPreviewDiagnosticSeverity severity,
                    std::string_view code, std::string_view message) {
  if (result.diagnostics.size() >= limits.max_diagnostics) {
    fail(result.source, "DIAGNOSTIC_LIMIT",
         "animation preview diagnostic output exceeds its limit");
  }
  reserve_for_append(
      result.diagnostics, limits.max_diagnostics, aggregate_bytes, limits,
      result.source, "DIAGNOSTIC_LIMIT",
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
};

struct PendingTransform {
  formats::Kn5Node *node = nullptr;
  formats::Kn5Matrix4 matrix{};
};

AnimationPreviewPose sample_pose(const formats::KsAnimation &animation,
                                 float position,
                                 const AnimationPreviewLimits &limits,
                                 std::size_t &aggregate_bytes) {
  AnimationPreviewPose result;
  if (!std::isfinite(position)) {
    fail(animation.source, "NON_FINITE_POSITION",
         "animation preview position must be finite");
  }
  result.position = std::clamp(position, 0.0F, 1.0F);
  result.tracks = animation.tracks.size();
  if (animation.tracks.size() > limits.max_tracks) {
    fail(animation.source, "TRACK_LIMIT",
         "animation preview track count exceeds its limit");
  }

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
      selected.push_back({track.name, &track});
    } else {
      selected[found->second].track = &track;
      add_diagnostic(result, limits, aggregate_bytes,
                     AnimationPreviewDiagnosticSeverity::warning,
                     "DUPLICATE_ANIMATED_TRACK",
                     "later animated duplicate track wins");
    }
  }

  std::size_t frame_count = 0U;
  for (const auto &item : selected) {
    if (item.track->frames.empty()) {
      fail(animation.source, "EMPTY_ANIMATED_TRACK",
           "animated KSANIM track contains no frames");
    }
    if (frame_count == 0U) {
      frame_count = item.track->frames.size();
    } else if (item.track->frames.size() != frame_count) {
      fail(animation.source, "INCONSISTENT_FRAME_COUNT",
           "animated KSANIM tracks must have one shared frame count");
    }
  }

  charge(selected.size(), sizeof(AnimationPreviewTrackPose), aggregate_bytes,
         limits, animation.source,
         "animation preview pose table exceeds its aggregate budget");
  result.transforms.reserve(selected.size());
  for (auto &item : selected) {
    const auto sampled =
        formats::sampleKsAnimationTrack(*item.track, result.position);
    if (!sampled.has_value())
      fail(animation.source, "EMPTY_ANIMATED_TRACK",
           "animated KSANIM track contains no frames");
    if (!finite_matrix(*sampled)) {
      fail(animation.source, "NON_FINITE_TRANSFORM",
           "sampled animation transform is not finite");
    }
    result.transforms.push_back({std::move(item.name), *sampled});
  }
  return result;
}

} // namespace

bool AnimationPreviewPose::has_errors() const noexcept {
  return std::any_of(
      diagnostics.begin(), diagnostics.end(), [](const auto &item) {
        return item.severity == AnimationPreviewDiagnosticSeverity::error;
      });
}

bool AnimationPreviewApplication::has_errors() const noexcept {
  return std::any_of(
      diagnostics.begin(), diagnostics.end(), [](const auto &item) {
        return item.severity == AnimationPreviewDiagnosticSeverity::error;
      });
}

AnimationPreviewPose
sample_animation_preview_pose(const formats::KsAnimation &animation,
                              float position, AnimationPreviewLimits limits) {
  std::size_t aggregate_bytes = 0U;
  return sample_pose(animation, position, limits, aggregate_bytes);
}

AnimationPreviewApplication
apply_animation_preview(formats::Kn5File &model,
                        const formats::KsAnimation &animation, float position,
                        AnimationPreviewLimits limits) {
  std::size_t aggregate_bytes = 0U;
  AnimationPreviewPose pose =
      sample_pose(animation, position, limits, aggregate_bytes);
  AnimationPreviewApplication result;
  result.source = std::move(pose.source);
  result.position = pose.position;
  result.tracks = pose.tracks;
  result.animated_tracks = pose.animated_tracks;
  result.unique_animated_tracks = pose.transforms.size();
  result.diagnostics = std::move(pose.diagnostics);

  std::map<std::string, std::size_t, std::less<>> by_name;
  for (std::size_t index = 0U; index < pose.transforms.size(); ++index) {
    charge(sizeof(std::pair<std::string, std::size_t>) + 3U * sizeof(void *) +
               pose.transforms[index].name.size(),
           1U, aggregate_bytes, limits, animation.source,
           "animation preview lookup table exceeds its aggregate budget");
    by_name.emplace(pose.transforms[index].name, index);
  }
  charge(pose.transforms.size(), sizeof(std::uint8_t), aggregate_bytes, limits,
         animation.source,
         "animation preview match table exceeds its aggregate budget");
  std::vector<std::uint8_t> matched(pose.transforms.size(), 0U);

  struct StackFrame {
    formats::Kn5Node *node = nullptr;
    std::size_t next_child = 0U;
    std::size_t depth = 0U;
  };
  std::vector<StackFrame> stack;
  if (limits.max_nodes == 0U)
    fail(model.source, "NODE_LIMIT",
         "animation preview hierarchy node count exceeds its limit");
  reserve_for_append(
      stack, limits.max_nodes, aggregate_bytes, limits, animation.source,
      "NODE_LIMIT",
      "animation preview traversal stack exceeds its aggregate budget");
  stack.push_back({&model.root, 0U, 0U});
  std::vector<PendingTransform> pending;
  std::size_t node_count = 0U;
  bool has_skinned_mesh = false;
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
      has_skinned_mesh = has_skinned_mesh || frame.node->kind == "skinnedMesh";
      if (frame.node->kind == "node") {
        const auto found = by_name.find(frame.node->name);
        if (found != by_name.end()) {
          reserve_for_append(
              pending, limits.max_nodes, aggregate_bytes, limits,
              animation.source, "NODE_LIMIT",
              "animation preview transform table exceeds its aggregate budget");
          pending.push_back(
              {frame.node, pose.transforms[found->second].transform});
          matched[found->second] = 1U;
        }
      }
    }
    if (frame.next_child < frame.node->children.size()) {
      auto *child = &frame.node->children[frame.next_child];
      ++frame.next_child;
      const std::size_t child_depth = frame.depth + 1U;
      reserve_for_append(
          stack, limits.max_nodes, aggregate_bytes, limits, animation.source,
          "NODE_LIMIT",
          "animation preview traversal stack exceeds its aggregate budget");
      stack.push_back({child, 0U, child_depth});
    } else {
      stack.pop_back();
    }
  }

  bool unmatched_limit_reported = false;
  for (std::size_t index = 0U; index < pose.transforms.size(); ++index) {
    const auto &track = pose.transforms[index];
    if (matched[index] != 0U) {
      ++result.matched_tracks;
      continue;
    }
    if (result.unmatched_tracks.size() < limits.max_unmatched_tracks) {
      reserve_for_append(
          result.unmatched_tracks, limits.max_unmatched_tracks, aggregate_bytes,
          limits, animation.source, "AGGREGATE_LIMIT",
          "unmatched animation tracks exceed their aggregate budget");
      charge(track.name.size(), 1U, aggregate_bytes, limits, animation.source,
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
  result.skinning_required = has_skinned_mesh && !pending.empty();
  return result;
}

} // namespace apex::domain
