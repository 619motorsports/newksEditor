#include "apex/core/deflate.hpp"
#include "apex/core/parse_error.hpp"
#include "apex/formats/png.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using apex::core::ParseError;
using apex::formats::decodePngRgba8;
using apex::formats::PngImage;
using apex::formats::PngLimits;

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> bytesFromHex(std::string_view hex) {
  require(hex.size() % 2u == 0u, "hex fixture length");
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    throw std::runtime_error("invalid hex fixture");
  };
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2u);
  for (std::size_t offset = 0u; offset < hex.size(); offset += 2u)
    bytes.push_back(static_cast<std::uint8_t>(
        static_cast<unsigned>(nibble(hex[offset])) * 16u +
        nibble(hex[offset + 1u])));
  return bytes;
}

std::uint32_t crcUpdate(std::uint32_t crc, std::uint8_t value) {
  crc ^= value;
  for (std::size_t bit = 0u; bit < 8u; ++bit)
    crc = (crc & 1u) != 0u ? (crc >> 1u) ^ 0xedb88320u : crc >> 1u;
  return crc;
}

std::uint32_t crc(std::string_view type, std::span<const std::uint8_t> data) {
  std::uint32_t value = 0xffffffffu;
  for (const auto character : type)
    value = crcUpdate(value, static_cast<std::uint8_t>(character));
  for (const auto byte : data)
    value = crcUpdate(value, byte);
  return ~value;
}

std::uint32_t adler(std::span<const std::uint8_t> data) {
  constexpr std::uint32_t modulo = 65521u;
  std::uint32_t a = 1u;
  std::uint32_t b = 0u;
  for (const auto byte : data) {
    a = (a + byte) % modulo;
    b = (b + a) % modulo;
  }
  return (b << 16u) | a;
}

void appendBe32(std::vector<std::uint8_t> &output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24u));
  output.push_back(static_cast<std::uint8_t>(value >> 16u));
  output.push_back(static_cast<std::uint8_t>(value >> 8u));
  output.push_back(static_cast<std::uint8_t>(value));
}

void appendChunk(std::vector<std::uint8_t> &output, std::string_view type,
                 std::span<const std::uint8_t> data) {
  require(type.size() == 4u, "PNG test chunk type size");
  appendBe32(output, static_cast<std::uint32_t>(data.size()));
  for (const auto character : type)
    output.push_back(static_cast<std::uint8_t>(character));
  output.insert(output.end(), data.begin(), data.end());
  appendBe32(output, crc(type, data));
}

std::vector<std::uint8_t> zlibStored(std::span<const std::uint8_t> data) {
  require(data.size() <= 65535u, "stored PNG fixture is bounded");
  std::vector<std::uint8_t> output{0x78u, 0x01u, 0x01u};
  const auto length = static_cast<std::uint16_t>(data.size());
  output.push_back(static_cast<std::uint8_t>(length));
  output.push_back(static_cast<std::uint8_t>(length >> 8u));
  const auto inverse = static_cast<std::uint16_t>(~length);
  output.push_back(static_cast<std::uint8_t>(inverse));
  output.push_back(static_cast<std::uint8_t>(inverse >> 8u));
  output.insert(output.end(), data.begin(), data.end());
  appendBe32(output, adler(data));
  return output;
}

std::vector<std::uint8_t> png(std::uint32_t width, std::uint32_t height,
                              std::uint8_t colorType,
                              std::span<const std::uint8_t> scanlines,
                              std::span<const std::uint8_t> palette = {},
                              std::span<const std::uint8_t> transparency = {},
                              bool splitIdat = false,
                              bool interruptIdat = false) {
  std::vector<std::uint8_t> output = {0x89u, 0x50u, 0x4eu, 0x47u,
                                      0x0du, 0x0au, 0x1au, 0x0au};
  std::array<std::uint8_t, 13> header{};
  header[0] = static_cast<std::uint8_t>(width >> 24u);
  header[1] = static_cast<std::uint8_t>(width >> 16u);
  header[2] = static_cast<std::uint8_t>(width >> 8u);
  header[3] = static_cast<std::uint8_t>(width);
  header[4] = static_cast<std::uint8_t>(height >> 24u);
  header[5] = static_cast<std::uint8_t>(height >> 16u);
  header[6] = static_cast<std::uint8_t>(height >> 8u);
  header[7] = static_cast<std::uint8_t>(height);
  header[8] = 8u;
  header[9] = colorType;
  appendChunk(output, "IHDR", header);
  if (!palette.empty())
    appendChunk(output, "PLTE", palette);
  if (!transparency.empty())
    appendChunk(output, "tRNS", transparency);
  const auto compressed = zlibStored(scanlines);
  if (splitIdat) {
    const auto split = compressed.size() / 2u;
    appendChunk(output, "IDAT",
                std::span<const std::uint8_t>(compressed).first(split));
    if (interruptIdat)
      appendChunk(output, "tEXt", std::span<const std::uint8_t>{});
    appendChunk(output, "IDAT",
                std::span<const std::uint8_t>(compressed).subspan(split));
  } else {
    appendChunk(output, "IDAT", compressed);
  }
  appendChunk(output, "IEND", std::span<const std::uint8_t>{});
  return output;
}

