#include "apex/core/parse_error.hpp"
#include "apex/formats/vao.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using apex::core::ParseError;
using namespace apex::formats;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
    bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

std::vector<std::uint8_t> record(std::string_view name, std::uint32_t type,
                                 std::array<float, 3> first, std::uint32_t count,
                                 std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> bytes(4u + name.size() + 4u + 12u + 4u + payload.size(), 0);
    std::size_t offset = 0;
    put32(bytes, offset, static_cast<std::uint32_t>(name.size())); offset += 4u;
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)); offset += name.size();
    put32(bytes, offset, type); offset += 4u;
    for (const auto value : first) { put32(bytes, offset, std::bit_cast<std::uint32_t>(value)); offset += 4u; }
    put32(bytes, offset, count); offset += 4u;
    std::copy(payload.begin(), payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return bytes;
}

std::vector<std::uint8_t> halfWords(std::initializer_list<std::uint16_t> values) {
    std::vector<std::uint8_t> bytes(values.size() * 2u);
    std::size_t offset = 0;
    for (const auto value : values) { put16(bytes, offset, value); offset += 2u; }
    return bytes;
}

struct ZipInput {
    std::string name;
    std::vector<std::uint8_t> compressed;
    std::vector<std::uint8_t> plain;
    std::uint16_t method = 0;
};

std::vector<std::uint8_t> zip(std::vector<ZipInput> entries) {
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> localOffsets;
    for (const auto& entry : entries) {
        localOffsets.push_back(bytes.size());
        const auto offset = bytes.size();
        bytes.resize(offset + 30u + entry.name.size() + entry.compressed.size());
        put32(bytes, offset, 0x04034b50u); put16(bytes, offset + 4u, 20u); put16(bytes, offset + 6u, 0u);
        put16(bytes, offset + 8u, entry.method); put16(bytes, offset + 10u, entry.method);
        put32(bytes, offset + 14u, crc32(entry.plain)); put32(bytes, offset + 18u, static_cast<std::uint32_t>(entry.compressed.size()));
        put32(bytes, offset + 22u, static_cast<std::uint32_t>(entry.plain.size())); put16(bytes, offset + 26u, static_cast<std::uint16_t>(entry.name.size())); put16(bytes, offset + 28u, 0u);
        std::copy(entry.name.begin(), entry.name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 30u));
        std::copy(entry.compressed.begin(), entry.compressed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 30u + entry.name.size()));
    }
    const auto centralOffset = bytes.size();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto offset = bytes.size();
        bytes.resize(offset + 46u + entry.name.size());
        put32(bytes, offset, 0x02014b50u); put16(bytes, offset + 4u, 20u); put16(bytes, offset + 6u, 20u);
        put16(bytes, offset + 8u, 0u); put16(bytes, offset + 10u, entry.method); put32(bytes, offset + 16u, crc32(entry.plain));
        put32(bytes, offset + 20u, static_cast<std::uint32_t>(entry.compressed.size())); put32(bytes, offset + 24u, static_cast<std::uint32_t>(entry.plain.size()));
        put16(bytes, offset + 28u, static_cast<std::uint16_t>(entry.name.size())); put16(bytes, offset + 30u, 0u); put16(bytes, offset + 32u, 0u);
        put16(bytes, offset + 34u, 0u); put16(bytes, offset + 36u, 0u); put32(bytes, offset + 38u, 0u); put32(bytes, offset + 42u, static_cast<std::uint32_t>(localOffsets[index]));
        std::copy(entry.name.begin(), entry.name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 46u));
    }
    const auto centralSize = bytes.size() - centralOffset;
    const auto end = bytes.size();
    bytes.resize(end + 22u, 0);
    put32(bytes, end, 0x06054b50u); put16(bytes, end + 8u, static_cast<std::uint16_t>(entries.size())); put16(bytes, end + 10u, static_cast<std::uint16_t>(entries.size()));
    put32(bytes, end + 12u, static_cast<std::uint32_t>(centralSize)); put32(bytes, end + 16u, static_cast<std::uint32_t>(centralOffset));
    return bytes;
}

template <typename Function>
void expectsParseError(Function&& function, std::string_view code) {
    bool caught = false;
    try { function(); }
    catch (const ParseError& error) {
        caught = true;
        require(error.format() == "VAO", "unexpected VAO error format");
        require(error.code() == code, "unexpected VAO error code");
    }
    require(caught, "expected VAO ParseError");
}

