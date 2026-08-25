#include "apex/formats/ai_spline.hpp"

#include <bit>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
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

void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
}

void f32(std::vector<std::uint8_t>& bytes, float value) {
    u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void putU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
            std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "test write must stay within fixture");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> validSpline() {
    std::vector<std::uint8_t> bytes;
    u32(bytes, 7);       // version
    u32(bytes, 1);       // points
    u32(bytes, 1234);    // lap time
    u32(bytes, 0);       // reserved
    f32(bytes, 1); f32(bytes, 2); f32(bytes, 3); f32(bytes, 4);
    u32(bytes, static_cast<std::uint32_t>(-5));
    u32(bytes, 1);       // payload count
    for (float value : {10.0f, 0.9f, 0.1f, -0.2f, 4.0f, 1.0f, 2.0f, 3.0f,
                        0.5f, 0.0f, 1.0f, 0.0f, 4.0f, 0.0f, 0.0f, 1.0f})
        f32(bytes, value);
    u32(bytes, 0);       // ignored payload word
    f32(bytes, 0.25f);   // grade
    u32(bytes, 1);       // grid present
    for (float value : {10.0f, 11.0f, 12.0f, -1.0f, -2.0f, -3.0f}) f32(bytes, value);
    u32(bytes, 2);       // neighbor count
    f32(bytes, 0.5f);    // sampling density
    u32(bytes, 1);       // rows
    u32(bytes, 1);       // cells in row
    u32(bytes, 1);       // indices in cell
    u32(bytes, 0);       // point index
    return bytes;
}

std::vector<std::uint8_t> validLegacyV2Spline() {
    std::vector<std::uint8_t> bytes;
    u32(bytes, 2);       // version
    u32(bytes, 4);       // source records
    u32(bytes, 98765);   // lap time
    for (std::uint32_t index = 0; index < 4U; ++index) {
        const std::array<std::array<float, 3>, 4> positions = {
            std::array<float, 3>{0.0F, 0.0F, 0.0F},
            std::array<float, 3>{10.0F, 0.0F, 0.0F},
            std::array<float, 3>{10.0F, 10.0F, 0.0F},
            std::array<float, 3>{10.0F, 10.0F, 10.0F},
        };
        for (const auto value : positions[index]) f32(bytes, value);
        u32(bytes, 0x11000000U + index); // opaque legacy word
        f32(bytes, 40.0F + static_cast<float>(index)); // speed
        f32(bytes, 0.5F);                                // gas
        f32(bytes, -0.25F);                               // lateral G
    }
    return bytes;
}

template <typename Function>
void expectsError(Function&& function, std::string_view code = {}) {
    try {
        function();
    } catch (const ParseError& error) {
        if (!code.empty() && error.code() != code)
            throw std::runtime_error("unexpected AI spline parse error code: " +
                                     error.code() + " (expected " + std::string(code) + ")");
        return;
    }
    throw std::runtime_error("expected AI spline parse error");
}

void parsesPointsPayloadsAndGrid() {
    const auto bytes = validSpline();
    const auto parsed = parseAiSpline(bytes, "fixture.ai");
    require(parsed.version == 7 && parsed.lapTime == 1234, "header");
    require(parsed.points.size() == 1 && parsed.payloads.size() == 1, "point and payload counts");
    require(parsed.points[0].position == std::array<float, 3>{1, 2, 3} &&
                parsed.points[0].length == 4 && parsed.points[0].tag == -5,
            "point values");
    require(parsed.payloads[0].speed == 10 && parsed.payloads[0].normal[1] == 1 &&
                parsed.payloads[0].grade == .25f,
            "payload values");
    require(parsed.grid.has_value() && parsed.grid->rows.size() == 1 &&
                parsed.grid->rows[0].cells[0].pointIndices[0] == 0,
            "grid values");
    require(parsed.bytesRead == bytes.size() && parsed.byteLength == bytes.size(), "byte accounting");
}

void parsesLegacyVersion2WithoutInventingV7Fields() {
    const auto bytes = validLegacyV2Spline();
    const auto parsed = parseAiSpline(bytes, "legacy-v2.ai");
    require(parsed.version == 2U && parsed.lapTime == 98765U,
            "legacy version-2 header");
    require(parsed.points.empty() && parsed.payloads.empty() && !parsed.grid.has_value(),
            "legacy version-2 does not invent version-7 fields");
    require(parsed.legacyV2Records.size() == 4U,
            "legacy version-2 raw record count");
    require(parsed.legacyV2Records[2].position == std::array<float, 3>{10.0F, 10.0F, 0.0F} &&
                parsed.legacyV2Records[2].legacyWord == 0x11000002U &&
                parsed.legacyV2Records[2].speed == 42.0F &&
                parsed.legacyV2Records[2].gas == 0.5F &&
                parsed.legacyV2Records[2].lateralG == -0.25F,
            "legacy version-2 raw fields");
    require(parsed.nativeRetainedIndices == std::vector<std::uint32_t>{0U, 3U},
            "native version-2 retained indices");
    require(parsed.nativeRetainedForwards.size() == 2U,
            "native version-2 retained forward count");
    const auto inverseRootThree = -1.0F / std::sqrt(3.0F);
    require(std::abs(parsed.nativeRetainedForwards[0][0] - inverseRootThree) < 1.0e-6F &&
                std::abs(parsed.nativeRetainedForwards[0][1] - inverseRootThree) < 1.0e-6F &&
                std::abs(parsed.nativeRetainedForwards[0][2] - inverseRootThree) < 1.0e-6F,
            "native version-2 first forward uses retained wraparound");
    require(parsed.nativeRetainedForwards[1] == std::array<float, 3>{0.0F, 0.0F, 1.0F},
            "native version-2 retained forward uses previous raw point");
    require(parsed.bytesRead == bytes.size() && parsed.byteLength == bytes.size(),
            "legacy version-2 byte accounting");
}

