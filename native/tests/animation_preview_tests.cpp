#include "apex/core/parse_error.hpp"
#include "apex/domain/analog_instruments.hpp"
#include "apex/domain/animation_preview.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename Function>
void expects_error(Function &&function, std::string_view code) {
  try {
    function();
  } catch (const apex::core::ParseError &error) {
    if (error.code() != code) {
      throw std::runtime_error("expected animation preview error " +
                               std::string(code) + ", received " +
                               error.code());
    }
    return;
  }
  throw std::runtime_error("invalid animation preview input was accepted");
}

apex::formats::Kn5Matrix4 identity() {
  return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
          0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

apex::formats::Kn5Node node(std::string name, std::string kind = "node") {
  apex::formats::Kn5Node result;
  result.name = std::move(name);
  result.kind = std::move(kind);
  result.transform = identity();
  return result;
}

apex::formats::KsAnimationFrame frame(float x) {
  return {{0.0F, 0.0F, 0.0F, 1.0F}, {x, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
}

apex::formats::KsAnimationTrack track(std::string name, float end,
                                      bool animated = true) {
  apex::formats::KsAnimationTrack result;
  result.name = std::move(name);
  result.frames = {frame(0.0F), frame(end)};
  result.animated = animated;
  return result;
}

void applies_exact_null_node_tracks_with_last_precedence() {
  apex::formats::Kn5File model;
  model.source = "car.kn5";
  model.root = node("ROOT");
  model.root.children.push_back(node("DOOR"));
  model.root.children.push_back(node("DOOR", "mesh"));
  model.root.children.push_back(node("door"));
  auto group = node("GROUP");
  group.children.push_back(node("DOOR"));
  model.root.children.push_back(std::move(group));

  apex::formats::KsAnimation animation;
  animation.source = "door.ksanim";
  animation.tracks.push_back(track("DOOR", 10.0F));
  animation.tracks.push_back(track("STATIC", 100.0F, false));
  animation.tracks.push_back(track("DOOR", 20.0F));
  animation.tracks.push_back(track("MISSING", 30.0F));

  const auto result =
      apex::domain::apply_animation_preview(model, animation, 0.25F);
  require(result.tracks == 4U && result.animated_tracks == 3U &&
              result.unique_animated_tracks == 2U &&
              result.matched_tracks == 1U && result.matched_nodes == 2U,
          "animation application counts");
  require(result.unmatched_tracks == std::vector<std::string>{"MISSING"},
          "unmatched source order");
  require(result.diagnostics.size() == 1U &&
              result.diagnostics[0].code == "DUPLICATE_ANIMATED_TRACK" &&
              result.diagnostics[0].source == "door.ksanim",
          "duplicate animated-track diagnostic");
  require(std::abs(model.root.children[0].transform[12] - 10.0F) < 1.0e-6F &&
              model.root.children[1].transform == identity() &&
              model.root.children[2].transform == identity() &&
              std::abs(model.root.children[3].children[0].transform[12] -
                       10.0F) < 1.0e-6F,
          "exact null-node binding and last-track precedence");
}

void samples_a_bounded_fixed_position_pose() {
  apex::formats::KsAnimation animation;
  animation.source = "door.ksanim";
  animation.tracks.push_back(track("DOOR", 10.0F));
  animation.tracks.push_back(track("STATIC", 100.0F, false));
  animation.tracks.push_back(track("DOOR", 20.0F));

  const auto middle =
      apex::domain::sample_animation_preview_pose(animation, 0.25F);
  require(middle.position == 0.25F && middle.tracks == 3U &&
              middle.animated_tracks == 2U && middle.transforms.size() == 1U &&
              middle.transforms[0].name == "DOOR" &&
              std::abs(middle.transforms[0].transform[12] - 10.0F) < 1.0e-6F,
          "fixed-position pose uses last duplicate track");
  require(middle.diagnostics.size() == 1U &&
              middle.diagnostics[0].code == "DUPLICATE_ANIMATED_TRACK" &&
              middle.diagnostics[0].source == "door.ksanim" &&
              !middle.has_errors(),
          "fixed-position pose retains duplicate diagnostic");

  const auto before =
      apex::domain::sample_animation_preview_pose(animation, -2.0F);
  const auto after =
      apex::domain::sample_animation_preview_pose(animation, 2.0F);
  require(
      before.position == 0.0F && before.transforms[0].transform[12] == 0.0F &&
          after.position == 1.0F && after.transforms[0].transform[12] == 20.0F,
      "fixed-position pose clamps both endpoints");

  apex::formats::KsAnimationTrack boundaries;
  boundaries.name = "BOUNDARY";
  boundaries.animated = true;
  boundaries.frames = {frame(0.0F), frame(10.0F), frame(20.0F)};
  animation.tracks = {boundaries};
  const auto first =
      apex::domain::sample_animation_preview_pose(animation, 0.0F);
  const auto second =
      apex::domain::sample_animation_preview_pose(animation, 1.0F / 3.0F);
  const auto final_interval =
      apex::domain::sample_animation_preview_pose(animation, 2.0F / 3.0F);
  const auto endpoint =
      apex::domain::sample_animation_preview_pose(animation, 1.0F);
  require(first.transforms[0].transform[12] == 0.0F &&
              second.transforms[0].transform[12] == 10.0F &&
              final_interval.transforms[0].transform[12] == 20.0F &&
              endpoint.transforms[0].transform[12] == 20.0F,
          "fixed-position pose follows recovered frame-count boundaries");

  boundaries.frames = {frame(7.0F)};
  animation.tracks = {boundaries};
  const auto one_frame =
      apex::domain::sample_animation_preview_pose(animation, 0.5F);
  require(one_frame.transforms.size() == 1U &&
              one_frame.transforms[0].transform[12] == 7.0F,
          "bounded one-frame pose selects its only frame");

  animation.tracks[0].frames.clear();
  expects_error(
      [&] {
        (void)apex::domain::sample_animation_preview_pose(animation, 0.0F);
      },
      "EMPTY_ANIMATED_TRACK");
  expects_error(
      [&] {
        (void)apex::domain::sample_animation_preview_pose(
            animation, std::numeric_limits<float>::infinity());
      },
      "NON_FINITE_POSITION");
}

void rejects_limits_and_non_finite_sampling_without_partial_mutation() {
  apex::formats::Kn5File model;
  model.source = "car.kn5";
  model.root = node("ROOT");
  model.root.children.push_back(node("FIRST"));
  model.root.children.push_back(node("OVERFLOW"));

  apex::formats::KsAnimation animation;
  animation.source = "unsafe.ksanim";
  animation.tracks.push_back(track("FIRST", 2.0F));
  auto overflow = track("OVERFLOW", 0.0F);
  overflow.frames[0].position[0] = std::numeric_limits<float>::max();
  overflow.frames[1].position[0] = -std::numeric_limits<float>::max();
  animation.tracks.push_back(std::move(overflow));
  const auto before = model.root.children[0].transform;
  expects_error(
      [&] {
        (void)apex::domain::apply_animation_preview(model, animation, 0.25F);
      },
      "NON_FINITE_TRANSFORM");
  require(model.root.children[0].transform == before,
          "failed sampling exposes no partial transform");

  apex::formats::KsAnimation inconsistent;
  inconsistent.source = "inconsistent.ksanim";
  inconsistent.tracks.push_back(track("FIRST", 2.0F));
  auto three_frames = track("OVERFLOW", 3.0F);
  three_frames.frames.insert(three_frames.frames.begin() + 1U, frame(1.5F));
  inconsistent.tracks.push_back(std::move(three_frames));
  const auto overflow_before = model.root.children[1].transform;
  expects_error(
      [&] {
        (void)apex::domain::apply_animation_preview(model, inconsistent, 0.25F);
      },
      "INCONSISTENT_FRAME_COUNT");
  require(model.root.children[0].transform == before &&
              model.root.children[1].transform == overflow_before,
          "inconsistent track lengths expose no partial transform");

  expects_error(
      [&] {
        (void)apex::domain::apply_animation_preview(
            model, animation, std::numeric_limits<float>::quiet_NaN());
      },
      "NON_FINITE_POSITION");

  auto node_limits = apex::domain::AnimationPreviewLimits{};
  node_limits.max_nodes = 1U;
  auto valid = apex::formats::KsAnimation{};
  valid.source = "valid.ksanim";
  valid.tracks.push_back(track("FIRST", 2.0F));
  expects_error(
      [&] {
        (void)apex::domain::apply_animation_preview(model, valid, 0.0F,
                                                    node_limits);
      },
      "NODE_LIMIT");
  require(model.root.children[0].transform == before,
          "failed hierarchy validation exposes no partial transform");

  auto name_limits = apex::domain::AnimationPreviewLimits{};
  name_limits.max_track_name_bytes = 2U;
  expects_error(
      [&] {
        (void)apex::domain::apply_animation_preview(model, valid, 0.0F,
                                                    name_limits);
      },
      "TRACK_NAME_LIMIT");
}

void bounds_unmatched_output_and_aggregate_work() {
  apex::formats::Kn5File model;
  model.source = "car.kn5";
  model.root = node("ROOT");
  apex::formats::KsAnimation animation;
  animation.source = "missing.ksanim";
  animation.tracks.push_back(track("A", 1.0F));
  animation.tracks.push_back(track("B", 1.0F));

  auto limits = apex::domain::AnimationPreviewLimits{};
  limits.max_unmatched_tracks = 1U;
  const auto result =
      apex::domain::apply_animation_preview(model, animation, 0.0F, limits);
  require(result.unmatched_tracks == std::vector<std::string>{"A"} &&
              result.diagnostics.size() == 1U &&
              result.diagnostics[0].code == "UNMATCHED_OUTPUT_LIMIT",
          "bounded unmatched output");

  limits = {};
  limits.max_aggregate_bytes = 1U;
  expects_error(
      [&] {
        (void)apex::domain::apply_animation_preview(model, animation, 0.0F,
                                                    limits);
      },
      "AGGREGATE_LIMIT");
  expects_error(
      [&] {
        (void)apex::domain::sample_animation_preview_pose(animation, 0.0F,
                                                          limits);
      },
      "AGGREGATE_LIMIT");

  limits = {};
  limits.max_tracks = 0U;
  expects_error(
      [&] {
        (void)apex::domain::sample_animation_preview_pose(animation, 0.0F,
                                                          limits);
      },
      "TRACK_LIMIT");

  apex::formats::KsAnimation duplicate;
  duplicate.source = "duplicate.ksanim";
  duplicate.tracks.push_back(track("A", 1.0F));
  duplicate.tracks.push_back(track("A", 2.0F));
  limits = {};
  limits.max_diagnostics = 0U;
  expects_error(
      [&] {
        (void)apex::domain::sample_animation_preview_pose(duplicate, 0.0F,
                                                          limits);
      },
      "DIAGNOSTIC_LIMIT");

  limits = {};
  limits.max_unmatched_tracks = 0U;
  const auto no_unmatched =
      apex::domain::apply_animation_preview(model, animation, 0.0F, limits);
  require(no_unmatched.unmatched_tracks.empty() &&
              no_unmatched.diagnostics.size() == 1U &&
              no_unmatched.diagnostics[0].code == "UNMATCHED_OUTPUT_LIMIT",
          "zero unmatched-output limit returns one bounded diagnostic");
}

void animation_overrides_the_analog_rpm_transform() {
  apex::formats::Kn5File model;
  model.source = "car.kn5";
  model.root = node("ROOT");
  model.root.children.push_back(node("NEEDLE"));

  apex::domain::AnalogRpmConfig rpm;
  rpm.source = "analog.ini";
  rpm.object_name = "NEEDLE";
  rpm.zero_degrees = 90.0;
  const auto analog = apex::domain::apply_analog_rpm(model, &rpm, 1000.0);
  require(analog.applied_nodes == 1U &&
              std::abs(model.root.children[0].transform[1] - 1.0F) < 1.0e-6F,
          "analog RPM transform applied first");

  apex::formats::KsAnimation animation;
  animation.source = "needle.ksanim";
  animation.tracks.push_back(track("NEEDLE", 4.0F));
  const auto animated =
      apex::domain::apply_animation_preview(model, animation, 1.0F);
  require(animated.matched_nodes == 1U &&
              model.root.children[0].transform[0] == 1.0F &&
              model.root.children[0].transform[1] == 0.0F &&
              model.root.children[0].transform[12] == 4.0F,
          "animation replaces the analog local transform");
}

void reports_when_the_frame_requires_cpu_skinning() {
  apex::formats::Kn5File model;
  model.source = "driver.kn5";
  model.root = node("ROOT");
  model.root.children.push_back(node("BONE"));
  model.root.children.push_back(node("BODY", "skinnedMesh"));

  apex::formats::KsAnimation animation;
  animation.source = "driver.ksanim";
  animation.tracks.push_back(track("BONE", 2.0F));
  const auto animated =
      apex::domain::apply_animation_preview(model, animation, 0.5F);
  require(animated.matched_nodes == 1U && animated.skinning_required,
          "an animated model with skinned geometry requires CPU skinning");

  model.root.children[1].kind = "mesh";
  const auto static_only =
      apex::domain::apply_animation_preview(model, animation, 0.5F);
  require(!static_only.skinning_required,
          "an animated static model does not request CPU skinning");
}

} // namespace

int main() {
  try {
    applies_exact_null_node_tracks_with_last_precedence();
    samples_a_bounded_fixed_position_pose();
    rejects_limits_and_non_finite_sampling_without_partial_mutation();
    bounds_unmatched_output_and_aggregate_work();
    animation_overrides_the_analog_rpm_transform();
    reports_when_the_frame_requires_cpu_skinning();
    std::cout << "animation preview tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "animation preview tests failed: " << error.what() << '\n';
    return 1;
  }
}
