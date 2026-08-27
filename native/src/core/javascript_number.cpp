#include "apex/core/javascript_number.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace apex::core {
namespace {

[[nodiscard]] std::string_view trim_javascript_whitespace(
    std::string_view value) noexcept {
    static constexpr std::array<std::string_view, 25> whitespace = {
        "\x09",       "\x0a",       "\x0b",       "\x0c",       "\x0d",
        "\x20",       "\xc2\xa0",   "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81",
        "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86",
        "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
        "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80", "\xef\xbb\xbf"};
    const auto trim_front = [&value] {
        for (const auto item : whitespace) {
            if (value.starts_with(item)) {
                value.remove_prefix(item.size());
                return true;
            }
        }
        return false;
    };
    const auto trim_back = [&value] {
        for (const auto item : whitespace) {
            if (value.ends_with(item)) {
                value.remove_suffix(item.size());
                return true;
            }
        }
        return false;
    };
    while (trim_front()) {}
    while (trim_back()) {}
    return value;
}

struct DecimalOrder {
    bool zero = false;
    std::int64_t value = 0;
};

[[nodiscard]] std::optional<DecimalOrder> decimal_order(
    std::string_view value) noexcept {
    if (!value.empty() && value.front() == '-') value.remove_prefix(1U);
    const auto exponent_marker = value.find_first_of("eE");
    const auto mantissa = value.substr(0U, exponent_marker);
    std::int64_t explicit_exponent = 0;
    if (exponent_marker != std::string_view::npos) {
        auto exponent = value.substr(exponent_marker + 1U);
        bool negative = false;
        if (!exponent.empty() && (exponent.front() == '+' || exponent.front() == '-')) {
            negative = exponent.front() == '-';
            exponent.remove_prefix(1U);
        }
        if (exponent.empty()) return std::nullopt;
        constexpr std::int64_t saturation = 1'000'000;
        for (const auto character : exponent) {
            if (character < '0' || character > '9') return std::nullopt;
            const auto digit = static_cast<std::int64_t>(character - '0');
            explicit_exponent = explicit_exponent > (saturation - digit) / 10
                                    ? saturation
                                    : explicit_exponent * 10 + digit;
        }
        if (negative) explicit_exponent = -explicit_exponent;
    }

    const auto point = mantissa.find('.');
    const auto digits_before_point = static_cast<std::int64_t>(
        point == std::string_view::npos ? mantissa.size() : point);
    std::int64_t digit_index = 0;
    std::optional<std::int64_t> first_nonzero;
    for (const auto character : mantissa) {
        if (character == '.') continue;
        if (character < '0' || character > '9') return std::nullopt;
        if (!first_nonzero && character != '0') first_nonzero = digit_index;
        ++digit_index;
    }
    if (!first_nonzero) return DecimalOrder{true, 0};
    return DecimalOrder{
        false, explicit_exponent + digits_before_point - *first_nonzero - 1};
}

}  // namespace

std::optional<double> parse_finite_javascript_number(
    std::string_view source) noexcept {
    auto value = trim_javascript_whitespace(source);
    if (value.empty()) return 0.0;
    if (value.size() > 2U && value.front() == '0' &&
        (value[1] == 'x' || value[1] == 'X' || value[1] == 'b' || value[1] == 'B' ||
         value[1] == 'o' || value[1] == 'O')) {
        const auto base = value[1] == 'x' || value[1] == 'X'
                              ? 16U
                              : value[1] == 'b' || value[1] == 'B' ? 2U : 8U;
        double result = 0.0;
        for (std::size_t index = 2U; index < value.size(); ++index) {
            const auto character = value[index];
            unsigned digit = 0U;
            if (character >= '0' && character <= '9')
                digit = static_cast<unsigned>(character - '0');
            else if (character >= 'a' && character <= 'f')
                digit = 10U + static_cast<unsigned>(character - 'a');
            else if (character >= 'A' && character <= 'F')
                digit = 10U + static_cast<unsigned>(character - 'A');
            else
                return std::nullopt;
            if (digit >= base) return std::nullopt;
            result = result * static_cast<double>(base) + static_cast<double>(digit);
            if (!std::isfinite(result)) return std::nullopt;
        }
        return result;
    }

    if (value.front() == '+') {
        value.remove_prefix(1U);
        if (value.empty()) return std::nullopt;
    }
    double result = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result,
                                        std::chars_format::general);
    if (parsed.ec == std::errc::result_out_of_range &&
        parsed.ptr == value.data() + value.size()) {
        const auto order = decimal_order(value);
        if (order && (order->zero || order->value <= -324)) {
            const bool negative = !value.empty() && value.front() == '-';
            return std::copysign(0.0, negative ? -1.0 : 1.0);
        }
    }
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(result))
        return std::nullopt;
    return result;
}

}  // namespace apex::core
