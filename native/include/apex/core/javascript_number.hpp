#pragma once

#include <optional>
#include <string_view>

namespace apex::core {

/**
 * Parse the finite subset of ECMAScript Number(string).
 *
 * The conversion accepts ECMAScript whitespace, decimal syntax, and unsigned
 * 0x/0b/0o literals. Empty text and decimal underflow produce zero. NaN,
 * infinity, malformed text, and overflow return no value.
 */
[[nodiscard]] std::optional<double> parse_finite_javascript_number(
    std::string_view value) noexcept;

}  // namespace apex::core
