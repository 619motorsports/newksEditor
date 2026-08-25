#include "apex/core/parse_error.hpp"
#include "apex/domain/analog_instruments.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename Function>
void expects_error(Function &&function, std::string_view code,
                   std::string_view source = {}) {
  try {
    function();
  } catch (const apex::core::ParseError &error) {
    require(error.code() == code, "unexpected analog instrument error code");
    if (!source.empty())
      require(error.source() == source, "analog instrument error source");
    return;
  }
  throw std::runtime_error("invalid analog instrument input was accepted");
}

apex::formats::Kn5Matrix4 identity(float x = 0.0F, float y = 0.0F,
                                   float z = 0.0F) {
  return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
          0.0F, 0.0F, 1.0F, 0.0F, x,    y,    z,    1.0F};
}

apex::formats::Kn5Node node(std::string name) {
  apex::formats::Kn5Node result;
  result.name = std::move(name);
  result.kind = "node";
  result.transform = identity();
  return result;
}

void parses_linear_rpm_and_rejects_malformed_input() {
  const auto parsed = apex::domain::parse_analog_instruments(
      "[RPM_INDICATOR]\nOBJECT_NAME=ARROW_RPM\nZERO=3\n"
      "MIN_VALUE=1000\nSTEP=0.02766667\n",
      "data/analog_instruments.ini");
  require(parsed.rpm.has_value(), "linear RPM configuration");
  require(parsed.rpm->object_name == "ARROW_RPM" &&
              parsed.rpm->zero_degrees == 3.0 &&
              parsed.rpm->minimum_value == 1000.0 &&
              parsed.rpm->step_degrees == 0.02766667 &&
              parsed.rpm->preview_supported,
          "linear RPM fields");

  const auto malformed = apex::domain::parse_analog_instruments(
      "[RPM_INDICATOR]\nOBJECT_NAME=\nZERO=nan\nMIN_VALUE=\nSTEP=Infinity\n",
      "malformed.ini");
  require(!malformed.rpm.has_value() && malformed.diagnostics.size() == 4U,
          "malformed RPM configuration has bounded diagnostics");
  require(malformed.diagnostics[0].source == "malformed.ini" &&
              malformed.diagnostics[0].line == 1U &&
              malformed.diagnostics[1].source == "malformed.ini" &&
              malformed.diagnostics[1].line == 3U,
          "malformed field source attribution");

  expects_error(
      [] {
        (void)apex::domain::parse_analog_instruments(
            "[RPM_INDICATOR]\nOBJECT_NAME=ARROW\\", "truncated.ini");
      },
      "TRUNCATED_CONTINUATION", "truncated.ini");

  auto limits = apex::domain::AnalogInstrumentLimits{};
  limits.max_object_name_bytes = 3U;
  const auto oversized = apex::domain::parse_analog_instruments(
      "[RPM_INDICATOR]\nOBJECT_NAME=LONG\nZERO=0\nMIN_VALUE=0\nSTEP=1\n",
      "oversized.ini", limits);
  require(!oversized.rpm.has_value() && !oversized.diagnostics.empty() &&
              oversized.diagnostics.front().code == "OBJECT_NAME_LIMIT",
          "object-name allocation boundary");

  auto input_limits = apex::domain::AnalogInstrumentLimits{};
  input_limits.max_input_bytes = 8U;
  expects_error(
      [&] {
        (void)apex::domain::parse_analog_instruments("[RPM_INDICATOR]\n",
                                                     "large.ini", input_limits);
      },
      "INPUT_LIMIT", "large.ini");
}

void keeps_lut_explicit_and_uses_the_first_duplicate_section() {
  const auto parsed = apex::domain::parse_analog_instruments(
      "[rpm_indicator]\nOBJECT_NAME=FIRST\nZERO=0\nMIN_VALUE=0\nSTEP=.03\n"
      "LUT=(0=0|1000=30)\n"
      "[RPM_INDICATOR]\nOBJECT_NAME=SECOND\nZERO=1\nMIN_VALUE=2\nSTEP=3\n");
  require(parsed.rpm.has_value() && parsed.rpm->object_name == "FIRST" &&
              !parsed.rpm->preview_supported,
          "first duplicate and unsupported LUT");
  require(parsed.diagnostics.size() == 2U &&
              parsed.diagnostics[0].code == "DUPLICATE_SECTION" &&
              parsed.diagnostics[0].source == "data/analog_instruments.ini" &&
              parsed.diagnostics[0].line == 1U &&
              parsed.diagnostics[1].code == "UNSUPPORTED_LUT" &&
              parsed.diagnostics[1].source == "data/analog_instruments.ini" &&
              parsed.diagnostics[1].line == 1U,
          "duplicate and LUT diagnostics");
}

