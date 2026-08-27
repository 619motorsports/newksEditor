#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace {

using namespace apex::render;

void require(const bool condition, const std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::uint8_t hex_digit(const char value) {
  if (value >= '0' && value <= '9')
    return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<std::uint8_t>(value - 'a' + 10);
  throw std::runtime_error("invalid embedded shader hex");
}

std::vector<std::uint8_t> shader_bytes(const std::string_view hex) {
  require(hex.size() % 2U == 0U, "embedded shader hex alignment");
  std::vector<std::uint8_t> bytes(hex.size() / 2U);
  for (std::size_t index = 0U; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::uint8_t>(
        (hex_digit(hex[index * 2U]) << 4U) |
        hex_digit(hex[index * 2U + 1U]));
  return bytes;
}

std::vector<std::uint8_t> reflection_vertex_shader() {
  // Generated from tests/shaders/indexed_multimap_reflection.vert.
  constexpr std::string_view hex =
      "03022307000001000b000800270000000000000011000200010000000b00060001000000474c534c2e7374642e343530000000000e00030000000000010000000f00070000000000040000006d61696e000000000d0000001d000000470003000b00000002000000480005000b000000000000000b00000000000000480005000b000000010000000b00000001000000480005000b000000020000000b00000003000000480005000b000000030000000b0000000400000047000300110000000200000048000400110000000000000005000000480005001100000000000000070000001000000048000500110000000000000023000000000000004800040011000000010000000500000048000500110000000100000007000000100000004800050011000000010000002300000040000000470004001d0000001e00000000000000130002000200000021000300030000000200000016000300060000002000000017000400070000000600000004000000150004000800000020000000000000002b0004000800000009000000010000001c0004000a00000006000000090000001e0006000b00000007000000060000000a0000000a000000200004000c000000030000000b0000003b0004000c0000000d00000003000000150004000e00000020000000010000002b0004000e0000000f00000000000000180004001000000007000000040000001e000400110000001000000010000000200004001200000009000000110000003b0004001200000013000000090000002b0004000e000000140000000100000020000400150000000900000010000000170004001b0000000600000003000000200004001c000000010000001b0000003b0004001c0000001d000000010000002b000400060000001f0000000000803f200004002500000003000000070000003600050002000000040000000000000003000000f80002000500000041000500150000001600000013000000140000003d000400100000001700000016000000410005001500000018000000130000000f0000003d00040010000000190000001800000092000500100000001a00000017000000190000003d0004001b0000001e0000001d0000005100050006000000200000001e000000000000005100050006000000210000001e000000010000005100050006000000220000001e000000020000005000070007000000230000002000000021000000220000001f0000009100050007000000240000001a000000230000004100050025000000260000000d0000000f0000003e0003002600000024000000fd00010038000100";
  return shader_bytes(hex);
}

std::vector<std::uint8_t> reflection_fragment_shader() {
  // Generated from tests/shaders/indexed_multimap_reflection.frag.
  constexpr std::string_view hex =
      "03022307000001000b000800220000000000000011000200010000000b00060001000000474c534c2e7374642e343530000000000e00030000000000010000000f00060004000000040000006d61696e000000000900000010000300040000000700000047000400090000001e00000000000000470004000c0000002100000015000000470004000c000000220000000000000047000400100000002100000016000000470004001000000022000000000000004700030019000000020000004800050019000000000000002300000000000000470004001b0000002100000017000000470004001b0000002200000000000000130002000200000021000300030000000200000016000300060000002000000017000400070000000600000004000000200004000800000003000000070000003b000400080000000900000003000000190009000a00000006000000030000000000000000000000000000000100000000000000200004000b000000000000000a0000003b0004000b0000000c000000000000001a0002000e000000200004000f000000000000000e0000003b0004000f00000010000000000000001b000300120000000a000000170004001400000006000000030000002b00040006000000150000000000803f2b0004000600000016000000000000002c00060014000000170000001500000016000000160000001e0003001900000007000000200004001a00000002000000190000003b0004001a0000001b00000002000000150004001c00000020000000010000002b0004001c0000001d00000000000000200004001e00000002000000070000003600050002000000040000000000000003000000f8000200050000003d0004000a0000000d0000000c0000003d0004000e00000011000000100000005600050012000000130000000d000000110000005700050007000000180000001300000017000000410005001e0000001f0000001b0000001d0000003d00040007000000200000001f00000085000500070000002100000018000000200000003e0003000900000021000000fd00010038000100";
  return shader_bytes(hex);
}

#if defined(_WIN32)
std::vector<std::uint8_t> d3d_shader(const std::string_view source,
                                     const char *target) {
  Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  const HRESULT result = D3DCompile(
      source.data(), source.size(), "indexed_multimap_reflection", nullptr,
      nullptr, "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0U, &bytecode,
      &errors);
  if (FAILED(result)) {
    const std::string message =
        errors == nullptr
            ? "D3DCompile failed"
            : std::string(static_cast<const char *>(errors->GetBufferPointer()),
                          errors->GetBufferSize());
    throw std::runtime_error(message);
  }
  const auto *begin =
      static_cast<const std::uint8_t *>(bytecode->GetBufferPointer());
  return {begin, begin + bytecode->GetBufferSize()};
}
#endif

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

void exercise_reflection_draw(Device &device, const Backend backend,
                              Texture &cube,
                              const std::array<std::byte, 4U> &pixel) {
  TextureDescription sampled;
  sampled.width = 1U;
  sampled.height = 1U;
  sampled.format = TextureFormat::rgba8_unorm;
  sampled.usage = TextureUsage::sampled;
  const TextureUploadPlan sampled_uploads{
      {{0U, 0U, 1U, 1U, 4U, pixel}}};
  TextureResult sampled_result =
      device.create_texture(sampled, sampled_uploads);
  require(sampled_result.ok(), "reflection draw creates material texture");

  SamplerResult sampler = device.create_sampler({});
  require(sampler.ok(), "reflection draw creates sampler");

  std::array<float, 33U> vertices{};
  vertices[0U] = -0.8F;
  vertices[1U] = -0.8F;
  vertices[11U] = 0.8F;
  vertices[12U] = -0.8F;
  vertices[23U] = 0.8F;
  constexpr std::array<std::uint16_t, 3U> indices = {0U, 1U, 2U};
  BufferResult vertex_buffer = device.create_buffer(
      {sizeof(vertices), BufferUsage::vertex, BufferMemory::device_local,
       BufferMutability::immutable},
      std::as_bytes(std::span(vertices)));
  BufferResult index_buffer = device.create_buffer(
      {sizeof(indices), BufferUsage::index, BufferMemory::device_local,
       BufferMutability::immutable},
      std::as_bytes(std::span(indices)));
  require(vertex_buffer.ok() && index_buffer.ok(),
          "reflection draw creates indexed geometry");

  std::array<std::byte, portable_material_buffer_view_bytes> material{};
  std::array<std::byte, portable_frame_buffer_view_bytes> frame{};
  std::array<std::byte, portable_multimap_reflection_buffer_view_bytes>
      reflection{};
  constexpr std::array<float, 4U> reflection_values = {1.0F, 1.0F, 1.0F,
                                                       1.0F};
  std::memcpy(reflection.data(), reflection_values.data(),
              sizeof(reflection_values));
  const auto make_uniform = [&](const std::span<const std::byte> bytes) {
    return device.create_buffer(
        {bytes.size(), BufferUsage::uniform, BufferMemory::host_visible,
         BufferMutability::immutable},
        bytes);
  };
  BufferResult material_buffer = make_uniform(material);
  BufferResult frame_buffer = make_uniform(frame);
  BufferResult reflection_buffer = make_uniform(reflection);
  require(material_buffer.ok() && frame_buffer.ok() && reflection_buffer.ok(),
          "reflection draw creates padded uniform views");

  TextureDescription target_description;
  target_description.width = 8U;
  target_description.height = 8U;
  target_description.format = TextureFormat::rgba8_unorm;
  target_description.usage =
      TextureUsage::color_attachment | TextureUsage::transfer_source;
  target_description.mutability = TextureMutability::mutable_data;
  TextureResult target = device.create_texture(target_description);
  require(target.ok(), "reflection draw creates color target");

  PipelineProgram pipeline;
  pipeline.name = "portable-multimap-reflection-cube";
  pipeline.targets.colors.push_back(
      {PipelineRenderTargetFormat::rgba8_unorm, 1U});
  pipeline.raster.cull = PipelineCullMode::none;
  pipeline.depth.test_enabled = false;
  pipeline.depth.write_enabled = false;
  pipeline.transform_contract = PipelineTransformContract::draw_matrices;
  pipeline.vertex_layout.stride = 11U * sizeof(float);
  pipeline.vertex_layout.attributes.push_back(
      {PipelineVertexSemantic::position,
       PipelineVertexAttributeFormat::float32x3, 0U, 0U});
  if (backend == Backend::Vulkan) {
    pipeline.shaders = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
         reflection_vertex_shader()},
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
         reflection_fragment_shader()},
    };
  } else {
#if defined(_WIN32)
    constexpr std::string_view vertex_source =
        "cbuffer DrawMatrices : register(b0) { column_major float4x4 world; "
        "column_major float4x4 viewProjection; };"
        "float4 main(float3 position : POSITION) : SV_Position { return "
        "mul(viewProjection, mul(world, float4(position, 1.0))); }";
    constexpr std::string_view fragment_source =
        "TextureCube reflectionCube : register(t21);"
        "SamplerState reflectionSampler : register(s22);"
        "cbuffer ReflectionConstants : register(b23) { float4 "
        "fresnelAndAdditive; };"
        "float4 main() : SV_Target { return "
        "reflectionCube.Sample(reflectionSampler, float3(1.0, 0.0, 0.0)) * "
        "fresnelAndAdditive; }";
    pipeline.shaders = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
         d3d_shader(vertex_source, "vs_5_0")},
        {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
         d3d_shader(fragment_source, "ps_5_0")},
    };
