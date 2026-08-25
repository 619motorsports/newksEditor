#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace apex::app {

inline constexpr float installed_editor_spline_closure_distance = 75.0F;
inline constexpr float installed_editor_spline_length_step = 0.001F;

using InstalledEditorSplinePoint = std::array<float, 3U>;

// Backend-neutral representation of the installed editor's Catmull-Rom
// spline. Camera CSV and loaded AI paths both call computeSplineLength() after
// they add their format-specific control points.
struct InstalledEditorSpline {
    std::vector<InstalledEditorSplinePoint> points;
    std::vector<float> cumulative_lengths;
    // Spline::length() adds this endpoint chord after computeSplineLength()
    // has already sampled the wrapped Catmull-Rom segment. Preserve that
    // recovered closed-path quirk for normalized lookup and playback.
    float closing_length = 0.0F;
    float length = 0.0F;
    bool closed = false;
};

[[nodiscard]] float installedEditorSplinePointDistance(
    const InstalledEditorSplinePoint& left,
    const InstalledEditorSplinePoint& right) noexcept;

[[nodiscard]] bool installedEditorSplineIsClosed(
    std::span<const InstalledEditorSplinePoint> points) noexcept;

// Replace cumulative_lengths and length with the recovered 0.001f
// per-segment Catmull-Rom arc-length approximation. The caller selects
// topology before this call.
[[nodiscard]] bool
recomputeInstalledEditorSplineLengths(InstalledEditorSpline& spline) noexcept;

[[nodiscard]] bool
validInstalledEditorSpline(const InstalledEditorSpline& spline,
                           std::size_t minimum_points = 4U) noexcept;

// Evaluate normalized distance through the installed editor's Catmull-Rom
// mode. Invalid data returns no point instead of reproducing native unchecked
// indexing or divide-by-zero behavior.
[[nodiscard]] std::optional<InstalledEditorSplinePoint>
sampleInstalledEditorSpline(const InstalledEditorSpline& spline,
                            float normalized_position) noexcept;

} // namespace apex::app
