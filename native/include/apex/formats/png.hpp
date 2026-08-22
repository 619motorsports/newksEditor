#pragma once

#include "apex/core/parse_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

struct PngLimits {
  apex::core::ParseLimits parse{};
  std::size_t maxChunks = 1'000'000;
  std::size_t maxChunkBytes = 64u * 1024u * 1024u;
  std::size_t maxIdatBytes = 256u * 1024u * 1024u;
  std::size_t maxDecompressedBytes = 512u * 1024u * 1024u;
  std::uint32_t maxDimension = 32768u;
};

struct PngImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bitDepth = 0;
  std::uint8_t colorType = 0;
  std::uint8_t interlaceMethod = 0;
  // Top-to-bottom, tightly packed RGBA8 pixels.
  std::vector<std::uint8_t> pixels;
  std::size_t bytesRead = 0;
};

// Decode the bounded, non-interlaced 8-bit PNG subset used by the editor's
// observed asset corpus. Supported color types are grayscale, RGB, indexed,
// grayscale-alpha, and RGBA. Unsupported bit depths and interlace modes are
// rejected explicitly; no image fallback is guessed.
[[nodiscard]] PngImage decodePngRgba8(std::span<const std::uint8_t> bytes,
                                      std::string source = "texture.png",
                                      PngLimits limits = {});

[[nodiscard]] inline PngImage
decode_png_rgba8(std::span<const std::uint8_t> bytes,
                 std::string source = "texture.png", PngLimits limits = {}) {
  return decodePngRgba8(bytes, std::move(source), std::move(limits));
}

} // namespace apex::formats
