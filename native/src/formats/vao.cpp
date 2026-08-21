#include "apex/formats/vao.hpp"

#include "apex/core/byte_reader.hpp"
#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace apex::formats {

namespace {

using apex::core::ParseError;

constexpr std::uint32_t ZIP_LOCAL = 0x04034b50u;
constexpr std::uint32_t ZIP_CENTRAL = 0x02014b50u;
constexpr std::uint32_t ZIP_END = 0x06054b50u;
constexpr std::size_t MAX_CONFIG_BYTES = 4u * 1024u * 1024u;
constexpr std::size_t MAX_ENTRY_BYTES = 512u * 1024u * 1024u;
constexpr std::size_t MAX_ZIP_ENTRIES = 4096u;
constexpr std::size_t MAX_RECORD_NAME = 4096u;
constexpr std::size_t MAX_VERTEX_COUNT = 10'000'000u;
constexpr std::size_t MAX_SPLIT_AO_WINGS = 100u;
constexpr std::size_t MAX_SPLIT_AO_NODES = 10'000u;
constexpr std::size_t MAX_SPLIT_AO_NAME = 1024u;

[[nodiscard]] ParseError vaoError(std::string_view source, std::size_t offset,
                                  std::string_view code, std::string_view message) {
    return ParseError("VAO", std::string(source), offset, std::string(code), std::string(message));
}

[[nodiscard]] std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

[[nodiscard]] std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
}

void need(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t size,
          std::string_view source, std::string_view label) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw vaoError(source, offset, "TRUNCATED", "truncated " + std::string(label));
}

[[nodiscard]] std::size_t checkedAdd(std::size_t left, std::size_t right,
                                     std::string_view source, std::size_t offset,
                                     std::string_view label) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        throw vaoError(source, offset, "SIZE_OVERFLOW", std::string(label) + " size overflows");
    return left + right;
}

[[nodiscard]] std::size_t checkedMultiply(std::size_t left, std::size_t right,
                                          std::string_view source, std::size_t offset,
                                          std::string_view label) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        throw vaoError(source, offset, "SIZE_OVERFLOW", std::string(label) + " size overflows");
    return left * right;
}

[[nodiscard]] float halfToFloat(std::uint16_t value) noexcept {
    const float sign = (value & 0x8000u) != 0u ? -1.0f : 1.0f;
    const auto exponent = static_cast<std::uint16_t>((value >> 10u) & 0x1fu);
    const auto fraction = static_cast<std::uint16_t>(value & 0x3ffu);
    if (exponent == 0u) return sign * std::ldexp(static_cast<float>(fraction) / 1024.0f, -14);
    if (exponent == 31u) return fraction != 0u ? std::numeric_limits<float>::quiet_NaN()
                                                : sign * std::numeric_limits<float>::infinity();
    return sign * std::ldexp(1.0f + static_cast<float>(fraction) / 1024.0f,
                             static_cast<int>(exponent) - 15);
}

[[nodiscard]] std::uint8_t legacyVaoByte(float value, const VaoLighting& lighting) noexcept {
    const auto rawAdjusted = (lighting.opacity * value + 1.0f - lighting.opacity) * lighting.brightness;
    const float adjusted = std::isfinite(rawAdjusted) ? std::clamp(rawAdjusted, 0.0f, 1.0f)
                                                       : (rawAdjusted > 0.0f ? 1.0f : 0.0f);
    const auto encodedValue = std::trunc(std::pow(adjusted, lighting.gamma) * 255.0f);
    // JavaScript's subsequent `& 255` is a modulo operation.  Keep that
    // operation bounded here instead of converting an untrusted infinity or
    // out-of-range float to int (which is undefined in C++).
    int encoded = 0;
    if (std::isfinite(encodedValue)) {
        auto remainder = std::fmod(encodedValue, 256.0f);
        if (remainder < 0.0f) remainder += 256.0f;
        encoded = static_cast<int>(remainder);
    }
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::trunc(
                                                   std::sqrt(static_cast<float>(encoded) / 255.0f) * 255.0f)),
                                               0, 255));
}

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return result;
}

[[nodiscard]] std::string upper(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    return result;
}

[[nodiscard]] bool safeZipName(std::string_view name) noexcept {
    if (name.empty() || name.front() == '/' || name.front() == '\\' ||
        (name.size() > 1u && name[1] == ':')) return false;
    std::size_t componentStart = 0;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        if (index != name.size() && name[index] != '/' && name[index] != '\\') continue;
        const auto component = name.substr(componentStart, index - componentStart);
        if (component.empty() || component == "." || component == "..") return false;
        if (component.back() == '.' || component.back() == ' ') return false;
        if (component.find(':') != std::string_view::npos) return false;
        const auto stemEnd = component.find('.');
        const auto stem = component.substr(0u, stemEnd == std::string_view::npos ? component.size() : stemEnd);
        const auto device = [&](std::string_view value) {
            if (value.size() == 3u &&
                ((value[0] == 'C' || value[0] == 'c') && (value[1] == 'O' || value[1] == 'o') &&
                 (value[2] == 'N' || value[2] == 'n')))
                return true;
            if (value.size() == 3u &&
                ((value[0] == 'P' || value[0] == 'p') && (value[1] == 'R' || value[1] == 'r') &&
                 (value[2] == 'N' || value[2] == 'n')))
                return true;
            if (value.size() == 3u &&
                ((value[0] == 'A' || value[0] == 'a') && (value[1] == 'U' || value[1] == 'u') &&
                 (value[2] == 'X' || value[2] == 'x')))
                return true;
            if (value.size() == 3u &&
                ((value[0] == 'N' || value[0] == 'n') && (value[1] == 'U' || value[1] == 'u') &&
                 (value[2] == 'L' || value[2] == 'l')))
                return true;
            return value.size() == 4u &&
                   ((value[0] == 'C' || value[0] == 'c' || value[0] == 'L' || value[0] == 'l') &&
                    (value[1] == 'O' || value[1] == 'o') && (value[2] == 'M' || value[2] == 'm' ||
                                                              value[2] == 'P' || value[2] == 'p') &&
                    value[3] >= '0' && value[3] <= '9');
        };
        if (device(stem)) return false;
        for (const auto character : component)
            if (static_cast<unsigned char>(character) < 0x20u || character == '\0') return false;
        componentStart = index + 1u;
    }
    return true;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

