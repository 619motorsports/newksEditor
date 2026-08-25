#include "apex/domain/analog_instruments.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace apex::domain {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  std::size_t first = 0U;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
          value[first] == '\n' || value[first] == '\f')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1U] == ' ' || value[last - 1U] == '\t' ||
          value[last - 1U] == '\r' || value[last - 1U] == '\n' ||
          value[last - 1U] == '\f')) {
    --last;
  }
  return value.substr(first, last - first);
}

[[nodiscard]] std::string upper_ascii(std::string_view value) {
  std::string result(value);
  for (char &character : result) {
    if (character >= 'a' && character <= 'z')
      character = static_cast<char>(character - ('a' - 'A'));
  }
  return result;
}

[[noreturn]] void fail(std::string_view source, std::size_t line,
                       std::string_view code, std::string_view message) {
  throw core::ParseError("ANALOG_INSTRUMENTS", std::string(source), line,
                         std::string(code), std::string(message));
}

void add_diagnostic(AnalogInstrumentConfig &result,
                    const AnalogInstrumentLimits &limits,
                    AnalogInstrumentDiagnosticSeverity severity,
                    std::string_view code, std::string_view message,
                    std::string_view source, std::size_t line) {
  if (result.diagnostics.size() >= limits.max_diagnostics) {
    fail(source, line, "DIAGNOSTIC_LIMIT",
         "analog instrument diagnostic output exceeds its limit");
  }
  result.diagnostics.push_back({severity, std::string(code),
                                std::string(message), std::string(source),
                                line});
}

[[nodiscard]] std::optional<double>
finite_field(const formats::IniSection &section, std::string_view key,
             AnalogInstrumentConfig &result,
             const AnalogInstrumentLimits &limits, std::string_view source) {
  const auto *entry = section.last_entry(key);
  if (entry == nullptr || trim(entry->value).empty()) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "MISSING_FIELD",
                   std::string("RPM_INDICATOR ") + std::string(key) +
                       " is missing",
                   source, section.line);
    return std::nullopt;
  }
  const auto value = formats::parse_csp_value(entry->value);
  const auto *number = value.number_value();
  if (number == nullptr || !std::isfinite(*number)) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "NON_FINITE_FIELD",
                   std::string("RPM_INDICATOR ") + std::string(key) +
                       " must be finite",
                   source, entry->line);
    return std::nullopt;
  }
  return *number;
}

[[nodiscard]] bool finite_matrix(const formats::Kn5Matrix4 &value) noexcept {
  return std::all_of(value.begin(), value.end(),
                     [](float component) { return std::isfinite(component); });
}

[[nodiscard]] formats::Kn5Matrix4 multiply(const formats::Kn5Matrix4 &left,
                                           const formats::Kn5Matrix4 &right) {
  formats::Kn5Matrix4 output{};
  for (std::size_t column = 0U; column < 4U; ++column) {
    for (std::size_t row = 0U; row < 4U; ++row) {
      double value = 0.0;
      for (std::size_t index = 0U; index < 4U; ++index) {
        value += static_cast<double>(left[index * 4U + row]) *
                 static_cast<double>(right[column * 4U + index]);
      }
      if (!std::isfinite(value) ||
          value < -static_cast<double>(std::numeric_limits<float>::max()) ||
          value > static_cast<double>(std::numeric_limits<float>::max())) {
        fail("analog RPM", 0U, "NON_FINITE_TRANSFORM",
             "analog RPM matrix multiplication produced a non-finite value");
      }
      output[column * 4U + row] = static_cast<float>(value);
    }
  }
  return output;
}

struct PendingTransform {
  formats::Kn5Node *node = nullptr;
  formats::Kn5Matrix4 transform{};
};

} // namespace

bool AnalogInstrumentConfig::has_errors() const noexcept {
  return std::any_of(
      diagnostics.begin(), diagnostics.end(), [](const auto &item) {
        return item.severity == AnalogInstrumentDiagnosticSeverity::error;
      });
}

