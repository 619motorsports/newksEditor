#pragma once

#include "apex/formats/ini.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::domain {

struct TrackDataLimits {
    std::size_t maxInputBytes = 64U * 1024U * 1024U;
    std::size_t maxOutputBytes = 64U * 1024U * 1024U;
    std::size_t maxLineBytes = 1U * 1024U * 1024U;
    std::size_t maxLines = 1'000'000;
    std::size_t maxSections = 100'000;
    std::size_t maxFieldsPerSection = 100'000;
    std::size_t maxFields = 1'000'000;
    std::size_t maxStringBytes = 1U * 1024U * 1024U;
    std::size_t maxSplinePoints = 1'000'000;
    std::size_t maxNodes = 2'000'000;
    std::size_t maxDiagnostics = 1'000'000;
};

enum class TrackDiagnosticSeverity { warning, error };

struct TrackDiagnostic {
    TrackDiagnosticSeverity severity = TrackDiagnosticSeverity::warning;
    std::string code;
    std::string message;
    std::string source;
    std::size_t line = 0;
    std::size_t section_index = 0;
    std::size_t entry_index = 0;
};

struct OrderedTrackField {
    std::string key;
    std::string value;
    std::size_t line = 0;
    std::size_t entry_index = 0;
};

struct TrackSurface {
    std::size_t index = 0;
    std::string section;
    std::size_t line = 0;
    std::string key;
    std::string normalized_key;
    double friction = 1.0;
    double damping = 0.0;
    double dirt_additive = 0.0;
    double black_flag_time = 0.0;
    bool is_valid_track = false;
    bool is_pitlane = false;
    double sin_height = 0.0;
    double sin_length = 0.0;
    double vibration_gain = 0.0;
    double vibration_length = 0.0;
    std::string wav;
    double wav_pitch = 0.0;
    std::string ff_effect;
    // All authored fields, including duplicate and unknown keys, in source
    // order. Typed projections above use each key's last authored value.
    std::vector<OrderedTrackField> fields;
    std::size_t source_section_index = 0;
};

struct TrackSurfaces {
    std::string source;
    std::vector<TrackSurface> surfaces;
    std::vector<TrackDiagnostic> diagnostics;
    std::size_t ignored_sections = 0;

    [[nodiscard]] bool has_errors() const noexcept;
};

[[nodiscard]] TrackSurfaces parse_track_surfaces(
    const apex::formats::IniDocument& document,
    TrackDataLimits limits = {});
[[nodiscard]] TrackSurfaces parse_track_surfaces(
    std::string_view text, std::string source = "data/surfaces.ini",
    TrackDataLimits limits = {});
[[nodiscard]] std::string serialize_track_surfaces_ini(
    const TrackSurfaces& surfaces, TrackDataLimits limits = {});
inline TrackSurfaces parse_surfaces_ini(std::string_view text,
                                        std::string source = "data/surfaces.ini",
                                        TrackDataLimits limits = {}) {
    return parse_track_surfaces(text, std::move(source), limits);
}
inline std::string serialize_surfaces_ini(const TrackSurfaces& surfaces,
                                          TrackDataLimits limits = {}) {
    return serialize_track_surfaces_ini(surfaces, limits);
}

enum class RuntimeSurfaceStatus { not_physics, matched, fallback, ambiguous };

struct RuntimeSurface {
    std::string key;
    std::string normalized_key;
    std::string origin;
    std::optional<std::size_t> configured_index;
};

struct RuntimeSurfaceMatch {
    RuntimeSurfaceStatus status = RuntimeSurfaceStatus::fallback;
    long long sector_id = 0;
    std::optional<RuntimeSurface> surface;
    std::vector<RuntimeSurface> candidates;
};

[[nodiscard]] std::vector<RuntimeSurface> runtime_surfaces(const TrackSurfaces* configured = nullptr);
[[nodiscard]] RuntimeSurfaceMatch resolve_runtime_surface(
    std::string_view physics_name, const TrackSurfaces* configured = nullptr);