class BitReader final {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint32_t bits(unsigned count) {
        if (count > 24u) throw std::runtime_error("deflate bit request is too large");
        std::uint32_t result = 0;
        for (unsigned index = 0; index < count; ++index) {
            if ((bitOffset_ >> 3u) >= bytes_.size()) throw std::runtime_error("truncated deflate stream");
            const auto byte = static_cast<std::uint32_t>(bytes_[bitOffset_ >> 3u]);
            result |= ((byte >> (bitOffset_ & 7u)) & 1u) << index;
            ++bitOffset_;
        }
        return result;
    }

    void align() noexcept { bitOffset_ = (bitOffset_ + 7u) & ~std::size_t{7u}; }

    [[nodiscard]] std::size_t position() const noexcept { return bitOffset_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t bitOffset_ = 0;
};

struct HuffmanCode {
    std::uint16_t code = 0;
    std::uint8_t length = 0;
    std::uint16_t symbol = 0;
};

class Huffman final {
public:
    void build(std::span<const std::uint8_t> lengths, bool allowSingleIncomplete) {
        std::array<unsigned, 16> count{};
        unsigned maximumLength = 0;
        for (const auto length : lengths) {
            if (length > 15u) throw std::runtime_error("invalid deflate code length");
            if (length != 0u) {
                ++count[length];
                maximumLength = std::max(maximumLength, static_cast<unsigned>(length));
            }
        }
        int left = 1;
        for (unsigned length = 1; length <= 15u; ++length) {
            left = (left << 1) - static_cast<int>(count[length]);
            if (left < 0) throw std::runtime_error("oversubscribed deflate Huffman tree");
        }
        std::array<unsigned, 16> next{};
        unsigned code = 0;
        for (unsigned length = 1; length <= 15u; ++length) {
            code = (code + count[length - 1u]) << 1u;
            next[length] = code;
        }
        codes_.clear();
        for (std::uint16_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const auto length = lengths[symbol];
            if (length == 0u) continue;
            // Deflate writes each canonical code least-significant bit first.
            // Keep the wire order in the lookup table while the canonical
            // numbering above remains the one from RFC 1951.
            const auto canonical = static_cast<std::uint16_t>(next[length]++);
            std::uint32_t wire = 0;
            for (std::uint8_t bit = 0; bit < length; ++bit)
                wire = (wire << 1u) | ((static_cast<std::uint32_t>(canonical) >> bit) & 1u);
            codes_.push_back({static_cast<std::uint16_t>(wire), length, symbol});
        }
        if (codes_.empty()) throw std::runtime_error("empty deflate Huffman tree");
        // RFC 1951 permits an incomplete tree only when it consists of one
        // one-bit code (the usual degenerate distance/code-length tree).
        if (left != 0 && !(allowSingleIncomplete && maximumLength == 1u && codes_.size() == 1u))
            throw std::runtime_error("incomplete deflate Huffman tree");
    }