AnalogInstrumentConfig
parse_analog_instruments(const formats::IniDocument &document,
                         AnalogInstrumentLimits limits) {
  if (document.sections.size() > limits.max_sections)
    fail(document.source, 0U, "SECTION_LIMIT",
         "analog instrument section count exceeds its limit");

  std::size_t entries = 0U;
  for (const auto &section : document.sections) {
    if (section.entries.size() >
        limits.max_entries - std::min(entries, limits.max_entries))
      fail(section.source.empty() ? document.source : section.source,
           section.line, "ENTRY_LIMIT",
           "analog instrument entry count exceeds its limit");
    entries += section.entries.size();
  }

  AnalogInstrumentConfig result;
  result.source = document.source;
  for (const auto &warning : document.warnings) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "INI_WARNING", warning.message, warning.source,
                   warning.line);
  }

  const formats::IniSection *selected = nullptr;
  std::size_t rpm_sections = 0U;
  for (const auto &section : document.sections) {
    if (upper_ascii(section.name) != "RPM_INDICATOR")
      continue;
    ++rpm_sections;
    if (selected == nullptr)
      selected = &section;
  }
  if (selected == nullptr)
    return result;

  const auto source = selected->source.empty()
                          ? std::string_view(document.source)
                          : std::string_view(selected->source);
  if (rpm_sections > 1U) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "DUPLICATE_SECTION",
                   "multiple RPM_INDICATOR sections are present; the first "
                   "section is used",
                   source, selected->line);
  }

  AnalogRpmConfig rpm;
  rpm.source = std::string(source);
  rpm.line = selected->line;
  rpm.object_name = std::string(trim(selected->last_value("OBJECT_NAME")));
  rpm.lut = std::string(trim(selected->last_value("LUT")));
  bool valid = true;
  if (rpm.object_name.empty()) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "MISSING_OBJECT_NAME",
                   "RPM_INDICATOR OBJECT_NAME is missing", source,
                   selected->line);
    valid = false;
  } else if (rpm.object_name.size() > limits.max_object_name_bytes) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "OBJECT_NAME_LIMIT",
                   "RPM_INDICATOR OBJECT_NAME exceeds its limit", source,
                   selected->line);
    valid = false;
  }

  const auto zero = finite_field(*selected, "ZERO", result, limits, source);
  const auto minimum =
      finite_field(*selected, "MIN_VALUE", result, limits, source);
  const auto step = finite_field(*selected, "STEP", result, limits, source);
  valid = valid && zero.has_value() && minimum.has_value() && step.has_value();
  if (!rpm.lut.empty()) {
    add_diagnostic(result, limits, AnalogInstrumentDiagnosticSeverity::warning,
                   "UNSUPPORTED_LUT",
                   "RPM_INDICATOR LUT preview is not supported", source,
                   selected->line);
    rpm.preview_supported = false;
  }
  if (!valid)
    return result;
  rpm.zero_degrees = *zero;
  rpm.minimum_value = *minimum;
  rpm.step_degrees = *step;
  result.rpm = std::move(rpm);
  return result;
}

AnalogInstrumentConfig parse_analog_instruments(std::string_view text,
                                                std::string source,
                                                AnalogInstrumentLimits limits) {
  if (text.size() > limits.max_input_bytes)
    fail(source, 0U, "INPUT_LIMIT",
         "analog instrument configuration exceeds its input limit");
  return parse_analog_instruments(
      formats::parse_ini(text, std::move(source), limits.ini), limits);
}

double analog_rpm_angle(const AnalogRpmConfig &config, double rpm) {
  if (!std::isfinite(rpm) || !std::isfinite(config.zero_degrees) ||
      !std::isfinite(config.minimum_value) ||
      !std::isfinite(config.step_degrees)) {
    fail(config.source, config.line, "NON_FINITE_VALUE",
         "analog RPM configuration and input must be finite");
  }
  const double degrees =
      config.zero_degrees + (rpm - config.minimum_value) * config.step_degrees;
  const double radians = degrees * std::numbers::pi / 180.0;
  if (!std::isfinite(degrees) || !std::isfinite(radians))
    fail(config.source, config.line, "NON_FINITE_VALUE",
         "analog RPM angle exceeds the finite numeric range");
  return radians;
}

