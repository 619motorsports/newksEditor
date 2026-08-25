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

template <typename Function>
void expectsError(Function&& function, std::string_view code = {}) {
    try {
        function();
    } catch (const ParseError& error) {
        if (!code.empty()) require(error.code() == code, "unexpected AI spline parse error code");
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
    unsupported[0] = 2;
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

}  // namespace

int main() {
    try {
        parsesPointsPayloadsAndGrid();
        rejectsEveryTruncatedPrefix();
        rejectsMalformedHeadersAndCounts();
        rejectsNonFiniteAndLimits();
        parsesInstalledFastLaneWhenAvailable();
        return 0;
    } catch (const std::exception& error) {
        return (std::fprintf(stderr, "AI spline tests failed: %s\n", error.what()), 1);
    }
}
