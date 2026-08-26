#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace apex::render::detail {

inline constexpr std::uint32_t dxbc_tag_shdr = 0x52444853U;
inline constexpr std::uint32_t dxbc_tag_shex = 0x58454853U;
inline constexpr std::uint32_t dxbc_tag_dxil = 0x4c495844U;
inline constexpr std::uint32_t dxbc_tag_rdef = 0x46454452U;
inline constexpr std::uint32_t dxbc_tag_isgn = 0x4e475349U;
inline constexpr std::uint32_t dxbc_tag_osgn = 0x4e47534fU;

enum class DxbcReaderStatus : std::uint8_t {
    ready,
    truncated_header,
    invalid_signature,
    invalid_header_version,
    declared_size_mismatch,
    invalid_chunk_count,
    chunk_table_out_of_bounds,
    chunk_header_out_of_bounds,
    chunk_payload_out_of_bounds,
    chunks_overlap,
    program_chunk_truncated,
};

enum class DxbcProgramFormat : std::uint8_t {
    none,
    legacy,
    dxil,
    mixed,
};

struct DxbcChunkView {
    std::uint32_t tag = 0U;
    std::size_t table_offset = 0U;
    std::size_t header_offset = 0U;
    std::span<const std::byte> payload;
};

struct DxbcContainerInspection {
    std::uint32_t chunk_count = 0U;
    std::uint32_t program_chunk_count = 0U;
    DxbcProgramFormat program_format = DxbcProgramFormat::none;
};

[[nodiscard]] DxbcReaderStatus inspect_dxbc_container(
    std::span<const std::byte> bytes, std::uint32_t max_chunks,
    DxbcContainerInspection& inspection, std::size_t& error_offset) noexcept;

// Call this only after inspect_dxbc_container succeeds. It still repeats the
// relevant bounds checks so a bad index cannot produce an invalid span.
[[nodiscard]] bool read_dxbc_chunk(std::span<const std::byte> bytes,
                                   std::uint32_t index,
                                   DxbcChunkView& chunk) noexcept;

[[nodiscard]] bool read_u32_le(std::span<const std::byte> bytes,
                               std::size_t offset,
                               std::uint32_t& value) noexcept;

[[nodiscard]] std::span<const std::byte> as_bytes(
    std::span<const std::uint8_t> bytes) noexcept;

} // namespace apex::render::detail