void reproduces_the_recovered_angle_and_matrix_order() {
  apex::domain::AnalogRpmConfig config;
  config.source = "analog.ini";
  config.line = 1U;
  config.object_name = "ARROW_RPM";
  config.zero_degrees = 3.0;
  config.minimum_value = 1000.0;
  config.step_degrees = 0.02766667;

  const double angle = apex::domain::analog_rpm_angle(config, 6000.0);
  require(std::abs(angle - 141.33335 * std::numbers::pi / 180.0) < 1.0e-12,
          "recovered unclamped angle");

  const auto transformed = apex::domain::analog_rpm_transform(
      identity(12.0F, 13.0F, 14.0F), config, 1000.0);
  const float cosine =
      static_cast<float>(std::cos(3.0 * std::numbers::pi / 180.0));
  const float sine =
      static_cast<float>(std::sin(3.0 * std::numbers::pi / 180.0));
  require(std::abs(transformed[0] - cosine) < 1.0e-6F &&
              std::abs(transformed[1] - sine) < 1.0e-6F &&
              std::abs(transformed[4] + sine) < 1.0e-6F &&
              std::abs(transformed[5] - cosine) < 1.0e-6F,
          "local positive-Z rotation");
  require(transformed[12] == 12.0F && transformed[13] == 13.0F &&
              transformed[14] == 14.0F,
          "original matrix multiplies rotation on the left");

  expects_error(
      [&] {
        (void)apex::domain::analog_rpm_angle(
            config, std::numeric_limits<double>::infinity());
      },
      "NON_FINITE_VALUE");
}

void binds_every_exact_match_and_mutates_atomically() {
  apex::formats::Kn5File model;
  model.source = "car.kn5";
  model.root = node("ROOT");
  model.root.children.push_back(node("ARROW_RPM"));
  model.root.children.push_back(node("arrow_rpm"));
  auto group = node("GROUP");
  group.children.push_back(node("ARROW_RPM"));
  model.root.children.push_back(std::move(group));

  apex::domain::AnalogRpmConfig config;
  config.source = "analog.ini";
  config.line = 1U;
  config.object_name = "ARROW_RPM";
  config.zero_degrees = 90.0;
  config.minimum_value = 0.0;
  config.step_degrees = 0.0;
  const auto applied = apex::domain::apply_analog_rpm(model, &config, 9000.0);
  require(applied.status ==
                  apex::domain::AnalogRpmBindingStatus::resolved_multiple &&
              applied.matches == 2U && applied.applied_nodes == 2U,
          "all exact-name matches");
  require(std::abs(model.root.children[0].transform[1] - 1.0F) < 1.0e-6F &&
              model.root.children[1].transform == identity() &&
              std::abs(model.root.children[2].children[0].transform[1] - 1.0F) <
                  1.0e-6F,
          "only exact matches change");

  auto limited = model;
  const auto before = limited.root.children[0].transform;
  auto limits = apex::domain::AnalogInstrumentLimits{};
  limits.max_nodes = 2U;
  expects_error(
      [&] {
        (void)apex::domain::apply_analog_rpm(limited, &config, 1000.0, limits);
      },
      "NODE_LIMIT");
  require(limited.root.children[0].transform == before,
          "failed hierarchy validation exposes no partial transform");

  apex::formats::Kn5File partial_model;
  partial_model.source = "partial.kn5";
  partial_model.root = node("ROOT");
  partial_model.root.children.push_back(node("ARROW_RPM"));
  partial_model.root.children.push_back(node("ARROW_RPM"));
  partial_model.root.children[1].transform[0] =
      std::numeric_limits<float>::quiet_NaN();
  const auto partial =
      apex::domain::apply_analog_rpm(partial_model, &config, 1000.0);
  require(partial.status == apex::domain::AnalogRpmBindingStatus::partial &&
              partial.matches == 2U && partial.usable_matches == 1U &&
              partial.applied_nodes == 1U,
          "non-finite exact-name matches remain explicit");

  apex::formats::Kn5File overflow_model;
  overflow_model.source = "overflow.kn5";
  overflow_model.root = node("ROOT");
  overflow_model.root.children.push_back(node("ARROW_RPM"));
  overflow_model.root.children.push_back(node("ARROW_RPM"));
  overflow_model.root.children[1].transform.fill(
      std::numeric_limits<float>::max());
  auto overflow_config = config;
  overflow_config.zero_degrees = 45.0;
  const auto unchanged = overflow_model.root.children[0].transform;
  expects_error(
      [&] {
        (void)apex::domain::apply_analog_rpm(overflow_model, &overflow_config,
                                             1000.0);
      },
      "NON_FINITE_TRANSFORM");
  require(overflow_model.root.children[0].transform == unchanged,
          "failed matrix staging exposes no partial transform");

  config.preview_supported = false;
  const auto unsupported =
      apex::domain::apply_analog_rpm(model, &config, 1000.0);
  require(unsupported.status ==
                  apex::domain::AnalogRpmBindingStatus::unsupported &&
              unsupported.applied_nodes == 0U,
          "LUT path remains unsupported");

  auto empty_name = config;
  empty_name.preview_supported = true;
  empty_name.object_name.clear();
  expects_error(
      [&] { (void)apex::domain::apply_analog_rpm(model, &empty_name, 1000.0); },
      "MISSING_OBJECT_NAME");
}

} // namespace

int main() {
  try {
    parses_linear_rpm_and_rejects_malformed_input();
    keeps_lut_explicit_and_uses_the_first_duplicate_section();
    reproduces_the_recovered_angle_and_matrix_order();
    binds_every_exact_match_and_mutates_atomically();
    std::cout << "analog instrument tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "analog instrument tests failed: " << error.what() << '\n';
    return 1;
  }
}
