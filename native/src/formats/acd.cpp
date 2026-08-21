#include "apex/formats/acd.hpp"

#include "apex/core/byte_reader.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace apex::formats {

namespace {

using apex::core::ByteReader;
using apex::core::ParseError;
using apex::core::ParseLimits;

[[nodiscard]] ParseError error(std::string_view source, std::size_t offset,
                               std::string_view code, std::string_view message) {
    return ParseError("ACD", std::string(source), offset, std::string(code), std::string(message));
}

[[nodiscard]] std::int32_t wrapped(std::int64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

[[nodiscard]] std::int32_t imul(std::int32_t left, std::int32_t right) {
    const auto bits = static_cast<std::uint32_t>(left) * static_cast<std::uint32_t>(right);
    return std::bit_cast<std::int32_t>(bits);
}

[[nodiscard]] std::string trimAndLowerAscii(std::string_view input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) --end;
    std::string output(input.substr(begin, end - begin));
    for (auto& character : output) {
        const auto value = static_cast<unsigned char>(character);
        if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(value + ('a' - 'A'));
    }
    return output;
}

// JavaScript's key derivation uses charCodeAt(0) over a spread string.  For
// non-BMP UTF-8 input this is therefore the high UTF-16 surrogate, not the
// Unicode scalar value.  Asset directory names are normally ASCII, but
// retaining that detail makes the native implementation deterministic too.
[[nodiscard]] std::vector<std::int32_t> utf16Codes(std::string_view value) {
    std::vector<std::int32_t> codes;
    for (std::size_t index = 0; index < value.size();) {
        const auto start = index;
        const auto first = static_cast<std::uint8_t>(value[index++]);
        std::uint32_t codePoint = 0;
        std::size_t continuationCount = 0;
        if (first <= 0x7fu) {
            codePoint = first;
        } else if (first >= 0xc2u && first <= 0xdfu) {
            codePoint = first & 0x1fu;
            continuationCount = 1;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codePoint = first & 0x0fu;
            continuationCount = 2;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codePoint = first & 0x07u;
            continuationCount = 3;
        } else {
            throw error("data.acd", start, "INVALID_UTF8", "invalid asset directory name");
        }
        if (continuationCount > value.size() - index)
            throw error("data.acd", start, "INVALID_UTF8", "truncated asset directory name");
        for (std::size_t component = 0; component < continuationCount; ++component) {
            const auto part = static_cast<std::uint8_t>(value[index++]);
            if ((part & 0xc0u) != 0x80u)
                throw error("data.acd", index - 1, "INVALID_UTF8", "invalid asset directory name");
            codePoint = (codePoint << 6u) | (part & 0x3fu);
        }
        if ((continuationCount == 2 && codePoint < 0x800u) ||
            (continuationCount == 3 && codePoint < 0x10000u) || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            throw error("data.acd", start, "INVALID_UTF8", "non-canonical asset directory name");
        }
        if (codePoint <= 0xffffu) {
            codes.push_back(static_cast<std::int32_t>(codePoint));
        } else {
            codes.push_back(static_cast<std::int32_t>(0xd800u + ((codePoint - 0x10000u) >> 10u)));
        }
    }
    return codes;
}

[[nodiscard]] std::int32_t readI32(ByteReader& reader, std::string_view what) {
    return std::bit_cast<std::int32_t>(reader.u32(what));
}

[[nodiscard]] std::string readAcdString(ByteReader& reader, const ParseLimits& limits,
                                         std::string_view source) {
    const auto lengthOffset = reader.offset();
    const auto length = readI32(reader, "entry-name length");
    if (length < 0)
        throw reader.error(lengthOffset, "NEGATIVE_LENGTH", "negative entry-name length");
    const auto count = static_cast<std::size_t>(length);
    if (count > limits.maxStringBytes)
        throw reader.error(lengthOffset, "STRING_TOO_LARGE", "entry name exceeds size limit");
    reader.need(count, "entry name");
    const auto value = ByteReader::decodeUtf8(reader.bytes().subspan(reader.offset(), count),
                                               "entry name", "ACD", source, reader.offset());
    reader.advance(count, "entry name");
    return value;
}

[[nodiscard]] std::string normalizedPath(std::string_view value) {
    std::string output(value);
    std::replace(output.begin(), output.end(), '\\', '/');
    return output;
}

[[nodiscard]] std::string lowerPath(std::string_view value) {
    std::string output(value);
    for (auto& character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(byte + ('a' - 'A'));
    }
    return output;
}

struct PathResult {
    std::string path;
    bool safe = false;
};

[[nodiscard]] PathResult safePath(std::string_view value) {
    PathResult result{normalizedPath(value), false};
    const auto& path = result.path;
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string::npos) return result;
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
        path[1] == ':' && path[2] == '/')
        return result;

    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto length = end == std::string::npos ? path.size() - begin : end - begin;
        if (length == 0 || (length == 1 && path[begin] == '.') ||
            (length == 2 && path[begin] == '.' && path[begin + 1] == '.'))
            return result;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    result.safe = true;
    return result;
}

