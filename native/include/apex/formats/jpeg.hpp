#pragma once

#include "apex/core/parse_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

struct JpegLimits {
    apex::core::ParseLimits parse{};
    std::uint32_t maxDimension = 32768u;
};

struct JpegImage {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    // Top-to-bottom, tightly packed RGBA8 pixels.
    std::vector<std::uint8_t> pixels;
    std::size_t bytesRead = 0u;
};

// Decode baseline JPEG into bounded RGBA8 pixels. Progressive, CMYK, and
// unsupported precision variants are rejected explicitly.
[[nodiscard]] JpegImage decodeJpegRgba8(std::span<const std::uint8_t> bytes,
                                        std::string source = "texture.jpg",
                                        JpegLimits limits = {});

[[nodiscard]] inline JpegImage decode_jpeg_rgba8(
    std::span<const std::uint8_t> bytes, std::string source = "texture.jpg",
    JpegLimits limits = {}) {
    return decodeJpegRgba8(bytes, std::move(source), std::move(limits));
}

}  // namespace apex::formats
