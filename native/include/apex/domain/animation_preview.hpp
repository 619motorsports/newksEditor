#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/formats/ksanim.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace apex::domain {

struct AnimationPreviewLimits {
  std::size_t max_tracks = 1'000'000U;
  std::size_t max_track_name_bytes = 1U * 1024U * 1024U;
  std::size_t max_nodes = 2'000'000U;
  std::size_t max_depth = 1'024U;
  std::size_t max_unmatched_tracks = 100'000U;
  std::size_t max_diagnostics = 100'000U;
  std::size_t max_aggregate_bytes = 64U * 1024U * 1024U;
};

enum class AnimationPreviewDiagnosticSeverity { warning, error };

struct AnimationPreviewDiagnostic {
  AnimationPreviewDiagnosticSeverity severity =
      AnimationPreviewDiagnosticSeverity::warning;
  std::string code;
  std::string message;
  std::string source;
};

struct AnimationPreviewTrackPose {
  std::string name;
  formats::Kn5Matrix4 transform{};
};

struct AnimationPreviewPose {
  std::string source;
  float position = 0.0F;
  std::size_t tracks = 0U;
  std::size_t animated_tracks = 0U;
  std::vector<AnimationPreviewTrackPose> transforms;
  std::vector<AnimationPreviewDiagnostic> diagnostics;

  [[nodiscard]] bool has_errors() const noexcept;
};

struct AnimationPreviewApplication {
  std::string source;
  float position = 0.0F;
  std::size_t tracks = 0U;
  std::size_t animated_tracks = 0U;
  std::size_t unique_animated_tracks = 0U;
  std::size_t matched_tracks = 0U;
  std::size_t matched_nodes = 0U;
  // True when an animated node affects a model that contains skinned
  // geometry. The frame must enable CPU skinning to show this pose.
  bool skinning_required = false;
  std::vector<std::string> unmatched_tracks;
  std::vector<AnimationPreviewDiagnostic> diagnostics;

  [[nodiscard]] bool has_errors() const noexcept;
};

/**
 * Sample the fixed normalized animation position without changing a model.
 *
 * The position is clamped to [0, 1]. Later animated duplicate tracks replace
 * earlier tracks. The returned track table uses first-seen name order.
 */
[[nodiscard]] AnimationPreviewPose
sample_animation_preview_pose(const formats::KsAnimation &animation,
                              float position,
                              AnimationPreviewLimits limits = {});

/**
 * Apply sampled KSANIM matrices to exact-name KN5 null nodes.
 *
 * Later animated duplicate tracks replace earlier tracks. Mesh records do not
 * receive transforms. The operation stages all matrices before it changes the
 * model, so an error exposes no partial preview.
 */
[[nodiscard]] AnimationPreviewApplication
apply_animation_preview(formats::Kn5File &model,
                        const formats::KsAnimation &animation, float position,
                        AnimationPreviewLimits limits = {});

} // namespace apex::domain
