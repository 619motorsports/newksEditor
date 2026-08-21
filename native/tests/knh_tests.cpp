#include <apex/formats/knh.hpp>

#include <bit>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::formats::KnhError;

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_f32(std::vector<std::byte>& bytes, float value) {
  append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_transform(std::vector<std::byte>& bytes,
                      std::array<float, 16> transform = {
                          1, 0, 0, 0, 0, 1, 0, 0,
                          0, 0, 1, 0, 0, 0, 0, 1}) {
  for (const auto value : transform) append_f32(bytes, value);
}

void append_node(std::vector<std::byte>& bytes, std::string_view name,
                 std::span<const std::vector<std::byte>> children = {},
                 std::array<float, 16> transform = {
                     1, 0, 0, 0, 0, 1, 0, 0,
                     0, 0, 1, 0, 0, 0, 0, 1}) {
  append_u32(bytes, static_cast<std::uint32_t>(name.size()));
  for (const auto character : name) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  append_transform(bytes, transform);
  append_u32(bytes, static_cast<std::uint32_t>(children.size()));
  for (const auto& child : children) bytes.insert(bytes.end(), child.begin(), child.end());
}

std::vector<std::byte> recursive_fixture() {
  std::vector<std::byte> child;
  append_node(child, "DRIVER:RIG_Center");
  std::vector<std::byte> root;
  append_node(root, "root", std::span<const std::vector<std::byte>>(&child, 1));
  return root;
}

template <typename Function>
void require_knh_error(Function&& function, std::string_view context) {
  try {
    function();
  } catch (const KnhError&) {
    return;
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(context) + ": wrong exception: " + error.what());
  }
  throw std::runtime_error(std::string(context) + ": parser accepted malformed input");
}

void test_recursive_names_and_transforms() {
  const auto bytes = recursive_fixture();
  const auto parsed = apex::formats::parse_knh(bytes);
  require(parsed.node_count == 2, "node count");
  require(parsed.bytes_read == bytes.size(), "bytes read");
  const auto rows = apex::formats::walk_knh(parsed.root);
  require(rows.size() == 2, "walk count");
  require(rows[0].node->name == "root" && rows[0].depth == 0 && rows[0].parent == nullptr,
          "root walk row");
  require(rows[1].node->name == "DRIVER:RIG_Center" && rows[1].depth == 1 &&
              rows[1].parent == rows[0].node,
          "child walk row");
}

void test_every_truncated_prefix() {
  const auto bytes = recursive_fixture();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    require_knh_error(
        [&] { apex::formats::parse_knh(std::span<const std::byte>(bytes.data(), length)); },
        "truncated prefix " + std::to_string(length));
  }
}

void test_malformed_values() {
  auto trailing = recursive_fixture();
  trailing.push_back(std::byte{0});
  require_knh_error([&] { apex::formats::parse_knh(trailing); }, "trailing bytes");

  std::vector<std::byte> huge_name;
  append_u32(huge_name, 1'048'577);
  require_knh_error([&] { apex::formats::parse_knh(huge_name); }, "huge name");

  std::vector<std::byte> non_finite;
  append_u32(non_finite, 0);
  for (unsigned index = 0; index < 16; ++index) {
    append_f32(non_finite, index == 0 ? std::numeric_limits<float>::quiet_NaN() : 0.0f);
  }
  append_u32(non_finite, 0);
  require_knh_error([&] { apex::formats::parse_knh(non_finite); }, "non-finite transform");
}

void test_depth_limit() {
  std::vector<std::byte> bytes;
  constexpr std::size_t depth = 1026;
  for (std::size_t index = 0; index < depth; ++index) {
    append_u32(bytes, 1);
    bytes.push_back(static_cast<std::byte>('n'));
    append_transform(bytes);
    append_u32(bytes, 1);
  }
  append_u32(bytes, 1);
  bytes.push_back(static_cast<std::byte>('n'));
  append_transform(bytes);
  append_u32(bytes, 0);
  require_knh_error([&] { apex::formats::parse_knh(bytes); }, "depth limit");
}

}  // namespace

int main() {
  try {
    test_recursive_names_and_transforms();
    test_every_truncated_prefix();
    test_malformed_values();
    test_depth_limit();
    std::cout << "KNH tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "KNH tests failed: " << error.what() << '\n';
    return 1;
  }
}
