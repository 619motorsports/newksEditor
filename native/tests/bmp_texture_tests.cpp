#include "apex/render/decoded_dds_texture.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

void append_u16(std::vector<std::uint8_t> &output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t> &output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

std::vector<std::uint8_t> fixture() {
  std::vector<std::uint8_t> output;
  output.reserve(58U);
  output.push_back(static_cast<std::uint8_t>('B'));
  output.push_back(static_cast<std::uint8_t>('M'));
  append_u32(output, 58U);
  append_u16(output, 0U);
  append_u16(output, 0U);
  append_u32(output, 54U);
  append_u32(output, 40U);
  append_u32(output, 1U);
  append_u32(output, 1U);
  append_u16(output, 1U);
  append_u16(output, 24U);
  append_u32(output, 0U);
  append_u32(output, 4U);
  append_u32(output, 0U);
  append_u32(output, 0U);
  append_u32(output, 0U);
  append_u32(output, 0U);
  output.insert(output.end(), {1U, 2U, 3U, 0U});
  return output;
}

void dispatches_bmp_to_one_owned_rgba8_level() {
  const auto bytes = fixture();
  const auto result =
      apex::render::plan_decoded_texture_payload(bytes, "fixture.bmp");
  require(result.ok() && result.plan.description.width == 1U &&
              result.plan.description.height == 1U &&
              result.plan.description.format ==
                  apex::render::TextureFormat::rgba8_unorm &&
              result.plan.levels.size() == 1U &&
              result.plan.levels[0].pixels ==
                  std::vector<std::uint8_t>({3U, 2U, 1U, 255U}),
          "BMP texture dispatch and BGR conversion");

  const auto truncated = apex::render::plan_decoded_texture_payload(
      std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1U),
      "truncated.bmp");
  require(!truncated.ok() &&
              truncated.status == apex::render::TextureUploadStatus::invalid &&
              truncated.diagnostic.code == "truncated" &&
              truncated.diagnostic.source == "truncated.bmp",
          "truncated BMP dispatch attribution");

  apex::core::ParseLimits limits;
  limits.maxOutputBytes = 3U;
  const auto limited =
      apex::render::plan_decoded_texture_payload(bytes, "limited.bmp", limits);
  require(!limited.ok() && limited.diagnostic.code == "output_too_large" &&
              limited.diagnostic.source == "limited.bmp",
          "BMP dispatch output budget");
}

} // namespace

int main() {
  try {
    dispatches_bmp_to_one_owned_rgba8_level();
    std::cout << "BMP texture tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "BMP texture tests failed: " << error.what() << '\n';
    return 1;
  }
}