void decodesAoVersions() {
    const auto modern = record("mesh", 1u, {1, 2, 3}, 3u, {0, 64, 255});
    require(parseVaoData(modern, 4).records[0].values == std::vector<std::uint8_t>{0, 127, 255}, "v4 square-root AO");
    require(parseVaoData(modern, 5).records[0].values == std::vector<std::uint8_t>{0, 64, 255}, "v5 linear AO");
    const auto modernRgb = record("rgb", 0u, {0, 0, 0}, 2u, {1, 1, 0, 2, 2, 2});
    require(parseVaoData(modernRgb, 4).records[0].values == std::vector<std::uint8_t>{0, 22},
            "v4 AO averages before square root");
    const auto legacy = record("mesh", 1u, {0, 0, 0}, 3u, halfWords({0, 0x3800u, 0x3c00u}));
    const auto parsed = parseVaoData(legacy, 1u, VaoLighting{1, 1, 1});
    require(parsed.records[0].values == std::vector<std::uint8_t>{0, 179, 255}, "legacy half AO");
    const auto extra = record("@@__EXTRA_AO@", 4u, {0, 0, 0}, 42u, {1, 2, 3});
    const auto parsedExtra = parseVaoData(extra, 5u);
    require(parsedExtra.records.empty() && parsedExtra.embeddedExtraSamples.has_value() &&
                parsedExtra.embeddedExtraSamples->samples == 42u && parsedExtra.embeddedExtraSamples->bytes == 3u,
            "embedded extra AO metadata");
}

void enforcesLimitsAndDeflateFailures() {
    const auto valid = record("mesh", 1u, {0, 0, 0}, 2u, {0, 255});
    auto inputLimited = apex::core::ParseLimits{};
    inputLimited.maxInputBytes = valid.size() - 1u;
    expectsParseError([&] { (void)parseVaoData(valid, 5u, {}, "limited", inputLimited); }, "INPUT_TOO_LARGE");
    auto outputLimited = apex::core::ParseLimits{};
    outputLimited.maxOutputBytes = 1u;
    expectsParseError([&] { (void)parseVaoData(valid, 5u, {}, "limited", outputLimited); }, "OUTPUT_TOO_LARGE");

    const auto archive = zip({{"Patch_v5.data", valid, valid, 0}});
    auto archiveLimited = apex::core::ParseLimits{};
    archiveLimited.maxOutputBytes = 1u;
    expectsParseError([&] { (void)parseVaoPatch(archive, "limited", archiveLimited); }, "SIZE_LIMIT");

    const std::vector<std::uint8_t> badDeflate{0x00u};
    const std::vector<std::uint8_t> plain{'x'};
    const auto malformed = zip({{"Patch_v5.data", valid, valid, 0}, {"Config.ini", badDeflate, plain, 8}});
    expectsParseError([&] { (void)parseVaoPatch(malformed); }, "INVALID_DEFLATE");
}

void decodesNormalsAndMetadata() {
    const auto legacy = parseVaoData(record("legacy", 2u, {0, 0, 0}, 2u,
                                             halfWords({0x3c00u, 0x3c00u, 0, 0, 0, 0x3c00u})), 1u);
    require(legacy.records[0].normals.size() == 6u && legacy.records[0].normals[0] > .707f &&
                legacy.records[0].normals[1] > .707f, "legacy normal normalization");
    const auto modernPayload = record("modern", 2u, {0, 0, 0}, 1u, {255, 128, 0});
    const auto modern = parseVaoData(modernPayload, 5u);
    const auto expectedX = 1.0f / std::hypot(1.0f, std::pow(128.0f / 255.0f, 2.0f));
    require(std::abs(modern.records[0].normals[0] - expectedX) < 1e-6f && modern.records[0].normals[2] == 0,
            "v5 normal conversion");
    require(vaoNormalOverrideNameEligible("ROAD_MAIN") && vaoNormalOverrideNameEligible("AC_SEMAPHORE") &&
                !vaoNormalOverrideNameEligible("AC_PIT") && !vaoNormalOverrideNameEligible("ROAD2"),
            "normal override eligibility");
    const auto config = "[SPLIT_AO]\nCOCKPIT_HR=COCKPIT_HR\nDOOR_EXP=2.5\nDOOR_NODES=DOOR_L, door_l\nWING_ANIM_0_NAME=rear_wing.ksanim\nWING_ANIM_0_EXP=1.25\nWING_ANIM_0_NODES=WING_ROOT\n";
    const auto split = parseVaoPatch(zip({{"Patch_v5.data", modernPayload, modernPayload, 0}, {"Config.ini", std::vector<std::uint8_t>(config, config + std::strlen(config)), std::vector<std::uint8_t>(config, config + std::strlen(config)), 0}}));
    require(split.version == 5u && split.data.recordCount == 1u && split.splitAo.present &&
                split.splitAo.doorExponent == 2.5f && split.splitAo.wings.size() == 1u &&
                split.splitAo.doorNodes.size() == 1u, "split AO metadata");
}

