#pragma once

#include "apex/core/parse_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <array>

namespace apex::formats {

// A KNH file is a recursive tree of named DirectX row-major transforms.  The
// format has no magic or version header; the first record starts with its
// UTF-8 name length.
struct KnhNode {
  std::string name;
  std::array<float, 16> transform{};
  std::vector<KnhNode> children;
  std::size_t record_offset = 0;
};

struct KnhFile {
  std::string source;
  KnhNode root;
  std::size_t node_count = 0;
  std::size_t bytes_read = 0;
  std::size_t byte_length = 0;
};

inline constexpr std::size_t default_knh_native_object_bytes =
    256U * 1024U * 1024U;
inline constexpr std::size_t default_knh_max_nodes = 1'000'000U;

struct KnhParseOptions {
  apex::core::ParseLimits limits{};
  std::size_t maxNodes = default_knh_max_nodes;
  // Bounds allocations made while materializing the recursive KNH object
  // graph. This is separate from maxInputBytes because a child count can
  // reserve many C++ node objects before all child records are read.
  std::size_t maxNativeObjectBytes = default_knh_native_object_bytes;
};

class KnhError final : public std::runtime_error {
 public:
  KnhError(std::string message, std::size_t offset);

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  std::size_t offset_;
};

struct KnhWalkEntry {
  const KnhNode* node = nullptr;
  const KnhNode* parent = nullptr;
  std::size_t depth = 0;
};

KnhFile parse_knh(std::span<const std::byte> bytes,
                  std::string_view source = "driver_base_pos.knh");

KnhFile parse_knh(std::span<const std::byte> bytes,
                  std::string_view source, KnhParseOptions options);

KnhFile parse_knh(std::span<const std::uint8_t> bytes,
                  std::string_view source = "driver_base_pos.knh");

KnhFile parse_knh(std::span<const std::uint8_t> bytes,
                  std::string_view source, KnhParseOptions options);

std::vector<KnhWalkEntry> walk_knh(const KnhNode& root);

}  // namespace apex::formats
