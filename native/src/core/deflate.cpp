#include "apex/core/deflate.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace apex::core {
namespace {

[[nodiscard]] ParseError deflateError(std::string_view source,
                                      std::size_t offset, std::string_view code,
                                      std::string_view message) {
  return ParseError("DEFLATE", std::string(source), offset, std::string(code),
                    std::string(message));
}

class BitReader final {
public:
  explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t bitPosition() const noexcept {
    return bitPosition_;
  }

  [[nodiscard]] std::uint32_t read(std::size_t count, std::string_view source) {
    if (count > 24u)
      throw deflateError(source, bitPosition_ / 8u, "TRUNCATED",
                         "DEFLATE bitstream is truncated");
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < count; ++index) {
      if (bitPosition_ / 8u >= bytes_.size())
        throw deflateError(source, bitPosition_ / 8u, "TRUNCATED",
                           "DEFLATE bitstream is truncated");
      const auto byte = bytes_[bitPosition_ / 8u];
      const auto bit = (byte >> (bitPosition_ % 8u)) & 1u;
      value |= static_cast<std::uint32_t>(bit) << index;
      ++bitPosition_;
    }
    return value;
  }

  void align(std::string_view source) {
    const auto remainder = bitPosition_ % 8u;
    if (remainder != 0u)
      (void)read(8u - remainder, source);
  }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t bitPosition_ = 0u;
};

[[nodiscard]] std::uint32_t reverseBits(std::uint32_t value,
                                        std::size_t count) noexcept {
  std::uint32_t result = 0u;
  for (std::size_t index = 0u; index < count; ++index) {
    result = (result << 1u) | (value & 1u);
    value >>= 1u;
  }
  return result;
}

template <std::size_t Capacity> class Huffman final {
public:
  void build(std::span<const std::uint8_t> lengths, std::string_view source,
             bool allowSingleIncomplete = false, bool allowEmpty = false) {
    if (lengths.empty() || lengths.size() > Capacity)
      throw deflateError(source, 0u, "HUFFMAN",
                         "invalid DEFLATE Huffman table size");
    lengths_.fill(0u);
    codes_.fill(0u);
    count_.fill(0u);
    symbolCount_ = lengths.size();
    std::size_t nonzero = 0u;
    std::uint8_t maximumLength = 0u;
    for (std::size_t symbol = 0u; symbol < lengths.size(); ++symbol) {
      const auto length = lengths[symbol];
      if (length > 15u)
        throw deflateError(source, 0u, "HUFFMAN",
                           "DEFLATE code length exceeds 15 bits");
      lengths_[symbol] = length;
      if (length != 0u) {
        ++count_[length];
        ++nonzero;
        maximumLength = std::max(maximumLength, length);
      }
    }
    codeCount_ = nonzero;
    if (nonzero == 0u) {
      if (!allowEmpty)
        throw deflateError(source, 0u, "HUFFMAN",
                           "DEFLATE Huffman table has no codes");
      return;
    }

    int left = 1;
    for (std::size_t length = 1u; length <= 15u; ++length) {
      left = (left << 1) - static_cast<int>(count_[length]);
      if (left < 0)
        throw deflateError(source, 0u, "HUFFMAN",
                           "DEFLATE Huffman table is oversubscribed");
    }
    if (left != 0 &&
        !(allowSingleIncomplete && maximumLength == 1u && nonzero == 1u))
      throw deflateError(source, 0u, "HUFFMAN",
                         "DEFLATE Huffman table is incomplete");

    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0u;
    for (std::size_t length = 1u; length <= 15u; ++length) {
      code = (code + count_[length - 1u]) << 1u;
      next[length] = code;
    }
    for (std::size_t symbol = 0u; symbol < symbolCount_; ++symbol) {
      const auto length = lengths_[symbol];
      if (length == 0u)
        continue;
      codes_[symbol] = reverseBits(next[length]++, length);
    }
  }

  [[nodiscard]] std::uint16_t decode(BitReader &reader,
                                     std::string_view source) const {
    if (codeCount_ == 0u)
      throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                         "DEFLATE Huffman table has no codes");
    std::uint32_t code = 0u;
    for (std::size_t length = 1u; length <= 15u; ++length) {
      code |= reader.read(1u, source) << (length - 1u);
      for (std::size_t symbol = 0u; symbol < symbolCount_; ++symbol) {
        if (lengths_[symbol] == length && codes_[symbol] == code)
          return static_cast<std::uint16_t>(symbol);
      }
    }
    throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                       "DEFLATE stream contains an invalid Huffman code");
  }