    [[nodiscard]] std::uint16_t decode(BitReader& bits) const {
        std::uint16_t value = 0;
        for (std::uint8_t length = 1; length <= 15u; ++length) {
            value = static_cast<std::uint16_t>(value | (bits.bits(1u) << (length - 1u)));
            for (const auto& code : codes_)
                if (code.length == length && code.code == value) return code.symbol;
        }
        throw std::runtime_error("invalid deflate Huffman symbol");
    }

private:
    std::vector<HuffmanCode> codes_;
};

[[nodiscard]] std::vector<std::uint8_t> inflateRaw(std::span<const std::uint8_t> compressed,
                                                   std::size_t expected,
                                                   std::size_t maximum) {
    try {
        BitReader bits(compressed);
        std::vector<std::uint8_t> output;
        output.reserve(std::min(expected, maximum));
        bool final = false;
        while (!final) {
            final = bits.bits(1u) != 0u;
            const auto type = bits.bits(2u);
            if (type == 0u) {
                bits.align();
                const auto length = static_cast<std::uint16_t>(bits.bits(16u));
                const auto inverse = static_cast<std::uint16_t>(bits.bits(16u));
                if (static_cast<std::uint16_t>(length ^ inverse) != 0xffffu)
                    throw std::runtime_error("invalid stored deflate block length");
                if (length > maximum - std::min(output.size(), maximum))
                    throw std::runtime_error("deflate output exceeds limit");
                for (std::uint32_t index = 0; index < length; ++index)
                    output.push_back(static_cast<std::uint8_t>(bits.bits(8u)));
                continue;
            }
            if (type == 3u) throw std::runtime_error("reserved deflate block type");
            Huffman literalTree, distanceTree;
            if (type == 1u) {
                std::array<std::uint8_t, 288> lengths{};
                for (std::size_t index = 0; index <= 143u; ++index) lengths[index] = 8;
                for (std::size_t index = 144; index <= 255u; ++index) lengths[index] = 9;
                for (std::size_t index = 256; index <= 279u; ++index) lengths[index] = 7;
                for (std::size_t index = 280; index < lengths.size(); ++index) lengths[index] = 8;
                literalTree.build(lengths, false);
                std::array<std::uint8_t, 32> distances{};
                distances.fill(5);
                distanceTree.build(distances, false);
            } else {
                const auto literalCount = static_cast<std::size_t>(bits.bits(5u)) + 257u;
                const auto distanceCount = static_cast<std::size_t>(bits.bits(5u)) + 1u;
                const auto codeLengthCount = static_cast<std::size_t>(bits.bits(4u)) + 4u;
                if (literalCount > 286u || distanceCount > 32u) throw std::runtime_error("invalid deflate tree counts");
                constexpr std::array<std::uint8_t, 19> order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                                  11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::array<std::uint8_t, 19> codeLengths{};
                for (std::size_t index = 0; index < codeLengthCount; ++index)
                    codeLengths[order[index]] = static_cast<std::uint8_t>(bits.bits(3u));
                Huffman codeLengthTree;
                codeLengthTree.build(codeLengths, true);
                std::vector<std::uint8_t> lengths(literalCount + distanceCount);
                std::size_t index = 0;
                while (index < lengths.size()) {
                    const auto symbol = codeLengthTree.decode(bits);
                    if (symbol <= 15u) {
                        lengths[index++] = static_cast<std::uint8_t>(symbol);
                    } else if (symbol == 16u) {
                        if (index == 0u) throw std::runtime_error("invalid deflate repeat");
                        const auto repeat = static_cast<std::size_t>(bits.bits(2u)) + 3u;
                        if (repeat > lengths.size() - index) throw std::runtime_error("deflate repeat exceeds tree");
                        std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat, lengths[index - 1u]);
                        index += repeat;
                    } else if (symbol == 17u || symbol == 18u) {
                        const auto repeat = static_cast<std::size_t>(bits.bits(symbol == 17u ? 3u : 7u)) +
                                            (symbol == 17u ? 3u : 11u);
                        if (repeat > lengths.size() - index) throw std::runtime_error("deflate zero repeat exceeds tree");
                        std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat,
                                    std::uint8_t{0});
                        index += repeat;
                    } else {
                        throw std::runtime_error("invalid deflate code length symbol");
                    }
                }
                if (lengths[256u] == 0u) throw std::runtime_error("deflate literal tree has no end-of-block code");
                literalTree.build(std::span<const std::uint8_t>(lengths.data(), literalCount), true);
                distanceTree.build(std::span<const std::uint8_t>(lengths.data() + literalCount, distanceCount), true);
            }
            constexpr std::array<unsigned, 29> lengthBase = {
                3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            constexpr std::array<unsigned, 29> lengthExtra = {
                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            constexpr std::array<unsigned, 30> distanceBase = {
                1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                6145, 8193, 12289, 16385, 24577};
            constexpr std::array<unsigned, 30> distanceExtra = {
                0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
            for (;;) {
                const auto symbol = literalTree.decode(bits);
                if (symbol < 256u) {
                    if (output.size() >= maximum) throw std::runtime_error("deflate output exceeds limit");
                    output.push_back(static_cast<std::uint8_t>(symbol));
                } else if (symbol == 256u) {
                    break;
                } else if (symbol <= 285u) {
                    const auto lengthIndex = static_cast<std::size_t>(symbol - 257u);
                    const auto length = static_cast<std::size_t>(lengthBase[lengthIndex] + bits.bits(lengthExtra[lengthIndex]));
                    const auto distanceSymbol = distanceTree.decode(bits);
                    if (distanceSymbol >= distanceBase.size()) throw std::runtime_error("invalid deflate distance symbol");
                    const auto distance = static_cast<std::size_t>(distanceBase[distanceSymbol] + bits.bits(distanceExtra[distanceSymbol]));
                    if (distance == 0u || distance > output.size() || length > maximum - std::min(output.size(), maximum))
                        throw std::runtime_error("invalid deflate back-reference");
                    for (std::size_t count = 0; count < length; ++count)
                        output.push_back(output[output.size() - distance]);
                } else {
                    throw std::runtime_error("invalid deflate literal/length symbol");
                }
            }
        }
        if (output.size() != expected) throw std::runtime_error("deflate output size mismatch");
        const auto position = bits.position();
        const auto consumedBytes = position / 8u + (position % 8u != 0u ? 1u : 0u);
        if (consumedBytes > compressed.size()) throw std::runtime_error("deflate stream exceeds compressed span");
        if (position % 8u != 0u && (compressed[position / 8u] >> (position % 8u)) != 0u)
            throw std::runtime_error("nonzero deflate final padding");
        if (consumedBytes != compressed.size()) throw std::runtime_error("trailing bytes after deflate stream");
        return output;
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string("invalid deflate stream: ") + error.what());
    }
}

struct ZipEntry {
    std::string name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t localOffset = 0;
};

struct ZipArchive {
    std::vector<ZipEntry> entries;
};

[[nodiscard]] ZipArchive readZipDirectory(std::span<const std::uint8_t> bytes,
                                           std::string_view source,
                                           const apex::core::ParseLimits& limits) {
    if (bytes.size() < 22u) throw vaoError(source, 0, "TRUNCATED", "ZIP end record is missing");
    const auto minimum = bytes.size() > 65557u ? bytes.size() - 65557u : 0u;
    std::optional<std::size_t> end;
    for (std::size_t offset = bytes.size() - 22u;; --offset) {
        if (u32(bytes, offset) == ZIP_END) {
            need(bytes, offset, 22u, source, "ZIP end record");
            const auto commentLength = u16(bytes, offset + 20u);
            if (commentLength <= bytes.size() - offset - 22u) { end = offset; break; }
        }
        if (offset == minimum) break;
    }
    if (!end.has_value()) throw vaoError(source, 0, "INVALID_ZIP", "ZIP end record is missing");
    const auto endOffset = *end;
    const auto commentLength = static_cast<std::size_t>(u16(bytes, endOffset + 20u));
    if (checkedAdd(endOffset, checkedAdd(22u, commentLength, source, endOffset, "ZIP end record"),
                   source, endOffset, "ZIP archive") != bytes.size())
        throw vaoError(source, endOffset, "INVALID_ZIP", "ZIP archive has trailing bytes after the end record");
    if (u16(bytes, endOffset + 4u) != 0u || u16(bytes, endOffset + 6u) != 0u)
        throw vaoError(source, endOffset, "UNSUPPORTED_ZIP", "multi-disk ZIP archives are unsupported");
    if (u16(bytes, endOffset + 8u) != u16(bytes, endOffset + 10u))
        throw vaoError(source, endOffset + 8u, "INVALID_ZIP", "ZIP entry counts differ between disks");
    const auto count = static_cast<std::size_t>(u16(bytes, endOffset + 10u));
    const auto centralSize = static_cast<std::size_t>(u32(bytes, endOffset + 12u));
    const auto centralOffset = static_cast<std::size_t>(u32(bytes, endOffset + 16u));
    if (count > MAX_ZIP_ENTRIES || count > limits.maxTracks)
        throw vaoError(source, endOffset + 10u, "COUNT_LIMIT", "ZIP entry count exceeds configured limit");
    const auto directoryEnd = checkedAdd(centralOffset, centralSize, source, endOffset + 16u, "ZIP directory");
    if (directoryEnd != endOffset)
        throw vaoError(source, endOffset + 12u, "INVALID_ZIP", "ZIP directory does not end at the end record");
    need(bytes, centralOffset, centralSize, source, "ZIP directory");
    ZipArchive archive;
    archive.entries.reserve(count);
    std::map<std::string, bool> names;
    std::size_t offset = centralOffset;
    for (std::size_t index = 0; index < count; ++index) {
        need(bytes, offset, 46u, source, "ZIP central header");
        if (u32(bytes, offset) != ZIP_CENTRAL)
            throw vaoError(source, offset, "INVALID_ZIP", "invalid ZIP central header");
        const auto flags = u16(bytes, offset + 8u);
        const auto method = u16(bytes, offset + 10u);
        const auto crc = u32(bytes, offset + 16u);
        const auto compressedSize = u32(bytes, offset + 20u);
        const auto uncompressedSize = u32(bytes, offset + 24u);
        const auto nameLength = static_cast<std::size_t>(u16(bytes, offset + 28u));
        const auto extraLength = static_cast<std::size_t>(u16(bytes, offset + 30u));
        const auto entryCommentLength = static_cast<std::size_t>(u16(bytes, offset + 32u));
        const auto localOffset = u32(bytes, offset + 42u);
        const auto body = checkedAdd(
            46u,
            checkedAdd(nameLength,
                       checkedAdd(extraLength, entryCommentLength, source, offset, "ZIP entry"),
                       source, offset, "ZIP entry"),
            source, offset, "ZIP entry");
        need(bytes, offset, body, source, "ZIP central entry");
        const auto nameBytes = bytes.subspan(offset + 46u, nameLength);
        const auto name = apex::core::ByteReader::decodeUtf8(nameBytes, "ZIP entry name", "VAO",
                                                             source, offset + 46u);
        if (!safeZipName(name)) throw vaoError(source, offset + 46u, "UNSAFE_NAME", "unsafe ZIP entry name");
        if (names.emplace(lower(name), true).second == false)
            throw vaoError(source, offset + 46u, "DUPLICATE_ENTRY", "duplicate ZIP entry name");
        if ((flags & ~0x0806u) != 0u || (method == 0u && (flags & 0x0006u) != 0u))
            throw vaoError(source, offset + 8u, "UNSUPPORTED_ZIP", "ZIP entry flags are unsupported");
        if (method != 0u && method != 8u)
            throw vaoError(source, offset + 10u, "UNSUPPORTED_ZIP", "ZIP compression method is unsupported");
        const auto maximumEntry = std::min(MAX_ENTRY_BYTES, limits.maxOutputBytes);
        if (uncompressedSize > maximumEntry)
            throw vaoError(source, offset + 24u, "SIZE_LIMIT", "ZIP entry exceeds configured size limit");
        const auto maximumCompressed = std::min(MAX_ENTRY_BYTES, limits.maxInputBytes);
        if (compressedSize > maximumCompressed)
            throw vaoError(source, offset + 20u, "SIZE_LIMIT", "ZIP compressed entry exceeds configured size limit");
        archive.entries.push_back({name, flags, method, crc, compressedSize, uncompressedSize, localOffset});
        offset += body;
    }
    if (offset != directoryEnd)
        throw vaoError(source, offset, "INVALID_ZIP", "ZIP central entries do not consume the directory");

    struct Range { std::size_t begin = 0; std::size_t end = 0; };
    std::vector<Range> ranges;
    ranges.reserve(archive.entries.size());
    for (const auto& entry : archive.entries) {
        const auto localOffset = static_cast<std::size_t>(entry.localOffset);
        if (localOffset >= centralOffset)
            throw vaoError(source, localOffset, "INVALID_ZIP", "ZIP local entry is not before the central directory");
        need(bytes, localOffset, 30u, source, "ZIP local header");
        if (u32(bytes, localOffset) != ZIP_LOCAL)
            throw vaoError(source, localOffset, "INVALID_ZIP", "invalid ZIP local header");
        const auto localFlags = u16(bytes, localOffset + 6u);
        const auto localMethod = u16(bytes, localOffset + 8u);
        if (localFlags != entry.flags || localMethod != entry.method)
            throw vaoError(source, localOffset, "INVALID_ZIP", "ZIP local and central headers differ");
        if (u32(bytes, localOffset + 14u) != entry.crc ||
            u32(bytes, localOffset + 18u) != entry.compressedSize ||
            u32(bytes, localOffset + 22u) != entry.uncompressedSize)
            throw vaoError(source, localOffset, "INVALID_ZIP", "ZIP local and central sizes differ");
        const auto localNameLength = static_cast<std::size_t>(u16(bytes, localOffset + 26u));
        const auto localExtraLength = static_cast<std::size_t>(u16(bytes, localOffset + 28u));
        const auto localHeaderEnd = checkedAdd(localOffset, 30u, source, localOffset, "ZIP local header");
        const auto dataStart = checkedAdd(localHeaderEnd,
                                          checkedAdd(localNameLength, localExtraLength, source, localOffset, "ZIP local header"),
                                          source, localOffset, "ZIP data");
        need(bytes, dataStart, entry.compressedSize, source, "ZIP entry data");
        const auto dataEnd = checkedAdd(dataStart, entry.compressedSize, source, localOffset, "ZIP data");
        if (dataEnd > centralOffset)
            throw vaoError(source, localOffset, "INVALID_ZIP", "ZIP entry overlaps the central directory");
        const auto localName = apex::core::ByteReader::decodeUtf8(
            bytes.subspan(localOffset + 30u, localNameLength), "ZIP local name", "VAO", source, localOffset + 30u);
        if (localName != entry.name)
            throw vaoError(source, localOffset, "INVALID_ZIP", "ZIP local and central names differ");
        ranges.push_back({localOffset, dataEnd});
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& left, const Range& right) {
        return left.begin < right.begin;
    });
    for (std::size_t index = 1; index < ranges.size(); ++index)
        if (ranges[index].begin < ranges[index - 1u].end)
            throw vaoError(source, ranges[index].begin, "INVALID_ZIP", "ZIP local entries overlap");
    return archive;
}

