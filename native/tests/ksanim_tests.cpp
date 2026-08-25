#include "apex/core/parse_error.hpp"
#include "apex/formats/ksanim.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
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
    const auto bits = std::bit_cast<std::uint32_t>(value);
    u32(bytes, bits);
}

KsAnimationFrame frame(Vector3 position, Quaternion quaternion = {0, 0, 0, 1},
                       Vector3 scale = {1, 1, 1}) {
    return {quaternion, position, scale};
}

std::vector<std::uint8_t> version2(const std::vector<KsAnimationTrack>& tracks) {
    std::vector<std::uint8_t> bytes;
    u32(bytes, 2);
    u32(bytes, static_cast<std::uint32_t>(tracks.size()));
    for (const auto& track : tracks) {
        u32(bytes, static_cast<std::uint32_t>(track.name.size()));
        bytes.insert(bytes.end(), track.name.begin(), track.name.end());
        u32(bytes, static_cast<std::uint32_t>(track.frames.size()));
        for (const auto& value : track.frames) {
            for (const auto component : value.quaternion) f32(bytes, component);
            for (const auto component : value.position) f32(bytes, component);
            for (const auto component : value.scale) f32(bytes, component);
        }
    }
    return bytes;
}

template <typename Function>
void expectsParseError(Function&& function, std::string_view code = {}) {
    bool caught = false;
    try {
        function();
    } catch (const ParseError& error) {
        caught = true;
        require(error.offset() <= 0x1000000, "parse error offset is unreasonable");
        if (!code.empty()) require(error.code() == code, "unexpected parse error code");
    }
    require(caught, "expected ParseError");
}

void parsesVersion2AndSerializesNativeLayout() {
    KsAnimation source;
    source.source = "source.ksanim";
    source.version = 2;
    KsAnimationTrack door;
    door.name = "DOOR_L";
    door.frames = {frame({0, 0, 0}), frame({1, 2, 3}, {0, 0, 1, 0}, {2, 3, 4})};
    door.animated = true;
    source.tracks.push_back(door);
    const auto bytes = serializeKsAnimation(source);
    const auto parsed = parseKsAnimation(bytes, "source.ksanim");
    require(parsed.version == 2, "version 2");
    require(parsed.tracks.size() == 1, "track count");
    require(parsed.tracks[0].animated, "animated track");
    require(parsed.tracks[0].frames[1].position == Vector3{1, 2, 3}, "frame position");
    require(parsed.tracks[0].frames[1].quaternion == Quaternion{0, 0, 1, 0}, "frame quaternion");
    require(parsed.bytesRead == bytes.size(), "bytes read");
    require(serializeKsAnimation(parsed) == bytes, "byte-exact serialization");
}

void preservesByteExactV2AnimationAndSamples() {
    auto bytes = version2({{"SIGNED_ZERO", {frame({0, 0, 0}), frame({-0.0f, 0, 0})}}});
    const auto parsed = parseKsAnimation(bytes);
    require(parsed.tracks[0].animated, "signed-zero frame must be animated");
    const auto matrix = sampleKsAnimationTrack(parsed.tracks[0], .25f);
    require(matrix.has_value(), "sample matrix");
    require(std::isfinite((*matrix)[12]), "finite sampled position");

    const auto track = KsAnimationTrack{"MOVE", {frame({0, 0, 0}), frame({10, 0, 0}), frame({20, 0, 0})}, true};
    require(std::abs((*sampleKsAnimationTrack(track, 1.0f / 6.0f))[12] - 5) < 1e-5f, "first interpolated sample");
    require(std::abs((*sampleKsAnimationTrack(track, .5f))[12] - 15) < 1e-5f, "middle interpolated sample");
    require(std::abs((*sampleKsAnimationTrack(track, 1))[12] - 20) < 1e-5f, "endpoint sample");
}

void derivesNativeAnimatedFlagFromAllV2FrameWords() {
    const auto bytes = version2({
        {"STATIC", {frame({1, 2, 3}, {0, 0, 0, 1}, {2, 3, 4}),
                     frame({1, 2, 3}, {0, 0, 0, 1}, {2, 3, 4})}},
        {"SCALE", {frame({1, 2, 3}, {0, 0, 0, 1}, {2, 3, 4}),
                    frame({1, 2, 3}, {0, 0, 0, 1}, {2, 3, 5})}},
    });
    const auto parsed = parseKsAnimation(bytes, "animated-flag.ksanim");
    require(!parsed.tracks[0].animated, "native flag remains clear for identical 40-byte frames");
    require(parsed.tracks[1].animated, "native flag sees a scale-only 40-byte difference");

    const auto sampled = sampleKsAnimation(parsed, 0.5F);
    require(sampled.size() == 1 && sampled.front().first == "SCALE",
            "native playback binding skips tracks whose flag remains clear");
}

