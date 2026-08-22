#include "apex/formats/vao.hpp"

#include "apex/core/byte_reader.hpp"
#include "apex/core/deflate.hpp"
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
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
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

class NativeAllocationBudget final {
public:
    explicit NativeAllocationBudget(std::size_t limit) : limit_(limit) {}

    void charge(std::size_t bytes, std::string_view source, std::size_t offset,
                std::string_view what) {
        if (bytes > limit_ - used_)
            throw vaoError(source, offset, "ALLOCATION_LIMIT",
                           "VAO native allocation budget exceeded before allocating " +
                               std::string(what));
        used_ += bytes;
    }

    void chargeCount(std::size_t count, std::size_t elementBytes,
                     std::string_view source, std::size_t offset,
                     std::string_view what) {
        if (count != 0U && elementBytes > std::numeric_limits<std::size_t>::max() / count)
            throw vaoError(source, offset, "SIZE_OVERFLOW",
                           std::string(what) + " allocation size overflows");
        charge(count * elementBytes, source, offset, what);
    }

    template <typename T>
    void reserve(std::vector<T>& values, std::size_t count,
                 std::string_view source, std::size_t offset,
                 std::string_view what) {
        if (count <= values.capacity()) return;
        chargeCount(count - values.capacity(), sizeof(T), source, offset, what);
        values.reserve(count);
    }

private:
    std::size_t limit_ = 0U;
    std::size_t used_ = 0U;
};

void reserveRecordCapacity(VaoData& result, NativeAllocationBudget& budget,
                           std::string_view source, std::size_t offset) {
    if (result.records.size() != result.records.capacity()) return;
    const auto current = result.records.capacity();
    const auto next = current == 0U
                          ? 1U
                          : checkedAdd(current, current, source, offset, "VAO record vector");
    budget.reserve(result.records, next, source, offset, "VAO record objects");
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

[[nodiscard]] bool lowerEquals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
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
                                           const apex::core::ParseLimits& limits,
                                           NativeAllocationBudget& budget) {
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
    budget.chargeCount(count, sizeof(ZipEntry), source, endOffset + 10u,
                      "ZIP entry objects");
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
        budget.charge(nameLength, source, offset + 46u, "ZIP entry name");
        const auto nameBytes = bytes.subspan(offset + 46u, nameLength);
        const auto name = apex::core::ByteReader::decodeUtf8(nameBytes, "ZIP entry name", "VAO",
                                                             source, offset + 46u);
        if (!safeZipName(name)) throw vaoError(source, offset + 46u, "UNSAFE_NAME", "unsafe ZIP entry name");
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
        budget.charge(checkedAdd(
                          checkedAdd(name.size(), sizeof(std::map<std::string, bool>::value_type),
                                     source, offset + 46u, "ZIP name map node"),
                          3U * sizeof(void*), source, offset + 46u,
                          "ZIP name map node"),
                      source, offset + 46u, "ZIP name map node");
        // lower(name) creates a temporary key and the map stores its own
        // string copy. Account for both dynamic string buffers before the
        // duplicate check, in addition to the map node itself.
        budget.chargeCount(2u, name.size(), source, offset + 46u,
                           "ZIP normalized entry names");
        if (names.emplace(lower(name), true).second == false)
            throw vaoError(source, offset + 46u, "DUPLICATE_ENTRY", "duplicate ZIP entry name");
        // ZipEntry owns a separate name string after the central-directory
        // local variable is moved/copied into the archive vector.
        budget.charge(name.size(), source, offset + 46u, "ZIP archive entry name");
        archive.entries.push_back({name, flags, method, crc, compressedSize, uncompressedSize, localOffset});
        offset += body;
    }
    if (offset != directoryEnd)
        throw vaoError(source, offset, "INVALID_ZIP", "ZIP central entries do not consume the directory");

    struct Range { std::size_t begin = 0; std::size_t end = 0; };
    budget.chargeCount(archive.entries.size(), sizeof(Range), source, centralOffset,
                       "ZIP entry ranges");
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
        budget.charge(localNameLength, source, localOffset + 30u,
                      "ZIP local entry name");
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
                                                        const apex::core::ParseLimits& limits,
                                                        NativeAllocationBudget& budget) {
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
    budget.charge(nameLength, source, offset + 30u, "ZIP extracted entry name");
    const auto localName = apex::core::ByteReader::decodeUtf8(bytes.subspan(offset + 30u, nameLength),
                                                               "ZIP local name", "VAO", source, offset + 30u);
    if (localName != entry.name) throw vaoError(source, offset, "INVALID_ZIP", "ZIP local and central names differ");
    const auto maximumEntry = std::min(MAX_ENTRY_BYTES, limits.maxOutputBytes);
    budget.charge(entry.uncompressedSize, source, offset, "ZIP entry payload");
    std::vector<std::uint8_t> output;
    if (entry.method == 0u) {
        if (entry.compressedSize != entry.uncompressedSize) throw vaoError(source, offset, "INVALID_ZIP", "stored ZIP size mismatch");
        output.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                      bytes.begin() + static_cast<std::ptrdiff_t>(start + entry.compressedSize));
    } else {
        apex::core::ParseLimits deflateLimits = limits;
        deflateLimits.maxOutputBytes = maximumEntry;
        try {
            output = apex::core::inflateDeflateRaw(
                bytes.subspan(start, entry.compressedSize),
                entry.uncompressedSize, std::string(source), deflateLimits,
                start);
        } catch (const std::runtime_error& error) {
            throw vaoError(source, start, "INVALID_DEFLATE", error.what());
        }
    }
    if (output.size() != entry.uncompressedSize)
        throw vaoError(source, start, "SIZE_MISMATCH", "ZIP entry expanded to an unexpected size");
    if (crc32(output) != entry.crc) throw vaoError(source, start, "CRC_MISMATCH", "ZIP entry CRC-32 does not match");
    return output;
}

