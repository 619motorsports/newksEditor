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
    require(error.code() == code, "unexpected animation preview error code");
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

} // namespace

int main() {
  try {
    applies_exact_null_node_tracks_with_last_precedence();
    rejects_limits_and_non_finite_sampling_without_partial_mutation();
    bounds_unmatched_output_and_aggregate_work();
    animation_overrides_the_analog_rpm_transform();
    std::cout << "animation preview tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "animation preview tests failed: " << error.what() << '\n';
    return 1;
  }
}
