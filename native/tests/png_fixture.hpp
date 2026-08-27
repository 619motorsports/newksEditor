#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace apex::tests {
namespace detail {

inline std::uint32_t pngCrcUpdate(std::uint32_t crc,
                                  std::uint8_t value) noexcept {
  crc ^= value;
  for (std::size_t bit = 0U; bit < 8U; ++bit)
    crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
  return crc;
}

inline void appendBe32(std::vector<std::uint8_t> &output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

inline void appendPngChunk(std::vector<std::uint8_t> &output,
                           std::string_view type,
                           std::span<const std::uint8_t> payload) {
  appendBe32(output, static_cast<std::uint32_t>(payload.size()));
  std::uint32_t crc = 0xffffffffU;
  for (const char value : type) {
    const auto byte = static_cast<std::uint8_t>(value);
    output.push_back(byte);
    crc = pngCrcUpdate(crc, byte);
  }
  for (const std::uint8_t byte : payload) {
    output.push_back(byte);
    crc = pngCrcUpdate(crc, byte);
  }
  appendBe32(output, ~crc);
}

inline std::uint32_t adler32(std::span<const std::uint8_t> bytes) noexcept {
  constexpr std::uint32_t modulus = 65521U;
  std::uint32_t first = 1U;
  std::uint32_t second = 0U;
  for (const std::uint8_t byte : bytes) {
    first = (first + byte) % modulus;
    second = (second + first) % modulus;
  }
  return (second << 16U) | first;
}

} // namespace detail

inline std::vector<std::uint8_t>
rgba8PngFixture(std::array<std::uint8_t, 4> pixel) {
  std::vector<std::uint8_t> output = {0x89U, 0x50U, 0x4eU, 0x47U,
                                      0x0dU, 0x0aU, 0x1aU, 0x0aU};
  const std::array<std::uint8_t, 13> header = {0U, 0U, 0U, 1U, 0U, 0U, 0U,
                                               1U, 8U, 6U, 0U, 0U, 0U};
  detail::appendPngChunk(output, "IHDR", header);

  const std::array<std::uint8_t, 5> scanline = {0U, pixel[0], pixel[1],
                                                pixel[2], pixel[3]};
  std::vector<std::uint8_t> compressed = {0x78U, 0x01U, 0x01U, 0x05U,
                                          0x00U, 0xfaU, 0xffU};
  compressed.insert(compressed.end(), scanline.begin(), scanline.end());
  detail::appendBe32(compressed, detail::adler32(scanline));
  detail::appendPngChunk(output, "IDAT", compressed);
  detail::appendPngChunk(output, "IEND", std::span<const std::uint8_t>{});
  return output;
}

} // namespace apex::tests