private:
  std::array<std::uint32_t, Capacity> codes_{};
  std::array<std::uint8_t, Capacity> lengths_{};
  std::array<std::uint16_t, 16> count_{};
  std::size_t symbolCount_ = 0u;
  std::size_t codeCount_ = 0u;
};

[[nodiscard]] std::uint32_t readBigEndian(std::span<const std::uint8_t> bytes,
                                          std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
         (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u) |
         static_cast<std::uint32_t>(bytes[offset + 3u]);
}

[[nodiscard]] std::uint32_t
adler32(std::span<const std::uint8_t> bytes) noexcept {
  constexpr std::uint32_t modulo = 65521u;
  std::uint32_t first = 1u;
  std::uint32_t second = 0u;
  for (const auto value : bytes) {
    first = (first + value) % modulo;
    second = (second + first) % modulo;
  }
  return (second << 16u) | first;
}

void fixedTables(Huffman<288> &literals, Huffman<32> &distances,
                 std::string_view source) {
  std::array<std::uint8_t, 288> literalLengths{};
  for (std::size_t symbol = 0u; symbol <= 143u; ++symbol)
    literalLengths[symbol] = 8u;
  for (std::size_t symbol = 144u; symbol <= 255u; ++symbol)
    literalLengths[symbol] = 9u;
  for (std::size_t symbol = 256u; symbol <= 279u; ++symbol)
    literalLengths[symbol] = 7u;
  for (std::size_t symbol = 280u; symbol < literalLengths.size(); ++symbol)
    literalLengths[symbol] = 8u;
  std::array<std::uint8_t, 32> distanceLengths{};
  distanceLengths.fill(5u);
  literals.build(literalLengths, source);
  distances.build(distanceLengths, source);
}

constexpr std::array<std::uint16_t, 29> lengthBases = {
    3u,  4u,  5u,  6u,   7u,   8u,   9u,   10u,  11u, 13u,
    15u, 17u, 19u, 23u,  27u,  31u,  35u,  43u,  51u, 59u,
    67u, 83u, 99u, 115u, 131u, 163u, 195u, 227u, 258u};
constexpr std::array<std::uint8_t, 29> lengthExtra = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u, 2u,
    2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u};
constexpr std::array<std::uint16_t, 30> distanceBases = {
    1u,    2u,    3u,    4u,    5u,    7u,    9u,    13u,    17u,    25u,
    33u,   49u,   65u,   97u,   129u,  193u,  257u,  385u,   513u,   769u,
    1025u, 1537u, 2049u, 3073u, 4097u, 6145u, 8193u, 12289u, 16385u, 24577u};
constexpr std::array<std::uint8_t, 30> distanceExtra = {
    0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u,  3u,  3u,  4u,  4u,  5u,  5u,  6u,
    6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u};

