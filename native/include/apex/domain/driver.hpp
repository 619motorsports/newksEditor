#pragma once

#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/formats/knh.hpp"
#include "apex/formats/ksanim.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::domain {

struct DriverDataLimits {
    std::size_t maxSections = 100'000;
    std::size_t maxFields = 1'000'000;
    std::size_t maxNames = 100'000;
    std::size_t maxNameBytes = 1U * 1024U * 1024U;
    std::size_t maxNodes = 2'000'000;
    std::size_t maxDepth = 1'024;
    std::size_t maxDiagnostics = 1'000'000;
};

enum class DriverDiagnosticSeverity { warning, error };

struct DriverDiagnostic {
    DriverDiagnosticSeverity severity = DriverDiagnosticSeverity::warning;
    std::string code;
    std::string message;
    std::string source;
    std::size_t line = 0;
    std::size_t section_index = 0;
    std::size_t entry_index = 0;
};

struct DriverModelConfig {
    std::string name;
    std::array<float, 3> position{};
    std::size_t section_index = 0;
    std::size_t line = 0;
};

struct DriverSteerConfig {
    std::string name;
    float lock = 0.0F;
    std::size_t section_index = 0;
    std::size_t line = 0;
};

struct DriverShiftConfig {
    // CSP/AC driver3d.ini uses a fixed shift.ksanim basename.
    std::string name = "shift.ksanim";
    float blend_time = 0.0F;
    float positive_time = 0.0F;
    float static_time = 0.0F;
    float negative_time = 0.0F;
    float preload_rpm = 0.0F;
    bool invert_shifting_hands = false;
    std::size_t section_index = 0;
    std::size_t line = 0;
};

struct DriverHiddenObject {
    std::string name;
    std::size_t section_index = 0;
    std::size_t line = 0;
};

struct DriverConfig {
    std::string source;
    apex::formats::IniDocument document;
    std::optional<DriverModelConfig> model;
    std::optional<DriverSteerConfig> steer;
    std::optional<DriverShiftConfig> shift;
    std::vector<DriverHiddenObject> hide_objects;
    std::vector<DriverDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

[[nodiscard]] DriverConfig parse_driver_config(
    const apex::formats::IniDocument& document, DriverDataLimits limits = {});
[[nodiscard]] DriverConfig parse_driver_config(
    std::string_view text, std::string source = "data/driver3d.ini",
    DriverDataLimits limits = {});

enum class DriverAssetKind { model_kn5, base_pose_knh, steering_ksanim, shifting_ksanim };

struct DriverAssetRequest {
    DriverAssetKind kind = DriverAssetKind::model_kn5;
    bool accepted = false;
    std::string relative_path;
    std::string source;
    std::size_t line = 0;
    std::string code;
    std::string message;
};

// Asset requests are capability-relative declarations. No filesystem API is
// called here; the desktop boundary resolves accepted requests separately.
[[nodiscard]] DriverAssetRequest request_driver_asset(
    DriverAssetKind kind, std::string_view relative_path,
    std::string source = {}, std::size_t line = 0);

struct DriverAssetCandidate {
    std::string relative_path;
};

enum class DriverAssetSelectionStatus { unconfigured, resolved, ambiguous, missing };

struct DriverAssetSelection {
    DriverAssetSelectionStatus status = DriverAssetSelectionStatus::unconfigured;
    std::string expected_basename;
    std::optional<std::size_t> selected;
    std::vector<std::size_t> matches;
    std::vector<DriverDiagnostic> diagnostics;
};

[[nodiscard]] DriverAssetSelection select_driver_model_asset(
    std::span<const DriverAssetCandidate> candidates, std::string_view model_name,
    DriverDataLimits limits = {});

struct DriverPoseApplication {
    std::size_t applied = 0;
    std::size_t pose_nodes = 0;
    std::vector<std::string> unmatched_pose;
    std::vector<DriverDiagnostic> diagnostics;
};

struct DriverAnimationApplication {
    std::string source;
    std::size_t tracks = 0;
    std::size_t animated_tracks = 0;
    std::size_t matched_tracks = 0;
    std::size_t applied = 0;
    std::vector<std::string> unmatched_tracks;
    std::vector<DriverDiagnostic> diagnostics;
};

[[nodiscard]] DriverPoseApplication apply_driver_base_pose(
    apex::formats::Kn5File& model, const apex::formats::KnhFile& pose,
    DriverDataLimits limits = {});
[[nodiscard]] DriverAnimationApplication apply_driver_animation(
    apex::formats::Kn5File& model, const apex::formats::KsAnimation& animation,
    float position, DriverDataLimits limits = {});

struct DriverHiddenMatch {
    std::string requested_name;
    std::size_t node_count = 0;
    std::size_t mesh_count = 0;
    std::vector<std::string> mesh_names;
};

struct DriverHiddenResolution {
    std::size_t requested = 0;
    std::size_t matched = 0;
    std::size_t mesh_count = 0;
    std::vector<DriverHiddenMatch> matches;
    std::vector<std::string> unmatched;
    std::vector<DriverDiagnostic> diagnostics;
};

// Resolves exact named subtrees and returns only their descendant meshes. It
// does not mutate the KN5, so unrelated cockpit geometry cannot be hidden.
[[nodiscard]] DriverHiddenResolution resolve_driver_hidden_subtrees(
    const apex::formats::Kn5File& model, std::span<const std::string> names,
    DriverDataLimits limits = {});

struct DriverRigAudit {
    std::size_t pose_nodes = 0;
    std::size_t pose_applied = 0;
    std::size_t steering_tracks = 0;
    std::size_t steering_matched = 0;
    std::size_t shift_tracks = 0;
    std::size_t shift_matched = 0;
    std::vector<DriverDiagnostic> diagnostics;
    [[nodiscard]] bool has_errors() const noexcept;
};

[[nodiscard]] DriverRigAudit audit_driver_rig(
    const DriverConfig& config, const apex::formats::Kn5File& model,
    const apex::formats::KnhFile* pose = nullptr,
    const apex::formats::KsAnimation* steering = nullptr,
    const apex::formats::KsAnimation* shifting = nullptr,
    DriverDataLimits limits = {});

}  // namespace apex::domain
