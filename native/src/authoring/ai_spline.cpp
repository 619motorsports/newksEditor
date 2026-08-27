#include "apex/authoring/ai_spline.hpp"

#include <bit>
#include <cmath>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace apex::authoring {
namespace {

void fail(AiSplineWaypointResult& result, AiSplineWaypointStatus status,
          std::string_view code, std::string_view path,
          std::string_view message) {
    result.status = status;
    result.diagnostics.push_back(
        {std::string(code), std::string(path), std::string(message)});
}

[[nodiscard]] bool finite(const AiSplineWaypointInfo& info) noexcept {
    return std::isfinite(info.radius) && std::isfinite(info.side0) &&
           std::isfinite(info.side1) && std::isfinite(info.camberDegrees) &&
           std::isfinite(info.length) && std::isfinite(info.grade);
}

[[nodiscard]] AiSplineWaypointInfo
readInfo(const formats::AiSplinePayload& payload) noexcept {
    return {
        payload.radius, payload.side0,
        payload.side1,  payload.camber * aiSplineRadiansToDegrees,
        payload.length, payload.grade,
    };
}

[[nodiscard]] bool
resolvePayload(const formats::AiSpline& spline,
               std::span<const std::uint32_t> selectedPointIndices,
               AiSplineWaypointResult& result) {
    const std::string& source = spline.source;
    if (spline.version != 7U) {
        fail(result, AiSplineWaypointStatus::unsupported, "UNSUPPORTED_VERSION",
             source, "AI spline waypoint editing supports version 7 only");
        return false;
    }
    if (spline.points.size() != spline.payloads.size()) {
        fail(result, AiSplineWaypointStatus::invalid, "COUNT_MISMATCH", source,
             "AI spline payload count must equal point count");
        return false;
    }
    if (selectedPointIndices.size() != 1U) {
        fail(result, AiSplineWaypointStatus::invalid, "SELECTION_COUNT", source,
             "AI spline waypoint editing requires exactly one selected point");
        return false;
    }

    result.pointIndex = selectedPointIndices.front();
    if (static_cast<std::size_t>(result.pointIndex) >= spline.points.size()) {
        fail(result, AiSplineWaypointStatus::invalid, "POINT_INDEX_INVALID",
             source, "selected AI spline point is outside the point array");
        return false;
    }

    const std::int32_t tag = spline.points[result.pointIndex].tag;
    if (tag < 0 || static_cast<std::size_t>(tag) >= spline.payloads.size()) {
        fail(result, AiSplineWaypointStatus::invalid, "PAYLOAD_INDEX_INVALID",
             source,
             "selected AI spline point tag is outside the payload array");
        return false;
    }
    result.payloadIndex = static_cast<std::uint32_t>(tag);
    return true;
}

void replaceNonzero(float& target, float value) noexcept {
    if (value != 0.0F)
        target = value;
}

[[nodiscard]] bool sameInfo(const AiSplineWaypointInfo& left,
                            const AiSplineWaypointInfo& right) noexcept {
    return std::bit_cast<std::uint32_t>(left.radius) ==
               std::bit_cast<std::uint32_t>(right.radius) &&
           std::bit_cast<std::uint32_t>(left.side0) ==
               std::bit_cast<std::uint32_t>(right.side0) &&
           std::bit_cast<std::uint32_t>(left.side1) ==
               std::bit_cast<std::uint32_t>(right.side1) &&
           std::bit_cast<std::uint32_t>(left.camberDegrees) ==
               std::bit_cast<std::uint32_t>(right.camberDegrees) &&
           std::bit_cast<std::uint32_t>(left.length) ==
               std::bit_cast<std::uint32_t>(right.length) &&
           std::bit_cast<std::uint32_t>(left.grade) ==
               std::bit_cast<std::uint32_t>(right.grade);
}

[[nodiscard]] bool
validateSelectedPayloads(const formats::AiSpline& spline,
                         std::span<const std::uint32_t> selectedPointIndices,
                         AiSplineWaypointResult& result,
                         std::vector<std::uint32_t>& payloadIndices,
                         std::size_t maxSelectionEntries) {
    if (spline.version != 7U) {
        fail(result, AiSplineWaypointStatus::unsupported, "UNSUPPORTED_VERSION",
             spline.source,
             "AI spline camber inversion supports version 7 only");
        return false;
    }
    if (spline.points.size() != spline.payloads.size()) {
        fail(result, AiSplineWaypointStatus::invalid, "COUNT_MISMATCH",
             spline.source, "AI spline payload count must equal point count");
        return false;
    }
    if (selectedPointIndices.size() > maxSelectionEntries) {
        fail(result, AiSplineWaypointStatus::invalid, "SELECTION_LIMIT",
             spline.source,
             "AI spline selection count exceeds its entry limit");
        return false;
    }

    payloadIndices.reserve(selectedPointIndices.size());
    for (const auto pointIndex : selectedPointIndices) {
        if (static_cast<std::size_t>(pointIndex) >= spline.points.size()) {
            fail(result, AiSplineWaypointStatus::invalid, "POINT_INDEX_INVALID",
                 spline.source,
                 "selected AI spline point is outside the point array");
            return false;
        }
        const std::int32_t tag = spline.points[pointIndex].tag;
        if (tag < 0 ||
            static_cast<std::size_t>(tag) >= spline.payloads.size()) {
            fail(result, AiSplineWaypointStatus::invalid,
                 "PAYLOAD_INDEX_INVALID", spline.source,
                 "selected AI spline point tag is outside the payload array");
            return false;
        }
        const auto payloadIndex = static_cast<std::uint32_t>(tag);
        if (!std::isfinite(spline.payloads[payloadIndex].camber)) {
            fail(result, AiSplineWaypointStatus::invalid, "NON_FINITE_VALUE",
                 spline.source,
                 "selected AI spline waypoint contains a non-finite camber "
                 "value");
            return false;
        }
        payloadIndices.push_back(payloadIndex);
    }
    return true;
}

} // namespace

