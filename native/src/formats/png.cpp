#include "apex/formats/png.hpp"

#include "apex/core/deflate.hpp"
#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::formats {
namespace {

using apex::core::ParseError;

constexpr std::array<std::uint8_t, 8> signature = {0x89u, 0x50u, 0x4eu, 0x47u,
                                                   0x0du, 0x0au, 0x1au, 0x0au};

[[nodiscard]] ParseError pngError(std::string_view source, std::size_t offset,
                                  std::string_view code,
                                  std::string_view message) {
  return ParseError("PNG", std::string(source), offset, std::string(code),
                    std::string(message));
}

[[nodiscard]] std::uint32_t readBe32(std::span<const std::uint8_t> bytes,
                                     std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
         (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u) |
         static_cast<std::uint32_t>(bytes[offset + 3u]);
}

[[nodiscard]] std::size_t checkedMultiply(std::size_t left, std::size_t right,
                                          std::string_view source,
                                          std::size_t offset,
                                          std::string_view what) {
  if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
    throw pngError(source, offset, "SIZE_OVERFLOW",
                   std::string(what) + " size overflows");
  return left * right;
}

[[nodiscard]] std::uint32_t crcUpdate(std::uint32_t crc,
                                      std::uint8_t value) noexcept {
  crc ^= value;
  for (std::size_t bit = 0u; bit < 8u; ++bit)
    crc = (crc & 1u) != 0u ? (crc >> 1u) ^ 0xedb88320u : crc >> 1u;
  return crc;
}

[[nodiscard]] std::uint32_t chunkCrc(std::span<const std::uint8_t> bytes,
                                     std::size_t typeOffset,
                                     std::size_t dataOffset,
                                     std::size_t dataBytes) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t index = 0u; index < 4u; ++index)
    crc = crcUpdate(crc, bytes[typeOffset + index]);
  for (std::size_t index = 0u; index < dataBytes; ++index)
    crc = crcUpdate(crc, bytes[dataOffset + index]);
  return ~crc;
}

[[nodiscard]] bool typeIs(std::span<const std::uint8_t> bytes,
                          std::size_t offset, std::string_view type) noexcept {
  if (type.size() != 4u)
    return false;
  for (std::size_t index = 0u; index < 4u; ++index)
    if (bytes[offset + index] != static_cast<std::uint8_t>(type[index]))
      return false;
  return true;
}

[[nodiscard]] bool criticalType(std::span<const std::uint8_t> bytes,
                                std::size_t offset) noexcept {
  return (bytes[offset] & 0x20u) == 0u;
}

struct ParsedPng {
  PngImage image;
  std::vector<std::uint8_t> idat;
  std::vector<std::uint8_t> palette;
  std::vector<std::uint8_t> transparency;
  std::size_t firstIdatOffset = 0u;
  std::size_t rowBytes = 0u;
  std::size_t channels = 0u;
};

