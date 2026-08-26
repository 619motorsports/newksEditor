#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace apex::render;

void require(const bool condition, const std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

constexpr std::array<CubeFace, texture_cube_face_count> cube_faces = {
    CubeFace::positive_x, CubeFace::negative_x, CubeFace::positive_y,
    CubeFace::negative_y, CubeFace::positive_z, CubeFace::negative_z};

TextureUploadPlan
make_cube_uploads(const std::span<const std::array<std::byte, 4U>> pixels,
                  const std::uint32_t cube_count) {
  require(pixels.size() ==
              static_cast<std::size_t>(cube_count) * texture_cube_face_count,
          "cube test pixels cover every face");
  TextureUploadPlan uploads;
  uploads.subresources.reserve(pixels.size());
  for (std::uint32_t cube = 0U; cube < cube_count; ++cube) {
    for (std::uint32_t face = 0U; face < texture_cube_face_count; ++face) {
      uploads.subresources.push_back(
          {0U, cube, 1U, 1U, 4U, pixels[cube * texture_cube_face_count + face],
           cube_faces[face]});
    }
  }
  return uploads;
}

TextureUploadPlan
make_array_uploads(const std::span<const std::array<std::byte, 4U>> pixels) {
  TextureUploadPlan uploads;
  uploads.subresources.reserve(pixels.size());
  for (std::uint32_t layer = 0U; layer < pixels.size(); ++layer)
    uploads.subresources.push_back({0U, layer, 1U, 1U, 4U, pixels[layer]});
  return uploads;
}

bool exercise_backend(const Backend backend, const DeviceOptions &options) {
  DeviceResult created = create_device(backend, options);
  if (!created.ok()) {
    const std::string_view backend_name =
        backend == Backend::Vulkan ? "Vulkan" : "D3D12";
    std::cout << "SKIP " << backend_name << ": " << created.diagnostic.code
              << ": " << created.diagnostic.message << '\n';
    return false;
  }

  std::array<std::array<std::byte, 4U>, 12U> pixels{};
  for (std::size_t index = 0U; index < pixels.size(); ++index) {
    pixels[index] = {std::byte{static_cast<std::uint8_t>(index + 1U)},
                     std::byte{static_cast<std::uint8_t>(31U + index)},
                     std::byte{static_cast<std::uint8_t>(127U - index)},
                     std::byte{255U}};
  }

  TextureDescription cube;
  cube.width = 1U;
  cube.height = 1U;
  cube.array_layers = 1U;
  cube.format = TextureFormat::rgba8_unorm;
  cube.usage = TextureUsage::sampled;
  cube.mutability = TextureMutability::mutable_data;
  cube.shape = TextureShape::texture_cube;
  TextureUploadPlan one_cube =
      make_cube_uploads(std::span(pixels).first(texture_cube_face_count), 1U);
  TextureResult cube_result = created.device->create_texture(cube, one_cube);
  require(cube_result.ok(), "backend creates and uploads one explicit cube");
  require(cube_result.texture->info().description.shape ==
                  TextureShape::texture_cube &&
              cube_result.texture->info().description.array_layers == 1U,
          "backend preserves logical cube metadata");

  TextureUploadPlan update;
  update.subresources.push_back(
      {0U, 0U, 1U, 1U, 4U, pixels.back(), CubeFace::negative_z});
  require(created.device->update_texture(*cube_result.texture, update).ok(),
          "backend updates one explicit cube face");

  TextureDescription cube_array = cube;
  cube_array.array_layers = 2U;
  cube_array.mutability = TextureMutability::immutable;
  TextureResult cube_array_result =
      created.device->create_texture(cube_array, make_cube_uploads(pixels, 2U));
  require(cube_array_result.ok(),
          "backend creates and uploads an explicit two-cube array");

  TextureDescription ordinary_array = cube;
  ordinary_array.array_layers = texture_cube_face_count;
  ordinary_array.mutability = TextureMutability::immutable;
  ordinary_array.shape = TextureShape::texture_2d;
  TextureResult ordinary_array_result = created.device->create_texture(
      ordinary_array,
      make_array_uploads(std::span(pixels).first(texture_cube_face_count)));
  require(ordinary_array_result.ok(),
          "backend keeps a six-layer 2D array distinct from a cube");
  return true;
}

} // namespace

int main() {
  try {
    DeviceOptions vulkan_options;
    vulkan_options.headless = true;
    vulkan_options.enable_validation = true;
    bool exercised = exercise_backend(Backend::Vulkan, vulkan_options);
#if defined(_WIN32)
    DeviceOptions d3d12_options;
    d3d12_options.headless = true;
    d3d12_options.enable_validation = true;
    d3d12_options.allow_software = true;
    d3d12_options.prefer_software = true;
    exercised = exercise_backend(Backend::D3D12, d3d12_options) || exercised;
#endif
    if (!exercised)
      return 77;
    std::cout << "texture cube backend tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "texture cube backend tests failed: " << error.what() << '\n';
    return 1;
  }
}
