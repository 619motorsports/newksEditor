#pragma once

#include "apex/formats/ai_spline.hpp"
#include "apex/formats/ai_spline_grid.hpp"
#include "apex/formats/ai_spline_write.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apex::app {

enum class AiSplineLegacyConversionStatus : std::uint8_t {
    converted,
    unsupported,
    invalid,
    resource_limit,
    allocation_failed,
};

struct AiSplineLegacyConversionDiagnostic {
    std::string code;
    std::string message;
};

struct AiSplineLegacyConversionLimits {
    std::size_t maxSourceNameBytes = 1U * 1024U * 1024U;
    std::size_t maxRecords = 1'000'000U;
    std::size_t maxRetainedPoints = 100'000U;
    // One unit is one Catmull-Rom evaluation in the recovered length pass.
    std::size_t maxLengthSampleEvaluations = 100'200'000U;
    // Bounds additional storage that this conversion owns. The parsed source
    // remains subject to its separate parser aggregate limit.
    std::size_t maxAggregateBytes = 512U * 1024U * 1024U;
    formats::AiSplineGridBuildLimits grid{};
    formats::AiSplineWriteLimits write{};
};

struct AiSplineLegacyConversionResult {
    AiSplineLegacyConversionStatus status =
        AiSplineLegacyConversionStatus::invalid;
    std::vector<std::uint8_t> bytes;
    std::size_t pointCount = 0U;
    bool gridBuilt = false;
    std::vector<AiSplineLegacyConversionDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return status == AiSplineLegacyConversionStatus::converted &&
               !bytes.empty();
    }
};

// Convert the recovered version-2 load result to a complete version-7 file.
// The operation is explicit and transactional. It does not change source.
// Resource errors return diagnostics when storage remains available. Severe
// allocation exhaustion can still throw while the result builds a diagnostic.
[[nodiscard]] AiSplineLegacyConversionResult convertAiSplineV2ToV7File(
    const formats::AiSpline& source,
    AiSplineLegacyConversionLimits limits = {});

[[nodiscard]] const char* aiSplineLegacyConversionStatusName(
    AiSplineLegacyConversionStatus status) noexcept;

} // namespace apex::app