void supportsRawDeflateAndRejectsCrc() {
    const std::vector<std::uint8_t> plain{'h', 'e', 'l', 'l', 'o'};
    const std::vector<std::uint8_t> compressed{0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    const auto patch = record("mesh", 1u, {0, 0, 0}, 1u, {64});
    const auto archive = zip({{"Patch_v5.data", patch, patch, 0}, {"Config.ini", compressed, plain, 8}});
    const auto parsed = parseVaoPatch(archive);
    require(parsed.configText == "hello", "raw deflate extraction");
    auto corrupt = archive;
    const auto patchCentralOffset = std::size_t{30u} + patch.size() + std::size_t{30u} + std::string("Config.ini").size();
    corrupt[patchCentralOffset + 16u] ^= 1u;
    expectsParseError([&] { (void)parseVaoPatch(corrupt); }, "CRC_MISMATCH");
}

void validatesZipStructureAndDeflateTrees() {
    const auto patch = record("mesh", 1u, {0, 0, 0}, 1u, {64});
    const std::vector<std::uint8_t> compressed{0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    const auto central = std::size_t{30u} + std::string("Patch_v5.data").size() + patch.size();
    const auto archive = zip({{"Patch_v5.data", patch, patch, 0}});

    auto localMethod = archive;
    put16(localMethod, 8u, 8u);
    expectsParseError([&] { (void)parseVaoPatch(localMethod); }, "INVALID_ZIP");
    auto localCrc = archive;
    put32(localCrc, 14u, crc32(patch) ^ 1u);
    expectsParseError([&] { (void)parseVaoPatch(localCrc); }, "INVALID_ZIP");

    auto descriptor = archive;
    descriptor[central + 8u] = 8u;
    expectsParseError([&] { (void)parseVaoPatch(descriptor); }, "UNSUPPORTED_ZIP");

    auto badCounts = archive;
    put16(badCounts, badCounts.size() - 22u + 8u, 0u);
    expectsParseError([&] { (void)parseVaoPatch(badCounts); }, "INVALID_ZIP");
    auto badDirectory = archive;
    put32(badDirectory, badDirectory.size() - 22u + 12u,
          static_cast<std::uint32_t>(central + 46u + std::string("Patch_v5.data").size() - central + 1u));
    expectsParseError([&] { (void)parseVaoPatch(badDirectory); }, "INVALID_ZIP");

    const auto two = zip({{"Patch_v5.data", patch, patch, 0}, {"Config.ini", patch, patch, 0}});
    const auto twoCentral = std::size_t{30u} + std::string("Patch_v5.data").size() + patch.size() +
                            std::size_t{30u} + std::string("Config.ini").size() + patch.size();
    const auto secondCentral = twoCentral + 46u + std::string("Patch_v5.data").size();
    auto overlap = two;
    put32(overlap, secondCentral + 42u, 0u);
    expectsParseError([&] { (void)parseVaoPatch(overlap); }, "INVALID_ZIP");
    auto centralLocal = archive;
    put32(centralLocal, central + 42u, static_cast<std::uint32_t>(central));
    expectsParseError([&] { (void)parseVaoPatch(centralLocal); }, "INVALID_ZIP");

    auto unsafeStream = zip({{"foo:bar", patch, patch, 0}});
    expectsParseError([&] { (void)parseVaoPatch(unsafeStream); }, "UNSAFE_NAME");
    auto unsafeDevice = zip({{"NUL.txt", patch, patch, 0}});
    expectsParseError([&] { (void)parseVaoPatch(unsafeDevice); }, "UNSAFE_NAME");

    const std::vector<std::uint8_t> oversubscribed{0x05u, 0x00u, 0x92u, 0x04u};
    const auto malformedTree = zip({{"Patch_v5.data", oversubscribed, {}, 8u}});
    expectsParseError([&] { (void)parseVaoPatch(malformedTree); }, "INVALID_DEFLATE");
    for (std::size_t length = 0; length <= oversubscribed.size(); ++length) {
        const std::vector<std::uint8_t> prefix(oversubscribed.begin(),
                                               oversubscribed.begin() + static_cast<std::ptrdiff_t>(length));
        const auto truncatedTree = zip({{"Patch_v5.data", prefix, {}, 8u}});
        expectsParseError([&] { (void)parseVaoPatch(truncatedTree); }, "INVALID_DEFLATE");
    }

    auto trailing = compressed;
    trailing.push_back(0u);
    const auto trailingArchive = zip({{"Patch_v5.data", patch, patch, 0}, {"Config.ini", trailing, {'h', 'e', 'l', 'l', 'o'}, 8}});
    expectsParseError([&] { (void)parseVaoPatch(trailingArchive); }, "INVALID_DEFLATE");
}

void validatesDecodedOutputBudget() {
    const auto normal = record("normal", 2u, {0, 0, 0}, 1u, {255, 0, 0});
    auto singleLimit = apex::core::ParseLimits{};
    singleLimit.maxOutputBytes = 3u;
    expectsParseError([&] { (void)parseVaoData(normal, 5u, {}, "limited", singleLimit); }, "OUTPUT_TOO_LARGE");
    auto twoNormals = normal;
    twoNormals.insert(twoNormals.end(), normal.begin(), normal.end());
    auto aggregateLimit = apex::core::ParseLimits{};
    aggregateLimit.maxOutputBytes = 12u;
    expectsParseError([&] { (void)parseVaoData(twoNormals, 5u, {}, "limited", aggregateLimit); }, "OUTPUT_TOO_LARGE");

}

void rejectsMalformedAndEveryTruncatedPrefix() {
    const auto valid = record("mesh", 1u, {0, 0, 0}, 2u, {0, 255});
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::vector<std::uint8_t> prefix(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        expectsParseError([&] { (void)parseVaoData(prefix, 5u); }, "TRUNCATED");
    }
    auto badName = valid; put32(badName, 0, 0xffffffffu);
    expectsParseError([&] { (void)parseVaoData(badName, 5u); }, "INVALID_RECORD");
    auto badType = valid; put32(badType, 8u, 99u);
    expectsParseError([&] { (void)parseVaoData(badType, 5u); }, "UNSUPPORTED_TYPE");
    expectsParseError([&] { (void)parseVaoData(valid, 2u); }, "UNSUPPORTED_VERSION");
    const auto zeroNormal = record("mesh", 2u, {0, 0, 0}, 1u, {0, 0, 0});
    expectsParseError([&] { (void)parseVaoData(zeroNormal, 5u); }, "INVALID_NORMAL");
    const auto archive = zip({{"Patch_v5.data", valid, valid, 0}});
    for (std::size_t length = 0; length < archive.size(); ++length) {
        const std::vector<std::uint8_t> prefix(archive.begin(), archive.begin() + static_cast<std::ptrdiff_t>(length));
        expectsParseError([&] { (void)parseVaoPatch(prefix); }, length < 22u ? "TRUNCATED" : "INVALID_ZIP");
    }
    const auto unsafe = zip({{"../Patch_v5.data", valid, valid, 0}});
    expectsParseError([&] { (void)parseVaoPatch(unsafe); }, "UNSAFE_NAME");
}

} // namespace

int main() {
    try {
        decodesAoVersions();
        decodesNormalsAndMetadata();
        supportsRawDeflateAndRejectsCrc();
        validatesZipStructureAndDeflateTrees();
        validatesDecodedOutputBudget();
        enforcesLimitsAndDeflateFailures();
        rejectsMalformedAndEveryTruncatedPrefix();
        std::cout << "vao tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vao tests failed: " << error.what() << '\n';
        return 1;
    }
}