[[nodiscard]] std::vector<std::uint8_t> extractZipEntry(std::span<const std::uint8_t> bytes,
                                                        const ZipEntry& entry,
                                                        std::string_view source,
                                                        const apex::core::ParseLimits& limits) {
    const auto offset = static_cast<std::size_t>(entry.localOffset);
    need(bytes, offset, 30u, source, "ZIP local header");
    if (u32(bytes, offset) != ZIP_LOCAL) throw vaoError(source, offset, "INVALID_ZIP", "invalid ZIP local header");
    if (u16(bytes, offset + 6u) != entry.flags || u16(bytes, offset + 8u) != entry.method)
        throw vaoError(source, offset, "INVALID_ZIP", "ZIP local and central headers differ");
    if (u32(bytes, offset + 14u) != entry.crc || u32(bytes, offset + 18u) != entry.compressedSize ||
        u32(bytes, offset + 22u) != entry.uncompressedSize)
        throw vaoError(source, offset, "INVALID_ZIP", "ZIP local and central sizes differ");
    const auto nameLength = static_cast<std::size_t>(u16(bytes, offset + 26u));
    const auto extraLength = static_cast<std::size_t>(u16(bytes, offset + 28u));
    const auto localHeaderEnd = checkedAdd(offset, 30u, source, offset, "ZIP local header");
    const auto start = checkedAdd(localHeaderEnd, checkedAdd(nameLength, extraLength, source, offset, "ZIP local header"), source, offset, "ZIP data");
    need(bytes, start, entry.compressedSize, source, "ZIP entry data");
    const auto localName = apex::core::ByteReader::decodeUtf8(bytes.subspan(offset + 30u, nameLength),
                                                               "ZIP local name", "VAO", source, offset + 30u);
    if (localName != entry.name) throw vaoError(source, offset, "INVALID_ZIP", "ZIP local and central names differ");
    const auto maximumEntry = std::min(MAX_ENTRY_BYTES, limits.maxOutputBytes);
    std::vector<std::uint8_t> output;
    if (entry.method == 0u) {
        if (entry.compressedSize != entry.uncompressedSize) throw vaoError(source, offset, "INVALID_ZIP", "stored ZIP size mismatch");
        output.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                      bytes.begin() + static_cast<std::ptrdiff_t>(start + entry.compressedSize));
    } else {
        try {
            output = inflateRaw(bytes.subspan(start, entry.compressedSize), entry.uncompressedSize, maximumEntry);
        } catch (const std::runtime_error& error) {
            throw vaoError(source, start, "INVALID_DEFLATE", error.what());
        }
    }
    if (output.size() != entry.uncompressedSize)
        throw vaoError(source, start, "SIZE_MISMATCH", "ZIP entry expanded to an unexpected size");
    if (crc32(output) != entry.crc) throw vaoError(source, start, "CRC_MISMATCH", "ZIP entry CRC-32 does not match");
    return output;
}