[[nodiscard]] const ZipEntry* findEntry(const ZipArchive& archive, std::string_view name) noexcept {
    for (const auto& entry : archive.entries)
        if (lowerEquals(entry.name, name)) return &entry;
    return nullptr;
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

VaoData parseVaoDataWithBudget(std::span<const std::uint8_t> bytes, std::uint32_t version,
                               VaoLighting lighting, std::string source,
                               apex::core::ParseLimits limits,
                               NativeAllocationBudget& budget) {
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
        if (result.records.size() >= limits.maxTracks)
            throw vaoError(source, recordOffset, "COUNT_LIMIT",
                           "VAO record count exceeds configured limit");
        budget.charge(nameLength, source, offset, "VAO record name");
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
            budget.charge(name.size(), source, recordOffset, "VAO embedded extra-sample name");
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
            const auto normalCount = checkedMultiply(vertexCount, 3u, source, offset, "normal output");
            budget.chargeCount(normalCount, sizeof(float), source, offset, "VAO normal values");
            record.normals.resize(normalCount);
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
            budget.chargeCount(vertexCount, sizeof(std::uint8_t), source, offset,
                               "VAO values");
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
        reserveRecordCapacity(result, budget, source, recordOffset);
        result.records.push_back(std::move(record));
        offset += payloadBytes;
    }
    result.recordCount = result.records.size();
    if (!result.embeddedExtraSamples) result.bytesRead = offset;
    return result;
}

VaoData parseVaoData(std::span<const std::uint8_t> bytes, std::uint32_t version,
                     VaoLighting lighting, std::string source,
                     apex::core::ParseLimits limits) {
    try {
        NativeAllocationBudget budget(VaoParseLimits{}.maxNativeObjectBytes);
        return parseVaoDataWithBudget(bytes, version, lighting, source, limits, budget);
    } catch (const std::bad_alloc&) {
        throw vaoError(source, 0U, "ALLOCATION_LIMIT",
                       "VAO native allocation failed within the configured budget");
    }
}

VaoData parseVaoDataWithLimits(std::span<const std::uint8_t> bytes, std::uint32_t version,
                               VaoLighting lighting, std::string source,
                               VaoParseLimits limits) {
    try {
        NativeAllocationBudget budget(limits.maxNativeObjectBytes);
        return parseVaoDataWithBudget(bytes, version, lighting, source, limits, budget);
    } catch (const std::bad_alloc&) {
        throw vaoError(source, 0U, "ALLOCATION_LIMIT",
                       "VAO native allocation failed within the configured budget");
    }
}

