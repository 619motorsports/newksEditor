#pragma once

#include "apex/core/parse_error.hpp"
#include "apex/core/parse_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::formats {

using AcdError = apex::core::ParseError;

struct AcdKey {
    std::string assetName;
    std::array<std::uint8_t, 8> octets{};
    std::string password;
};

// A decoded ACD record is a virtual, read-only file.  The parser intentionally
// exposes no archive writer: packed data.acd files are inspection inputs only.
struct AcdEntry {
    std::string name;
    std::string path;
    bool safe = false;
    std::vector<std::uint8_t> data;
    std::size_t size = 0;
    std::size_t storedBytes = 0;
    std::size_t recordOffset = 0;
    std::size_t payloadOffset = 0;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return data; }
};

struct AcdArchive {
    std::string source;
    std::string assetName;
    std::string key;
    std::array<std::uint8_t, 8> keyOctets{};
    std::optional<std::int32_t> header;
    std::vector<AcdEntry> entries;
    // Keys are lower-case normalized paths.  The first safe record wins when
    // the archive contains case-only or slash-style duplicates.
    std::map<std::string, std::size_t> byPath;
    std::vector<std::string> warnings;
    std::size_t byteLength = 0;
    std::size_t bytesRead = 0;
};

[[nodiscard]] AcdKey createAcdKey(std::string_view assetName);

[[nodiscard]] std::vector<std::uint8_t> decryptAcdPayload(
    std::span<const std::uint8_t> storedBytes, std::size_t plainLength,
    std::string_view password, apex::core::ParseLimits limits = {},
    std::string_view source = "data.acd", std::size_t offset = 0);

[[nodiscard]] AcdArchive parseAcd(
    std::span<const std::uint8_t> bytes, std::string_view assetName,
    std::string_view source = "data.acd", apex::core::ParseLimits limits = {});

[[nodiscard]] const AcdEntry* findAcdEntry(const AcdArchive& archive,
                                           std::string_view path);

// Snake-case aliases keep this format convenient for the older native module
// naming convention while the primary API mirrors the JavaScript reference.
[[nodiscard]] inline AcdKey create_acd_key(std::string_view assetName) {
    return createAcdKey(assetName);
}

[[nodiscard]] inline AcdArchive parse_acd(
    std::span<const std::uint8_t> bytes, std::string_view assetName,
    std::string_view source = "data.acd", apex::core::ParseLimits limits = {}) {
    return parseAcd(bytes, assetName, source, limits);
}

[[nodiscard]] inline const AcdEntry* find_acd_entry(const AcdArchive& archive,
                                                    std::string_view path) {
    return findAcdEntry(archive, path);
}

} // namespace apex::formats