ParsedPng parsePng(std::span<const std::uint8_t> bytes, std::string_view source,
                   const PngLimits &limits) {
  if (limits.maxChunks == 0u || limits.maxChunkBytes == 0u ||
      limits.maxIdatBytes == 0u || limits.maxDecompressedBytes == 0u ||
      limits.maxDimension == 0u || limits.parse.maxInputBytes == 0u ||
      limits.parse.maxOutputBytes == 0u)
    throw pngError(source, 0u, "LIMIT", "PNG limits must be nonzero");
  if (bytes.size() > limits.parse.maxInputBytes)
    throw pngError(source, 0u, "INPUT_TOO_LARGE",
                   "PNG input exceeds configured limit");
  if (bytes.size() < signature.size())
    throw pngError(source, 0u, "TRUNCATED", "PNG signature is truncated");
  if (!std::equal(signature.begin(), signature.end(), bytes.begin()))
    throw pngError(source, 0u, "SIGNATURE", "PNG signature is invalid");

  ParsedPng result;
  std::size_t offset = signature.size();
  std::size_t chunkCount = 0u;
  bool seenHeader = false;
  bool seenPalette = false;
  bool seenTransparency = false;
  bool seenIdat = false;
  bool idatClosed = false;
  bool seenEnd = false;
  while (offset < bytes.size()) {
    const auto chunkOffset = offset;
    if (bytes.size() - offset < 12u)
      throw pngError(source, offset, "TRUNCATED",
                     "PNG chunk header is truncated");
    if (chunkCount++ >= limits.maxChunks)
      throw pngError(source, offset, "CHUNK_LIMIT",
                     "PNG chunk count exceeds configured limit");
    const auto length = static_cast<std::size_t>(readBe32(bytes, offset));
    if (length > limits.maxChunkBytes)
      throw pngError(source, offset, "CHUNK_LIMIT",
                     "PNG chunk exceeds configured limit");
    if (length > bytes.size() - offset - 12u)
      throw pngError(source, offset, "TRUNCATED",
                     "PNG chunk payload is truncated");
    const auto typeOffset = offset + 4u;
    const auto dataOffset = offset + 8u;
    const auto crcOffset = dataOffset + length;
    if (chunkCrc(bytes, typeOffset, dataOffset, length) !=
        readBe32(bytes, crcOffset))
      throw pngError(source, chunkOffset, "CRC",
                     "PNG chunk CRC does not match payload");

    if (!seenHeader && !typeIs(bytes, typeOffset, "IHDR"))
      throw pngError(source, chunkOffset, "CHUNK_ORDER",
                     "PNG IHDR must be the first chunk");
    if (seenEnd)
      throw pngError(source, chunkOffset, "CHUNK_ORDER",
                     "PNG contains data after IEND");

    if (typeIs(bytes, typeOffset, "IHDR")) {
      if (seenHeader || length != 13u)
        throw pngError(source, chunkOffset, "IHDR",
                       "PNG IHDR chunk is invalid");
      seenHeader = true;
      result.image.width = readBe32(bytes, dataOffset);
      result.image.height = readBe32(bytes, dataOffset + 4u);
      result.image.bitDepth = bytes[dataOffset + 8u];
      result.image.colorType = bytes[dataOffset + 9u];
      const auto compression = bytes[dataOffset + 10u];
      const auto filter = bytes[dataOffset + 11u];
      result.image.interlaceMethod = bytes[dataOffset + 12u];
      if (result.image.width == 0u || result.image.height == 0u ||
          result.image.width > limits.maxDimension ||
          result.image.height > limits.maxDimension)
        throw pngError(source, dataOffset, "DIMENSION_LIMIT",
                       "PNG dimensions are invalid or too large");
      if (result.image.bitDepth != 8u)
        throw pngError(source, dataOffset + 8u, "UNSUPPORTED_BIT_DEPTH",
                       "only 8-bit PNG channels are supported");
      if (result.image.colorType != 0u && result.image.colorType != 2u &&
          result.image.colorType != 3u && result.image.colorType != 4u &&
          result.image.colorType != 6u)
        throw pngError(source, dataOffset + 9u, "UNSUPPORTED_COLOR_TYPE",
                       "PNG color type is unsupported");
      if (compression != 0u || filter != 0u)
        throw pngError(source, dataOffset + 10u, "UNSUPPORTED_METHOD",
                       "PNG compression or filter method is unsupported");
      if (result.image.interlaceMethod != 0u)
        throw pngError(source, dataOffset + 12u, "UNSUPPORTED_INTERLACE",
                       "interlaced PNG images are unsupported");
      switch (result.image.colorType) {
      case 0u:
        result.channels = 1u;
        break;
      case 2u:
        result.channels = 3u;
        break;
      case 3u:
        result.channels = 1u;
        break;
      case 4u:
        result.channels = 2u;
        break;
      case 6u:
        result.channels = 4u;
        break;
      default:
        break;
      }
      result.rowBytes =
          checkedMultiply(static_cast<std::size_t>(result.image.width),
                          result.channels, source, dataOffset, "PNG row");
      const auto scanlineBytes = checkedMultiply(
          static_cast<std::size_t>(result.image.height), result.rowBytes + 1u,
          source, dataOffset, "PNG decompressed");
      const auto pixelBytes = checkedMultiply(
          checkedMultiply(static_cast<std::size_t>(result.image.width),
                          static_cast<std::size_t>(result.image.height), source,
                          dataOffset, "PNG pixels"),
          4u, source, dataOffset, "PNG pixels");
      if (scanlineBytes > limits.maxDecompressedBytes ||
          scanlineBytes > limits.parse.maxOutputBytes ||
          pixelBytes > limits.parse.maxOutputBytes)
        throw pngError(source, dataOffset, "OUTPUT_TOO_LARGE",
                       "PNG decoded pixels exceed configured limit");
    } else if (typeIs(bytes, typeOffset, "PLTE")) {
      if (!seenHeader || seenPalette || seenIdat ||
          result.image.colorType == 0u || result.image.colorType == 4u ||
          length == 0u || length % 3u != 0u || length > 768u)
        throw pngError(source, chunkOffset, "PALETTE",
                       "PNG palette chunk is invalid");
      seenPalette = true;
      result.palette.assign(
          bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
          bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + length));
    } else if (typeIs(bytes, typeOffset, "tRNS")) {
      if (!seenHeader || seenTransparency || seenIdat ||
          (result.image.colorType == 3u && !seenPalette))
        throw pngError(source, chunkOffset, "TRANSPARENCY",
                       "PNG transparency chunk is invalid");
      seenTransparency = true;
      result.transparency.assign(
          bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
          bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + length));
    } else if (typeIs(bytes, typeOffset, "IDAT")) {
      if (!seenHeader || idatClosed)
        throw pngError(source, chunkOffset, "CHUNK_ORDER",
                       "PNG IDAT chunk is out of order");
      if (result.idat.size() >
          limits.maxIdatBytes - std::min(length, limits.maxIdatBytes))
        throw pngError(source, chunkOffset, "IDAT_LIMIT",
                       "PNG IDAT bytes exceed configured limit");
      if (length > limits.maxIdatBytes - result.idat.size())
        throw pngError(source, chunkOffset, "IDAT_LIMIT",
                       "PNG IDAT bytes exceed configured limit");
      if (!seenIdat)
        result.firstIdatOffset = dataOffset;
      seenIdat = true;
      result.idat.insert(
          result.idat.end(),
          bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
          bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + length));
    } else if (typeIs(bytes, typeOffset, "IEND")) {
      if (!seenHeader || !seenIdat || length != 0u)
        throw pngError(source, chunkOffset, "CHUNK_ORDER",
                       "PNG IEND chunk is invalid");
      seenEnd = true;
    } else if (criticalType(bytes, typeOffset)) {
      throw pngError(source, chunkOffset, "UNSUPPORTED_CHUNK",
                     "PNG critical chunk is unsupported");
    }

    if (seenIdat && !typeIs(bytes, typeOffset, "IDAT"))
      idatClosed = true;
    offset = crcOffset + 4u;
    if (seenEnd)
      break;
  }
  if (!seenHeader || !seenIdat || !seenEnd)
    throw pngError(source, offset, "TRUNCATED",
                   "PNG is missing IHDR, IDAT, or IEND");
  if (offset != bytes.size())
    throw pngError(source, offset, "TRAILING_BYTES",
                   "PNG has bytes after IEND");
  if (result.image.colorType == 3u && result.palette.empty())
    throw pngError(source, 0u, "PALETTE", "indexed PNG has no PLTE chunk");
  if ((result.image.colorType == 0u || result.image.colorType == 4u) &&
      !result.palette.empty())
    throw pngError(source, 0u, "PALETTE",
                   "PNG PLTE is not valid for this color type");
  const auto paletteEntries = result.palette.size() / 3u;
  if (result.image.colorType == 3u &&
      result.transparency.size() > paletteEntries)
    throw pngError(source, 0u, "TRANSPARENCY",
                   "PNG palette transparency exceeds palette entries");
  if (result.image.colorType == 0u && !result.transparency.empty() &&
      result.transparency.size() != 2u)
    throw pngError(source, 0u, "TRANSPARENCY",
                   "grayscale PNG transparency is invalid");
  if (result.image.colorType == 2u && !result.transparency.empty() &&
      result.transparency.size() != 6u)
    throw pngError(source, 0u, "TRANSPARENCY",
                   "RGB PNG transparency is invalid");
  if ((result.image.colorType == 4u || result.image.colorType == 6u) &&
      !result.transparency.empty())
    throw pngError(source, 0u, "TRANSPARENCY",
                   "PNG transparency is invalid for alpha color types");
  return result;
}