bool vaoNormalOverrideNameEligible(std::string_view name) noexcept {
    if (name == "AC_SEMAPHORE") return true;
    if (name.rfind("AC_", 0u) == 0u) return false;
    return std::none_of(name.begin(), name.end(), [](char value) { return value >= '0' && value <= '9'; });
}

namespace {

struct VaoBindingMatch {
    std::size_t record_index = 0U;
    std::size_t mesh_index = 0U;
};

bool checkedAddBindingBytes(std::size_t left, std::size_t right,
                            std::size_t& output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    output = left + right;
    return true;
}

bool addBindingDiagnostic(VaoBindingResult& result, const VaoBindingLimits& limits,
                          VaoBindingSeverity severity, std::string_view code,
                          std::string_view message, std::size_t record_index = 0U,
                          std::size_t mesh_index = 0U) {
    if (result.diagnostics.size() >= limits.max_diagnostics) {
        result.valid = false;
        return false;
    }
    if (code.size() > std::numeric_limits<std::size_t>::max() - message.size()) {
        result.valid = false;
        return false;
    }
    const std::size_t bytes = code.size() + message.size();
    if (bytes > limits.max_diagnostic_bytes) {
        result.valid = false;
        return false;
    }
    std::size_t used = 0U;
    for (const auto& diagnostic : result.diagnostics) {
        if (!checkedAddBindingBytes(used, diagnostic.code.size(), used) ||
            !checkedAddBindingBytes(used, diagnostic.message.size(), used)) {
            result.valid = false;
            return false;
        }
    }
    if (bytes > limits.max_diagnostic_bytes - std::min(used, limits.max_diagnostic_bytes)) {
        result.valid = false;
        return false;
    }
    result.diagnostics.push_back({severity, std::string(code), std::string(message),
                                  record_index, mesh_index});
    if (severity == VaoBindingSeverity::error) result.valid = false;
    return true;
}

bool validBindingRecord(const VaoRecord& record, std::size_t record_index,
                        const VaoBindingLimits& limits, VaoBindingResult& result,
                        std::size_t& value_bytes) {
    if (record.name.empty() || record.name.size() > limits.max_string_bytes) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_record_name_invalid",
                             "VAO binding record name is empty or exceeds its limit",
                             record_index);
        return false;
    }
    if (!std::all_of(record.firstVertex.begin(), record.firstVertex.end(),
                     [](float value) { return std::isfinite(value); })) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_record_non_finite",
                             "VAO binding record position is not finite", record_index);
        return false;
    }
    const auto expected_channel = record.type == 2U
                                      ? VaoChannel::normal
                                      : record.type == 3U ? VaoChannel::secondary
                                                          : VaoChannel::primary;
    if ((record.type != 0U && record.type != 1U && record.type != 2U && record.type != 3U) ||
        record.channel != expected_channel) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_record_type_invalid",
                             "VAO binding record type and channel are inconsistent",
                             record_index);
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(record.vertexCount);
    if (count == 0U || count > limits.max_vertices_per_mesh) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_record_vertex_limit",
                             "VAO binding record vertex count is outside its configured limit",
                             record_index);
        return false;
    }
    if (record.type == 2U) {
        if (count > std::numeric_limits<std::size_t>::max() / 3U ||
            record.normals.size() != count * 3U) {
            addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                                 "vao_binding_normal_count_invalid",
                                 "VAO normal record count does not match its values",
                                 record_index);
            return false;
        }
        if (!std::all_of(record.normals.begin(), record.normals.end(),
                         [](float value) { return std::isfinite(value); })) {
            addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                                 "vao_binding_normal_non_finite",
                                 "VAO normal record contains a non-finite value",
                                 record_index);
            return false;
        }
        if (record.normals.size() > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
            !checkedAddBindingBytes(value_bytes,
                                    record.normals.size() * sizeof(float), value_bytes)) {
            addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                                 "vao_binding_value_size_overflow",
                                 "VAO normal values exceed the size range", record_index);
            return false;
        }
    } else if (record.values.size() != count) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_value_count_invalid",
                             "VAO value record count does not match its values", record_index);
        return false;
    } else if (!checkedAddBindingBytes(value_bytes, record.values.size(), value_bytes)) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_value_size_overflow",
                             "VAO values exceed the size range", record_index);
        return false;
    }
    if (value_bytes > limits.max_value_bytes) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_value_limit",
                             "VAO binding values exceed their configured limit", record_index);
        return false;
    }
    return true;
}

}  // namespace

