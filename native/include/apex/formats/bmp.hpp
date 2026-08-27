#pragma once

#include "apex/core/parse_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

struct BmpLimits {
  apex::core::ParseLimits parse{};
  std::uint32_t maxDimension = 32768u;
};

struct BmpImage {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::vector<std::uint8_t> pixels;
  std::size_t bytesRead = 0u;
};

// Decode the observed uncompressed 24-bit Windows BMP layout into RGBA8.
[[nodiscard]] BmpImage decodeBmpRgba8(std::span<const std::uint8_t> bytes,
                                      std::string source = "texture.bmp",
                                      BmpLimits limits = {});

[[nodiscard]] inline BmpImage
decode_bmp_rgba8(std::span<const std::uint8_t> bytes,
                 std::string source = "texture.bmp", BmpLimits limits = {}) {
  return decodeBmpRgba8(bytes, std::move(source), std::move(limits));
}

} // namespace apex::formats