[[nodiscard]] std::optional<ZipEntry> findEntry(const ZipArchive& archive, std::string_view name) {
    const auto wanted = lower(name);
    for (const auto& entry : archive.entries)
        if (lower(entry.name) == wanted) return entry;
    return std::nullopt;
}

[[nodiscard]] float configFloat(const std::map<std::string, std::string>& values,
                                std::string_view key, float fallback,
                                std::vector<std::string>& warnings) {
    const auto found = values.find(std::string(key));
    if (found == values.end()) return fallback;
    char* end = nullptr;
    const auto value = std::strtof(found->second.c_str(), &end);
    if (end == found->second.c_str() || *end != '\0' || !std::isfinite(value) || value < 0.0f) {
        warnings.push_back(std::string(key) + ": exponent must be a finite nonnegative number");
        return fallback;
    }
    return value;
}

[[nodiscard]] std::vector<std::string> configNames(const std::map<std::string, std::string>& values,
                                                   std::string_view key,
                                                   std::vector<std::string>& warnings) {
    std::vector<std::string> result;
    const auto found = values.find(std::string(key));
    if (found == values.end()) return result;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= found->second.size(); ++index) {
        if (index != found->second.size() && found->second[index] != ',') continue;
        auto value = found->second.substr(start, index - start);
        const auto first = value.find_first_not_of(" \t");
        const auto last = value.find_last_not_of(" \t");
        value = first == std::string::npos ? std::string{} : value.substr(first, last - first + 1u);
        if (!value.empty()) {
            if (value.size() > MAX_SPLIT_AO_NAME) warnings.push_back(std::string(key) + ": node name exceeds 1024 characters");
            else if (result.size() >= MAX_SPLIT_AO_NODES) warnings.push_back(std::string(key) + ": node list exceeds 10000 entries");
            else if (std::none_of(result.begin(), result.end(), [&](const std::string& candidate) { return lower(candidate) == lower(value); })) result.push_back(std::move(value));
        }
        start = index + 1u;
        if (result.size() >= MAX_SPLIT_AO_NODES) break;
    }
    return result;
}