void preservesFirstTrackFrameCountWhenItIsZero() {
    const auto bytes = version2({{"EMPTY", {}}, {"NODE", {frame({0, 0, 0})}}});
    const auto parsed = parseKsAnimation(bytes, "zero-first.ksanim");
    require(parsed.frameCount == 0, "first track frame count");
    require(parsed.warnings.size() == 1, "mismatched frame warning count");
    require(parsed.warnings[0] == "zero-first.ksanim: NODE has 1 frames; the first track has 0", "mismatched frame warning");
}

void decomposesVersion1Matrix() {
    const Matrix4 identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1};
    std::vector<std::uint8_t> bytes;
    u32(bytes, 1); u32(bytes, 1); u32(bytes, 4); bytes.insert(bytes.end(), {'r', 'i', 'g', '0'}); u32(bytes, 1);
    for (const auto value : identity) f32(bytes, value);
    const auto parsed = parseKsAnimation(bytes);
    require(parsed.version == 1, "version 1");
    require(parsed.tracks[0].frames[0].position == Vector3{4, 5, 6}, "v1 position");
    require(parsed.tracks[0].frames[0].scale == Vector3{1, 1, 1}, "v1 scale");
    require(std::abs(parsed.tracks[0].frames[0].quaternion[3] - 1) < 1e-6f, "v1 quaternion");
}

void rejectsMalformedValues() {
    expectsParseError([] { parseKsAnimation({}); }, "TRUNCATED");
    const std::vector<std::uint8_t> unsupported = {3, 0, 0, 0, 0, 0, 0, 0};
    expectsParseError([&] { parseKsAnimation(unsupported); }, "UNSUPPORTED_VERSION");
    const std::vector<std::uint8_t> hugeTracks = {2, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
    expectsParseError([&] { parseKsAnimation(hugeTracks); }, "COUNT_LIMIT");

    std::vector<std::uint8_t> invalidUtf8;
    u32(invalidUtf8, 2); u32(invalidUtf8, 1); u32(invalidUtf8, 1); invalidUtf8.push_back(0xff);
    expectsParseError([&] { parseKsAnimation(invalidUtf8); }, "INVALID_UTF8");

    auto truncatedFrame = version2({{"NODE", {frame({0, 0, 0})}}});
    truncatedFrame.pop_back();
    expectsParseError([&] { parseKsAnimation(truncatedFrame); }, "COUNT_INVALID");
    auto trailing = version2({{"NODE", {frame({0, 0, 0})}}});
    trailing.push_back(0);
    expectsParseError([&] { parseKsAnimation(trailing); }, "TRAILING_BYTES");

    auto nonFinite = version2({{"NODE", {frame({0, 0, 0})}}});
    const auto nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    std::memcpy(nonFinite.data() + 20, &nan, sizeof(nan));
    expectsParseError([&] { parseKsAnimation(nonFinite); }, "NON_FINITE");
}

void rejectsEveryTruncatedPrefix() {
    const auto valid = version2({{"NODE", {frame({0, 0, 0}), frame({1, 2, 3})}}});
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::vector<std::uint8_t> prefix(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        expectsParseError([&] { parseKsAnimation(prefix, "prefix.ksanim"); });
    }
    require(parseKsAnimation(valid).bytesRead == valid.size(), "complete fixture bytes read");
}

void validatesSerializerInput() {
    KsAnimation noName;
    noName.source = "";
    noName.version = 2;
    noName.tracks.push_back(KsAnimationTrack{});
    expectsParseError([&] { serializeKsAnimation(noName); }, "INVALID_NAME");
    KsAnimation invalidUtf8Name;
    invalidUtf8Name.source = "";
    invalidUtf8Name.version = 2;
    KsAnimationTrack invalidNameTrack;
    invalidNameTrack.name = std::string("bad\xff", 4);
    invalidUtf8Name.tracks.push_back(invalidNameTrack);
    expectsParseError([&] { serializeKsAnimation(invalidUtf8Name); }, "INVALID_UTF8");
    KsAnimation nonFinite;
    nonFinite.source = "";
    nonFinite.version = 2;
    KsAnimationTrack nonFiniteTrack;
    nonFiniteTrack.name = "NODE";
    nonFiniteTrack.frames.push_back(frame({std::numeric_limits<float>::infinity(), 0, 0}));
    nonFinite.tracks.push_back(nonFiniteTrack);
    expectsParseError([&] { serializeKsAnimation(nonFinite); }, "NON_FINITE");
}

} // namespace

int main() {
    try {
        parsesVersion2AndSerializesNativeLayout();
        preservesByteExactV2AnimationAndSamples();
        derivesNativeAnimatedFlagFromAllV2FrameWords();
        preservesFirstTrackFrameCountWhenItIsZero();
        decomposesVersion1Matrix();
        rejectsMalformedValues();
        rejectsEveryTruncatedPrefix();
        validatesSerializerInput();
        std::cout << "ksanim tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ksanim tests failed: " << error.what() << '\n';
        return 1;
    }
}
