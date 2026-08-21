#pragma once

#include "apex/core/parse_error.hpp"
#include "apex/core/parse_limits.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace apex::core {

class ByteReader final {
public:
    ByteReader(std::span<const std::uint8_t> bytes, std::string source,
               ParseLimits limits = {}, std::string format = "binary");

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const ParseLimits& limits() const noexcept { return limits_; }

    void need(std::size_t count, std::string_view what) const;
    void advance(std::size_t count, std::string_view what);

    [[nodiscard]] std::uint8_t u8(std::string_view what);
    [[nodiscard]] std::uint32_t u32(std::string_view what);
    [[nodiscard]] float f32(std::string_view what);
    [[nodiscard]] std::string string(std::string_view what);

    [[nodiscard]] ParseError error(std::size_t at, std::string_view code,
                                   std::string_view message) const;

    static std::string decodeUtf8(std::span<const std::uint8_t> bytes,
                                  std::string_view what, std::string_view format,
                                  std::string_view source, std::size_t offset);
    static void validateUtf8(std::string_view value, std::string_view what,
                             std::string_view format, std::string_view source,
                             std::size_t offset);

private:
    std::span<const std::uint8_t> bytes_;
    std::string source_;
    std::string format_;
    ParseLimits limits_;
    std::size_t offset_ = 0;
};

[[nodiscard]] std::size_t checkedAdd(std::size_t left, std::size_t right,
                                     std::string_view format, std::string_view source,
                                     std::size_t offset, std::string_view what);
[[nodiscard]] std::size_t checkedMultiply(std::size_t left, std::size_t right,
                                          std::string_view format, std::string_view source,
                                          std::size_t offset, std::string_view what);

}
