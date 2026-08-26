#include "dxbc_reader.hpp"

#include <limits>

namespace apex::render::detail {
namespace {

constexpr std::size_t header_bytes = 32U;
constexpr std::size_t chunk_header_bytes = 8U;

[[nodiscard]] bool is_signature(std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= 4U &&
           bytes[0] == std::byte{'D'} && bytes[1] == std::byte{'X'} &&
           bytes[2] == std::byte{'B'} && bytes[3] == std::byte{'C'};
}

[[nodiscard]] bool program_tag(std::uint32_t tag) noexcept {
    return tag == dxbc_tag_shdr || tag == dxbc_tag_shex ||
           tag == dxbc_tag_dxil;
}

} // namespace

bool read_u32_le(std::span<const std::byte> bytes, std::size_t offset,
                 std::uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = std::to_integer<std::uint32_t>(bytes[offset]) |
            (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
            (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
    return true;
}

std::span<const std::byte> as_bytes(
    std::span<const std::uint8_t> bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

bool read_dxbc_chunk(std::span<const std::byte> bytes, std::uint32_t index,
                     DxbcChunkView& chunk) noexcept {
    std::uint32_t count = 0U;
    if (!read_u32_le(bytes, 28U, count) || index >= count ||
        count > (bytes.size() - header_bytes) / 4U)
        return false;
    const std::size_t table_offset =
        header_bytes + static_cast<std::size_t>(index) * 4U;
    std::uint32_t offset_word = 0U;
    if (!read_u32_le(bytes, table_offset, offset_word)) return false;
    const std::size_t offset = static_cast<std::size_t>(offset_word);
    if (offset > bytes.size() || bytes.size() - offset < chunk_header_bytes)
        return false;
    std::uint32_t tag = 0U;
    std::uint32_t payload_word = 0U;
    if (!read_u32_le(bytes, offset, tag) ||
        !read_u32_le(bytes, offset + 4U, payload_word))
        return false;
    const std::size_t payload_bytes = static_cast<std::size_t>(payload_word);
    if (payload_bytes > bytes.size() - offset - chunk_header_bytes)
        return false;
    chunk = {tag, table_offset, offset,
             bytes.subspan(offset + chunk_header_bytes, payload_bytes)};
    return true;
}

DxbcReaderStatus inspect_dxbc_container(
    std::span<const std::byte> bytes, std::uint32_t max_chunks,
    DxbcContainerInspection& inspection, std::size_t& error_offset) noexcept {
    inspection = {};
    error_offset = 0U;
    if (bytes.size() < header_bytes)
        return DxbcReaderStatus::truncated_header;
    if (!is_signature(bytes)) return DxbcReaderStatus::invalid_signature;

    std::uint32_t header_version = 0U;
    std::uint32_t declared_size = 0U;
    std::uint32_t chunk_count = 0U;
    (void)read_u32_le(bytes, 20U, header_version);
    (void)read_u32_le(bytes, 24U, declared_size);
    (void)read_u32_le(bytes, 28U, chunk_count);
    if (header_version != 1U) {
        error_offset = 20U;
        return DxbcReaderStatus::invalid_header_version;
    }
    if (static_cast<std::size_t>(declared_size) != bytes.size()) {
        error_offset = 24U;
        return DxbcReaderStatus::declared_size_mismatch;
    }
    if (chunk_count == 0U || max_chunks == 0U || chunk_count > max_chunks) {
        error_offset = 28U;
        return DxbcReaderStatus::invalid_chunk_count;
    }
    if (chunk_count > (bytes.size() - header_bytes) / 4U) {
        error_offset = 28U;
        return DxbcReaderStatus::chunk_table_out_of_bounds;
    }
    const std::size_t table_end =
        header_bytes + static_cast<std::size_t>(chunk_count) * 4U;

    bool found_legacy = false;
    bool found_dxil = false;
    for (std::uint32_t index = 0U; index < chunk_count; ++index) {
        const std::size_t table_offset =
            header_bytes + static_cast<std::size_t>(index) * 4U;
        std::uint32_t offset_word = 0U;
        (void)read_u32_le(bytes, table_offset, offset_word);
        const std::size_t offset = static_cast<std::size_t>(offset_word);
        if (offset < table_end || offset > bytes.size() ||
            bytes.size() - offset < chunk_header_bytes) {
            error_offset = table_offset;
            return DxbcReaderStatus::chunk_header_out_of_bounds;
        }
        std::uint32_t tag = 0U;
        std::uint32_t payload_word = 0U;
        (void)read_u32_le(bytes, offset, tag);
        (void)read_u32_le(bytes, offset + 4U, payload_word);
        const std::size_t payload_bytes =
            static_cast<std::size_t>(payload_word);
        if (payload_bytes > bytes.size() - offset - chunk_header_bytes) {
            error_offset = offset + 4U;
            return DxbcReaderStatus::chunk_payload_out_of_bounds;
        }
        const std::size_t end = offset + chunk_header_bytes + payload_bytes;

        for (std::uint32_t previous = 0U; previous < index; ++previous) {
            DxbcChunkView other;
            if (!read_dxbc_chunk(bytes, previous, other)) {
                error_offset = table_offset;
                return DxbcReaderStatus::chunk_header_out_of_bounds;
            }
            const std::size_t other_end =
                other.header_offset + chunk_header_bytes + other.payload.size();
            if (offset < other_end && other.header_offset < end) {
                error_offset = offset;
                return DxbcReaderStatus::chunks_overlap;
            }
        }

        if (!program_tag(tag)) continue;
        if (payload_bytes < 4U) {
            error_offset = offset;
            return DxbcReaderStatus::program_chunk_truncated;
        }
        ++inspection.program_chunk_count;
        found_dxil = found_dxil || tag == dxbc_tag_dxil;
        found_legacy = found_legacy || tag == dxbc_tag_shdr ||
                       tag == dxbc_tag_shex;
    }

    inspection.chunk_count = chunk_count;
    inspection.program_format =
        found_legacy && found_dxil
            ? DxbcProgramFormat::mixed
            : found_legacy ? DxbcProgramFormat::legacy
                           : found_dxil ? DxbcProgramFormat::dxil
                                        : DxbcProgramFormat::none;
    return DxbcReaderStatus::ready;
}

} // namespace apex::render::detail
