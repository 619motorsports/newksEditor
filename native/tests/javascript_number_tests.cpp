#include "apex/core/javascript_number.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void accepts_finite_javascript_numbers() {
    using apex::core::parse_finite_javascript_number;
    require(parse_finite_javascript_number("") == 0.0, "empty text");
    require(parse_finite_javascript_number(" \t\n") == 0.0, "ASCII whitespace");
    require(parse_finite_javascript_number(
                std::string("\xc2\xa0") + "0x10" + "\xef\xbb\xbf") == 16.0,
            "Unicode whitespace and hexadecimal");
    require(parse_finite_javascript_number("0b101") == 5.0, "binary");
    require(parse_finite_javascript_number("0o17") == 15.0, "octal");
    require(parse_finite_javascript_number("+12.5e1") == 125.0,
            "positive decimal sign");
    require(parse_finite_javascript_number("1e-323").value_or(0.0) > 0.0,
            "representable subnormal");
}

void preserves_decimal_underflow() {
    using apex::core::parse_finite_javascript_number;
    const auto positive = parse_finite_javascript_number("1e-324");
    const auto negative = parse_finite_javascript_number("-1e-324");
    require(positive && *positive == 0.0 && !std::signbit(*positive),
            "positive underflow");
    require(negative && *negative == 0.0 && std::signbit(*negative),
            "negative underflow");
}

void rejects_nonfinite_and_malformed_numbers() {
    using apex::core::parse_finite_javascript_number;
    for (const auto* value : {"Infinity", "-Infinity", "NaN", "1e9999", "0x",
                              "0b2", "0o8", "+0x1", "-0x1", "1 trailing"}) {
        require(!parse_finite_javascript_number(value), value);
    }
}

}  // namespace

int main() {
    try {
        accepts_finite_javascript_numbers();
        preserves_decimal_underflow();
        rejects_nonfinite_and_malformed_numbers();
        std::cout << "JavaScript number tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "JavaScript number test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
