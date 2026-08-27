#include "apex/formats/bmp.hpp"

#include "apex/core/parse_error.hpp"

#include <bit>
#include <limits>
#include <string>
#include <string_view>

namespace apex::formats {
namespace {

using apex::core::ParseError;

ParseError error(std::string_view source, std::size_t offset,
                 std::string_view code, std::string_view message) {
  return ParseError("BMP", std::string(source), offset, std::string(code),
                    std::string(message));
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

} // namespace

BmpImage decodeBmpRgba8(std::span<const std::uint8_t> bytes, std::string source,
                        BmpLimits limits) {
  if (limits.parse.maxInputBytes == 0U || limits.parse.maxOutputBytes == 0U ||
      limits.maxDimension == 0U) {
    throw error(source, 0U, "LIMIT", "BMP limits must be nonzero");
  }
  if (bytes.size() > limits.parse.maxInputBytes) {
    throw error(source, 0U, "INPUT_TOO_LARGE",
                "BMP input exceeds the configured limit");
  }
  if (bytes.size() < 14U) {
    throw error(source, 0U, "TRUNCATED", "BMP file header is truncated");
  }
  if (bytes[0] != static_cast<std::uint8_t>('B') ||
      bytes[1] != static_cast<std::uint8_t>('M')) {
    throw error(source, 0U, "SIGNATURE", "BMP signature is invalid");
  }
  if (bytes.size() < 54U) {
    throw error(source, 14U, "TRUNCATED", "BMP info header is truncated");
  }

  const auto declared_file_size = static_cast<std::size_t>(read_u32(bytes, 2U));
  const auto pixel_offset = static_cast<std::size_t>(read_u32(bytes, 10U));
  const auto header_size = static_cast<std::size_t>(read_u32(bytes, 14U));
  if (header_size != 40U) {
    throw error(source, 14U, "HEADER",
                "BMP requires the observed 40-byte info header");
  }
  if (pixel_offset < 14U + header_size || pixel_offset > bytes.size()) {
    throw error(source, 10U, "HEADER", "BMP pixel offset is invalid");
  }
  if (declared_file_size == 0U) {
    throw error(source, 2U, "HEADER", "BMP declared file size is invalid");
  }
  if (declared_file_size > bytes.size()) {
    throw error(source, bytes.size(), "TRUNCATED",
                "BMP declared file size exceeds the input");
  }

  const auto width_raw = std::bit_cast<std::int32_t>(read_u32(bytes, 18U));
  const auto height_raw = std::bit_cast<std::int32_t>(read_u32(bytes, 22U));
  if (width_raw <= 0 || height_raw == 0 ||
      height_raw == std::numeric_limits<std::int32_t>::min()) {
    throw error(source, 18U, "DIMENSION", "BMP dimensions are invalid");
  }
  const auto width = static_cast<std::uint32_t>(width_raw);
  const auto height =
      static_cast<std::uint32_t>(height_raw < 0 ? -height_raw : height_raw);
  if (width > limits.maxDimension || height > limits.maxDimension) {
    throw error(source, 18U, "DIMENSION_LIMIT",
                "BMP dimensions exceed the configured limit");
  }
  if (read_u16(bytes, 26U) != 1U) {
    throw error(source, 26U, "HEADER", "BMP planes must be one");
  }
  if (read_u16(bytes, 28U) != 24U) {
    throw error(source, 28U, "UNSUPPORTED_FORMAT",
                "BMP requires the observed 24-bit pixel layout");
  }
  if (read_u32(bytes, 30U) != 0U) {
    throw error(source, 30U, "UNSUPPORTED_FORMAT",
                "Compressed BMP pixels are unsupported");
  }

  constexpr std::size_t bytes_per_pixel = 3U;
  const auto width_size = static_cast<std::size_t>(width);
  const auto height_size = static_cast<std::size_t>(height);
  if (width_size >
      (std::numeric_limits<std::size_t>::max() - 3U) / bytes_per_pixel) {
    throw error(source, 18U, "SIZE_OVERFLOW", "BMP row size overflows");
  }
  const auto unpadded_row = width_size * bytes_per_pixel;
  const auto row_bytes = (unpadded_row + 3U) & ~std::size_t{3U};
  if (height_size > std::numeric_limits<std::size_t>::max() / row_bytes) {
    throw error(source, 22U, "SIZE_OVERFLOW",
                "BMP pixel storage size overflows");
  }
  const auto storage_bytes = height_size * row_bytes;
  if (storage_bytes > bytes.size() - pixel_offset) {
    throw error(source, pixel_offset, "TRUNCATED",
                "BMP pixel data is truncated");
  }
  const auto pixel_end = pixel_offset + storage_bytes;
  if (declared_file_size < pixel_end) {
    throw error(source, 2U, "HEADER",
                "BMP declared file size excludes pixel data");
  }
  if (width_size > std::numeric_limits<std::size_t>::max() / height_size ||
      width_size * height_size > std::numeric_limits<std::size_t>::max() / 4U) {
    throw error(source, 18U, "SIZE_OVERFLOW", "BMP output size overflows");
  }
  const auto output_bytes = width_size * height_size * 4U;
  if (output_bytes > limits.parse.maxOutputBytes) {
    throw error(source, 0U, "OUTPUT_TOO_LARGE",
                "BMP decoded pixels exceed the configured limit");
  }

  BmpImage result;
  result.width = width;
  result.height = height;
  result.bytesRead = pixel_end;
  result.pixels.resize(output_bytes);
  const bool top_down = height_raw < 0;
  for (std::size_t row = 0U; row < height_size; ++row) {
    const auto source_row = pixel_offset + row * row_bytes;
    const auto output_row = top_down ? row : height_size - 1U - row;
    for (std::size_t column = 0U; column < width_size; ++column) {
      const auto input = source_row + column * bytes_per_pixel;
      const auto output = (output_row * width_size + column) * 4U;
      result.pixels[output] = bytes[input + 2U];
      result.pixels[output + 1U] = bytes[input + 1U];
      result.pixels[output + 2U] = bytes[input];
      result.pixels[output + 3U] = 255U;
    }
  }
  return result;
}

} // namespace apex::formats
