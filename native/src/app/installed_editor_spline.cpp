#include "apex/app/installed_editor_spline.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>

namespace apex::app {
namespace {

[[nodiscard]] bool
finite_point(const InstalledEditorSplinePoint& point) noexcept {
    return std::all_of(point.begin(), point.end(),
                       [](float value) { return std::isfinite(value); });
}

[[nodiscard]] std::size_t spline_index(std::ptrdiff_t index, std::size_t count,
                                       bool closed) noexcept {
    if (closed) {
        if (index < 0)
            index += static_cast<std::ptrdiff_t>(count);
        else if (index >= static_cast<std::ptrdiff_t>(count))
            index -= static_cast<std::ptrdiff_t>(count);
        return static_cast<std::size_t>(index);
    }
    if (index < 0) return 0U;
    return std::min(static_cast<std::size_t>(index), count - 1U);
}

[[nodiscard]] InstalledEditorSplinePoint
catmull_segment(std::span<const InstalledEditorSplinePoint> points,
                std::size_t segment, float value, bool closed) noexcept {
    const auto count = points.size();
    const auto base = static_cast<std::ptrdiff_t>(segment);
    const auto& p0 = points[spline_index(base - 1, count, closed)];
    const auto& p1 = points[spline_index(base, count, closed)];
    const auto& p2 = points[spline_index(base + 1, count, closed)];
    const auto& p3 = points[spline_index(base + 2, count, closed)];
    const float value2 = value * value;
    const float value3 = value2 * value;
    const float c0 = -value3 + 2.0F * value2 - value;
    const float c1 = 3.0F * value3 - 5.0F * value2 + 2.0F;
    const float c2 = 4.0F * value2 - 3.0F * value3 + value;
    const float c3 = value3 - value2;
    InstalledEditorSplinePoint result{};
    for (std::size_t axis = 0U; axis < result.size(); ++axis) {
        result[axis] = 0.5F * (p0[axis] * c0 + p1[axis] * c1 + p2[axis] * c2 +
                               p3[axis] * c3);
    }
    return result;
}

[[nodiscard]] float
catmull_segment_length(std::span<const InstalledEditorSplinePoint> points,
                       std::size_t segment, bool closed) noexcept {
    InstalledEditorSplinePoint previous =
        catmull_segment(points, segment, 0.0F, closed);
    float length = 0.0F;
    for (float value = 0.0F; value <= 1.0F;
         value += installed_editor_spline_length_step) {
        const InstalledEditorSplinePoint current =
            catmull_segment(points, segment, value, closed);
        length += installedEditorSplinePointDistance(current, previous);
        previous = current;
    }
    return length;
}

} // namespace

float installedEditorSplinePointDistance(
    const InstalledEditorSplinePoint& left,
    const InstalledEditorSplinePoint& right) noexcept {
    const float x = left[0] - right[0];
    const float y = left[1] - right[1];
    const float z = left[2] - right[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool installedEditorSplineIsClosed(
    std::span<const InstalledEditorSplinePoint> points) noexcept {
    return points.size() >= 2U &&
           installedEditorSplinePointDistance(points.back(), points.front()) <=
               installed_editor_spline_closure_distance;
}

bool recomputeInstalledEditorSplineLengths(
    InstalledEditorSpline& spline) noexcept {
    if (spline.points.size() < 2U ||
        !std::all_of(spline.points.begin(), spline.points.end(), finite_point))
        return false;
    try {
        spline.cumulative_lengths.assign(spline.points.size(), 0.0F);
    } catch (const std::bad_alloc&) {
        return false;
    }
    float length = 0.0F;
    for (std::size_t segment = 0U; segment + 1U < spline.points.size();
         ++segment) {
        length += catmull_segment_length(spline.points, segment, spline.closed);
        if (!std::isfinite(length)) return false;
        spline.cumulative_lengths[segment + 1U] = length;
    }
    if (spline.closed) {
        length += catmull_segment_length(spline.points,
                                         spline.points.size() - 1U, true);
    }
    spline.closing_length =
        spline.closed ? installedEditorSplinePointDistance(
                            spline.points.back(), spline.points.front())
                      : 0.0F;
    if (!(length > 0.0F) || !std::isfinite(length) ||
        !std::isfinite(spline.closing_length) ||
        (spline.closed && !(spline.closing_length > 0.0F)))
        return false;
    spline.length = length + spline.closing_length;
    if (!std::isfinite(spline.length)) return false;
    return true;
}

bool validInstalledEditorSpline(const InstalledEditorSpline& spline,
                                std::size_t minimum_points) noexcept {
    if (spline.points.size() < minimum_points ||
        spline.points.size() != spline.cumulative_lengths.size() ||
        !(spline.length > 0.0F) || !std::isfinite(spline.length) ||
        !std::isfinite(spline.closing_length) ||
        (spline.closed && !(spline.closing_length > 0.0F)) ||
        (!spline.closed && spline.closing_length != 0.0F) ||
        !std::all_of(spline.points.begin(), spline.points.end(),
                     finite_point) ||
        spline.cumulative_lengths.empty() ||
        spline.cumulative_lengths.front() != 0.0F)
        return false;
    float previous = 0.0F;
    for (const float value : spline.cumulative_lengths) {
        if (!std::isfinite(value) || value < previous || value > spline.length)
            return false;
        previous = value;
    }
    return true;
}

std::optional<InstalledEditorSplinePoint>
sampleInstalledEditorSpline(const InstalledEditorSpline& spline,
                            float normalized_position) noexcept {
    if (!std::isfinite(normalized_position) ||
        !validInstalledEditorSpline(spline))
        return std::nullopt;
    const float position = std::clamp(normalized_position, 0.0F, 1.0F);
    if (position == 0.0F) return spline.points.front();

    const float distance = spline.length * position;
    const auto next =
        std::upper_bound(spline.cumulative_lengths.begin(),
                         spline.cumulative_lengths.end(), distance);
    if (next != spline.cumulative_lengths.end()) {
        const std::size_t segment = static_cast<std::size_t>(
            next - spline.cumulative_lengths.begin() - 1);
        const float begin = spline.cumulative_lengths[segment];
        const float end = *next;
        if (!(end > begin)) return std::nullopt;
        return catmull_segment(spline.points, segment,
                               (distance - begin) / (end - begin),
                               spline.closed);
    }
    if (spline.closed) {
        const float begin = spline.cumulative_lengths.back();
        if (spline.closing_length > 0.0F) {
            return catmull_segment(spline.points, spline.points.size() - 1U,
                                   (distance - begin) / spline.closing_length,
                                   true);
        }
    }
    return spline.points.back();
}

} // namespace apex::app
