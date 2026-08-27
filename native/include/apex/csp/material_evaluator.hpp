#pragma once

#include "apex/csp/config_model.hpp"
#include "apex/formats/kn5.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace apex::csp {

// This is a declarative projection of CSP material effects. It deliberately
// does not contain shader handles, file handles, or an Inipp/template runtime.
struct MaterialValue {
    enum class Kind { string, scalar, vector };
    Kind kind = Kind::string;
    std::string text;
    double scalar = 0.0;
    std::vector<double> vector;

    [[nodiscard]] static MaterialValue from_string(std::string value);
    [[nodiscard]] static MaterialValue from_scalar(double value);
    [[nodiscard]] static MaterialValue from_vector(std::vector<double> value);
};

struct MaterialPropertyAdjustment {
    std::string name;
    MaterialValue value;
    CspSourceRef provenance;
};

struct MaterialResourceAdjustment {
    std::string slot;
    std::string texture;
    std::string file;
    std::optional<MaterialValue> color;
    CspSourceRef provenance;
    enum class PathStatus { absent, safe, rejected } file_status = PathStatus::absent;
};

struct MaterialEvaluationState {
    std::string shader;
    std::string blend_mode;
    std::string depth_mode;
    std::optional<std::string> cull_mode;
    bool is_transparent = false;
    double layer = 0.0;
    double lod_in = 0.0;
    double lod_out = 0.0;
    bool cast_shadows = false;
};

struct MaterialEvaluationDiagnostic {
    CspDiagnosticSeverity severity = CspDiagnosticSeverity::warning;
    std::string code;
    std::string message;
    CspSourceRef provenance;
};

struct MaterialStateProvenance {
    std::string field;
    std::string value;
    CspSourceRef provenance;
};

struct MaterialEvaluationLimits {
    std::size_t max_sections = 100'000;
    std::size_t max_selector_terms = 100'000;
    std::size_t max_properties = 4'096;
    std::size_t max_resources = 4'096;
    std::size_t max_state_overrides = 4'096;
    std::size_t max_diagnostics = 100'000;
    std::size_t max_input_entries = 1'000'000;
    std::size_t max_input_properties = 100'000;
    std::size_t max_input_resources = 100'000;
    std::size_t max_string_bytes = 1U << 20;
    std::size_t max_texture_scan_per_selector = 100'000;
    std::size_t max_texture_scan_total = 1'000'000;
    // Reserved for safe declarative expansion accounting. No arbitrary
    // built-in/template body is executed by this stage.
    std::size_t max_expansions = 4'096;
};

struct MaterialEvaluationResult {
    MaterialEvaluationState state;
    std::map<std::string, MaterialValue> properties;
    std::map<std::string, MaterialResourceAdjustment> resources;
    std::vector<CspSourceRef> matched_sections;
    std::vector<MaterialStateProvenance> state_provenance;
    std::vector<MaterialPropertyAdjustment> property_provenance;
    std::vector<MaterialResourceAdjustment> resource_provenance;
    std::vector<MaterialEvaluationDiagnostic> diagnostics;
    bool supported = true;
    bool limit_exceeded = false;
};

[[nodiscard]] MaterialEvaluationResult evaluate_csp_material(
    const CspConfigModel& config, const apex::formats::Kn5Node& node,
    const apex::formats::Kn5Material& material, MaterialEvaluationLimits limits = {});

[[nodiscard]] inline MaterialEvaluationResult evaluate_material_adjustments(
    const CspConfigModel& config, const apex::formats::Kn5Node& node,
    const apex::formats::Kn5Material& material, MaterialEvaluationLimits limits = {}) {
    return evaluate_csp_material(config, node, material, limits);
}

}  // namespace apex::csp