AiSplineWaypointReadResult
readAiSplineWaypointInfo(const formats::AiSpline& spline,
                         std::span<const std::uint32_t> selectedPointIndices) {
    AiSplineWaypointReadResult result;
    if (!resolvePayload(spline, selectedPointIndices, result))
        return result;

    const auto info = readInfo(spline.payloads[result.payloadIndex]);
    if (!finite(info)) {
        fail(result, AiSplineWaypointStatus::invalid, "NON_FINITE_VALUE",
             spline.source,
             "selected AI spline waypoint contains a non-finite value");
        return result;
    }
    result.info = info;
    result.status = AiSplineWaypointStatus::ok;
    return result;
}

AiSplineWaypointApplyResult
applyAiSplineWaypointEdit(const formats::AiSpline& spline,
                          std::span<const std::uint32_t> selectedPointIndices,
                          const AiSplineWaypointEdit& edit,
                          formats::AiSplineWriteLimits limits) {
    AiSplineWaypointApplyResult result;
    if (!resolvePayload(spline, selectedPointIndices, result))
        return result;
    if (!finite(edit.replacement) || !finite(edit.additive)) {
        fail(result, AiSplineWaypointStatus::invalid, "NON_FINITE_EDIT",
             spline.source,
             "AI spline waypoint edit contains a non-finite value");
        return result;
    }

    const auto before = readInfo(spline.payloads[result.payloadIndex]);
    if (!finite(before)) {
        fail(result, AiSplineWaypointStatus::invalid, "NON_FINITE_VALUE",
             spline.source,
             "selected AI spline waypoint contains a non-finite value");
        return result;
    }

    try {
        auto candidate = spline;
        auto& payload = candidate.payloads[result.payloadIndex];
        replaceNonzero(payload.radius, edit.replacement.radius);
        replaceNonzero(payload.side0, edit.replacement.side0);
        replaceNonzero(payload.side1, edit.replacement.side1);
        if (edit.replacement.camberDegrees != 0.0F)
            payload.camber =
                edit.replacement.camberDegrees * aiSplineDegreesToRadians;
        replaceNonzero(payload.length, edit.replacement.length);
        replaceNonzero(payload.grade, edit.replacement.grade);

        payload.radius += edit.additive.radius;
        payload.side0 += edit.additive.side0;
        payload.side1 += edit.additive.side1;
        payload.camber +=
            edit.additive.camberDegrees * aiSplineDegreesToRadians;
        payload.length += edit.additive.length;
        payload.grade += edit.additive.grade;

        const auto after = readInfo(payload);
        if (!finite(after)) {
            fail(result, AiSplineWaypointStatus::invalid, "NON_FINITE_RESULT",
                 spline.source,
                 "AI spline waypoint edit produced a non-finite value");
            return result;
        }

        result.bytes = formats::serializeAiSpline(candidate, limits);
        result.before = before;
        result.after = after;
        result.changed = !sameInfo(before, after);
        result.candidate = std::move(candidate);
        result.status = AiSplineWaypointStatus::ok;
    } catch (const formats::AiSplineWriteError& error) {
        const auto status = error.code() == "UNSUPPORTED_VERSION"
                                ? AiSplineWaypointStatus::unsupported
                                : AiSplineWaypointStatus::invalid;
        fail(result, status, error.code(), spline.source, error.what());
        result.bytes.clear();
        result.candidate.reset();
    } catch (const std::exception& error) {
        fail(result, AiSplineWaypointStatus::failed, "WAYPOINT_EDIT_FAILED",
             spline.source, error.what());
        result.bytes.clear();
        result.candidate.reset();
    }
    return result;
}

AiSplineSelectedApplyResult applyAiSplineCamberInversion(
    const formats::AiSpline& spline,
    std::span<const std::uint32_t> selectedPointIndices,
    formats::AiSplineWriteLimits limits, std::size_t maxSelectionEntries) {
    AiSplineSelectedApplyResult result;
    std::vector<std::uint32_t> payloadIndices;
    try {
        if (!validateSelectedPayloads(spline, selectedPointIndices, result,
                                      payloadIndices, maxSelectionEntries))
            return result;

        auto candidate = spline;
        for (const auto payloadIndex : payloadIndices)
            candidate.payloads[payloadIndex].camber =
                -candidate.payloads[payloadIndex].camber;

        bool changed = false;
        for (const auto payloadIndex : payloadIndices) {
            changed = changed || std::bit_cast<std::uint32_t>(
                                     candidate.payloads[payloadIndex].camber) !=
                                     std::bit_cast<std::uint32_t>(
                                         spline.payloads[payloadIndex].camber);
        }
        result.bytes = formats::serializeAiSpline(candidate, limits);
        result.applied = selectedPointIndices.size();
        result.changed = changed;
        result.candidate = std::move(candidate);
        result.status = AiSplineWaypointStatus::ok;
    } catch (const formats::AiSplineWriteError& error) {
        const auto status = error.code() == "UNSUPPORTED_VERSION"
                                ? AiSplineWaypointStatus::unsupported
                                : AiSplineWaypointStatus::invalid;
        fail(result, status, error.code(), spline.source, error.what());
        result.bytes.clear();
        result.candidate.reset();
    } catch (const std::exception& error) {
        fail(result, AiSplineWaypointStatus::failed, "CAMBER_INVERSION_FAILED",
             spline.source, error.what());
        result.bytes.clear();
        result.candidate.reset();
    }
    return result;
}

} // namespace apex::authoring
