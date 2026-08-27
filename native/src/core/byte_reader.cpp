#include "apex/core/byte_reader.hpp"

#include <cmath>
#include <sstream>

namespace apex::core {

namespace {

[[nodiscard]] bool continuation(std::uint8_t value) { return (value & 0xc0u) == 0x80u; }

[[nodiscard]] std::uint32_t readCodePoint(std::span<const std::uint8_t> bytes,
                                           std::size_t& index, std::string_view format,
                                           std::string_view source, std::size_t offset,
                                           std::string_view what) {
    const auto fail = [&](std::string_view message, std::size_t at) -> ParseError {
        return ParseError(std::string(format), std::string(source), offset + at,
                          "INVALID_UTF8", std::string(message) + " in " + std::string(what));
    };
    if (index >= bytes.size()) throw fail("truncated UTF-8 sequence", index);
    const std::size_t start = index;
    const std::uint8_t first = bytes[index++];
    std::uint32_t codePoint = 0;
    std::size_t count = 0;
    if (first <= 0x7f) {
        codePoint = first;
        count = 0;
    } else if (first >= 0xc2 && first <= 0xdf) {
        codePoint = first & 0x1fu;
        count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
        codePoint = first & 0x0fu;
        count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
        codePoint = first & 0x07u;
        count = 3;
    } else {
        throw fail("invalid UTF-8 leading byte", start);
    }
    if (count > bytes.size() - index) throw fail("truncated UTF-8 sequence", start);
    for (std::size_t component = 0; component < count; ++component) {
        const auto value = bytes[index++];
        if (!continuation(value)) throw fail("invalid UTF-8 continuation byte", index - 1);
        codePoint = (codePoint << 6u) | (value & 0x3fu);
    }
    if ((count == 2 && codePoint < 0x800u) || (count == 3 && codePoint < 0x10000u) ||
        codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
        throw fail("non-canonical UTF-8 code point", start);
    }
    return codePoint;
}

void appendCodePoint(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

} // namespace

ByteReader::ByteReader(std::span<const std::uint8_t> bytes, std::string source,
                       ParseLimits limits, std::string format)
    : bytes_(bytes), source_(std::move(source)), format_(std::move(format)), limits_(limits) {
    if (bytes_.size() > limits_.maxInputBytes) {
        throw error(0, "INPUT_TOO_LARGE", "input exceeds configured size limit");
    }
}

void ByteReader::need(std::size_t count, std::string_view what) const {
    if (count > bytes_.size() - offset_) {
        throw error(offset_, "TRUNCATED", "unexpected end while reading " + std::string(what));
    }
}

void ByteReader::advance(std::size_t count, std::string_view what) {
    need(count, what);
    offset_ += count;
}

std::uint8_t ByteReader::u8(std::string_view what) {
    need(1, what);
    return bytes_[offset_++];
}

std::uint32_t ByteReader::u32(std::string_view what) {
    need(4, what);
    const auto value = static_cast<std::uint32_t>(bytes_[offset_]) |
                       (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8u) |
                       (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16u) |
                       (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24u);
    offset_ += 4;
    return value;
}

float ByteReader::f32(std::string_view what) {
    const auto start = offset_;
    const auto bits = u32(what);
    const auto value = std::bit_cast<float>(bits);
    if (!std::isfinite(value)) {
        throw error(start, "NON_FINITE", "non-finite value while reading " + std::string(what));
    }
    return value;
}

std::string ByteReader::string(std::string_view what) {
    const auto lengthOffset = offset_;
    const auto length = u32(std::string(what) + " length");
    if (length > limits_.maxStringBytes) {
        throw error(lengthOffset, "STRING_TOO_LARGE", std::string(what) + " exceeds size limit");
    }
    need(length, what);
    const auto value = decodeUtf8(bytes_.subspan(offset_, length), what, format_, source_, offset_);
    offset_ += length;
    return value;
}

ParseError ByteReader::error(std::size_t at, std::string_view code,
                             std::string_view message) const {
    return ParseError(format_, source_, at, std::string(code), std::string(message));
}

std::string ByteReader::decodeUtf8(std::span<const std::uint8_t> bytes,
                                   std::string_view what, std::string_view format,
                                   std::string_view source, std::size_t offset) {
    std::string output;
    output.reserve(bytes.size());
    std::size_t index = 0;
    while (index < bytes.size()) appendCodePoint(output, readCodePoint(bytes, index, format, source, offset, what));
    return output;
}

void ByteReader::validateUtf8(std::string_view value, std::string_view what,
                              std::string_view format, std::string_view source,
                              std::size_t offset) {
    const auto* pointer = reinterpret_cast<const std::uint8_t*>(value.data());
    (void)decodeUtf8(std::span<const std::uint8_t>(pointer, value.size()), what, format, source, offset);
}

std::size_t checkedAdd(std::size_t left, std::size_t right, std::string_view format,
                       std::string_view source, std::size_t offset, std::string_view what) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw ParseError(std::string(format), std::string(source), offset, "SIZE_OVERFLOW",
                         std::string(what) + " size overflows");
    }
    return left + right;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right, std::string_view format,
                            std::string_view source, std::size_t offset, std::string_view what) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw ParseError(std::string(format), std::string(source), offset, "SIZE_OVERFLOW",
                         std::string(what) + " size overflows");
    }
    return left * right;
}

}