void dynamicTables(BitReader &reader, Huffman<288> &literals,
                   Huffman<32> &distances, std::string_view source) {
  const auto literalCount =
      static_cast<std::size_t>(reader.read(5u, source)) + 257u;
  const auto distanceCount =
      static_cast<std::size_t>(reader.read(5u, source)) + 1u;
  const auto codeLengthCount =
      static_cast<std::size_t>(reader.read(4u, source)) + 4u;
  if (literalCount > 286u || distanceCount > 32u || codeLengthCount > 19u)
    throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                       "DEFLATE table count is invalid");

  constexpr std::array<std::size_t, 19> order = {
      16u, 17u, 18u, 0u, 8u,  7u, 9u,  6u, 10u, 5u,
      11u, 4u,  12u, 3u, 13u, 2u, 14u, 1u, 15u};
  std::array<std::uint8_t, 19> codeLengthLengths{};
  for (std::size_t index = 0u; index < codeLengthCount; ++index)
    codeLengthLengths[order[index]] =
        static_cast<std::uint8_t>(reader.read(3u, source));
  Huffman<19> codeLengths;
  codeLengths.build(codeLengthLengths, source);

  std::array<std::uint8_t, 318> lengths{};
  const auto total = literalCount + distanceCount;
  std::size_t index = 0u;
  while (index < total) {
    const auto symbol = codeLengths.decode(reader, source);
    if (symbol <= 15u) {
      lengths[index++] = static_cast<std::uint8_t>(symbol);
      continue;
    }
    if (symbol == 16u) {
      if (index == 0u)
        throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                           "DEFLATE repeat has no previous length");
      const auto repeat =
          static_cast<std::size_t>(reader.read(2u, source)) + 3u;
      if (repeat > total - index)
        throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                           "DEFLATE length repeat exceeds table");
      std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat,
                  lengths[index - 1u]);
      index += repeat;
    } else if (symbol == 17u || symbol == 18u) {
      const auto repeat =
          symbol == 17u
              ? static_cast<std::size_t>(reader.read(3u, source)) + 3u
              : static_cast<std::size_t>(reader.read(7u, source)) + 11u;
      if (repeat > total - index)
        throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                           "DEFLATE zero repeat exceeds table");
      std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat,
                  std::uint8_t{0});
      index += repeat;
    } else {
      throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                         "invalid DEFLATE length symbol");
    }
  }
  if (lengths[256u] == 0u)
    throw deflateError(source, reader.bitPosition() / 8u, "HUFFMAN",
                       "DEFLATE literal tree has no end-of-block code");
  literals.build(std::span<const std::uint8_t>(lengths).first(literalCount),
                 source);
  distances.build(std::span<const std::uint8_t>(lengths).subspan(literalCount,
                                                                 distanceCount),
                  source, true, true);
}

void copyMatch(std::vector<std::uint8_t> &output, std::size_t length,
               std::size_t distance, std::size_t expected,
               std::string_view source, std::size_t offset) {
  if (distance == 0u || distance > output.size())
    throw deflateError(source, offset, "DISTANCE",
                       "DEFLATE distance exceeds output history");
  if (length > expected - output.size())
    throw deflateError(source, offset, "OUTPUT_TOO_LARGE",
                       "DEFLATE output exceeds configured size");
  for (std::size_t index = 0u; index < length; ++index)
    output.push_back(output[output.size() - distance]);
}

void decodeCompressedBlock(BitReader &reader, const Huffman<288> &literals,
                           const Huffman<32> &distances,
                           std::vector<std::uint8_t> &output,
                           std::size_t expected, std::string_view source) {
  for (;;) {
    const auto symbol = literals.decode(reader, source);
    if (symbol < 256u) {
      if (output.size() == expected)
        throw deflateError(source, reader.bitPosition() / 8u,
                           "OUTPUT_TOO_LARGE",
                           "DEFLATE output exceeds configured size");
      output.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }
    if (symbol == 256u)
      return;
    if (symbol < 257u || symbol > 285u)
      throw deflateError(source, reader.bitPosition() / 8u, "LENGTH",
                         "invalid DEFLATE length symbol");
    const auto lengthIndex = static_cast<std::size_t>(symbol - 257u);
    const auto length =
        static_cast<std::size_t>(lengthBases[lengthIndex]) +
        static_cast<std::size_t>(reader.read(lengthExtra[lengthIndex], source));
    const auto distanceSymbol = distances.decode(reader, source);
    if (distanceSymbol >= distanceBases.size())
      throw deflateError(source, reader.bitPosition() / 8u, "DISTANCE",
                         "invalid DEFLATE distance symbol");
    const auto distance =
        static_cast<std::size_t>(distanceBases[distanceSymbol]) +
        static_cast<std::size_t>(
            reader.read(distanceExtra[distanceSymbol], source));
    copyMatch(output, length, distance, expected, source,
              reader.bitPosition() / 8u);
  }
}

} // namespace

