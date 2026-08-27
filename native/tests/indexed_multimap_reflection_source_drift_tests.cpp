#include "apex/core/sha256.hpp"

#include "indexed_multimap_reflection_spirv.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef APEX_NATIVE_SOURCE_DIR
#error "APEX_NATIVE_SOURCE_DIR must identify the native source tree"
#endif

namespace {

using namespace apex::render::test;

void require(const bool condition, const std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "MultiMap reflection shader source is readable");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::uint8_t hex_digit(const char value) {
  if (value >= '0' && value <= '9')
    return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<std::uint8_t>(value - 'a' + 10);
  throw std::runtime_error(
      "embedded MultiMap reflection SPIR-V is hexadecimal");
}

std::vector<std::uint8_t> decode_hex(const std::string_view hex) {
  require(hex.size() % 2U == 0U,
          "embedded MultiMap reflection SPIR-V is byte aligned");
  std::vector<std::uint8_t> bytes(hex.size() / 2U);
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(
        (hex_digit(hex[index * 2U]) << 4U) | hex_digit(hex[index * 2U + 1U]));
  }
  return bytes;
}

void verifies_source_and_spirv_identity() {
  const std::string root = APEX_NATIVE_SOURCE_DIR;
  const auto vertex_source =
      read_file(root + "/tests/shaders/indexed_multimap_reflection.vert");
  const auto source =
      read_file(root + "/tests/shaders/indexed_multimap_reflection.frag");
  const auto vertex_spirv =
      decode_hex(indexed_multimap_reflection_vertex_spirv_hex);
  const auto spirv = decode_hex(indexed_multimap_reflection_fragment_spirv_hex);

  require(apex::core::sha256Hex(vertex_source) ==
              indexed_multimap_reflection_vertex_source_sha256,
          "MultiMap reflection vertex source has not drifted from its embedded "
          "package");
  require(
      apex::core::sha256Hex(vertex_spirv) ==
          indexed_multimap_reflection_vertex_spirv_sha256,
      "embedded MultiMap reflection vertex SPIR-V has its recorded identity");
  require(
      apex::core::sha256Hex(source) ==
          indexed_multimap_reflection_fragment_source_sha256,
      "MultiMap reflection source has not drifted from its embedded package");
  require(apex::core::sha256Hex(spirv) ==
              indexed_multimap_reflection_fragment_spirv_sha256,
          "embedded MultiMap reflection SPIR-V has its recorded identity");
}

} // namespace

int main() {
  try {
    verifies_source_and_spirv_identity();
    std::cout << "indexed MultiMap reflection source drift tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "indexed MultiMap reflection source drift tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
