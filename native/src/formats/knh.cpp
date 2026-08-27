#include <apex/formats/knh.hpp>

#include <apex/core/byte_reader.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::size_t kMaxNameBytes = 1U << 20;
constexpr std::size_t kMaxDepth = 1024;
// A child record always contains at least a name length, 16 floats, and a
// child count. This lets us reject impossible counts before allocating.
constexpr std::size_t kMinimumNodeBytes = 4 + 16 * sizeof(float) + 4;

[[noreturn]] void fail(std::string message, std::size_t offset) {
  throw KnhError(std::move(message), offset);
}

class NativeAllocationBudget final {
 public:
  explicit NativeAllocationBudget(std::size_t limit) : limit_(limit) {}

  void charge(std::size_t bytes, std::size_t offset, std::string_view what) {
    if (bytes > limit_ - used_) {
      fail("KNH native allocation budget exceeded while reserving " +
               std::string(what),
           offset);
    }
    used_ += bytes;
  }

  void charge_count(std::size_t count, std::size_t element_bytes,
                    std::size_t offset, std::string_view what) {
    if (count != 0U &&
        element_bytes > std::numeric_limits<std::size_t>::max() / count) {
      fail("KNH native allocation size overflows while reserving " +
               std::string(what),
           offset);
    }
    charge(count * element_bytes, offset, what);
  }

  template <typename T>
  void reserve(std::vector<T>& values, std::size_t count, std::size_t offset,
               std::string_view what) {
    if (count <= values.capacity()) return;
    charge_count(count - values.capacity(), sizeof(T), offset, what);
    values.reserve(count);
  }

 private:
  std::size_t limit_ = 0U;
  std::size_t used_ = 0U;
};

[[nodiscard]] std::uint32_t peek_u32(const apex::core::ByteReader& reader,
                                     std::size_t offset) noexcept {
  const auto bytes = reader.bytes();
  if (offset > bytes.size() || bytes.size() - offset < 4U) return 0U;
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::string read_name(apex::core::ByteReader& reader,
                      NativeAllocationBudget& budget) {
  const auto offset = reader.offset();
  if (reader.remaining() >= 4U) {
    const auto length = peek_u32(reader, offset);
    if (length <= reader.limits().maxStringBytes &&
        static_cast<std::size_t>(length) <= reader.remaining() - 4U) {
      budget.charge(static_cast<std::size_t>(length), offset, "node name");
    }
  }
  return reader.string("node name");
}

KnhNode read_node(apex::core::ByteReader& reader, std::size_t depth,
                  std::size_t max_nodes, std::size_t& node_count,
                  NativeAllocationBudget& budget) {
  if (depth > kMaxDepth) {
    fail("KNH hierarchy is too deep", reader.offset());
  }
  if (node_count >= max_nodes) {
    fail("Too many KNH nodes", reader.offset());
  }
  const auto record_offset = reader.offset();
  KnhNode node;
  node.record_offset = record_offset;
  node.name = read_name(reader, budget);
  for (float& value : node.transform) value = reader.f32("transform");

  const auto child_count_offset = reader.offset();
  const auto child_count = reader.u32("child count");
  if (child_count > reader.remaining() / kMinimumNodeBytes) {
    fail("Unreasonable child count " + std::to_string(child_count) +
             " for " + (node.name.empty() ? "node" : node.name),
         child_count_offset);
  }
  if (static_cast<std::size_t>(child_count) > max_nodes - node_count - 1U) {
    fail("Too many KNH nodes", child_count_offset);
  }
  ++node_count;
  budget.reserve(node.children, child_count, child_count_offset,
                 "KNH child nodes");
  for (std::uint32_t index = 0; index < child_count; ++index) {
    node.children.push_back(read_node(reader, depth + 1, max_nodes,
                                      node_count, budget));
  }
  return node;
}

}  // namespace

KnhError::KnhError(std::string message, std::size_t offset)
    : std::runtime_error([&] {
        std::ostringstream output;
        output << message << " at byte " << offset;
        return output.str();
      }()),
      offset_(offset) {}

KnhFile parse_knh(std::span<const std::byte> bytes, std::string_view source) {
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  return parse_knh(std::span<const std::uint8_t>(data, bytes.size()), source,
                   KnhParseOptions{});
}

KnhFile parse_knh(std::span<const std::uint8_t> bytes, std::string_view source) {
  return parse_knh(bytes, source, KnhParseOptions{});
}

KnhFile parse_knh(std::span<const std::byte> bytes, std::string_view source,
                  KnhParseOptions options) {
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  return parse_knh(std::span<const std::uint8_t>(data, bytes.size()), source,
                   std::move(options));
}

KnhFile parse_knh(std::span<const std::uint8_t> bytes, std::string_view source,
                  KnhParseOptions options) {
  if (bytes.empty()) fail("Empty KNH file", 0);
  if (options.maxNativeObjectBytes == 0U)
    fail("KNH native allocation budget is zero", 0U);
  options.limits.maxStringBytes =
      std::min(options.limits.maxStringBytes, kMaxNameBytes);
  const auto max_nodes =
      std::min(options.maxNodes, default_knh_max_nodes);
  try {
    apex::core::ByteReader reader(bytes, std::string(source), options.limits,
                                  "KNH");
    NativeAllocationBudget budget(options.maxNativeObjectBytes);
    budget.charge(sizeof(KnhNode), 0U, "root node");
    budget.charge(source.size(), 0U, "source name");
    std::size_t node_count = 0;
    KnhNode root = read_node(reader, 0, max_nodes, node_count, budget);
    if (reader.remaining() != 0) fail("Unexpected trailing KNH data", reader.offset());
    return KnhFile{std::string(source), std::move(root), node_count,
                   reader.offset(), bytes.size()};
  } catch (const KnhError&) {
    throw;
  } catch (const apex::core::ParseError& error) {
    throw KnhError(error.what(), error.offset());
  } catch (const std::bad_alloc&) {
    throw KnhError("KNH native allocation failed within the configured budget",
                   0U);
  }
}

std::vector<KnhWalkEntry> walk_knh(const KnhNode& root) {
  std::vector<KnhWalkEntry> result;
  struct Pending {
    const KnhNode* node;
    const KnhNode* parent;
    std::size_t depth;
  };
  std::vector<Pending> pending;
  pending.push_back({&root, nullptr, 0});
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    if (current.depth > kMaxDepth) {
      throw KnhError("KNH hierarchy is too deep", current.node->record_offset);
    }
    if (result.size() >= default_knh_max_nodes) {
      throw KnhError("Too many KNH nodes", current.node->record_offset);
    }
    for (const auto value : current.node->transform) {
      if (!std::isfinite(value)) {
        throw KnhError("Non-finite KNH transform", current.node->record_offset);
      }
    }
    result.push_back({current.node, current.parent, current.depth});
    if (current.node->children.size() >
        default_knh_max_nodes - result.size()) {
      throw KnhError("Too many KNH nodes", current.node->record_offset);
    }
    for (std::size_t index = current.node->children.size(); index != 0; --index) {
      pending.push_back({&current.node->children[index - 1], current.node, current.depth + 1});
    }
  }
  return result;
}

}  // namespace apex::formats
