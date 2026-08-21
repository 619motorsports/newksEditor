#include "apex/formats/bc7.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace apex::formats {

namespace {

using apex::core::ParseError;

// Ported from bcdec commit 93628fe5627102fe5187b7eeb99122dec6612c36.
// The partition data and decode equations match src/bc7-decoder.js.
constexpr char kPartitionData[] = "gAABAQAAAQEAAAEBAAABgYAAAAEAAAABAAAAAQAAAIGAAQEBAAEBAQABAQEAAQGBgAAAAQAAAQEAAAEBAAEBgYAAAAAAAAABAAAAAQAAAYGAAAEBAAEBAQABAQEBAQGBgAAAAQAAAQEAAQEBAQEBgYAAAAAAAAABAAABAQABAYGAAAAAAAAAAAAAAAEAAAGBgAABAQABAQEBAQEBAQEBgYAAAAAAAAABAAEBAQEBAYGAAAAAAAAAAAAAAAEAAQGBgAAAAQABAQEBAQEBAQEBgYAAAAAAAAAAAQEBAQEBAYGAAAAAAQEBAQEBAQEBAQGBgAAAAAAAAAAAAAAAAQEBgYAAAAABAAAAAQEBAAEBAYGAAYEBAAAAAQAAAAAAAAAAgAAAAAAAAACBAAAAAQEBAIABgQEAAAEBAAAAAQAAAACAAIEBAAAAAQAAAAAAAAAAgAAAAAEAAACBAQAAAQEBAIAAAAAAAAAAgQAAAAEBAACAAQEBAAABAQAAAQEAAACBgACBAQAAAAEAAAABAAAAAIAAAAABAAAAgQAAAAEBAACAAYEAAAEBAAABAQAAAQEAgACBAQABAQAAAQEAAQEAAIAAAAEAAQEBgQEBAAEAAACAAAAAAQEBAYEBAQEAAAAAgAGBAQAAAAEBAAAAAQEBAIAAgQEBAAABAQAAAQEBAACAAQABAAEAAQABAAEAAQCBgAAAAAEBAQEAAAAAAQEBgYABAAEBAIEAAAEAAQEAAQCAAAEBAAABAYEBAAABAQAAgACBAQEBAAAAAAEBAQEAAIABAAEAAQABgQABAAEAAQCAAQEAAQAAAQABAQABAACBgAEAAQEAAQABAAEAAAEAgYABgQEAAAEBAQEAAAEBAQCAAAABAAABAYEBAAABAAAAgACBAQAAAQAAAQAAAQEAAIAAgQEBAAEBAQEAAQEBAACAAYEAAQAAAQEAAAEAAQEAgAABAQEBAAABAQAAAAABgYABAQAAAQEAAQAAAQEAAIGAAAAAAAGBAAABAQAAAAAAgAEAAAEBgQAAAQAAAAAAAIAAgQAAAQEBAAABAAAAAACAAAAAAACBAAABAQEAAAEAgAAAAAABAACBAQEAAAEAAIABAQABAQAAAQAAAQAAAYGAAAEBAAEBAAEBAAABAACBgAGBAAAAAQEBAAABAQEAAIAAgQEBAAABAQEAAAABAQCAAQEAAQEAAAEBAAABAACBgAEBAAAAAQEAAAEBAQAAgYABAQEBAQEAAQAAAAAAAIGAAAABAQAAAAEBAQAAAQGBgAAAAAEBAQEAAAEBAAABgYAAgQEAAAEBAQEBAQAAAACAAIEAAAABAAEBAQABAQEAgAEAAAABAAAAAQEBAAEBgYAAAYEAAAEBAAICAQICAoKAAACBAAABAYICAQECAgIBgAAAAAIAAAGCAgEBAgIBgYACAoIAAAICAAABAQABAYGAAAAAAAAAAIEBAgIBAQKCgAABgQAAAQEAAAICAAACgoAAAoIAAAICAQEBAQEBAYGAAAEBAAABAYICAQECAgGBgAAAAAAAAACBAQEBAgICgoAAAAABAQEBgQEBAQICAoKAAAAAAQGBAQICAgICAgKCgAABAgAAgQIAAAECAAABgoABAQIAAYECAAEBAgABAYKAAQICAIECAgABAgIAAQKCgAABgQABAQIBAQICAQICgoAAAYECAAABggIAAAICAgCAAACBAAABAQABAQIBAQKCgAEBgQAAAQGCAAABAgIAAIAAAAABAQICgQECAgEBAoKAAAKCAAACAgAAAgIBAQGBgAEBgQABAQEAAgICAAICgoAAAIEAAAABggICAQICAgGAAAAAAACBAQABAgIAAQKCgAAAAAEBAACCAoEAAgIBAIABAoIAgQICAAABAQAAAACAAAECAAABAoEBAgICAgKCgAEBAAECggGBAgIBAAEBAIAAAAAAAYEAAQKCAQECAgGAAAICAQEAAoEBAAIAAAKCgAEBAACBAQACAAACAgICgoAAAQEAAQICAAGCAgAAAYGAAAAAAgAAAIICAQECAgKBgAAAAAAAAAKBAQICAQICgoACAoIAAAICAAABAgAAAYGAAAGBAAABAgAAAgIAAgKCgAECAACBAgAAAYIAAAECAIAAAAABAYEBAgKCAgAAAACAAQIAAQIAAYIAgQIAAQIAgAECAAIAAQKBggABAAECAIAAAQECAgAAAQGCAgAAAYGAAAEBAQGCAgICAAAAAAGBgAEAgQABAAECAgICAgICgoAAAAAAAAAAggECAQIBAoGAAAICAYECAgAAAgIBAQKCgAACggAAAQEAAAICAAABgYACAgABAoIBAAICAAECAoGAAQABAgKCAgICAgIAAQCBgAAAAAIBAgGCAQIBAgECgYABAIEAAQABAAEAAQICAoKAAgKCAAEBAQACAgIAAQGBgAAAAgGBAQIAAAACAQEBgoAAAAACgQECAgEBAgIBAYKAAgICAIEBAQABAQEAAgKCgAAAAgEBAQKBAQECAAAAgoABAQAAgQEAAAEBAAICAoKAAAAAAAAAAAIBgQICAQGCgAEBAACBAQACAgICAgICgoAAAgIAAAEBAACBAQAAAoKAAAICAQECAoEBAgIAAAKCgAAAAAAAAAAAAAAAAoEBgoAAAIIAAAABAAAAAgAAAIGAAgICAQICAgACAgKBAgKCgAEAgQICAgICAgICAgICgoABAYECAAEBggIAAQICAgA=";

[[nodiscard]] ParseError bc7Error(std::string_view source, std::size_t offset,
                                  std::string_view code, std::string_view message) {
    return ParseError("BC7", std::string(source), offset, std::string(code), std::string(message));
}

[[nodiscard]] constexpr int base64Value(char value) noexcept {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

struct PartitionTable {
    std::array<std::uint8_t, bc7_partition_table_bytes> bytes{};
    bool valid = false;
};

[[nodiscard]] const PartitionTable& partitionTableData() {
    static const auto table = [] {
        PartitionTable result{};
        std::size_t output = 0;
        std::size_t index = 0;
        constexpr std::size_t encodedSize = sizeof(kPartitionData) - 1u;
        for (; index + 4u <= encodedSize && output < result.bytes.size(); index += 4) {
            const int a = base64Value(kPartitionData[index]);
            const int b = base64Value(kPartitionData[index + 1]);
            const char cchar = kPartitionData[index + 2];
            const char dchar = kPartitionData[index + 3];
            const int c = cchar == '=' ? 0 : base64Value(cchar);
            const int d = dchar == '=' ? 0 : base64Value(dchar);
            if (a < 0 || b < 0 || c < 0 || d < 0 ||
                (cchar == '=' && dchar != '=') ||
                (cchar == '=' && index + 4u != encodedSize) ||
                (dchar == '=' && index + 4u != encodedSize))
                break;
            result.bytes[output++] = static_cast<std::uint8_t>((a << 2) | (b >> 4));
            if (cchar != '=' && output < result.bytes.size())
                result.bytes[output++] = static_cast<std::uint8_t>((b << 4) | (c >> 2));
            if (dchar != '=' && output < result.bytes.size())
                result.bytes[output++] = static_cast<std::uint8_t>((c << 6) | d);
        }
        result.valid = output == result.bytes.size() && index == encodedSize;
        return result;
    }();
    return table;
}

[[nodiscard]] const std::array<std::uint8_t, bc7_partition_table_bytes>& partitionTable() {
    return partitionTableData().bytes;
}

[[nodiscard]] std::uint32_t readBits(std::span<const std::uint8_t> bytes, std::size_t input_offset,
                                     Bc7Scratch& scratch, std::uint32_t count,
                                     std::string_view source) {
    if (count > 128u - scratch.bit_position)
        throw bc7Error(source, input_offset, "BITSTREAM_BOUNDS", "BC7 block exceeds 128 bits");
    std::uint32_t value = 0;
    for (std::uint32_t bit = 0; bit < count; ++bit, ++scratch.bit_position) {
        const auto byte = static_cast<std::uint32_t>(bytes[input_offset + (scratch.bit_position >> 3u)]);
        const auto extracted = (byte >> (scratch.bit_position & 7u)) & 1u;
        value |= extracted << bit;
    }
    return value;
}

[[nodiscard]] std::uint8_t partitionSetAt(std::uint32_t num_partitions, std::uint32_t partition,
                                          std::uint32_t row, std::uint32_t column) noexcept {
    if (num_partitions == 1u) return (row | column) != 0u ? 0u : 128u;
    return partitionTable()[((num_partitions - 2u) * 64u + partition) * 16u + row * 4u + column];
}

[[nodiscard]] std::uint8_t interpolate(std::uint16_t first, std::uint16_t second,
                                       std::span<const std::uint8_t> weights,
                                       std::uint8_t index) noexcept {
    const auto weight = weights[index];
    return static_cast<std::uint8_t>((first * (64u - weight) + second * weight + 32u) >> 6u);
}

} // namespace

bool bc7PartitionTableValid() noexcept {
    return partitionTableData().valid;
}

void decodeBc7Block(std::span<const std::uint8_t> input, std::size_t input_offset,
                    std::span<std::uint8_t> output, std::size_t output_offset,
                    std::size_t output_pitch, Bc7Scratch& scratch, std::string_view source) {
    if (input_offset > input.size() || input.size() - input_offset < bc7_block_bytes)
        throw bc7Error(source, input_offset, "TRUNCATED", "BC7 block needs 16 input bytes");
    if (output_pitch < 16u || output_offset > output.size() ||
        output_pitch > (std::numeric_limits<std::size_t>::max() - 16u) / 3u)
        throw bc7Error(source, output_offset, "OUTPUT_BOUNDS", "BC7 output cannot hold a 4x4 RGBA block");
    const auto footprint = output_pitch * 3u + 16u;
    if (footprint > output.size() || output_offset > output.size() - footprint)
        throw bc7Error(source, output_offset, "OUTPUT_BOUNDS", "BC7 output cannot hold a 4x4 RGBA block");
    if (!bc7PartitionTableValid())
        throw bc7Error(source, input_offset, "PARTITION_TABLE_INVALID", "BC7 partition table is not 2048 bytes");
    if (input[input_offset] == 0u)
        throw bc7Error(source, input_offset, "INVALID_MODE", "BC7 block has no valid mode");

    constexpr std::array<std::array<std::uint8_t, 8>, 2> actual_bits = {
        std::array<std::uint8_t, 8>{4, 6, 5, 7, 5, 7, 7, 5},
        std::array<std::uint8_t, 8>{0, 0, 0, 0, 6, 8, 7, 5}};
    constexpr std::array<std::array<std::uint8_t, 16>, 3> weights = {
        std::array<std::uint8_t, 16>{},
        std::array<std::uint8_t, 16>{0, 21, 43, 64},
        std::array<std::uint8_t, 16>{0, 9, 18, 27, 37, 46, 55, 64},
    };
    constexpr std::array<std::uint8_t, 16> weights4 = {
        0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    constexpr std::uint8_t modes_with_p_bits = 0b11001011u;

    scratch.bit_position = 0;
    scratch.endpoints.fill(0);
    std::uint32_t mode = 0;
    while (mode < 8u && readBits(input, input_offset, scratch, 1u, source) == 0u) ++mode;
    if (mode == 8u) throw bc7Error(source, input_offset, "INVALID_MODE", "BC7 block has no valid mode");

    std::uint32_t partition = 0;
    std::uint32_t num_partitions = 1;
    std::uint32_t rotation = 0;
    std::uint32_t index_selection = 0;
    if (mode == 0u || mode == 1u || mode == 2u || mode == 3u || mode == 7u) {
        num_partitions = mode == 0u || mode == 2u ? 3u : 2u;
        partition = readBits(input, input_offset, scratch, mode == 0u ? 4u : 6u, source);
    }
    const std::uint32_t num_endpoints = num_partitions * 2u;
    if (mode == 4u || mode == 5u) {
        rotation = readBits(input, input_offset, scratch, 2u, source);
        if (mode == 4u) index_selection = readBits(input, input_offset, scratch, 1u, source);
    }

    for (std::uint32_t component = 0; component < 3u; ++component)
        for (std::uint32_t endpoint = 0; endpoint < num_endpoints; ++endpoint)
            scratch.endpoints[endpoint * 4u + component] = static_cast<std::uint16_t>(
                readBits(input, input_offset, scratch, actual_bits[0][mode], source));
    if (actual_bits[1][mode] != 0u)
        for (std::uint32_t endpoint = 0; endpoint < num_endpoints; ++endpoint)
            scratch.endpoints[endpoint * 4u + 3u] = static_cast<std::uint16_t>(
                readBits(input, input_offset, scratch, actual_bits[1][mode], source));

    if (mode == 0u || mode == 1u || mode == 3u || mode == 6u || mode == 7u) {
        for (std::uint32_t endpoint = 0; endpoint < num_endpoints; ++endpoint)
            for (std::uint32_t component = 0; component < 4u; ++component) {
                auto& value = scratch.endpoints[endpoint * 4u + component];
                value = static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) << 1u);
            }
        if (mode == 1u) {
            const auto first = readBits(input, input_offset, scratch, 1u, source);
            const auto second = readBits(input, input_offset, scratch, 1u, source);
            for (std::uint32_t component = 0; component < 3u; ++component) {
                scratch.endpoints[component] |= static_cast<std::uint16_t>(first);
                scratch.endpoints[4u + component] |= static_cast<std::uint16_t>(first);
                scratch.endpoints[8u + component] |= static_cast<std::uint16_t>(second);
                scratch.endpoints[12u + component] |= static_cast<std::uint16_t>(second);
            }
        } else if ((modes_with_p_bits & static_cast<std::uint8_t>(1u << mode)) != 0u) {
            for (std::uint32_t endpoint = 0; endpoint < num_endpoints; ++endpoint) {
                const auto p_bit = readBits(input, input_offset, scratch, 1u, source);
                for (std::uint32_t component = 0; component < 4u; ++component)
                    scratch.endpoints[endpoint * 4u + component] |= static_cast<std::uint16_t>(p_bit);
            }
        }
    }