struct TrackMeshName {
    std::string name;
    bool mesh = true;
};

struct TrackMarkerAudit {
    std::size_t starts = 0;
    std::size_t pits = 0;
    std::size_t time_gates = 0;
    bool hotlap = false;
    std::size_t runtime_surface_count = 0;
    std::vector<std::pair<std::string, std::size_t>> surface_matches;
    std::vector<std::string> unmatched_surfaces;
    std::vector<std::string> unmatched_physical;
    std::vector<std::pair<std::string, std::vector<std::string>>> ambiguous_physical;
    std::vector<TrackDiagnostic> diagnostics;
    [[nodiscard]] bool has_errors() const noexcept;
};

[[nodiscard]] TrackMarkerAudit audit_track_markers(
    std::span<const std::string> node_names,
    std::span<const TrackMeshName> meshes,
    const TrackSurfaces* configured = nullptr,
    TrackDataLimits limits = {});

struct CameraData {
    std::size_t index = 0;
    std::string name;
    std::size_t line = 0;
    std::array<double, 3> position{};
    std::array<double, 3> forward{0.0, 0.0, -1.0};
    std::array<double, 3> up{0.0, 1.0, 0.0};
    double min_fov = 45.0;
    double max_fov = 45.0;
    double in_point = -1.0;
    double out_point = -1.0;
    double near_plane = 0.1;
    double far_plane = 10'000.0;
    double min_exposure = 0.0;
    double max_exposure = 0.0;
    double dof_factor = 0.0;
    double dof_range = 0.0;
    double dof_focus = 0.0;
    bool dof_manual = false;
    std::string spline;
    double spline_rotation = 0.0;
    double spline_animation_length = 0.0;
    double fov_gamma = 1.0;
    bool fixed = false;
    std::vector<OrderedTrackField> fields;
    std::size_t source_section_index = 0;
};

struct SplineRequest {
    bool accepted = false;
    std::string relative_path;
    std::string source;
    std::size_t line = 0;
    std::string code;
    std::string message;
};

// This creates a capability-relative request only. It never opens or probes
// the requested file; the caller must resolve it beneath its own capability.
[[nodiscard]] SplineRequest request_camera_spline(const CameraData& camera,
                                                   std::string source = {});

struct CameraSet {
    std::string source;
    double version = 0.0;
    double declared_count = 0.0;
    std::string name;
    std::vector<CameraData> cameras;
    std::vector<TrackDiagnostic> diagnostics;
    std::size_t ignored_sections = 0;
    [[nodiscard]] bool has_errors() const noexcept;
};

[[nodiscard]] CameraSet parse_track_cameras(
    const apex::formats::IniDocument& document,
    TrackDataLimits limits = {});
[[nodiscard]] CameraSet parse_track_cameras(
    std::string_view text, std::string source = "data/cameras.ini",
    TrackDataLimits limits = {});
inline CameraSet parse_cameras_ini(std::string_view text,
                                   std::string source = "data/cameras.ini",
                                   TrackDataLimits limits = {}) {
    return parse_track_cameras(text, std::move(source), limits);
}

struct SplinePointSet {
    std::string source;
    std::vector<std::array<double, 3>> points;
    double length = 0.0;
    std::vector<TrackDiagnostic> diagnostics;
    [[nodiscard]] bool has_errors() const noexcept;
};

[[nodiscard]] SplinePointSet parse_camera_spline(
    std::string_view text, std::string source = "camera spline.csv",
    TrackDataLimits limits = {});
inline SplinePointSet parse_camera_spline_csv(
    std::string_view text, std::string source = "camera spline.csv",
    TrackDataLimits limits = {}) {
    return parse_camera_spline(text, std::move(source), limits);
}
[[nodiscard]] std::vector<std::array<double, 3>> rotate_camera_spline(
    std::span<const std::array<double, 3>> points, double degrees);
[[nodiscard]] std::array<double, 3> sample_camera_spline(
    std::span<const std::array<double, 3>> points, double position = 0.0);

}  // namespace apex::domain
