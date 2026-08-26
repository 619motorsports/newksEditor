#include "apex/app/installed_editor_spline.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <utility>

namespace apex::app {
namespace {

// Preserve the scalar single-precision stores in the recovered SSE sequence.
[[nodiscard]] float rounded_float(float value) noexcept {
    volatile float rounded = value;
    return rounded;
}

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
    const float value2 = rounded_float(value * value);
    const float value3 = rounded_float(value2 * value);
    const float c0 = rounded_float(
        rounded_float(rounded_float(2.0F * value2) - value3) - value);
    const float c1 = rounded_float(
        rounded_float(rounded_float(3.0F * value3) -
                      rounded_float(5.0F * value2)) +
        2.0F);
    const float c2 = rounded_float(
        rounded_float(rounded_float(4.0F * value2) -
                      rounded_float(3.0F * value3)) +
        value);
    const float c3 = rounded_float(value3 - value2);
    InstalledEditorSplinePoint result{};
    for (std::size_t axis = 0U; axis < result.size(); ++axis) {
        const float h0 = rounded_float(p0[axis] * 0.5F);
        const float h1 = rounded_float(p1[axis] * 0.5F);
        const float h2 = rounded_float(p2[axis] * 0.5F);
        const float h3 = rounded_float(p3[axis] * 0.5F);
        const float term0 = rounded_float(h0 * c0);
        const float term1 = rounded_float(h1 * c1);
        const float term2 = rounded_float(h2 * c2);
        const float term3 = rounded_float(h3 * c3);
        result[axis] = rounded_float(
            rounded_float(rounded_float(term0 + term1) + term2) + term3);
    }
    return result;
}

[[nodiscard]] float
catmull_segment_length(std::span<const InstalledEditorSplinePoint> points,
                       std::size_t segment, bool closed) noexcept {
    InstalledEditorSplinePoint previous = points[segment];
    float length = 0.0F;
    for (float value = 0.0F; value <= 1.0F;
         value += installed_editor_spline_length_step) {
        const InstalledEditorSplinePoint current =
            catmull_segment(points, segment, value, closed);
        length = rounded_float(
            length + installedEditorSplinePointDistance(current, previous));
        previous = current;
    }
    return length;
}

} // namespace

float installedEditorSplinePointDistance(
    const InstalledEditorSplinePoint& left,
    const InstalledEditorSplinePoint& right) noexcept {
    const float x = rounded_float(left[0] - right[0]);
    const float y = rounded_float(left[1] - right[1]);
    const float z = rounded_float(left[2] - right[2]);
    const float squared_x = rounded_float(x * x);
    const float squared_y = rounded_float(y * y);
    const float squared_z = rounded_float(z * z);
    return rounded_float(std::sqrt(
        rounded_float(rounded_float(squared_x + squared_y) + squared_z)));
}

bool installedEditorSplineIsClosed(
    std::span<const InstalledEditorSplinePoint> points) noexcept {
    return points.size() >= 2U &&
           installedEditorSplinePointDistance(points.back(), points.front()) <=
               installed_editor_spline_closure_distance;
}

bool recomputeInstalledEditorSplineLengths(
    InstalledEditorSpline& spline) noexcept {
    if ((spline.closed && spline.points.size() < 2U) ||
        !std::all_of(spline.points.begin(), spline.points.end(), finite_point))
        return false;
    std::vector<float> cumulative_lengths;
    try {
        cumulative_lengths.assign(spline.points.size(), 0.0F);
    } catch (const std::bad_alloc&) {
        return false;
    }
    float length = 0.0F;
    for (std::size_t segment = 0U; segment + 1U < spline.points.size();
         ++segment) {
        length = rounded_float(
            length + catmull_segment_length(spline.points, segment,
                                            spline.closed));
        if (!std::isfinite(length)) return false;
        cumulative_lengths[segment + 1U] = length;
    }
    if (spline.closed) {
        length = rounded_float(
            length + catmull_segment_length(
                         spline.points, spline.points.size() - 1U, true));
    }
    const float closing_length =
        spline.closed ? installedEditorSplinePointDistance(
                            spline.points.back(), spline.points.front())
                      : 0.0F;
    if (!std::isfinite(length) || !std::isfinite(closing_length) ||
        length < 0.0F || closing_length < 0.0F)
        return false;
    const float total_length = rounded_float(length + closing_length);
    if (!std::isfinite(total_length)) return false;
    spline.cumulative_lengths = std::move(cumulative_lengths);
    spline.closing_length = closing_length;
    spline.length = total_length;
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