[[nodiscard]] VaoSplitAo parseSplitAo(std::string_view text) {
    std::string section;
    bool present = false;
    std::map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = std::string(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (const auto comment = line.find(';'); comment != std::string::npos) line.resize(comment);
        const auto first = line.find_first_not_of(" \t");
        const auto last = line.find_last_not_of(" \t");
        line = first == std::string::npos ? std::string{} : line.substr(first, last - first + 1u);
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            section = upper(line.substr(1, line.size() - 2u));
            present = present || section == "SPLIT_AO";
        } else if (section == "SPLIT_AO") {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                auto key = upper(line.substr(0, equals));
                auto value = line.substr(equals + 1u);
                const auto keyFirst = key.find_first_not_of(" \t"), keyLast = key.find_last_not_of(" \t");
                key = keyFirst == std::string::npos ? std::string{} : key.substr(keyFirst, keyLast - keyFirst + 1u);
                const auto valueFirst = value.find_first_not_of(" \t"), valueLast = value.find_last_not_of(" \t");
                value = valueFirst == std::string::npos ? std::string{} : value.substr(valueFirst, valueLast - valueFirst + 1u);
                if (!key.empty()) values[key] = std::move(value);
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1u;
    }
    VaoSplitAo result;
    result.present = present;
    result.cockpitHr = configNames(values, "COCKPIT_HR", result.warnings);
    result.doorExponent = configFloat(values, "DOOR_EXP", 2.0f, result.warnings);
    result.doorNodes = configNames(values, "DOOR_NODES", result.warnings);
    result.headlightsExponent = configFloat(values, "HEADLIGHTS_EXP", 2.0f, result.warnings);
    result.headlightsNodes = configNames(values, "HEADLIGHTS_NODES", result.warnings);
    result.steeringWheelNodes = configNames(values, "STEERING_WHEEL_NODES", result.warnings);
    for (std::uint32_t index = 0; index < MAX_SPLIT_AO_WINGS; ++index) {
        const auto prefix = "WING_ANIM_" + std::to_string(index);
        const auto found = values.find(prefix + "_NAME");
        if (found == values.end() || found->second.empty()) break;
        if (found->second.size() > MAX_SPLIT_AO_NAME) {
            result.warnings.push_back(prefix + "_NAME: animation name exceeds 1024 characters");
            continue;
        }
        VaoSplitWing wing;
        wing.index = index;
        wing.name = found->second;
        wing.exponent = configFloat(values, prefix + "_EXP", 1.0f, result.warnings);
        wing.nodes = configNames(values, prefix + "_NODES", result.warnings);
        result.wings.push_back(std::move(wing));
    }
    return result;
}

