#pragma once

#include "apex/core/parse_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

using Matrix4 = std::array<float, 16>;
using Quaternion = std::array<float, 4>;
using Vector3 = std::array<float, 3>;

struct KsAnimationFrame {
    Quaternion quaternion{};
    Vector3 position{};
    Vector3 scale{};
};

struct KsAnimationTrack {
    std::string name;
    std::vector<KsAnimationFrame> frames;
    bool animated = false;
};

struct KsAnimation {
    std::string source;
    std::uint32_t version = 0;
    std::vector<KsAnimationTrack> tracks;
    std::size_t frameCount = 0;
    std::size_t bytesRead = 0;
    std::size_t byteLength = 0;
    std::vector<std::string> warnings;
};

KsAnimation parseKsAnimation(std::span<const std::uint8_t> bytes,
                             std::string source = "animation.ksanim",
                             apex::core::ParseLimits limits = {});

// KSANIM export uses the native v2 quaternion/position/scale representation.
std::vector<std::uint8_t> serializeKsAnimation(const KsAnimation& animation,
                                               apex::core::ParseLimits limits = {});

[[nodiscard]] KsAnimationFrame decomposeKsAnimationMatrix(const Matrix4& matrix);
[[nodiscard]] Matrix4 ksAnimationMatrix(const KsAnimationFrame& frame);

[[nodiscard]] std::optional<Matrix4> sampleKsAnimationTrack(
    const KsAnimationTrack& track, float position);

// Returns one matrix per animated track, retaining source order.
[[nodiscard]] std::vector<std::pair<std::string, Matrix4>> sampleKsAnimation(
    const KsAnimation& animation, float position);

}