[[nodiscard]] std::uint8_t paeth(std::uint8_t left, std::uint8_t up,
                                 std::uint8_t upperLeft) noexcept {
  const int p = static_cast<int>(left) + static_cast<int>(up) -
                static_cast<int>(upperLeft);
  const int pa = std::abs(p - static_cast<int>(left));
  const int pb = std::abs(p - static_cast<int>(up));
  const int pc = std::abs(p - static_cast<int>(upperLeft));
  if (pa <= pb && pa <= pc)
    return left;
  if (pb <= pc)
    return up;
  return upperLeft;
}

} // namespace

PngImage decodePngRgba8(std::span<const std::uint8_t> bytes, std::string source,
                        PngLimits limits) {
  auto parsed = parsePng(bytes, source, limits);
  const auto scanlineBytes = checkedMultiply(
      static_cast<std::size_t>(parsed.image.height), parsed.rowBytes + 1u,
      source, parsed.firstIdatOffset, "PNG decompressed");
  apex::core::ParseLimits deflateLimits = limits.parse;
  deflateLimits.maxInputBytes =
      std::min(deflateLimits.maxInputBytes, limits.maxIdatBytes);
  deflateLimits.maxOutputBytes =
      std::min(deflateLimits.maxOutputBytes, limits.maxDecompressedBytes);
  std::vector<std::uint8_t> filtered;
  try {
    filtered = apex::core::inflateZlib(parsed.idat, scanlineBytes, source,
                                       deflateLimits, parsed.firstIdatOffset);
  } catch (const ParseError &error) {
    throw pngError(source, parsed.firstIdatOffset + error.offset(),
                   std::string("DEFLATE_") + error.code(), error.what());
  }

  const auto rawBytes = checkedMultiply(
      static_cast<std::size_t>(parsed.image.height), parsed.rowBytes, source,
      parsed.firstIdatOffset, "PNG pixels");
  std::vector<std::uint8_t> raw(rawBytes);
  std::vector<std::uint8_t> previous(parsed.rowBytes, 0u);
  std::vector<std::uint8_t> row(parsed.rowBytes, 0u);
  std::size_t inputOffset = 0u;
  const auto bytesPerPixel = parsed.channels;
  for (std::uint32_t y = 0u; y < parsed.image.height; ++y) {
    const auto filter = filtered[inputOffset++];
    for (std::size_t x = 0u; x < parsed.rowBytes; ++x) {
      const auto encoded = filtered[inputOffset++];
      const auto left = x >= bytesPerPixel ? row[x - bytesPerPixel]
                                           : static_cast<std::uint8_t>(0u);
      const auto up = previous[x];
      const auto upperLeft = x >= bytesPerPixel ? previous[x - bytesPerPixel]
                                                : static_cast<std::uint8_t>(0u);
      switch (filter) {
      case 0u:
        row[x] = encoded;
        break;
      case 1u:
        row[x] = static_cast<std::uint8_t>(encoded + left);
        break;
      case 2u:
        row[x] = static_cast<std::uint8_t>(encoded + up);
        break;
      case 3u:
        row[x] = static_cast<std::uint8_t>(
            encoded +
            static_cast<std::uint8_t>((static_cast<unsigned>(left) + up) / 2u));
        break;
      case 4u:
        row[x] = static_cast<std::uint8_t>(
            static_cast<unsigned>(encoded) +
            static_cast<unsigned>(paeth(left, up, upperLeft)));
        break;
      default:
        throw pngError(source, parsed.firstIdatOffset, "FILTER",
                       "PNG filter type is unsupported");
      }
    }
    std::copy(row.begin(), row.end(),
              raw.begin() + static_cast<std::ptrdiff_t>(y * parsed.rowBytes));
    std::swap(row, previous);
  }

  const auto pixelCount = checkedMultiply(
      static_cast<std::size_t>(parsed.image.width),
      static_cast<std::size_t>(parsed.image.height), source, 0u, "PNG pixels");
  const auto outputBytes =
      checkedMultiply(pixelCount, 4u, source, 0u, "PNG pixels");
  if (outputBytes > limits.parse.maxOutputBytes)
    throw pngError(source, 0u, "OUTPUT_TOO_LARGE",
                   "PNG RGBA output exceeds configured limit");
  parsed.image.pixels.resize(outputBytes);
  for (std::size_t pixel = 0u; pixel < pixelCount; ++pixel) {
    const auto input = raw.data() + pixel * parsed.channels;
    auto output = parsed.image.pixels.data() + pixel * 4u;
    if (parsed.image.colorType == 0u) {
      output[0] = output[1] = output[2] = input[0];
      output[3] =
          parsed.transparency.empty() ||
                  (static_cast<std::uint16_t>(parsed.transparency[0]) << 8u |
                   parsed.transparency[1]) != input[0]
              ? 255u
              : 0u;
    } else if (parsed.image.colorType == 2u) {
      output[0] = input[0];
      output[1] = input[1];
      output[2] = input[2];
      const auto transparent =
          !parsed.transparency.empty() &&
          (static_cast<std::uint16_t>(parsed.transparency[0]) << 8u |
           parsed.transparency[1]) == input[0] &&
          (static_cast<std::uint16_t>(parsed.transparency[2]) << 8u |
           parsed.transparency[3]) == input[1] &&
          (static_cast<std::uint16_t>(parsed.transparency[4]) << 8u |
           parsed.transparency[5]) == input[2];
      output[3] = transparent ? 0u : 255u;
    } else if (parsed.image.colorType == 3u) {
      const auto index = static_cast<std::size_t>(input[0]);
      if (index >= parsed.palette.size() / 3u)
        throw pngError(source, 0u, "PALETTE_INDEX",
                       "PNG palette index is out of range");
      output[0] = parsed.palette[index * 3u];
      output[1] = parsed.palette[index * 3u + 1u];
      output[2] = parsed.palette[index * 3u + 2u];
      output[3] = index < parsed.transparency.size()
                      ? parsed.transparency[index]
                      : 255u;
    } else if (parsed.image.colorType == 4u) {
      output[0] = output[1] = output[2] = input[0];
      output[3] = input[1];
    } else {
      output[0] = input[0];
      output[1] = input[1];
      output[2] = input[2];
      output[3] = input[3];
    }
  }
  parsed.image.bytesRead = bytes.size();
  return std::move(parsed.image);
}

} // namespace apex::formats
