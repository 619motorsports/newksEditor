#include <apex/formats/knh.hpp>

#include <apex/core/byte_reader.hpp>

#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::size_t kMaxNameBytes = 1U << 20;
constexpr std::size_t kMaxNodes = 1'000'000;
constexpr std::size_t kMaxDepth = 1024;
// A child record always contains at least a name length, 16 floats, and a
// child count. This lets us reject impossible counts before allocating.
constexpr std::size_t kMinimumNodeBytes = 4 + 16 * sizeof(float) + 4;

[[noreturn]] void fail(std::string message, std::size_t offset) {
  throw KnhError(std::move(message), offset);
}

KnhNode read_node(apex::core::ByteReader& reader, std::size_t depth,
                  std::size_t& node_count) {
  if (depth > kMaxDepth) {
    fail("KNH hierarchy is too deep", reader.offset());
  }
  const auto record_offset = reader.offset();
  KnhNode node;
  node.record_offset = record_offset;
  node.name = reader.string("node name");
  for (float& value : node.transform) value = reader.f32("transform");

  const auto child_count_offset = reader.offset();
  const auto child_count = reader.u32("child count");
  if (child_count > kMaxNodes ||
      child_count > reader.remaining() / kMinimumNodeBytes) {
    fail("Unreasonable child count " + std::to_string(child_count) +
             " for " + (node.name.empty() ? "node" : node.name),
         child_count_offset);
  }
  if (node_count == kMaxNodes) {
    fail("Too many KNH nodes", child_count_offset);
  }
  ++node_count;
  node.children.reserve(child_count);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    node.children.push_back(read_node(reader, depth + 1, node_count));
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
  return parse_knh(std::span<const std::uint8_t>(data, bytes.size()), source);
}

KnhFile parse_knh(std::span<const std::uint8_t> bytes, std::string_view source) {
  if (bytes.empty()) fail("Empty KNH file", 0);
  apex::core::ParseLimits limits;
  limits.maxStringBytes = kMaxNameBytes;
  try {
    apex::core::ByteReader reader(bytes, std::string(source), limits, "KNH");
    std::size_t node_count = 0;
    KnhNode root = read_node(reader, 0, node_count);
    if (reader.remaining() != 0) fail("Unexpected trailing KNH data", reader.offset());
    return KnhFile{std::string(source), std::move(root), node_count,
                   reader.offset(), bytes.size()};
  } catch (const KnhError&) {
    throw;
  } catch (const apex::core::ParseError& error) {
    throw KnhError(error.what(), error.offset());
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
    if (result.size() >= kMaxNodes) {
      throw KnhError("Too many KNH nodes", current.node->record_offset);
    }
    for (const auto value : current.node->transform) {
      if (!std::isfinite(value)) {
        throw KnhError("Non-finite KNH transform", current.node->record_offset);
      }
    }
    result.push_back({current.node, current.parent, current.depth});
    if (current.node->children.size() > kMaxNodes - result.size()) {
      throw KnhError("Too many KNH nodes", current.node->record_offset);
    }
    for (std::size_t index = current.node->children.size(); index != 0; --index) {
      pending.push_back({&current.node->children[index - 1], current.node, current.depth + 1});
    }
  }
  return result;
}

}  // namespace apex::formats