void handlesLegacyVersion2EmptyAndSinglePointForwardEdges() {
    std::vector<std::uint8_t> empty;
    u32(empty, 2U);
    u32(empty, 0U);
    u32(empty, 11U);
    const auto parsedEmpty = parseAiSpline(empty, "legacy-empty.ai");
    require(parsedEmpty.legacyV2Records.empty() &&
                parsedEmpty.nativeRetainedIndices.empty() &&
                parsedEmpty.nativeRetainedForwards.empty(),
            "empty legacy version-2 spline has no retained points");

    std::vector<std::uint8_t> single;
    u32(single, 2U);
    u32(single, 1U);
    u32(single, 12U);
    f32(single, 0.0F); f32(single, 0.0F); f32(single, 0.0F);
    u32(single, 0U);
    f32(single, 1.0F); f32(single, 0.5F); f32(single, 0.25F);
    const auto parsedSingle = parseAiSpline(single, "legacy-single.ai");
    require(parsedSingle.nativeRetainedIndices == std::vector<std::uint32_t>{0U} &&
                parsedSingle.nativeRetainedForwards.size() == 1U &&
                parsedSingle.nativeRetainedForwards[0] == std::array<float, 3>{0.0F, 0.0F, 0.0F},
            "single origin legacy point keeps native zero forward");

    putU32(single, 16U, std::bit_cast<std::uint32_t>(3.0F));
    putU32(single, 20U, std::bit_cast<std::uint32_t>(4.0F));
    const auto parsedNonzero = parseAiSpline(single, "legacy-single-nonzero.ai");
    require(parsedNonzero.nativeRetainedForwards.size() == 1U &&
                parsedNonzero.nativeRetainedForwards[0] == std::array<float, 3>{0.0F, 0.6F, 0.8F},
            "single nonzero legacy point keeps normalized initial displacement");
}

void rejectsEveryTruncatedPrefix() {
    const auto bytes = validSpline();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        expectsError([&] {
            (void)parseAiSpline(std::span<const std::uint8_t>(bytes.data(), length), "prefix.ai");
        });
    }
}

void rejectsMalformedHeadersAndCounts() {
    auto unsupported = validSpline();
    unsupported[0] = 3;
    expectsError([&] { (void)parseAiSpline(unsupported); }, "UNSUPPORTED_VERSION");

    auto ignoredWords = validSpline();
    ignoredWords[12] = 1;
    ignoredWords[16 + 20 + 4 + 64] = 2;
    const auto ignoredResult = parseAiSpline(ignoredWords);
    require(ignoredResult.reserved == 1U &&
                ignoredResult.payloads[0].reserved == 2U,
            "native-ignored words are retained without invented validation");

    auto mismatch = validSpline();
    mismatch[16 + 20] = 2;
    expectsError([&] { (void)parseAiSpline(mismatch); }, "COUNT_MISMATCH");

    auto trailing = validSpline();
    trailing.push_back(0);
    expectsError([&] { (void)parseAiSpline(trailing); }, "TRAILING_DATA");

    auto invalidGrid = validSpline();
    // The grid flag follows the 16-byte header, point, payload-count, and 72-byte payload.
    invalidGrid[16 + 20 + 4 + 72] = 2;
    expectsError([&] { (void)parseAiSpline(invalidGrid); }, "INVALID_GRID_FLAG");

    auto invalidIndex = validSpline();
    invalidIndex.back() = 1;
    expectsError([&] { (void)parseAiSpline(invalidIndex); }, "INDEX_OUT_OF_RANGE");
}