[[nodiscard]] VaoLighting parseLighting(std::string_view text) {
    std::map<std::string, std::string> values;
    std::string section;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = std::string(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (const auto comment = line.find(';'); comment != std::string::npos) line.resize(comment);
        const auto first = line.find_first_not_of(" \t"), last = line.find_last_not_of(" \t");
        line = first == std::string::npos ? std::string{} : line.substr(first, last - first + 1u);
        if (!line.empty() && line.front() == '[' && line.back() == ']') section = upper(line.substr(1, line.size() - 2u));
        else if (section == "LIGHTING") {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                auto key = line.substr(0, equals);
                auto value = line.substr(equals + 1u);
                const auto keyFirst = key.find_first_not_of(" \t"), keyLast = key.find_last_not_of(" \t");
                const auto valueFirst = value.find_first_not_of(" \t"), valueLast = value.find_last_not_of(" \t");
                key = keyFirst == std::string::npos ? std::string{} : key.substr(keyFirst, keyLast - keyFirst + 1u);
                value = valueFirst == std::string::npos ? std::string{} : value.substr(valueFirst, valueLast - valueFirst + 1u);
                if (!key.empty()) values[upper(key)] = std::move(value);
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1u;
    }
    const auto finiteValue = [&](std::string_view key, float fallback) {
        const auto found = values.find(std::string(key));
        if (found == values.end()) return fallback;
        char* end = nullptr;
        const auto value = std::strtof(found->second.c_str(), &end);
        return end != found->second.c_str() && std::isfinite(value) ? value : fallback;
    };
    VaoLighting result;
    result.opacity = finiteValue("OPACITY", .85f);
    result.brightness = finiteValue("BRIGHTNESS", 1.1f);
    result.gamma = finiteValue("GAMMA", 1.0f);
    return result;
}

} // namespace

VaoData parseVaoData(std::span<const std::uint8_t> bytes, std::uint32_t version,
                     VaoLighting lighting, std::string source,
                     apex::core::ParseLimits limits) {
    if (bytes.size() > limits.maxInputBytes)
        throw vaoError(source, 0, "INPUT_TOO_LARGE", "VAO data exceeds configured input size limit");
    if (version != 1u && version != 3u && version != 4u && version != 5u)
        throw vaoError(source, 0, "UNSUPPORTED_VERSION", "unsupported VAO patch version");
    if (bytes.empty()) throw vaoError(source, 0, "TRUNCATED", "VAO data has no records");
    VaoData result;
    result.version = version;
    result.byteLength = bytes.size();
    std::size_t offset = 0;
    std::size_t decodedBytes = 0;
    while (offset < bytes.size()) {
        const auto recordOffset = offset;
        need(bytes, offset, 4u, source, "record name length");
        const auto nameLength = static_cast<std::size_t>(u32(bytes, offset));
        offset += 4u;
        if (nameLength == 0u || nameLength > MAX_RECORD_NAME)
            throw vaoError(source, recordOffset, "INVALID_RECORD", "invalid record name length");
        const auto recordHeaderSize = checkedAdd(nameLength, 20u, source, recordOffset, "record header");
        need(bytes, offset, recordHeaderSize, source, "record header");
        auto name = apex::core::ByteReader::decodeUtf8(bytes.subspan(offset, nameLength), "record name",
                                                       "VAO", source, offset);
        offset += nameLength;
        const auto type = u32(bytes, offset);
        offset += 4u;
        std::array<float, 3> firstVertex{};
        for (auto& component : firstVertex) {
            component = std::bit_cast<float>(u32(bytes, offset));
            if (!std::isfinite(component)) throw vaoError(source, offset, "NON_FINITE", "record first vertex is not finite");
            offset += 4u;
        }
        const auto vertexCount = u32(bytes, offset);
        offset += 4u;
        if (name == "@@__EXTRA_AO@" || type == 4u) {
            result.embeddedExtraSamples = VaoEmbeddedExtraSamples{name, 1u, vertexCount, bytes.size() - offset};
            result.bytesRead = bytes.size();
            break;
        }
        const bool alternate = name.rfind("@@__ALT@:", 0u) == 0u;
        if (alternate) name.erase(0u, 9u);
        if (vertexCount > MAX_VERTEX_COUNT)
            throw vaoError(source, recordOffset, "COUNT_LIMIT", "record vertex count exceeds configured limit");
        const std::size_t components = type == 0u || type == 2u ? 3u : type == 1u || type == 3u ? 1u : 0u;
        if (components == 0u) throw vaoError(source, recordOffset, "UNSUPPORTED_TYPE", "unsupported VAO record type");
        const auto componentBytes = version >= 4u ? 1u : 2u;
        const auto payloadBytes = checkedMultiply(checkedMultiply(vertexCount, components, source, offset, "record"),
                                                  componentBytes, source, offset, "record");
        need(bytes, offset, payloadBytes, source, "record payload");
        const auto recordOutputBytes = type == 2u
                                           ? checkedMultiply(checkedMultiply(vertexCount, 3u, source, offset, "normal output"),
                                                            sizeof(float), source, offset, "normal output")
                                           : static_cast<std::size_t>(vertexCount);
        if (recordOutputBytes > limits.maxOutputBytes ||
            recordOutputBytes > limits.maxOutputBytes - std::min(decodedBytes, limits.maxOutputBytes))
            throw vaoError(source, offset, "OUTPUT_TOO_LARGE", "decoded VAO records exceed configured output limit");
        decodedBytes = checkedAdd(decodedBytes, recordOutputBytes, source, offset, "decoded VAO records");
        VaoRecord record;
        record.name = std::move(name);
        record.type = type;
        record.channel = type == 3u ? VaoChannel::secondary : type == 2u ? VaoChannel::normal : VaoChannel::primary;
        record.alternate = alternate;
        record.firstVertex = firstVertex;
        record.vertexCount = vertexCount;
        record.offset = recordOffset;
        if (type == 2u) {
            record.normals.resize(checkedMultiply(vertexCount, 3u, source, offset, "normal output"));
            for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                float x = 0, y = 0, z = 0;
                if (version >= 4u) {
                    const auto sampleOffset = offset + vertex * 3u;
                    x = static_cast<float>(bytes[sampleOffset]) / 255.0f;
                    y = static_cast<float>(bytes[sampleOffset + 1u]) / 255.0f;
                    z = static_cast<float>(bytes[sampleOffset + 2u]) / 255.0f;
                    if (version == 5u) { x *= x; y *= y; z *= z; }
                } else {
                    const auto sampleOffset = offset + vertex * 6u;
                    x = halfToFloat(u16(bytes, sampleOffset));
                    y = halfToFloat(u16(bytes, sampleOffset + 2u));
                    z = halfToFloat(u16(bytes, sampleOffset + 4u));
                }
                const auto length = std::hypot(std::hypot(x, y), z);
                if (!std::isfinite(length) || length == 0.0f)
                    throw vaoError(source, offset + vertex * componentBytes * components, "INVALID_NORMAL", "invalid normal");
                record.normals[vertex * 3u] = x / length;
                record.normals[vertex * 3u + 1u] = y / length;
                record.normals[vertex * 3u + 2u] = z / length;
            }
        } else {
            record.values.resize(vertexCount);
            for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                float sample = 0.0f;
                if (version >= 4u) {
                    if (components == 1u) sample = static_cast<float>(bytes[offset + vertex]);
                    else sample = static_cast<float>(bytes[offset + vertex * 3u] +
                                                     bytes[offset + vertex * 3u + 1u] +
                                                     bytes[offset + vertex * 3u + 2u]) / 3.0f;
                    if (version == 4u)
                        sample = std::sqrt(std::trunc(sample) / 255.0f) * 255.0f;
                } else {
                    float sum = 0.0f;
                    for (std::size_t component = 0; component < components; ++component)
                        sum += halfToFloat(u16(bytes, offset + (vertex * components + component) * 2u));
                    sample = static_cast<float>(legacyVaoByte(sum / static_cast<float>(components), lighting));
                }
                const auto rounded = static_cast<int>(std::trunc(sample));
                record.values[vertex] = static_cast<std::uint8_t>(std::clamp(rounded, 0, 255));
            }
        }
        result.records.push_back(std::move(record));
        offset += payloadBytes;
        if (result.records.size() > limits.maxTracks)
            throw vaoError(source, recordOffset, "COUNT_LIMIT", "record count exceeds configured limit");
    }
    result.recordCount = result.records.size();
    if (!result.embeddedExtraSamples) result.bytesRead = offset;
    return result;
}

