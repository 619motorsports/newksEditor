#pragma once

#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::domain {

struct AnalogInstrumentLimits {
  formats::IniParseLimits ini{};
  std::size_t max_input_bytes = 4U * 1024U * 1024U;
  std::size_t max_sections = 100'000U;
  std::size_t max_entries = 1'000'000U;
  std::size_t max_object_name_bytes = 1'024U;
  std::size_t max_nodes = 2'000'000U;
  std::size_t max_depth = 1'024U;
  std::size_t max_diagnostics = 100'000U;
};

enum class AnalogInstrumentDiagnosticSeverity { warning, error };

struct AnalogInstrumentDiagnostic {
  AnalogInstrumentDiagnosticSeverity severity =
      AnalogInstrumentDiagnosticSeverity::warning;
  std::string code;
  std::string message;
  std::string source;
  std::size_t line = 0U;
};

struct AnalogRpmConfig {
  std::string source;
  std::size_t line = 0U;
  std::string object_name;
  double zero_degrees = 0.0;
  double minimum_value = 0.0;
  double step_degrees = 0.0;
  std::string lut;
  bool preview_supported = true;
};

struct AnalogInstrumentConfig {
  std::string source;
  std::optional<AnalogRpmConfig> rpm;
  std::vector<AnalogInstrumentDiagnostic> diagnostics;

  [[nodiscard]] bool has_errors() const noexcept;
};

/** Parse the first RPM_INDICATOR section. Later duplicate sections are
 * diagnosed. */
[[nodiscard]] AnalogInstrumentConfig
parse_analog_instruments(const formats::IniDocument &document,
                         AnalogInstrumentLimits limits = {});
[[nodiscard]] AnalogInstrumentConfig
parse_analog_instruments(std::string_view text,
                         std::string source = "data/analog_instruments.ini",
                         AnalogInstrumentLimits limits = {});

/**
 * Return the recovered ksEditor angle without input clamping:
 * ZERO + (rpm - MIN_VALUE) * STEP, converted from degrees to radians.
 */
[[nodiscard]] double analog_rpm_angle(const AnalogRpmConfig &config,
                                      double rpm);
[[nodiscard]] formats::Kn5Matrix4
analog_rpm_rotation(const AnalogRpmConfig &config, double rpm);
[[nodiscard]] formats::Kn5Matrix4
analog_rpm_transform(const formats::Kn5Matrix4 &original,
                     const AnalogRpmConfig &config, double rpm);

enum class AnalogRpmBindingStatus {
  unconfigured,
  unsupported,
  missing,
  no_transform,
  partial,
  resolved,
  resolved_multiple,
};

struct AnalogRpmApplication {
  AnalogRpmBindingStatus status = AnalogRpmBindingStatus::unconfigured;
  std::string object_name;
  double rpm = 0.0;
  double angle_degrees = 0.0;
  std::size_t matches = 0U;
  std::size_t usable_matches = 0U;
  std::size_t applied_nodes = 0U;
};

/**
 * Apply the RPM transform to every exact-name match. The operation validates
 * the complete hierarchy before it changes a transform. Callers must start
 * each preview update from the immutable source transforms.
 */
[[nodiscard]] AnalogRpmApplication
apply_analog_rpm(formats::Kn5File &model, const AnalogRpmConfig *config,
                 double rpm, AnalogInstrumentLimits limits = {});

[[nodiscard]] const char *
analog_rpm_binding_status_name(AnalogRpmBindingStatus status) noexcept;

} // namespace apex::domain