void repairChunkCrc(std::vector<std::uint8_t> &bytes, std::size_t chunkOffset) {
  require(chunkOffset + 12u <= bytes.size(), "PNG test chunk offset");
  const auto length =
      (static_cast<std::uint32_t>(bytes[chunkOffset]) << 24u) |
      (static_cast<std::uint32_t>(bytes[chunkOffset + 1u]) << 16u) |
      (static_cast<std::uint32_t>(bytes[chunkOffset + 2u]) << 8u) |
      bytes[chunkOffset + 3u];
  const auto typeOffset = chunkOffset + 4u;
  const auto dataOffset = chunkOffset + 8u;
  const auto value =
      crc(std::string_view(
              reinterpret_cast<const char *>(bytes.data() + typeOffset), 4u),
          std::span<const std::uint8_t>(bytes).subspan(dataOffset, length));
  bytes[dataOffset + length] = static_cast<std::uint8_t>(value >> 24u);
  bytes[dataOffset + length + 1u] = static_cast<std::uint8_t>(value >> 16u);
  bytes[dataOffset + length + 2u] = static_cast<std::uint8_t>(value >> 8u);
  bytes[dataOffset + length + 3u] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> onePixelFixture() {
  return {0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u,
          0x00u, 0x0du, 0x49u, 0x48u, 0x44u, 0x52u, 0x00u, 0x00u, 0x00u, 0x01u,
          0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x04u, 0x00u, 0x00u, 0x00u, 0xb5u,
          0x1cu, 0x0cu, 0x02u, 0x00u, 0x00u, 0x00u, 0x0bu, 0x49u, 0x44u, 0x41u,
          0x54u, 0x78u, 0xdau, 0x63u, 0x64u, 0xf8u, 0x0fu, 0x00u, 0x01u, 0x05u,
          0x01u, 0x01u, 0x27u, 0x18u, 0xe3u, 0x66u, 0x00u, 0x00u, 0x00u, 0x00u,
          0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
}

template <typename Function>
void expectsError(Function &&function, std::string_view code) {
  try {
    function();
  } catch (const ParseError &error) {
    require(error.format() == "PNG" && error.code() == code,
            "unexpected PNG parse error");
    require(error.source() == "fixture.png",
            "PNG diagnostic source attribution");
    return;
  }
  throw std::runtime_error("expected PNG ParseError");
}

void decodesObservedAndBoundedColorTypes() {
  const auto embedded = decodePngRgba8(onePixelFixture(), "fixture.png");
  require(embedded.width == 1u && embedded.height == 1u &&
              embedded.pixels.size() == 4u,
          "embedded PNG dimensions");
  require(embedded.pixels == std::vector<std::uint8_t>({0u, 0u, 0u, 255u}),
          "embedded PNG RGBA pixels");

  const std::vector<std::uint8_t> rgbScanlines = {0u,  10u, 20u, 30u,
                                                  40u, 50u, 60u};
  const auto rgb = decodePngRgba8(png(2u, 1u, 2u, rgbScanlines), "fixture.png");
  require(rgb.pixels == std::vector<std::uint8_t>(
                            {10u, 20u, 30u, 255u, 40u, 50u, 60u, 255u}),
          "PNG RGB conversion");
  const std::vector<std::uint8_t> suggestedPalette = {1u, 2u, 3u};
  const auto rgbWithPalette = decodePngRgba8(
      png(2u, 1u, 2u, rgbScanlines, suggestedPalette), "fixture.png");
  require(rgbWithPalette.pixels == rgb.pixels,
          "PNG RGB accepts and ignores its optional suggested palette");

  const std::vector<std::uint8_t> indexedScanlines = {0u, 0u, 1u};
  const std::vector<std::uint8_t> palette = {255u, 0u, 0u, 0u, 0u, 255u};
  const std::vector<std::uint8_t> transparency = {128u, 255u};
  const auto indexed = decodePngRgba8(
      png(2u, 1u, 3u, indexedScanlines, palette, transparency), "fixture.png");
  require(indexed.pixels == std::vector<std::uint8_t>(
                                {255u, 0u, 0u, 128u, 0u, 0u, 255u, 255u}),
          "PNG indexed conversion");

  const std::vector<std::uint8_t> grayScanlines = {0u, 77u, 99u};
  const auto grayAlpha =
      decodePngRgba8(png(1u, 1u, 4u, grayScanlines), "fixture.png");
  require(grayAlpha.pixels == std::vector<std::uint8_t>({77u, 77u, 77u, 99u}),
          "PNG grayscale-alpha conversion");

  const auto split = decodePngRgba8(
      png(1u, 1u, 6u, std::vector<std::uint8_t>{0u, 1u, 2u, 3u, 4u}, {}, {},
          true),
      "fixture.png");
  require(split.pixels == std::vector<std::uint8_t>({1u, 2u, 3u, 4u}),
          "consecutive IDAT chunks are concatenated before one inflate");
}

void decodesAllFilters() {
  // Two RGB pixels. Each filtered row is encoded from the same source row.
  const std::array<std::uint8_t, 6> source = {10u, 20u, 30u, 40u, 50u, 60u};
  // Decode one row at a time because each row is a separate fixture.
  for (std::uint8_t filter = 0u; filter <= 4u; ++filter) {
    std::vector<std::uint8_t> row{filter};
    for (std::size_t index = 0u; index < source.size(); ++index) {
      const auto left = index >= 3u ? source[index - 3u] : 0u;
      const auto predictor = filter == 0u                   ? 0u
                             : filter == 1u || filter == 4u ? left
                             : filter == 3u
                                 ? static_cast<std::uint8_t>(left / 2u)
                                 : 0u;
      row.push_back(static_cast<std::uint8_t>(source[index] - predictor));
    }
    const auto image = decodePngRgba8(png(2u, 1u, 2u, row), "fixture.png");
    require(image.pixels[0] == 10u && image.pixels[1] == 20u &&
                image.pixels[2] == 30u && image.pixels[4] == 40u &&
                image.pixels[5] == 50u && image.pixels[6] == 60u,
            "PNG filter reconstruction");
  }
}

void rejectsTruncationAndMalformedChunks() {
  const auto valid = onePixelFixture();
  for (std::size_t length = 0u; length < valid.size(); ++length) {
    const std::vector<std::uint8_t> prefix(
        valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
    expectsError([&] { (void)decodePngRgba8(prefix, "fixture.png"); },
                 "TRUNCATED");
  }
  auto badCrc = valid;
  badCrc[31u] ^= 1u;
  expectsError([&] { (void)decodePngRgba8(badCrc, "fixture.png"); }, "CRC");
  auto trailing = valid;
  trailing.push_back(0u);
  expectsError([&] { (void)decodePngRgba8(trailing, "fixture.png"); },
               "TRAILING_BYTES");

  auto invalidSignature = valid;
  invalidSignature[0] = 0u;
  expectsError([&] { (void)decodePngRgba8(invalidSignature, "fixture.png"); },
               "SIGNATURE");
  const std::vector<std::uint8_t> badPaletteBytes = {255u, 0u};
  auto badPalette =
      png(1u, 1u, 3u, std::vector<std::uint8_t>{0u, 0u}, badPaletteBytes, {});
  expectsError([&] { (void)decodePngRgba8(badPalette, "fixture.png"); },
               "PALETTE");
  const std::vector<std::uint8_t> badIndexPalette = {255u, 0u, 0u};
  auto badIndex =
      png(1u, 1u, 3u, std::vector<std::uint8_t>{0u, 1u}, badIndexPalette, {});
  expectsError([&] { (void)decodePngRgba8(badIndex, "fixture.png"); },
               "PALETTE_INDEX");

  const auto noncontiguous =
      png(1u, 1u, 6u, std::vector<std::uint8_t>{0u, 1u, 2u, 3u, 4u}, {}, {},
          true, true);
  expectsError([&] { (void)decodePngRgba8(noncontiguous, "fixture.png"); },
               "CHUNK_ORDER");
}

void rejectsUnsupportedModesAndLimits() {
  auto unsupportedDepth = onePixelFixture();
  unsupportedDepth[24u] = 16u;
  repairChunkCrc(unsupportedDepth, 8u);
  expectsError([&] { (void)decodePngRgba8(unsupportedDepth, "fixture.png"); },
               "UNSUPPORTED_BIT_DEPTH");

  auto limits = PngLimits{};
  limits.maxDimension = 1u;
  const auto large =
      png(2u, 1u, 2u, std::vector<std::uint8_t>{0u, 1u, 2u, 3u, 4u, 5u, 6u});
  expectsError([&] { (void)decodePngRgba8(large, "fixture.png", limits); },
               "DIMENSION_LIMIT");
  limits = {};
  limits.maxChunks = 1u;
  expectsError(
      [&] { (void)decodePngRgba8(onePixelFixture(), "fixture.png", limits); },
      "CHUNK_LIMIT");
  limits = {};
  limits.parse.maxOutputBytes = 3u;
  expectsError(
      [&] { (void)decodePngRgba8(onePixelFixture(), "fixture.png", limits); },
      "OUTPUT_TOO_LARGE");

  auto filtered = png(1u, 1u, 2u, std::vector<std::uint8_t>{5u, 1u, 2u, 3u});
  expectsError([&] { (void)decodePngRgba8(filtered, "fixture.png"); },
               "FILTER");
}

void rejectsSharedDeflateLimitsAndTrailingBytes() {
  const std::array<std::uint8_t, 3> payload = {1u, 2u, 3u};
  const auto zlib = zlibStored(payload);
  apex::core::ParseLimits limits;
  limits.maxOutputBytes = 2u;
  try {
    (void)apex::core::inflateZlib(zlib, payload.size(), "fixture.zlib", limits);
    throw std::runtime_error("expected DEFLATE output limit error");
  } catch (const ParseError &error) {
    require(error.format() == "DEFLATE" && error.code() == "LIMIT",
            "shared DEFLATE rejects expected output above its maximum");
  }

  std::vector<std::uint8_t> raw(zlib.begin() + 2, zlib.end() - 4);
  raw.push_back(0u);
  try {
    (void)apex::core::inflateDeflateRaw(raw, payload.size(), "fixture.deflate");
    throw std::runtime_error("expected DEFLATE trailing-byte error");
  } catch (const ParseError &error) {
    require(error.format() == "DEFLATE" && error.code() == "TRAILING_BYTES",
            "shared raw DEFLATE rejects bytes after its final block");
  }

  // Generated with zlib's Huffman-only strategy. This is a valid dynamic
  // all-literal block with no usable distance codes.
  const auto dynamic = bytesFromHex(
      "05c1010203210803b0965214d4dbff7fbb2458b09048404dd6b51e26c868ba6480"
      "2bc319bdc29e151fb599a8ed896567df2cacfa75a89004364383f4f484348f8c48"
      "0724dfe55a387b5a4201a400429862489c5f26a068e29a86bf59216eef607fd3bd"
      "690111ebe5fdae48b24e6735eaec9ddcea27dd45f234fb2b9978e59bd38c088778"
      "943e3a6923f907");
  const auto dynamicExpected = bytesFromHex(
      "02010600050300040004000003090101060c05030d000a02010102090105060305"
      "00000107040205040209070205050a07020e0103080104000608050a0207050504"
      "090c04060007060f090203060004010000080102030a0004050a090a0203030a0d"
      "01010202040502000303050c07050607000b080a09030300060000010103000001"
      "0003000a0601020303010a0f0404000003020901000c05010500050e0a07020301"
      "0805080201090e0a09090801050300000202070d040c0e0c0301010101060b0904"
      "060900060b080804010803090d03030c070101010b0901090e06030501000d0605"
      "0c040a0901020202050203010b0304050b030b040505000401");
  require(apex::core::inflateDeflateRaw(dynamic, dynamicExpected.size(),
                                        "dynamic.deflate") == dynamicExpected,
          "shared DEFLATE accepts an all-literal dynamic block");
}

} // namespace

int main() {
  try {
    decodesObservedAndBoundedColorTypes();
    decodesAllFilters();
    rejectsTruncationAndMalformedChunks();
    rejectsUnsupportedModesAndLimits();
    rejectsSharedDeflateLimitsAndTrailingBytes();
    std::cout << "png tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "png tests failed: " << error.what() << '\n';
    return 1;
  }
}