    for (std::uint32_t endpoint = 0; endpoint < num_endpoints; ++endpoint) {
        const auto color_bits = static_cast<std::uint32_t>(actual_bits[0][mode]) +
                                ((modes_with_p_bits >> mode) & 1u);
        for (std::uint32_t component = 0; component < 3u; ++component) {
            auto& target = scratch.endpoints[endpoint * 4u + component];
            target = static_cast<std::uint16_t>(target << (8u - color_bits));
            target = static_cast<std::uint16_t>(target | (target >> color_bits));
        }
        const auto alpha_bits = static_cast<std::uint32_t>(actual_bits[1][mode]) +
                                ((modes_with_p_bits >> mode) & 1u);
        auto& alpha = scratch.endpoints[endpoint * 4u + 3u];
        if (alpha_bits != 0u) {
            alpha = static_cast<std::uint16_t>(alpha << (8u - alpha_bits));
            alpha = static_cast<std::uint16_t>(alpha | (alpha >> alpha_bits));
        }
    }
    if (actual_bits[1][mode] == 0u)
        for (std::uint32_t endpoint = 0; endpoint < num_endpoints; ++endpoint)
            scratch.endpoints[endpoint * 4u + 3u] = 255u;

    const std::uint32_t primary_bits = mode == 0u || mode == 1u ? 3u : mode == 6u ? 4u : 2u;
    const std::uint32_t secondary_bits = mode == 4u ? 3u : mode == 5u ? 2u : 0u;
    const auto primary_weights = primary_bits == 4u ? std::span<const std::uint8_t>(weights4) :
                                 primary_bits == 3u ? std::span<const std::uint8_t>(weights[2]) :
                                                       std::span<const std::uint8_t>(weights[1]);
    const auto secondary_weights = secondary_bits == 3u ? std::span<const std::uint8_t>(weights[2]) :
                                   std::span<const std::uint8_t>(weights[1]);