#else
    throw std::runtime_error("D3D12 reflection test requires Windows");
#endif
  }
  pipeline.resources = {
      {PipelineResourceKind::sampled_texture, 0U, 0U, "diffuseTexture"},
      {PipelineResourceKind::sampler, 0U, 1U, "diffuseSampler"},
      {PipelineResourceKind::uniform_buffer, 0U, 2U, "material"},
      {PipelineResourceKind::uniform_buffer, 0U, 3U, "frame"},
      {PipelineResourceKind::sampled_texture, 0U, 4U, "normalTexture"},
      {PipelineResourceKind::sampler, 0U, 5U, "normalSampler"},
      {PipelineResourceKind::sampled_texture, 0U, 6U, "mapsTexture"},
      {PipelineResourceKind::sampler, 0U, 7U, "mapsSampler"},
      {PipelineResourceKind::sampled_texture, 0U,
       portable_multimap_cube_texture_binding, "reflectionCube"},
      {PipelineResourceKind::sampler, 0U,
       portable_multimap_cube_sampler_binding, "reflectionSampler"},
      {PipelineResourceKind::uniform_buffer, 0U,
       portable_multimap_reflection_constants_binding,
       "reflectionConstants"},
  };

  DrawPacket packet;
  packet.primitive = DrawPrimitiveKind::static_mesh;
  packet.vertex_count = 3U;
  packet.index_count = 3U;
  packet.vertex_stride_floats = 11U;
  packet.shader_execution_supported = true;
  packet.flags.depth_test = false;
  packet.flags.depth_write = false;
  CameraFrame camera;
  camera.clip_space = backend == Backend::Vulkan
                          ? CameraClipSpace::vulkan
                          : CameraClipSpace::d3d12;
  IndexedStaticMeshDrawRequest request;
  request.packet = &packet;
  request.pipeline = &pipeline;
  request.vertex_buffer = vertex_buffer.buffer.get();
  request.index_buffer = index_buffer.buffer.get();
  request.camera_frame = camera;
  request.resource_authority = IndexedResourceAuthority::explicit_bindings;
  request.sampled_binding = {sampled_result.texture.get(),
                             sampler.sampler.get()};
  request.normal_binding = request.sampled_binding;
  request.maps_binding = request.sampled_binding;
  request.material_binding = {material_buffer.buffer.get(), 0U,
                              portable_material_buffer_view_bytes};
  request.frame_binding = {frame_buffer.buffer.get(), 0U,
                           portable_frame_buffer_view_bytes};
  request.multimap_reflection_binding = {
      {&cube, sampler.sampler.get()},
      {reflection_buffer.buffer.get(), 0U,
       portable_multimap_reflection_buffer_view_bytes}};
  const IndexedStaticMeshDrawResult result =
      device.draw_indexed_static_mesh_and_readback(*target.texture, request);
  if (!result.ok())
    throw std::runtime_error(
        "Reflection draw failed: " + result.diagnostic.code + ": " +
        result.diagnostic.message);
  const std::size_t center = (4U * 8U + 4U) * 4U;
  for (std::size_t channel = 0U; channel < pixel.size(); ++channel)
    require(result.rgba8[center + channel] == pixel[channel],
            "reflection draw samples the positive-X cube face");
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
  pixels[0U] = {std::byte{200U}, std::byte{50U}, std::byte{25U},
                std::byte{255U}};

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
  exercise_reflection_draw(*created.device, backend, *cube_result.texture,
                           pixels[0U]);

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
