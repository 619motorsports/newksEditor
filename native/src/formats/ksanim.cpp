#include "apex/formats/ksanim.hpp"

#include "apex/core/byte_reader.hpp"
#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace apex::formats {

namespace {

using apex::core::ByteReader;
using apex::core::ParseError;

constexpr std::size_t V1_FRAME_SIZE = 64;
constexpr std::size_t V2_FRAME_SIZE = 40;
// Every serialized track has at least a name-length and frame-count word.
// Check this before reserving the track vector so a count cannot force a
// large allocation when the input is truncated immediately after the header.
constexpr std::size_t MINIMUM_TRACK_BYTES = 8;
constexpr float EPSILON = std::numeric_limits<float>::epsilon();

[[nodiscard]] ParseError invalid(std::string_view source, std::size_t offset,
                                 std::string_view code, std::string_view message) {
    return ParseError("KSANIM", std::string(source), offset, std::string(code), std::string(message));
}

[[nodiscard]] bool sameFloat(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

[[nodiscard]] bool frameChanged(const KsAnimationFrame& first, const KsAnimationFrame& candidate) {
    for (std::size_t i = 0; i < first.quaternion.size(); ++i)
        if (!sameFloat(first.quaternion[i], candidate.quaternion[i])) return true;
    for (std::size_t i = 0; i < first.position.size(); ++i)
        if (!sameFloat(first.position[i], candidate.position[i])) return true;
    for (std::size_t i = 0; i < first.scale.size(); ++i)
        if (!sameFloat(first.scale[i], candidate.scale[i])) return true;
    return false;
}

[[nodiscard]] bool finiteFrame(const KsAnimationFrame& frame) {
    for (const auto value : frame.quaternion) if (!std::isfinite(value)) return false;
    for (const auto value : frame.position) if (!std::isfinite(value)) return false;
    for (const auto value : frame.scale) if (!std::isfinite(value)) return false;
    return true;
}

[[nodiscard]] Quaternion normalizedQuaternion(const Quaternion& input) {
    const float length = std::sqrt(input[0] * input[0] + input[1] * input[1] +
                                   input[2] * input[2] + input[3] * input[3]);
    if (!std::isfinite(length) || length <= EPSILON) return {0, 0, 0, 1};
    return {input[0] / length, input[1] / length, input[2] / length, input[3] / length};
}

[[nodiscard]] Quaternion matrixQuaternion(const Matrix4& matrix, const Vector3& scale) {
    Matrix4 normalized = matrix;
    for (std::size_t row = 0; row < 3; ++row) {
        const float divisor = std::abs(scale[row]) > EPSILON ? scale[row] : 1.0f;
        for (std::size_t column = 0; column < 3; ++column)
            normalized[row * 4 + column] /= divisor;
    }
    const float trace = normalized[0] + normalized[5] + normalized[10];
    float x = 0, y = 0, z = 0, w = 1;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        w = s / 4; x = (normalized[6] - normalized[9]) / s;
        y = (normalized[8] - normalized[2]) / s; z = (normalized[1] - normalized[4]) / s;
    } else if (normalized[0] > normalized[5] && normalized[0] > normalized[10]) {
        const float s = std::sqrt(1 + normalized[0] - normalized[5] - normalized[10]) * 2;
        w = (normalized[6] - normalized[9]) / s; x = s / 4;
        y = (normalized[4] + normalized[1]) / s; z = (normalized[8] + normalized[2]) / s;
    } else if (normalized[5] > normalized[10]) {
        const float s = std::sqrt(1 + normalized[5] - normalized[0] - normalized[10]) * 2;
        w = (normalized[8] - normalized[2]) / s; x = (normalized[4] + normalized[1]) / s;
        y = s / 4; z = (normalized[9] + normalized[6]) / s;
    } else {
        const float s = std::sqrt(1 + normalized[10] - normalized[0] - normalized[5]) * 2;
        w = (normalized[1] - normalized[4]) / s; x = (normalized[8] + normalized[2]) / s;
        y = (normalized[9] + normalized[6]) / s; z = s / 4;
    }
    return normalizedQuaternion({x, y, z, w});
}

[[nodiscard]] Quaternion slerpQuaternion(const Quaternion& from, const Quaternion& to, float amount) {
    const Quaternion source = normalizedQuaternion(from);
    Quaternion target = normalizedQuaternion(to);
    float dot = source[0] * target[0] + source[1] * target[1] +
                source[2] * target[2] + source[3] * target[3];
    if (dot < 0) {
        for (auto& value : target) value = -value;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        Quaternion result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = source[i] + (target[i] - source[i]) * amount;
        return normalizedQuaternion(result);
    }
    const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sine = std::sin(angle);
    const float a = std::sin((1 - amount) * angle) / sine;
    const float b = std::sin(amount * angle) / sine;
    Quaternion result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = source[i] * a + target[i] * b;
    return result;
}

class ByteWriter final {
public:
    explicit ByteWriter(std::size_t size) : bytes_(size) {}
    void u32(std::uint32_t value) {
        bytes_[offset_++] = static_cast<std::uint8_t>(value);
        bytes_[offset_++] = static_cast<std::uint8_t>(value >> 8u);
        bytes_[offset_++] = static_cast<std::uint8_t>(value >> 16u);
        bytes_[offset_++] = static_cast<std::uint8_t>(value >> 24u);
    }
    void f32(float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        u32(bits);
    }
    void raw(std::string_view value) {
        std::memcpy(bytes_.data() + offset_, value.data(), value.size());
        offset_ += value.size();
    }
    [[nodiscard]] std::vector<std::uint8_t>&& finish() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

} // namespace

KsAnimationFrame decomposeKsAnimationMatrix(const Matrix4& matrix) {
    const Vector3 scale = {
        std::hypot(matrix[0], matrix[1], matrix[2]),
        std::hypot(matrix[4], matrix[5], matrix[6]),
        std::hypot(matrix[8], matrix[9], matrix[10])};
    return {matrixQuaternion(matrix, scale), {matrix[12], matrix[13], matrix[14]}, scale};
}

Matrix4 ksAnimationMatrix(const KsAnimationFrame& frame) {
    const auto [x, y, z, w] = normalizedQuaternion(frame.quaternion);
    const auto [sx, sy, sz] = frame.scale;
    const auto [px, py, pz] = frame.position;
    return {
        (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx,
        (2 * (x * z - y * w)) * sx, 0,
        (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy,
        (2 * (y * z + x * w)) * sy, 0,
        (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz,
        (1 - 2 * (x * x + y * y)) * sz, 0,
        px, py, pz, 1};
}

KsAnimation parseKsAnimation(std::span<const std::uint8_t> bytes, std::string source,
                             apex::core::ParseLimits limits) {
    ByteReader reader(bytes, source, limits, "KSANIM");
    const auto version = reader.u32("version");
    if (version != 1 && version != 2)
        throw reader.error(0, "UNSUPPORTED_VERSION", "unsupported KSANIM version " + std::to_string(version));
    const auto trackCountOffset = reader.offset();
    const auto trackCount = reader.u32("track count");
    if (trackCount > limits.maxTracks)
        throw reader.error(trackCountOffset, "COUNT_LIMIT", "track count exceeds configured limit");
    if (static_cast<std::size_t>(trackCount) > reader.remaining() / MINIMUM_TRACK_BYTES)
        throw reader.error(trackCountOffset, "COUNT_INVALID",
                           "track count exceeds remaining KSANIM bytes");

    KsAnimation animation;
    animation.source = std::move(source);
    animation.version = version;
    animation.byteLength = bytes.size();
    animation.tracks.reserve(trackCount);
    std::size_t totalFrames = 0;
    std::optional<std::size_t> firstFrameCount;
    for (std::uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        const auto name = reader.string("track name");
        const auto countOffset = reader.offset();
        const auto count = reader.u32("frame count");
        const std::size_t frameSize = version == 1 ? V1_FRAME_SIZE : V2_FRAME_SIZE;
        if (count > limits.maxFramesPerTrack || count > reader.remaining() / frameSize)
            throw reader.error(countOffset, "COUNT_INVALID", "invalid frame count for " + name);
        if (count > limits.maxTotalFrames - std::min(totalFrames, limits.maxTotalFrames))
            throw reader.error(countOffset, "COUNT_LIMIT", "total frame count exceeds configured limit");
        totalFrames += count;
        const auto framesOffset = reader.offset();
        KsAnimationTrack track;
        track.name = name;
        track.frames.reserve(count);
        for (std::uint32_t frameIndex = 0; frameIndex < count; ++frameIndex) {
            const auto frameOffset = reader.offset();
            KsAnimationFrame frame{};
            if (version == 1) {
                Matrix4 matrix{};
                for (auto& value : matrix) value = reader.f32(name + " matrix frame");
                frame = decomposeKsAnimationMatrix(matrix);
            } else {
                for (auto& value : frame.quaternion) value = reader.f32(name + " quaternion frame");
                for (auto& value : frame.position) value = reader.f32(name + " position frame");
                for (auto& value : frame.scale) value = reader.f32(name + " scale frame");
            }
            if (!finiteFrame(frame)) throw reader.error(frameOffset, "NON_FINITE", "frame contains a non-finite value");
            track.frames.push_back(frame);
        }
        if (track.frames.size() > 1) {
            if (version == 2) {
                const auto frameBytes = track.frames.size() * V2_FRAME_SIZE;
                const auto raw = reader.bytes().subspan(framesOffset, frameBytes);
                track.animated = false;
                for (std::size_t frame = 1; frame < track.frames.size() && !track.animated; ++frame) {
                    for (std::size_t byte = 0; byte < V2_FRAME_SIZE; ++byte) {
                        if (raw[byte] != raw[frame * V2_FRAME_SIZE + byte]) { track.animated = true; break; }
                    }
                }
            } else {
                for (std::size_t frame = 1; frame < track.frames.size(); ++frame)
                    if (frameChanged(track.frames.front(), track.frames[frame])) { track.animated = true; break; }
            }
        }
        if (!firstFrameCount.has_value()) {
            firstFrameCount = track.frames.size();
            animation.frameCount = *firstFrameCount;
        } else if (track.frames.size() != *firstFrameCount)
            animation.warnings.push_back(animation.source + ": " + name + " has " +
                                         std::to_string(track.frames.size()) + " frames; the first track has " +
                                         std::to_string(*firstFrameCount));
        animation.tracks.push_back(std::move(track));
    }
    if (reader.offset() != bytes.size())
        throw reader.error(reader.offset(), "TRAILING_BYTES", std::to_string(bytes.size() - reader.offset()) + " trailing bytes after KSANIM data");
    animation.bytesRead = reader.offset();
    return animation;
}

std::vector<std::uint8_t> serializeKsAnimation(const KsAnimation& animation,
                                               apex::core::ParseLimits limits) {
    if (animation.tracks.size() > limits.maxTracks)
        throw invalid("serialize", 0, "COUNT_LIMIT", "track count exceeds configured limit");
    if (animation.tracks.size() > std::numeric_limits<std::uint32_t>::max())
        throw invalid("serialize", 0, "COUNT_LIMIT", "track count cannot fit in KSANIM");
    std::size_t size = 8;
    std::size_t totalFrames = 0;
    for (std::size_t index = 0; index < animation.tracks.size(); ++index) {
        const auto& track = animation.tracks[index];
        if (track.name.empty()) throw invalid("serialize", index, "INVALID_NAME", "KSANIM track has no name");
        ByteReader::validateUtf8(track.name, "track name", "KSANIM", "serialize", index);
        if (track.name.size() > limits.maxStringBytes || track.name.size() > std::numeric_limits<std::uint32_t>::max())
            throw invalid("serialize", index, "STRING_TOO_LARGE", "track name exceeds configured limit");
        if (track.frames.size() > limits.maxFramesPerTrack)
            throw invalid("serialize", index, "COUNT_LIMIT", "frame count exceeds configured limit");
        if (track.frames.size() > std::numeric_limits<std::uint32_t>::max())
            throw invalid("serialize", index, "COUNT_LIMIT", "frame count cannot fit in KSANIM");
        if (track.frames.size() > limits.maxTotalFrames - std::min(totalFrames, limits.maxTotalFrames))
            throw invalid("serialize", index, "COUNT_LIMIT", "total frame count exceeds configured limit");
        totalFrames += track.frames.size();
        const auto frameBytes = apex::core::checkedMultiply(track.frames.size(), V2_FRAME_SIZE, "KSANIM", "serialize", index, "frame");
        size = apex::core::checkedAdd(size, 8, "KSANIM", "serialize", index, "track");
        size = apex::core::checkedAdd(size, track.name.size(), "KSANIM", "serialize", index, "track");
        size = apex::core::checkedAdd(size, frameBytes, "KSANIM", "serialize", index, "track");
        for (std::size_t frame = 0; frame < track.frames.size(); ++frame)
            if (!finiteFrame(track.frames[frame])) throw invalid("serialize", index, "NON_FINITE", "frame contains a non-finite value");
    }
    if (size > limits.maxOutputBytes || size > std::numeric_limits<std::uint32_t>::max())
        throw invalid("serialize", 0, "OUTPUT_TOO_LARGE", "KSANIM output exceeds configured size limit");
    ByteWriter writer(size);
    writer.u32(2);
    writer.u32(static_cast<std::uint32_t>(animation.tracks.size()));
    for (const auto& track : animation.tracks) {
        writer.u32(static_cast<std::uint32_t>(track.name.size()));
        writer.raw(track.name);
        writer.u32(static_cast<std::uint32_t>(track.frames.size()));
        for (const auto& frame : track.frames) {
            for (const auto value : frame.quaternion) writer.f32(value);
            for (const auto value : frame.position) writer.f32(value);
            for (const auto value : frame.scale) writer.f32(value);
        }
    }
    return writer.finish();
}

std::optional<Matrix4> sampleKsAnimationTrack(const KsAnimationTrack& track, float position) {
    if (track.frames.empty()) return std::nullopt;
    const float value = std::isfinite(position) ? std::clamp(position, 0.0f, 1.0f) : 0.0f;
    const auto count = track.frames.size();
    if (count == 1 || value >= static_cast<float>(count - 1) / static_cast<float>(count))
        return ksAnimationMatrix(track.frames.back());
    const float scaled = static_cast<float>(count) * value;
    const auto frameIndex = static_cast<std::size_t>(std::floor(scaled));
    const float amount = scaled - static_cast<float>(frameIndex);
    const auto& from = track.frames[frameIndex];
    const auto& to = track.frames[frameIndex + 1];
    auto interpolate = [amount](const auto& first, const auto& second) {
        std::array<float, 3> result{};
        for (std::size_t index = 0; index < 3; ++index) result[index] = first[index] + (second[index] - first[index]) * amount;
        return result;
    };
    return ksAnimationMatrix({slerpQuaternion(from.quaternion, to.quaternion, amount),
                              interpolate(from.position, to.position), interpolate(from.scale, to.scale)});
}

std::vector<std::pair<std::string, Matrix4>> sampleKsAnimation(const KsAnimation& animation, float position) {
    std::vector<std::pair<std::string, Matrix4>> result;
    for (const auto& track : animation.tracks) {
        if (!track.animated) continue;
        if (const auto matrix = sampleKsAnimationTrack(track, position)) result.emplace_back(track.name, *matrix);
    }
    return result;
}

}