    for (std::uint32_t row = 0; row < 4u; ++row)
        for (std::uint32_t column = 0; column < 4u; ++column) {
            const auto set = partitionSetAt(num_partitions, partition, row, column);
            scratch.indices[row * 4u + column] = static_cast<std::uint8_t>(
                readBits(input, input_offset, scratch, primary_bits - ((set & 0x80u) != 0u ? 1u : 0u), source));
        }

    for (std::uint32_t row = 0; row < 4u; ++row)
        for (std::uint32_t column = 0; column < 4u; ++column) {
            const auto set = partitionSetAt(num_partitions, partition, row, column);
            const auto subset = static_cast<std::uint32_t>(set & 3u);
            const auto first = subset * 8u;
            const auto second = first + 4u;
            const auto index = scratch.indices[row * 4u + column];
            std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
            if (secondary_bits == 0u) {
                red = interpolate(scratch.endpoints[first], scratch.endpoints[second], primary_weights, index);
                green = interpolate(scratch.endpoints[first + 1u], scratch.endpoints[second + 1u], primary_weights, index);
                blue = interpolate(scratch.endpoints[first + 2u], scratch.endpoints[second + 2u], primary_weights, index);
                alpha = interpolate(scratch.endpoints[first + 3u], scratch.endpoints[second + 3u], primary_weights, index);
            } else {
                const auto index2 = static_cast<std::uint8_t>(readBits(
                    input, input_offset, scratch, secondary_bits - ((row | column) != 0u ? 0u : 1u), source));
                const auto color_weights = index_selection != 0u ? secondary_weights : primary_weights;
                const auto color_index = index_selection != 0u ? index2 : index;
                const auto alpha_weights = index_selection != 0u ? primary_weights : secondary_weights;
                const auto alpha_index = index_selection != 0u ? index : index2;
                red = interpolate(scratch.endpoints[first], scratch.endpoints[second], color_weights, color_index);
                green = interpolate(scratch.endpoints[first + 1u], scratch.endpoints[second + 1u], color_weights, color_index);
                blue = interpolate(scratch.endpoints[first + 2u], scratch.endpoints[second + 2u], color_weights, color_index);
                alpha = interpolate(scratch.endpoints[first + 3u], scratch.endpoints[second + 3u], alpha_weights, alpha_index);
            }
            if (rotation == 1u) std::swap(alpha, red);
            else if (rotation == 2u) std::swap(alpha, green);
            else if (rotation == 3u) std::swap(alpha, blue);
            const auto target = output_offset + static_cast<std::size_t>(row) * output_pitch +
                                static_cast<std::size_t>(column) * 4u;
            output[target] = red;
            output[target + 1u] = green;
            output[target + 2u] = blue;
            output[target + 3u] = alpha;
        }
}

} // namespace apex::formats
