#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace apex::render::detail {

[[nodiscard]] inline float half_to_float(std::uint16_t value) noexcept {
    const float sign = (value & 0x8000U) != 0U ? -1.0F : 1.0F;
    const std::uint32_t exponent = (value >> 10U) & 0x1fU;
    const std::uint32_t fraction = value & 0x3ffU;
    if (exponent == 0U)
        return sign * std::ldexp(static_cast<float>(fraction), -24);
    if (exponent == 0x1fU) {
        if (fraction == 0U)
            return sign * std::numeric_limits<float>::infinity();
        return std::numeric_limits<float>::quiet_NaN();
    }
    return sign * std::ldexp(1.0F + static_cast<float>(fraction) / 1024.0F,
                             static_cast<int>(exponent) - 15);
}

}  // namespace apex::render::detail