bool vaoNormalOverrideNameEligible(std::string_view name) noexcept {
    if (name == "AC_SEMAPHORE") return true;
    if (name.rfind("AC_", 0u) == 0u) return false;
    return std::none_of(name.begin(), name.end(), [](char value) { return value >= '0' && value <= '9'; });
}

VaoPatch parseVaoPatch(std::span<const std::uint8_t> bytes, std::string source,
                       apex::core::ParseLimits limits) {
    if (bytes.size() > limits.maxInputBytes)
        throw vaoError(source, 0, "INPUT_TOO_LARGE", "VAO archive exceeds configured input size limit");
    const auto archive = readZipDirectory(bytes, source, limits);
    const auto patchV5 = findEntry(archive, "Patch_v5.data");
    const auto patchV4 = findEntry(archive, "Patch_v4.data");
    const auto patchV3 = findEntry(archive, "Patch_v3.data");
    const auto patchV1 = findEntry(archive, "Patch.data");
    std::optional<ZipEntry> patchEntry;
    std::uint32_t version = 0;
    if (patchV5) { patchEntry = patchV5; version = 5; }
    else if (patchV4) { patchEntry = patchV4; version = 4; }
    else if (patchV3) { patchEntry = patchV3; version = 3; }
    else if (patchV1) { patchEntry = patchV1; version = 1; }
    if (!patchEntry) throw vaoError(source, 0, "UNSUPPORTED_VERSION", "archive has no supported Patch.data entry");
    const auto configEntry = findEntry(archive, "config.ini");
    if (configEntry && configEntry->uncompressedSize > MAX_CONFIG_BYTES)
        throw vaoError(source, 0, "SIZE_LIMIT", "Config.ini exceeds configured size limit");
    const auto payload = extractZipEntry(bytes, *patchEntry, source, limits);
    std::vector<std::uint8_t> configBytes;
    if (configEntry) configBytes = extractZipEntry(bytes, *configEntry, source, limits);
    const auto configText = apex::core::ByteReader::decodeUtf8(configBytes, "Config.ini", "VAO", source, 0);
    VaoPatch result;
    result.source = source;
    result.version = version;
    result.entry = patchEntry->name;
    result.configText = configText;
    result.lighting = parseLighting(configText);
    result.splitAo = parseSplitAo(configText);
    result.data = parseVaoData(payload, version, result.lighting, source, limits);
    for (const auto& entry : archive.entries)
        result.archiveEntries.push_back({entry.name, entry.method, entry.compressedSize, entry.uncompressedSize});
    if (const auto extra = findEntry(archive, "extrasamples.data"))
        result.extraSamples = VaoEmbeddedExtraSamples{extra->name, 2u, 0u, extra->uncompressedSize};
    if (const auto trees = findEntry(archive, "treesamples.data"))
        result.treeSamples = VaoArchiveEntry{trees->name, trees->method, trees->compressedSize, trees->uncompressedSize};
    if (result.data.embeddedExtraSamples && !result.extraSamples) result.extraSamples = result.data.embeddedExtraSamples;
    return result;
}

} // namespace apex::formats
