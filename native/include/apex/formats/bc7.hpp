#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace apex::formats {

inline constexpr std::size_t bc7_block_bytes = 16u;
inline constexpr std::size_t bc7_block_width = 4u;
inline constexpr std::size_t bc7_block_height = 4u;
inline constexpr std::size_t bc7_partition_table_bytes = 2048u;

// Scratch storage is caller-owned so a mip decoder can reuse it for every
// block without allocating or retaining untrusted-size state.
struct Bc7Scratch {
    std::array<std::uint16_t, 24> endpoints{};
    std::array<std::uint8_t, 16> indices{};
    std::uint32_t bit_position = 0;
};

// Structural self-check for the embedded 64-entry two/three-subset table.
// This is exposed for bounded regression tests and does not expose the table.
[[nodiscard]] bool bc7PartitionTableValid() noexcept;

// Decode one BC7 block to a 4x4 RGBA8 tile. All bounds are checked before the
// bitstream is read or output is written. Throws ParseError with format BC7.
void decodeBc7Block(std::span<const std::uint8_t> input, std::size_t input_offset,
                    std::span<std::uint8_t> output, std::size_t output_offset,
                    std::size_t output_pitch, Bc7Scratch& scratch,
                    std::string_view source = "texture.dds");

} // namespace apex::formats