void rejectsMalformedLegacyVersion2Input() {
    const auto bytes = validLegacyV2Spline();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        expectsError([&] {
            (void)parseAiSpline(
                std::span<const std::uint8_t>(bytes.data(), length), "legacy-prefix.ai");
        }, "TRUNCATED");
    }

    auto claimedHuge = bytes;
    putU32(claimedHuge, 4U, std::numeric_limits<std::uint32_t>::max());
    auto hugeLimits = AiSplineParseLimits{};
    hugeLimits.maxPoints = std::numeric_limits<std::uint32_t>::max();
    expectsError([&] { (void)parseAiSpline(claimedHuge, "legacy-huge.ai", hugeLimits); },
                 "TRUNCATED");

    auto trailing = bytes;
    trailing.push_back(0U);
    expectsError([&] { (void)parseAiSpline(trailing); }, "TRAILING_DATA");

    auto nonFinite = bytes;
    const auto nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    putU32(nonFinite, 12U, nan);
    expectsError([&] { (void)parseAiSpline(nonFinite); }, "NON_FINITE");

    auto pointLimited = AiSplineParseLimits{};
    pointLimited.maxPoints = 3U;
    expectsError([&] { (void)parseAiSpline(bytes, "legacy-limited.ai", pointLimited); },
                 "COUNT_LIMIT");

    auto retainedLimited = AiSplineParseLimits{};
    retainedLimited.maxRetainedPoints = 1U;
    expectsError([&] { (void)parseAiSpline(bytes, "legacy-retained-limited.ai", retainedLimited); },
                 "COUNT_LIMIT");

    auto aggregateLimited = AiSplineParseLimits{};
    aggregateLimited.maxAggregateBytes = 1U;
    expectsError([&] { (void)parseAiSpline(bytes, "legacy-aggregate-limited.ai", aggregateLimited); },
                 "AGGREGATE_LIMIT");
}

void rejectsNonFiniteAndLimits() {
    auto nonFinite = validSpline();
    const auto nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    nonFinite[16] = static_cast<std::uint8_t>(nan);
    nonFinite[17] = static_cast<std::uint8_t>(nan >> 8u);
    nonFinite[18] = static_cast<std::uint8_t>(nan >> 16u);
    nonFinite[19] = static_cast<std::uint8_t>(nan >> 24u);
    expectsError([&] { (void)parseAiSpline(nonFinite); }, "NON_FINITE");

    auto pointLimited = AiSplineParseLimits{};
    pointLimited.maxPoints = 0;
    expectsError([&] { (void)parseAiSpline(validSpline(), "limited.ai", pointLimited); }, "COUNT_LIMIT");

    auto aggregateLimited = AiSplineParseLimits{};
    aggregateLimited.maxAggregateBytes = 1;
    expectsError([&] { (void)parseAiSpline(validSpline(), "limited.ai", aggregateLimited); }, "AGGREGATE_LIMIT");

    auto rowLimited = AiSplineParseLimits{};
    rowLimited.maxGridRows = 0;
    expectsError([&] { (void)parseAiSpline(validSpline(), "limited.ai", rowLimited); },
                 "COUNT_LIMIT");

    auto cellLimited = AiSplineParseLimits{};
    cellLimited.maxGridCellsPerRow = 0;
    expectsError([&] { (void)parseAiSpline(validSpline(), "limited.ai", cellLimited); },
                 "COUNT_LIMIT");

    auto indexLimited = AiSplineParseLimits{};
    indexLimited.maxGridIndices = 0;
    expectsError([&] { (void)parseAiSpline(validSpline(), "limited.ai", indexLimited); },
                 "COUNT_LIMIT");
}

void parsesInstalledFastLaneWhenAvailable() {
    constexpr const char* path =
        "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/tracks/imola/ai/fast_lane.ai";
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
    const auto parsed = parseAiSpline(bytes, path);
    require(parsed.points.size() == 3166 && parsed.payloads.size() == 3166,
            "installed fast lane counts");
    require(parsed.grid.has_value(), "installed fast lane grid");
}

void parsesInstalledLegacyIdealLineWhenAvailable() {
    constexpr const char* path =
        "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/tracks/imola/data/ideal_line.ai";
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
    const auto parsed = parseAiSpline(bytes, path);
    require(parsed.version == 2U && parsed.lapTime == 114207U,
            "installed legacy version-2 header");
    require(parsed.legacyV2Records.size() == 8533U &&
                parsed.nativeRetainedIndices.size() == 2845U &&
                bytes.size() == 12U + 8533U * 28U,
            "installed legacy version-2 shape");
    require(parsed.legacyV2Records.front().legacyWord == 0x01332424U,
            "installed legacy version-2 opaque word");
}

}  // namespace

int main() {
    try {
        parsesPointsPayloadsAndGrid();
        parsesLegacyVersion2WithoutInventingV7Fields();
        handlesLegacyVersion2EmptyAndSinglePointForwardEdges();
        rejectsEveryTruncatedPrefix();
        rejectsMalformedHeadersAndCounts();
        rejectsMalformedLegacyVersion2Input();
        rejectsNonFiniteAndLimits();
        parsesInstalledFastLaneWhenAvailable();
        parsesInstalledLegacyIdealLineWhenAvailable();
        return 0;
    } catch (const std::exception& error) {
        return (std::fprintf(stderr, "AI spline tests failed: %s\n", error.what()), 1);
    }
}