[[nodiscard]] std::string quoted(std::string_view value) {
    std::string result = "\"";
    for (const auto character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

} // namespace

AcdKey createAcdKey(std::string_view assetName) {
    AcdKey key;
    key.assetName = trimAndLowerAscii(assetName);
    if (key.assetName.empty())
        throw std::invalid_argument("an asset directory name is required for data.acd");
    const auto codes = utf16Codes(key.assetName);
    const auto count = codes.size();

    std::int32_t a = 0;
    for (const auto code : codes) a = wrapped(static_cast<std::int64_t>(a) + code);

    std::int32_t b = 0;
    for (std::size_t index = 0; index + 1 < count; index += 2)
        b = wrapped(static_cast<std::int64_t>(imul(b, codes[index])) - codes[index + 1]);

    std::int32_t c = 0;
    for (std::size_t index = 1; index + 3 < count; index += 3) {
        const auto denominator = static_cast<std::int64_t>(codes[index + 1]) + 27;
        const auto quotient = static_cast<std::int64_t>(imul(c, codes[index])) / denominator;
        c = wrapped(quotient - 27 - codes[index - 1]);
    }

    std::int32_t d = 5763;
    for (std::size_t index = 1; index < count; ++index)
        d = wrapped(static_cast<std::int64_t>(d) - codes[index]);

    std::int32_t e = 66;
    for (std::size_t index = 1; index + 4 < count; index += 4) {
        const auto left = wrapped(static_cast<std::int64_t>(codes[index]) + 15);
        const auto right = wrapped(static_cast<std::int64_t>(codes[index - 1]) + 15);
        e = wrapped(static_cast<std::int64_t>(imul(imul(left, e), right)) + 22);
    }

    std::int32_t f = 101;
    for (std::size_t index = 0; index + 2 < count; index += 2)
        f = wrapped(static_cast<std::int64_t>(f) - codes[index]);

    std::int32_t g = 171;
    for (std::size_t index = 0; index + 2 < count; index += 2) {
        if (codes[index] == 0) {
            // JavaScript's remainder becomes NaN for a zero divisor and the
            // final byte conversion maps NaN to zero.
            g = 0;
        } else {
            g = static_cast<std::int32_t>(g % codes[index]);
        }
    }

    std::int32_t h = 171;
    bool hNan = false;
    for (std::size_t index = 0; index + 1 < count; ++index) {
        if (codes[index] == 0) {
            hNan = true;
            continue;
        }
        if (!hNan) h = wrapped(static_cast<std::int64_t>(h / codes[index]) + codes[index + 1]);
    }

    const std::array<std::int32_t, 8> values = {a, b, c, d, e, f, g, hNan ? 0 : h};
    for (std::size_t index = 0; index < values.size(); ++index)
        key.octets[index] = static_cast<std::uint8_t>(values[index]);
    for (std::size_t index = 0; index < key.octets.size(); ++index) {
        if (index != 0) key.password.push_back('-');
        key.password += std::to_string(key.octets[index]);
    }
    return key;
}

std::vector<std::uint8_t> decryptAcdPayload(std::span<const std::uint8_t> storedBytes,
                                            std::size_t plainLength,
                                            std::string_view password,
                                            ParseLimits limits, std::string_view source,
                                            std::size_t offset) {
    if (password.empty()) throw error(source, offset, "INVALID_PASSWORD", "an ACD password is required");
    if (plainLength > limits.maxOutputBytes)
        throw error(source, offset, "OUTPUT_TOO_LARGE", "decoded ACD payload exceeds size limit");
    const auto storedLength = apex::core::checkedMultiply(plainLength, 4, "ACD", source, offset,
                                                           "ACD payload");
    if (storedLength > storedBytes.size())
        throw error(source, offset, "TRUNCATED", "truncated ACD payload");

    std::vector<std::uint8_t> data(plainLength);
    for (std::size_t index = 0; index < plainLength; ++index) {
        const auto passwordByte = static_cast<unsigned char>(password[index % password.size()]);
        data[index] = static_cast<std::uint8_t>(storedBytes[index * 4] - passwordByte);
    }
    return data;
}

AcdArchive parseAcd(std::span<const std::uint8_t> bytes, std::string_view assetName,
                    std::string_view source, ParseLimits limits) {
    const auto derived = createAcdKey(assetName);
    ByteReader reader(bytes, std::string(source), limits, "ACD");
    if (bytes.empty()) throw reader.error(0, "TRUNCATED", "empty ACD archive");
    AcdArchive archive;
    archive.source = std::string(source);
    archive.assetName = derived.assetName;
    archive.key = derived.password;
    archive.keyOctets = derived.octets;
    archive.byteLength = bytes.size();

    if (bytes.size() >= 4 && std::bit_cast<std::int32_t>(
                                  static_cast<std::uint32_t>(bytes[0]) |
                                  (static_cast<std::uint32_t>(bytes[1]) << 8u) |
                                  (static_cast<std::uint32_t>(bytes[2]) << 16u) |
                                  (static_cast<std::uint32_t>(bytes[3]) << 24u)) == -1111) {
        reader.advance(4, "ACD marker");
        archive.header = readI32(reader, "ACD header value");
    }

    std::size_t decodedBytes = 0;
    while (reader.offset() < bytes.size()) {
        if (archive.entries.size() >= limits.maxTracks)
            throw reader.error(reader.offset(), "COUNT_LIMIT", "ACD entry count exceeds configured limit");
        const auto recordOffset = reader.offset();
        const auto name = readAcdString(reader, limits, source);
        const auto lengthOffset = reader.offset();
        const auto plainLengthSigned = readI32(reader, "entry payload length");
        if (plainLengthSigned < 0)
            throw reader.error(lengthOffset, "NEGATIVE_LENGTH", "negative ACD payload length");
        const auto plainLength = static_cast<std::size_t>(plainLengthSigned);
        if (plainLength > limits.maxOutputBytes - std::min(decodedBytes, limits.maxOutputBytes))
            throw reader.error(lengthOffset, "OUTPUT_TOO_LARGE", "decoded ACD output exceeds size limit");
        const auto storedLength = apex::core::checkedMultiply(plainLength, 4, "ACD", source,
                                                               lengthOffset, "ACD payload");
        reader.need(storedLength, "entry payload");
        const auto payloadOffset = reader.offset();
        const auto data = decryptAcdPayload(reader.bytes().subspan(payloadOffset, storedLength),
                                            plainLength, archive.key, limits, source, payloadOffset);
        reader.advance(storedLength, "entry payload");
        decodedBytes += plainLength;

        const auto path = safePath(name);
        AcdEntry entry;
        entry.name = name;
        entry.path = path.path;
        entry.safe = path.safe;
        entry.data = data;
        entry.size = plainLength;
        entry.storedBytes = storedLength;
        entry.recordOffset = recordOffset;
        entry.payloadOffset = payloadOffset;
        archive.entries.push_back(std::move(entry));
        const auto entryIndex = archive.entries.size() - 1;
        if (!path.safe) {
            archive.warnings.push_back(archive.source + ": ignored unsafe archive path " + quoted(name));
            continue;
        }
        const auto lookup = lowerPath(path.path);
        if (archive.byPath.contains(lookup)) {
            archive.warnings.push_back(archive.source + ": duplicate archive path " + path.path);
        } else {
            archive.byPath.emplace(lookup, entryIndex);
        }
    }
    if (archive.entries.empty())
        throw reader.error(reader.offset(), "TRUNCATED", "ACD archive contains no records");
    archive.bytesRead = reader.offset();
    return archive;
}

const AcdEntry* findAcdEntry(const AcdArchive& archive, std::string_view path) {
    const auto lookup = lowerPath(normalizedPath(path));
    const auto found = archive.byPath.find(lookup);
    if (found == archive.byPath.end() || found->second >= archive.entries.size()) return nullptr;
    return &archive.entries[found->second];
}

} // namespace apex::formats
