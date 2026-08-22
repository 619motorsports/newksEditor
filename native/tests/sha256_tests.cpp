#include "apex/core/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::core::Sha256Digest;
using apex::core::Sha256Result;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string digestHex(const Sha256Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64u);
    for (const auto value : digest) {
        result.push_back(digits[value >> 4u]);
        result.push_back(digits[value & 0x0fu]);
    }
    return result;
}

void requireDigest(const Sha256Result& result, std::string_view expected,
                   std::string_view message) {
    require(result.hex == expected, message);
    require(result.hex == digestHex(result.digest), "hex and digest disagree");
    require(result.hex.size() == 64u, "SHA-256 hex length");
}

void hashesStandardVectors() {
    requireDigest(apex::core::sha256({}),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                  "empty SHA-256 vector");
    constexpr std::array<std::uint8_t, 3> abc = {'a', 'b', 'c'};
    requireDigest(apex::core::sha256(abc),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  "abc SHA-256 vector");
}

void hashesMultiBlockVector() {
    const std::vector<std::uint8_t> millionAs(1'000'000u, static_cast<std::uint8_t>('a'));
    requireDigest(apex::core::sha256(millionAs),
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                  "million-a SHA-256 vector");
}

void hashesEmbeddedNulBytes() {
    constexpr std::array<std::uint8_t, 4> bytes = {0x00u, 0x01u, 0xffu, 0x00u};
    requireDigest(apex::core::sha256(bytes),
                  "7d5c5de84c8ff4027cfb003aed237d4b27d0fe6edfb7e83e3e4bda87c5eec7d9",
                  "embedded NUL SHA-256 vector");
}

void repeatedCallsAreDeterministic() {
    constexpr std::array<std::uint8_t, 7> bytes = {0x00u, 0xffu, 0x00u, 0x01u,
                                                    0x02u, 0x00u, 0xffu};
    const auto first = apex::core::sha256(bytes);
    const auto second = apex::core::sha256(bytes);
    require(first.digest == second.digest && first.hex == second.hex,
            "repeated SHA-256 calls are deterministic");
}

}  // namespace

int main() {
    try {
        hashesStandardVectors();
        hashesMultiBlockVector();
        hashesEmbeddedNulBytes();
        repeatedCallsAreDeterministic();
        std::cout << "SHA-256 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SHA-256 tests failed: " << error.what() << '\n';
        return 1;
    }
}