VaoBindingResult bindVaoPatch(const VaoPatch& patch,
                              std::span<const VaoMeshTarget> meshes,
                              VaoBindingLimits limits) {
    VaoBindingResult result;
    result.mean = 0.0;
    result.patch_record_count = patch.data.records.size();
    if (limits.max_meshes == 0U || limits.max_vertices_per_mesh == 0U ||
        limits.max_records == 0U || limits.max_string_bytes == 0U ||
        limits.max_value_bytes == 0U || limits.max_diagnostics == 0U ||
        limits.max_diagnostic_bytes == 0U) {
        result.valid = false;
        return result;
    }
    if (meshes.size() > limits.max_meshes) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_mesh_limit",
                             "VAO binding mesh count exceeds its configured limit");
        return result;
    }
    if (patch.data.records.size() > limits.max_records) {
        addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                             "vao_binding_record_limit",
                             "VAO binding record count exceeds its configured limit");
        return result;
    }
    for (std::size_t mesh_index = 0U; mesh_index < meshes.size(); ++mesh_index) {
        const auto& mesh = meshes[mesh_index];
        if (mesh.name.empty() || mesh.name.size() > limits.max_string_bytes ||
            mesh.vertex_stride_floats < 3U || mesh.vertices.empty() ||
            mesh.vertices.size() % mesh.vertex_stride_floats != 0U ||
            mesh.vertices.size() / mesh.vertex_stride_floats > limits.max_vertices_per_mesh) {
            addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                                 "vao_binding_mesh_invalid",
                                 "VAO binding mesh name, stride, or vertex count is invalid",
                                 0U, mesh_index);
            return result;
        }
        for (std::size_t vertex = 0U;
             vertex < mesh.vertices.size(); vertex += mesh.vertex_stride_floats) {
            if (!std::isfinite(mesh.vertices[vertex]) ||
                !std::isfinite(mesh.vertices[vertex + 1U]) ||
                !std::isfinite(mesh.vertices[vertex + 2U])) {
                addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                                     "vao_binding_mesh_non_finite",
                                     "VAO binding mesh position is not finite", 0U,
                                     mesh_index);
                return result;
            }
        }
    }

    std::size_t source_value_bytes = 0U;
    for (std::size_t record_index = 0U; record_index < patch.data.records.size(); ++record_index) {
        if (!validBindingRecord(patch.data.records[record_index], record_index, limits,
                                result, source_value_bytes))
            return result;
    }
    if (patch.splitAo.present) {
        result.split_ao_staged = true;
        addBindingDiagnostic(result, limits, VaoBindingSeverity::warning,
                             "vao_split_ao_staged",
                             "VAO split-AO animation application remains staged");
    }

    std::unordered_map<std::string_view, std::vector<std::size_t>> meshes_by_name;
    meshes_by_name.reserve(meshes.size());
    for (std::size_t mesh_index = 0U; mesh_index < meshes.size(); ++mesh_index)
        meshes_by_name[meshes[mesh_index].name].push_back(mesh_index);

    std::vector<VaoBindingMatch> matches;
    matches.reserve(patch.data.records.size());
    for (std::size_t record_index = 0U; record_index < patch.data.records.size(); ++record_index) {
        const auto& record = patch.data.records[record_index];
        const bool normal = record.channel == VaoChannel::normal;
        if (normal) ++result.normal_records;
        if (normal && !vaoNormalOverrideNameEligible(record.name)) {
            addBindingDiagnostic(result, limits, VaoBindingSeverity::warning,
                                 "vao_normal_override_ineligible",
                                 "VAO normal override name is not eligible", record_index);
            continue;
        }
        if (record.alternate) {
            if (!normal) {
                ++result.alternate_records;
                if (result.alternate.size() < 32U) result.alternate.emplace_back(record.name);
                addBindingDiagnostic(result, limits, VaoBindingSeverity::warning,
                                     "vao_alternate_record_staged",
                                     "VAO alternate record is excluded from normal binding",
                                     record_index);
            }
            continue;
        }
        std::size_t matched_mesh = std::numeric_limits<std::size_t>::max();
        const auto candidates = meshes_by_name.find(record.name);
        if (candidates != meshes_by_name.end()) {
            for (const std::size_t mesh_index : candidates->second) {
                const auto& mesh = meshes[mesh_index];
                if (mesh.vertices.size() / mesh.vertex_stride_floats !=
                    record.vertexCount)
                    continue;
                const auto& first = mesh.vertices;
                const double dx =
                    static_cast<double>(first[0]) - record.firstVertex[0];
                const double dy =
                    static_cast<double>(first[1]) - record.firstVertex[1];
                const double dz =
                    static_cast<double>(first[2]) - record.firstVertex[2];
                if (dx * dx + dy * dy + dz * dz < 0.01) {
                    matched_mesh = mesh_index;
                    break;
                }
            }
        }
        if (matched_mesh == std::numeric_limits<std::size_t>::max()) {
            if (normal) {
                ++result.unmatched_normal_records;
                addBindingDiagnostic(result, limits, VaoBindingSeverity::warning,
                                     "vao_normal_record_unmatched",
                                     "VAO normal record has no matching mesh", record_index);
            } else {
                ++result.unmatched_records;
                if (result.unmatched.size() < 32U) result.unmatched.emplace_back(record.name);
                addBindingDiagnostic(result, limits, VaoBindingSeverity::warning,
                                     "vao_record_unmatched",
                                     "VAO record has no matching mesh", record_index);
            }
            continue;
        }
        matches.push_back({record_index, matched_mesh});
    }

    std::size_t matched_value_bytes = 0U;
    for (const auto& match : matches) {
        const auto& record = patch.data.records[match.record_index];
        const std::size_t bytes = record.channel == VaoChannel::normal
                                      ? record.normals.size() * sizeof(float)
                                      : record.values.size();
        if (!checkedAddBindingBytes(matched_value_bytes, bytes, matched_value_bytes) ||
            matched_value_bytes > limits.max_value_bytes) {
            addBindingDiagnostic(result, limits, VaoBindingSeverity::error,
                                 "vao_binding_value_limit",
                                 "Matched VAO values exceed their configured limit",
                                 match.record_index, match.mesh_index);
            return result;
        }
    }

    const std::size_t invalid_binding = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> binding_index(meshes.size(), invalid_binding);
    result.bindings.reserve(matches.size());
    for (const auto& match : matches) {
        const auto& record = patch.data.records[match.record_index];
        auto& index = binding_index[match.mesh_index];
        if (index == invalid_binding) {
            index = result.bindings.size();
            result.bindings.push_back({match.mesh_index, std::nullopt, std::nullopt,
                                       std::nullopt});
        }
        auto& binding = result.bindings[index];
        if (record.channel == VaoChannel::primary)
            binding.primary = record.values;
        else if (record.channel == VaoChannel::secondary)
            binding.secondary = record.values;
        else
            binding.normal = record.normals;
        if (record.channel == VaoChannel::normal)
            ++result.matched_normal_records;
        else
            ++result.matched_records;
    }
    result.matched_meshes = result.bindings.size();
    result.unmatched_normal_records = result.normal_records - result.matched_normal_records;
    for (const auto& binding : result.bindings) {
        if (binding.primary.has_value()) {
            ++result.primary_meshes;
            for (const auto value : *binding.primary) {
                result.value_count += 1U;
                result.minimum = std::min(result.minimum, value);
                result.maximum = std::max(result.maximum, value);
                result.mean += static_cast<double>(value);
            }
        }
        if (binding.secondary.has_value()) ++result.secondary_meshes;
        if (binding.normal.has_value()) {
            ++result.normal_meshes;
            result.normal_vertex_count += binding.normal->size() / 3U;
        }
    }
    if (result.value_count != 0U)
        result.mean /= static_cast<double>(result.value_count);
    else
        result.mean = 255.0;
    return result;
}

