#include "apex/core/parse_error.hpp"
#include "apex/formats/bmp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

void append_u16(std::vector<std::uint8_t> &output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t> &output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void write_u32(std::vector<std::uint8_t> &output, std::size_t offset,
               std::uint32_t value) {
  output[offset] = static_cast<std::uint8_t>(value);
  output[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
  output[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> fixture(bool top_down = false) {
  std::vector<std::uint8_t> output;
  output.reserve(70U);
  output.push_back(static_cast<std::uint8_t>('B'));
  output.push_back(static_cast<std::uint8_t>('M'));
  append_u32(output, 70U);
  append_u16(output, 0U);
  append_u16(output, 0U);
  append_u32(output, 54U);
  append_u32(output, 40U);
  append_u32(output, 2U);
  append_u32(output, top_down ? 0xfffffffeU : 2U);
  append_u16(output, 1U);
  append_u16(output, 24U);
  append_u32(output, 0U);
  append_u32(output, 16U);
  append_u32(output, 0U);
  append_u32(output, 0U);
  append_u32(output, 0U);
  append_u32(output, 0U);
  const std::array<std::uint8_t, 8U> bottom = {255U, 0U,   0U, 255U,
                                               255U, 255U, 0U, 0U};
  const std::array<std::uint8_t, 8U> top = {0U, 0U, 255U, 0U, 255U, 0U, 0U, 0U};
  if (top_down) {
    output.insert(output.end(), top.begin(), top.end());
    output.insert(output.end(), bottom.begin(), bottom.end());
  } else {
    output.insert(output.end(), bottom.begin(), bottom.end());
    output.insert(output.end(), top.begin(), top.end());
  }
  return output;
}

template <typename Function>
void expects_error(
    Function &&function, std::string_view code,
    std::size_t offset = std::numeric_limits<std::size_t>::max()) {
  try {
    function();
  } catch (const apex::core::ParseError &error) {
    require(error.format() == "BMP" && error.code() == code,
            "unexpected BMP error code");
    require(error.source() == "fixture.bmp", "BMP error source attribution");
    if (offset != std::numeric_limits<std::size_t>::max()) {
      require(error.offset() == offset, "unexpected BMP error offset");
    }
    return;
  }
  throw std::runtime_error("invalid BMP input was accepted");
}

void decodes_observed_bottom_up_and_top_down_layouts() {
  const auto image = apex::formats::decodeBmpRgba8(fixture(), "fixture.bmp");
  require(image.width == 2U && image.height == 2U && image.bytesRead == 70U,
          "BMP dimensions and consumed bytes");
  require(image.pixels == std::vector<std::uint8_t>(
                              {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U, 0U, 0U,
                               255U, 255U, 255U, 255U, 255U, 255U}),
          "bottom-up BGR conversion");
  const auto top_down =
      apex::formats::decodeBmpRgba8(fixture(true), "fixture.bmp");
  require(top_down.pixels == image.pixels, "top-down BGR conversion");

  auto trailing = fixture();
  trailing.push_back(99U);
  const auto with_trailing =
      apex::formats::decodeBmpRgba8(trailing, "fixture.bmp");
  require(with_trailing.bytesRead == 70U &&
              with_trailing.pixels == image.pixels,
          "trailing input does not change the decoded pixel boundary");
}

void rejects_every_truncated_boundary() {
  const auto valid = fixture();
  for (std::size_t length = 0U; length < valid.size(); ++length) {
    expects_error(
        [&] {
          (void)apex::formats::decodeBmpRgba8(
              std::span<const std::uint8_t>(valid.data(), length),
              "fixture.bmp");
        },
        "TRUNCATED");
  }
}

void rejects_malformed_headers_counts_offsets_and_limits() {
  const auto valid = fixture();
  auto changed = valid;
  changed[0] = 0U;
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "SIGNATURE", 0U);

  changed = valid;
  write_u32(changed, 14U, 0xffffffffU);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "HEADER", 14U);

  changed = valid;
  write_u32(changed, 14U, 41U);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "HEADER", 14U);

  changed = valid;
  write_u32(changed, 10U, 53U);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "HEADER", 10U);

  changed = valid;
  write_u32(changed, 2U, 69U);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "HEADER", 2U);

  changed = valid;
  write_u32(changed, 2U, 0U);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "HEADER", 2U);

  changed = valid;
  write_u32(changed, 18U, 0U);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "DIMENSION", 18U);

  changed = valid;
  write_u32(changed, 22U, 0x80000000U);
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "DIMENSION", 18U);

  changed = valid;
  changed[26U] = 2U;
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "HEADER", 26U);

  changed = valid;
  changed[28U] = 32U;
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "UNSUPPORTED_FORMAT", 28U);

  changed = valid;
  changed[30U] = 1U;
  expects_error(
      [&] { (void)apex::formats::decodeBmpRgba8(changed, "fixture.bmp"); },
      "UNSUPPORTED_FORMAT", 30U);

  auto limits = apex::formats::BmpLimits{};
  limits.parse.maxOutputBytes = 15U;
  expects_error(
      [&] {
        (void)apex::formats::decodeBmpRgba8(valid, "fixture.bmp", limits);
      },
      "OUTPUT_TOO_LARGE", 0U);

  limits = {};
  limits.parse.maxInputBytes = valid.size() - 1U;
  expects_error(
      [&] {
        (void)apex::formats::decodeBmpRgba8(valid, "fixture.bmp", limits);
      },
      "INPUT_TOO_LARGE", 0U);

  limits = {};
  limits.maxDimension = 1U;
  expects_error(
      [&] {
        (void)apex::formats::decodeBmpRgba8(valid, "fixture.bmp", limits);
      },
      "DIMENSION_LIMIT", 18U);

  limits = {};
  limits.parse.maxInputBytes = 0U;
  expects_error(
      [&] {
        (void)apex::formats::decodeBmpRgba8(valid, "fixture.bmp", limits);
      },
      "LIMIT", 0U);
}

} // namespace

int main() {
  try {
    decodes_observed_bottom_up_and_top_down_layouts();
    rejects_every_truncated_boundary();
    rejects_malformed_headers_counts_offsets_and_limits();
    std::cout << "BMP tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "BMP tests failed: " << error.what() << '\n';
    return 1;
  }
}
