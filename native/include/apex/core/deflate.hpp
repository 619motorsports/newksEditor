#pragma once

#include "apex/core/parse_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace apex::core {

// Inflate one zlib-wrapped DEFLATE stream. The stream and output are bounded
// before storage is reserved. This small shared seam is used by format
// readers; it does not expose a renderer or filesystem dependency.
[[nodiscard]] std::vector<std::uint8_t>
inflateZlib(std::span<const std::uint8_t> bytes,
            std::size_t expectedOutputBytes,
            std::string source = "deflate stream", ParseLimits limits = {},
            std::size_t sourceOffset = 0);

// Inflate a raw DEFLATE stream. The complete supplied span must be consumed;
// this prevents a caller from silently accepting a second payload after the
// final block. The API is shared so archive and image readers do not each
// carry a private inflater.
[[nodiscard]] std::vector<std::uint8_t>
inflateDeflateRaw(std::span<const std::uint8_t> bytes,
                  std::size_t expectedOutputBytes,
                  std::string source = "deflate stream",
                  ParseLimits limits = {}, std::size_t sourceOffset = 0);

} // namespace apex::core
