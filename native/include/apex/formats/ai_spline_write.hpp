#pragma once

#include "apex/formats/ai_spline.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::formats {

struct AiSplineWriteLimits {
    std::size_t maxPoints = 1'000'000U;
    std::size_t maxPayloads = 1'000'000U;
    std::size_t maxGridNeighbors = 1'000'000U;
    std::size_t maxGridRows = 1'000'000U;
    std::size_t maxGridCellsPerRow = 1'000'000U;
    std::size_t maxGridIndicesPerCell = 1'000'000U;
    std::size_t maxGridIndices = 10'000'000U;
    std::size_t maxOutputBytes = 256U * 1024U * 1024U;
};

class AiSplineWriteError final : public std::runtime_error {
public:
    AiSplineWriteError(std::string code, std::string message);

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

// Serialize the recovered AISpline::save version-7 layout. The native writer
// writes zero for the header and payload reserved words. Version 2 is rejected.
// Callers must use the explicit legacy conversion boundary first.
[[nodiscard]] std::vector<std::uint8_t> serializeAiSpline(
    const AiSpline& spline, AiSplineWriteLimits limits = {});

[[nodiscard]] inline std::vector<std::uint8_t> serialize_ai_spline(
    const AiSpline& spline, AiSplineWriteLimits limits = {}) {
    return serializeAiSpline(spline, limits);
}

} // namespace apex::formats