std::vector<std::uint8_t> inflateDeflateRaw(std::span<const std::uint8_t> bytes,
                                            std::size_t expectedOutputBytes,
                                            std::string source,
                                            ParseLimits limits,
                                            std::size_t sourceOffset) {
  if (bytes.size() > limits.maxInputBytes ||
      expectedOutputBytes > limits.maxOutputBytes)
    throw deflateError(source, sourceOffset, "LIMIT",
                       "DEFLATE stream exceeds configured limits");
  BitReader reader(bytes);
  std::vector<std::uint8_t> output;
  output.reserve(expectedOutputBytes);
  bool final = false;
  while (!final) {
    final = reader.read(1u, source) != 0u;
    const auto type = reader.read(2u, source);
    if (type == 0u) {
      reader.align(source);
      const auto length = reader.read(16u, source);
      const auto inverse = reader.read(16u, source);
      if ((length ^ inverse) != 0xffffu)
        throw deflateError(source, reader.bitPosition() / 8u, "BLOCK",
                           "stored DEFLATE block length is invalid");
      if (static_cast<std::size_t>(length) >
          expectedOutputBytes - output.size())
        throw deflateError(source, reader.bitPosition() / 8u,
                           "OUTPUT_TOO_LARGE",
                           "DEFLATE output exceeds configured size");
      for (std::size_t index = 0u; index < length; ++index)
        output.push_back(static_cast<std::uint8_t>(reader.read(8u, source)));
    } else if (type == 1u || type == 2u) {
      Huffman<288> literals;
      Huffman<32> distances;
      if (type == 1u)
        fixedTables(literals, distances, source);
      else
        dynamicTables(reader, literals, distances, source);
      decodeCompressedBlock(reader, literals, distances, output,
                            expectedOutputBytes, source);
    } else {
      throw deflateError(source, reader.bitPosition() / 8u, "BLOCK",
                         "reserved DEFLATE block type");
    }
  }
  const auto bitPosition = reader.bitPosition();
  const auto endByte = (bitPosition + 7u) / 8u;
  if (endByte > bytes.size())
    throw deflateError(source, sourceOffset + bytes.size(), "TRUNCATED",
                       "DEFLATE stream exceeds its compressed span");
  if (bitPosition % 8u != 0u &&
      (bytes[bitPosition / 8u] >> (bitPosition % 8u)) != 0u)
    throw deflateError(source, sourceOffset + bitPosition / 8u, "PADDING",
                       "DEFLATE final padding bits must be zero");
  if (endByte != bytes.size())
    throw deflateError(source, sourceOffset + endByte, "TRAILING_BYTES",
                       "DEFLATE stream has trailing bytes");
  if (output.size() != expectedOutputBytes)
    throw deflateError(source, sourceOffset + endByte, "OUTPUT_SIZE",
                       "DEFLATE output size does not match the PNG image");
  return output;
}

std::vector<std::uint8_t> inflateZlib(std::span<const std::uint8_t> bytes,
                                      std::size_t expectedOutputBytes,
                                      std::string source, ParseLimits limits,
                                      std::size_t sourceOffset) {
  if (bytes.size() > limits.maxInputBytes ||
      expectedOutputBytes > limits.maxOutputBytes)
    throw deflateError(source, sourceOffset, "LIMIT",
                       "DEFLATE stream exceeds configured limits");
  if (bytes.size() < 6u)
    throw deflateError(source, sourceOffset, "TRUNCATED",
                       "zlib stream is truncated");
  const auto cmf = bytes[0];
  const auto flg = bytes[1];
  const auto header =
      (static_cast<std::uint32_t>(cmf) << 8u) | static_cast<std::uint32_t>(flg);
  if ((cmf & 0x0fu) != 8u || (cmf >> 4u) > 7u || header % 31u != 0u)
    throw deflateError(source, sourceOffset, "HEADER",
                       "zlib header is invalid");
  if ((flg & 0x20u) != 0u && bytes.size() < 10u)
    throw deflateError(source, sourceOffset + 2u, "TRUNCATED",
                       "zlib preset dictionary identifier is truncated");
  if ((flg & 0x20u) != 0u)
    throw deflateError(source, sourceOffset + 1u, "HEADER",
                       "zlib preset dictionaries are unsupported");

  const auto deflateBytes = bytes.subspan(2u, bytes.size() - 6u);
  auto output = inflateDeflateRaw(deflateBytes, expectedOutputBytes, source,
                                  limits, sourceOffset + 2u);
  const auto expectedAdler = readBigEndian(bytes, bytes.size() - 4u);
  if (adler32(output) != expectedAdler)
    throw deflateError(source, sourceOffset + bytes.size() - 4u, "CHECKSUM",
                       "zlib Adler-32 checksum does not match output");
  return output;
}

} // namespace apex::core