VaoPatch parseVaoPatch(std::span<const std::uint8_t> bytes, std::string source,
                       apex::core::ParseLimits limits) {
    VaoParseLimits vaoLimits;
    static_cast<apex::core::ParseLimits&>(vaoLimits) = limits;
    return parseVaoPatchWithLimits(bytes, std::move(source), std::move(vaoLimits));
}

VaoPatch parseVaoPatchWithLimits(std::span<const std::uint8_t> bytes, std::string source,
                                 VaoParseLimits limits) {
    try {
        if (bytes.size() > limits.maxInputBytes)
            throw vaoError(source, 0, "INPUT_TOO_LARGE", "VAO archive exceeds configured input size limit");
        NativeAllocationBudget budget(limits.maxNativeObjectBytes);
        const auto archive = readZipDirectory(bytes, source, limits, budget);
        const auto patchV5 = findEntry(archive, "Patch_v5.data");
        const auto patchV4 = findEntry(archive, "Patch_v4.data");
        const auto patchV3 = findEntry(archive, "Patch_v3.data");
        const auto patchV1 = findEntry(archive, "Patch.data");
        const ZipEntry* patchEntry = nullptr;
        std::uint32_t version = 0;
        if (patchV5 != nullptr) { patchEntry = patchV5; version = 5; }
        else if (patchV4 != nullptr) { patchEntry = patchV4; version = 4; }
        else if (patchV3 != nullptr) { patchEntry = patchV3; version = 3; }
        else if (patchV1 != nullptr) { patchEntry = patchV1; version = 1; }
        if (!patchEntry) throw vaoError(source, 0, "UNSUPPORTED_VERSION", "archive has no supported Patch.data entry");
        const ZipEntry* configEntry = findEntry(archive, "config.ini");
        if (configEntry != nullptr && configEntry->uncompressedSize > MAX_CONFIG_BYTES)
            throw vaoError(source, 0, "SIZE_LIMIT", "Config.ini exceeds configured size limit");
        const auto payload = extractZipEntry(bytes, *patchEntry, source, limits, budget);
        std::vector<std::uint8_t> configBytes;
        if (configEntry) configBytes = extractZipEntry(bytes, *configEntry, source, limits, budget);
        budget.charge(configBytes.size(), source, 0U, "Config.ini text");
        VaoPatch result;
        budget.charge(source.size(), source, 0U, "VAO source name");
        budget.charge(patchEntry->name.size(), source, 0U, "VAO patch entry name");
        result.source = source;
        result.version = version;
        result.entry = patchEntry->name;
        result.configText = apex::core::ByteReader::decodeUtf8(configBytes, "Config.ini", "VAO", source, 0);
        result.lighting = parseLighting(result.configText);
        result.splitAo = parseSplitAo(result.configText);
        result.data = parseVaoDataWithBudget(payload, version, result.lighting, source, limits, budget);
        budget.reserve(result.archiveEntries, archive.entries.size(), source, 0U,
                       "VAO archive records");
        for (const auto& entry : archive.entries) {
            budget.charge(entry.name.size(), source, 0U, "VAO archive record name");
            result.archiveEntries.push_back({entry.name, entry.method, entry.compressedSize, entry.uncompressedSize});
        }
        if (const auto* extra = findEntry(archive, "extrasamples.data")) {
            budget.charge(extra->name.size(), source, 0U, "VAO extra-sample entry name");
            result.extraSamples = VaoEmbeddedExtraSamples{extra->name, 2u, 0u, extra->uncompressedSize};
        }
        if (const auto* trees = findEntry(archive, "treesamples.data")) {
            budget.charge(trees->name.size(), source, 0U, "VAO tree-sample entry name");
            result.treeSamples = VaoArchiveEntry{trees->name, trees->method, trees->compressedSize, trees->uncompressedSize};
        }
        if (result.data.embeddedExtraSamples && !result.extraSamples) result.extraSamples = result.data.embeddedExtraSamples;
        return result;
    } catch (const std::bad_alloc&) {
        throw vaoError(source, 0U, "ALLOCATION_LIMIT",
                       "VAO native allocation failed within the configured budget");
    }
}

} // namespace apex::formats
