#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace apex::core {

using Sha256Digest = std::array<std::uint8_t, 32>;

struct Sha256Result {
    Sha256Digest digest{};
    std::string hex;
};

// Computes the FIPS 180-4 SHA-256 digest without external dependencies.
// The hexadecimal form uses lower-case ASCII digits and represents the same
// 32-byte digest in network (big-endian) byte order.
[[nodiscard]] Sha256Result sha256(std::span<const std::uint8_t> bytes);

[[nodiscard]] inline std::string sha256Hex(
    std::span<const std::uint8_t> bytes) {
    return sha256(bytes).hex;
}

}  // namespace apex::core