formats::Kn5Matrix4 analog_rpm_rotation(const AnalogRpmConfig &config,
                                        double rpm) {
  const double angle = analog_rpm_angle(config, rpm);
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  return {static_cast<float>(cosine),
          static_cast<float>(sine),
          0.0F,
          0.0F,
          static_cast<float>(-sine),
          static_cast<float>(cosine),
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          1.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          1.0F};
}

formats::Kn5Matrix4 analog_rpm_transform(const formats::Kn5Matrix4 &original,
                                         const AnalogRpmConfig &config,
                                         double rpm) {
  if (!finite_matrix(original))
    fail(config.source, config.line, "NON_FINITE_TRANSFORM",
         "analog RPM source transform must be finite");
  return multiply(original, analog_rpm_rotation(config, rpm));
}

AnalogRpmApplication apply_analog_rpm(formats::Kn5File &model,
                                      const AnalogRpmConfig *config, double rpm,
                                      AnalogInstrumentLimits limits) {
  AnalogRpmApplication result;
  result.rpm = rpm;
  if (config == nullptr)
    return result;
  result.object_name = config->object_name;
  if (config->object_name.empty())
    fail(config->source, config->line, "MISSING_OBJECT_NAME",
         "analog RPM object name is empty");
  if (config->object_name.size() > limits.max_object_name_bytes)
    fail(config->source, config->line, "OBJECT_NAME_LIMIT",
         "analog RPM object name exceeds its limit");
  if (!config->preview_supported) {
    result.status = AnalogRpmBindingStatus::unsupported;
    return result;
  }
  result.angle_degrees = config->zero_degrees +
                         (rpm - config->minimum_value) * config->step_degrees;
  (void)analog_rpm_angle(*config, rpm);

  struct StackFrame {
    formats::Kn5Node *node;
    std::size_t next_child;
    std::size_t depth;
  };
  std::vector<StackFrame> stack;
  stack.push_back({&model.root, 0U, 0U});
  std::vector<PendingTransform> pending;
  std::size_t node_count = 0U;
  while (!stack.empty()) {
    auto &frame = stack.back();
    if (frame.next_child == 0U) {
      if (frame.depth > limits.max_depth)
        fail(model.source, 0U, "DEPTH_LIMIT",
             "analog RPM hierarchy depth exceeds its limit");
      if (node_count >= limits.max_nodes)
        fail(model.source, 0U, "NODE_LIMIT",
             "analog RPM hierarchy node count exceeds its limit");
      ++node_count;
      if (frame.node->name == config->object_name) {
        ++result.matches;
        if (finite_matrix(frame.node->transform)) {
          ++result.usable_matches;
          pending.push_back(
              {frame.node,
               analog_rpm_transform(frame.node->transform, *config, rpm)});
        }
      }
    }
    if (frame.next_child < frame.node->children.size()) {
      auto *child = &frame.node->children[frame.next_child];
      ++frame.next_child;
      stack.push_back({child, 0U, frame.depth + 1U});
    } else {
      stack.pop_back();
    }
  }

  if (result.matches == 0U)
    result.status = AnalogRpmBindingStatus::missing;
  else if (result.usable_matches == 0U)
    result.status = AnalogRpmBindingStatus::no_transform;
  else if (result.usable_matches < result.matches)
    result.status = AnalogRpmBindingStatus::partial;
  else if (result.matches > 1U)
    result.status = AnalogRpmBindingStatus::resolved_multiple;
  else
    result.status = AnalogRpmBindingStatus::resolved;

  for (const auto &item : pending)
    item.node->transform = item.transform;
  result.applied_nodes = pending.size();
  return result;
}

const char *
analog_rpm_binding_status_name(AnalogRpmBindingStatus status) noexcept {
  switch (status) {
  case AnalogRpmBindingStatus::unconfigured:
    return "unconfigured";
  case AnalogRpmBindingStatus::unsupported:
    return "unsupported";
  case AnalogRpmBindingStatus::missing:
    return "missing";
  case AnalogRpmBindingStatus::no_transform:
    return "no-transform";
  case AnalogRpmBindingStatus::partial:
    return "partial";
  case AnalogRpmBindingStatus::resolved:
    return "resolved";
  case AnalogRpmBindingStatus::resolved_multiple:
    return "resolved-multiple";
  }
  return "unconfigured";
}

} // namespace apex::domain
