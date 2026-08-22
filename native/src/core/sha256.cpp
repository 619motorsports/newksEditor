#include "apex/core/sha256.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace apex::core {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

[[nodiscard]] std::uint32_t bigEndianWord(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24u) |
           (static_cast<std::uint32_t>(bytes[1]) << 16u) |
           (static_cast<std::uint32_t>(bytes[2]) << 8u) |
           static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                                   std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

[[nodiscard]] std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                                     std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] std::uint32_t bigSigma0(std::uint32_t value) noexcept {
    return std::rotr(value, 2u) ^ std::rotr(value, 13u) ^ std::rotr(value, 22u);
}

[[nodiscard]] std::uint32_t bigSigma1(std::uint32_t value) noexcept {
    return std::rotr(value, 6u) ^ std::rotr(value, 11u) ^ std::rotr(value, 25u);
}

[[nodiscard]] std::uint32_t smallSigma0(std::uint32_t value) noexcept {
    return std::rotr(value, 7u) ^ std::rotr(value, 18u) ^ (value >> 3u);
}

[[nodiscard]] std::uint32_t smallSigma1(std::uint32_t value) noexcept {
    return std::rotr(value, 17u) ^ std::rotr(value, 19u) ^ (value >> 10u);
}

void compress(std::array<std::uint32_t, 8>& state,
              const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16u; ++index)
        schedule[index] = bigEndianWord(block + index * 4u);
    for (std::size_t index = 16u; index < schedule.size(); ++index) {
        schedule[index] = smallSigma1(schedule[index - 2u]) + schedule[index - 7u] +
                          smallSigma0(schedule[index - 15u]) + schedule[index - 16u];
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < kRoundConstants.size(); ++index) {
        const auto temporary1 = h + bigSigma1(e) + choose(e, f, g) +
                                kRoundConstants[index] + schedule[index];
        const auto temporary2 = bigSigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

Sha256Result sha256(std::span<const std::uint8_t> bytes) {
    auto state = kInitialState;
    const auto fullBytes = bytes.size() - (bytes.size() % 64u);
    for (std::size_t offset = 0; offset < fullBytes; offset += 64u)
        compress(state, bytes.data() + offset);

    const auto remainder = bytes.size() - fullBytes;
    std::array<std::uint8_t, 128> finalBlocks{};
    for (std::size_t index = 0; index < remainder; ++index)
        finalBlocks[index] = bytes[fullBytes + index];
    finalBlocks[remainder] = 0x80u;

    // SHA-256 encodes the message length modulo 2^64. The explicit cast is
    // intentional: a span cannot practically exceed the representable bit
    // length on supported hosts, while the standard defines this truncation.
    const auto bitLength = static_cast<std::uint64_t>(bytes.size()) * 8u;
    const auto finalBytes = remainder < 56u ? std::size_t{64u} : std::size_t{128u};
    for (std::size_t index = 0; index < 8u; ++index) {
        const auto shift = static_cast<unsigned>((7u - index) * 8u);
        finalBlocks[finalBytes - 8u + index] =
            static_cast<std::uint8_t>(bitLength >> shift);
    }
    compress(state, finalBlocks.data());
    if (finalBytes == 128u) compress(state, finalBlocks.data() + 64u);

    Sha256Result result;
    result.hex.reserve(64u);
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < state.size(); ++index) {
        const auto word = state[index];
        for (std::size_t byte = 0; byte < 4u; ++byte) {
            const auto shift = static_cast<unsigned>((3u - byte) * 8u);
            const auto value = static_cast<std::uint8_t>(word >> shift);
            result.digest[index * 4u + byte] = value;
            result.hex.push_back(digits[value >> 4u]);
            result.hex.push_back(digits[value & 0x0fu]);
        }
    }
    return result;
}

}  // namespace apex::core
