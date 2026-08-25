#pragma once

#include "apex/formats/ai_spline.hpp"
#include "apex/formats/ai_spline_write.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::authoring {

// The installed editor exposes these six fields in ksWaypoinInfo. Camber is
// displayed and accepted in degrees, while the version-7 file stores radians.
struct AiSplineWaypointInfo {
    float radius = 0.0F;
    float side0 = 0.0F;
    float side1 = 0.0F;
    float camberDegrees = 0.0F;
    float length = 0.0F;
    float grade = 0.0F;
};

// Recovered setWaypointInfo compatibility inputs. A zero replacement field is
// an unchanged sentinel, so this operation cannot replace a field with zero.
// Additive fields are always added after the nonzero replacements are applied.
struct AiSplineWaypointEdit {
    AiSplineWaypointInfo replacement;
    AiSplineWaypointInfo additive;
};

enum class AiSplineWaypointStatus : std::uint8_t {
    ok,
    invalid,
    unsupported,
    failed,
};

struct AiSplineWaypointDiagnostic {
    std::string code;
    std::string path;
    std::string message;
};

struct AiSplineWaypointResult {
    AiSplineWaypointStatus status = AiSplineWaypointStatus::failed;
    std::vector<AiSplineWaypointDiagnostic> diagnostics;
    std::uint32_t pointIndex = 0U;
    std::uint32_t payloadIndex = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return status == AiSplineWaypointStatus::ok;
    }
};

struct AiSplineWaypointReadResult : AiSplineWaypointResult {
    std::optional<AiSplineWaypointInfo> info;
};

struct AiSplineWaypointApplyResult : AiSplineWaypointResult {
    std::optional<formats::AiSpline> candidate;
    std::vector<std::uint8_t> bytes;
    std::optional<AiSplineWaypointInfo> before;
    std::optional<AiSplineWaypointInfo> after;
    bool changed = false;
};

inline constexpr float aiSplineRadiansToDegrees = 57.295780181884766F;
inline constexpr float aiSplineDegreesToRadians = 0.01745299994945526F;

// Native access is enabled only when exactly one point is selected. This safe
// adapter reports an explicit error instead of returning the native zero/-1
// sentinel object for invalid selection or point state.
[[nodiscard]] AiSplineWaypointReadResult
readAiSplineWaypointInfo(const formats::AiSpline& spline,
                         std::span<const std::uint32_t> selectedPointIndices);

// Derive, validate, and serialize a complete candidate without mutating the
// input spline. The point tag, not the point index, selects the payload record.
[[nodiscard]] AiSplineWaypointApplyResult
applyAiSplineWaypointEdit(const formats::AiSpline& spline,
                          std::span<const std::uint32_t> selectedPointIndices,
                          const AiSplineWaypointEdit& edit,
                          formats::AiSplineWriteLimits limits = {});

} // namespace apex::authoring
