#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/stock_multimap_reflection.hpp"

#include "indexed_multimap_reflection_spirv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
        (hex_digit(hex[index * 2U]) << 4U) | hex_digit(hex[index * 2U + 1U]));
  return bytes;
}

std::vector<std::uint8_t> reflection_vertex_shader() {
  // Generated from tests/shaders/indexed_multimap_reflection.vert.
  constexpr std::string_view hex =
      "03022307000001000b000800270000000000000011000200010000000b00060001000000"
      "474c534c2e7374642e343530000000000e00030000000000010000000f00070000000000"
      "040000006d61696e000000000d0000001d000000470003000b0000000200000048000500"
      "0b000000000000000b00000000000000480005000b000000010000000b00000001000000"
      "480005000b000000020000000b00000003000000480005000b000000030000000b000000"
      "040000004700030011000000020000004800040011000000000000000500000048000500"
      "110000000000000007000000100000004800050011000000000000002300000000000000"
      "480004001100000001000000050000004800050011000000010000000700000010000000"
      "4800050011000000010000002300000040000000470004001d0000001e00000000000000"
      "130002000200000021000300030000000200000016000300060000002000000017000400"
      "070000000600000004000000150004000800000020000000000000002b00040008000000"
      "09000000010000001c0004000a00000006000000090000001e0006000b00000007000000"
      "060000000a0000000a000000200004000c000000030000000b0000003b0004000c000000"
      "0d00000003000000150004000e00000020000000010000002b0004000e0000000f000000"
      "00000000180004001000000007000000040000001e000400110000001000000010000000"
      "200004001200000009000000110000003b0004001200000013000000090000002b000400"
      "0e000000140000000100000020000400150000000900000010000000170004001b000000"
      "0600000003000000200004001c000000010000001b0000003b0004001c0000001d000000"
      "010000002b000400060000001f0000000000803f20000400250000000300000007000000"
      "3600050002000000040000000000000003000000f8000200050000004100050015000000"
      "1600000013000000140000003d0004001000000017000000160000004100050015000000"
      "18000000130000000f0000003d0004001000000019000000180000009200050010000000"
      "1a00000017000000190000003d0004001b0000001e0000001d0000005100050006000000"
      "200000001e000000000000005100050006000000210000001e0000000100000051000500"
      "06000000220000001e000000020000005000070007000000230000002000000021000000"
      "220000001f0000009100050007000000240000001a000000230000004100050025000000"
      "260000000d0000000f0000003e0003002600000024000000fd00010038000100";
  static_assert(
      hex == apex::render::test::indexed_multimap_reflection_vertex_spirv_hex);
  return shader_bytes(hex);
}

std::vector<std::uint8_t> reflection_fragment_shader() {
  // Generated from tests/shaders/indexed_multimap_reflection.frag.
  constexpr std::string_view hex =
      "03022307000001000b000800960000000000000011000200010000000b00060001000000"
      "474c534c2e7374642e343530000000000e00030000000000010000000f00060004000000"
      "040000006d61696e000000006d000000100003000400000007000000470003000a000000"
      "02000000480005000a000000000000002300000000000000470004000c00000021000000"
      "17000000470004000c0000002200000000000000470004003b0000002100000015000000"
      "470004003b0000002200000000000000470004003f000000210000001600000047000400"
      "3f0000002200000000000000470004006d0000001e000000000000001300020002000000"
      "210003000300000002000000160003000600000020000000170004000900000006000000"
      "040000001e0003000a00000009000000200004000b000000020000000a0000003b000400"
      "0b0000000c00000002000000150004000d00000020000000010000002b0004000d000000"
      "0e00000000000000150004000f00000020000000000000002b0004000f00000010000000"
      "03000000200004001100000002000000060000002b000400060000001600000000000040"
      "14000200170000002b000400060000001d0000000000c0402b000400060000001e000000"
      "0000803f2b00040006000000230000000000000017000400260000000600000003000000"
      "2b0004000f0000002c000000000000002b0004000f00000033000000020000002b000400"
      "0600000038000000cdcc4c3f190009003900000006000000030000000000000000000000"
      "000000000100000000000000200004003a00000000000000390000003b0004003a000000"
      "3b000000000000001a0002003d000000200004003e000000000000003d0000003b000400"
      "3e0000003f000000000000001b0003004100000039000000200004006c00000003000000"
      "090000003b0004006c0000006d000000030000002b0004000600000074000000cdcc4c3e"
      "2c00060026000000750000007400000074000000740000002b000400060000008e000000"
      "0000003f2b000400060000009100000000006fc12c00060026000000920000001e000000"
      "23000000230000003600050002000000040000000000000003000000f800020005000000"
      "4100060011000000120000000c0000000e000000100000003d0004000600000013000000"
      "12000000b400050017000000180000001300000016000000a90006000600000022000000"
      "18000000910000008e0000000c0008000600000024000000010000002b00000022000000"
      "230000001e0000008500050006000000250000001d000000240000003d00040039000000"
      "3c0000003b0000003d0004003d000000400000003f000000560005004100000042000000"
      "3c0000004000000058000700090000004500000042000000920000000200000025000000"
      "4f000800260000004600000045000000450000000000000001000000020000008e000500"
      "26000000470000004600000038000000f900020051000000f80002005100000041000600"
      "11000000650000000c0000000e0000002c0000003d000400060000006600000065000000"
      "4100060011000000690000000c0000000e000000330000003d000400060000006a000000"
      "690000000c000700060000006b0000000100000025000000660000006a000000b4000500"
      "170000006f000000130000001e000000f70003007300000000000000fa0004006f000000"
      "720000007e000000f80002007e0000005000060026000000810000006b0000006b000000"
      "6b0000000c0008002600000082000000010000002e000000750000004700000081000000"
      "510005000600000083000000820000000000000051000500060000008400000082000000"
      "010000005100050006000000850000008200000002000000500007000900000086000000"
      "8300000084000000850000001e000000f900020073000000f8000200720000008e000500"
      "2600000078000000470000006b0000008100050026000000790000007500000078000000"
      "51000500060000007a000000790000000000000051000500060000007b00000079000000"
      "0100000051000500060000007c000000790000000200000050000700090000007d000000"
      "7a0000007b0000007c00000016000000f900020073000000f800020073000000f5000700"
      "0900000095000000860000007e0000007d000000720000003e0003006d00000095000000"
      "fd00010038000100";
  static_assert(
      hex ==
      apex::render::test::indexed_multimap_reflection_fragment_spirv_hex);
  return shader_bytes(hex);
}

#if defined(_WIN32)
std::vector<std::uint8_t> d3d_shader(const std::string_view source,
                                     const char *target) {
  Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  const HRESULT result =
      D3DCompile(source.data(), source.size(), "indexed_multimap_reflection",
                 nullptr, nullptr, "main", target, D3DCOMPILE_ENABLE_STRICTNESS,
                 0U, &bytecode, &errors);
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
  const TextureUploadPlan sampled_uploads{{{0U, 0U, 1U, 1U, 4U, pixel}}};
  TextureResult sampled_result =
      device.create_texture(sampled, sampled_uploads);
  require(sampled_result.ok(), "reflection draw creates material texture");

  SamplerResult sampler = device.create_sampler({});
  require(sampler.ok(), "reflection draw creates sampler");

  std::array<float, 33U> vertices{};
  vertices[0U] = -1.0F;
  vertices[1U] = -1.0F;
  vertices[11U] = 3.0F;
  vertices[12U] = -1.0F;
  vertices[22U] = -1.0F;
  vertices[23U] = 3.0F;
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
  const auto make_uniform = [&](const std::span<const std::byte> bytes) {
    return device.create_buffer({bytes.size(), BufferUsage::uniform,
                                 BufferMemory::host_visible,
                                 BufferMutability::immutable},
                                bytes);
  };
  BufferResult material_buffer = make_uniform(material);
  BufferResult frame_buffer = make_uniform(frame);
  std::array<std::byte, portable_multimap_reflection_buffer_view_bytes>
      reflection{};
  BufferResult reflection_buffer = device.create_buffer(
      {reflection.size(), BufferUsage::uniform, BufferMemory::host_visible,
       BufferMutability::mutable_data},
      reflection);
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
        "float4 main() : SV_Target {"
        "float3 normal = float3(-1.0, 0.0, 0.0);"
        "float3 view = float3(1.0, 0.0, 0.0);"
        "float branch = fresnelAndAdditive.w;"
        "float divisor = branch == 2.0 ? 8.0 : 255.0;"
        "float mip = 6.0 * saturate(1.0 - 0.5 * 255.0 / divisor);"
        "float3 reflected = normalize(view - 2.0 * dot(view, normal) * normal);"
        "float3 cubeDirection = float3(-reflected.x, reflected.y, reflected.z);"
        "float3 reflectedColor = 0.8 * reflectionCube.SampleLevel("
        "reflectionSampler, cubeDirection, mip).rgb;"
        "float grazing = saturate(1.0 + dot(normal, view));"
        "float exponent = branch == 1.0 || branch == 2.0 "
        "? fresnelAndAdditive.y : max(fresnelAndAdditive.y, 1.0);"
        "float anglePower = grazing > 0.0 ? pow(grazing, exponent) : 0.0;"
        "float fresnel = min(fresnelAndAdditive.x + anglePower, "
        "fresnelAndAdditive.z);"
        "float3 lit = float3(0.2, 0.2, 0.2);"
        "return branch == 1.0 "
        "? float4(lit + fresnel * reflectedColor, 2.0) "
        ": float4(lerp(lit, reflectedColor, fresnel), 1.0); }";
    pipeline.shaders = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
         d3d_shader(vertex_source, "vs_5_0")},
        {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
         d3d_shader(fragment_source, "ps_5_1")},
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
       portable_multimap_reflection_constants_binding, "reflectionConstants"},
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
  camera.clip_space = backend == Backend::Vulkan ? CameraClipSpace::vulkan
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

  constexpr std::array<float, 3U> branches = {1.0F, 2.0F, 3.0F};
  for (const float branch : branches) {
    const std::array<float, 4U> reflection_values = {0.1F, 5.0F, 0.5F, branch};
    std::fill(reflection.begin(), reflection.end(), std::byte{});
    std::memcpy(reflection.data(), reflection_values.data(),
                sizeof(reflection_values));
    require(
        device.update_buffer(*reflection_buffer.buffer, 0U, reflection).ok(),
        "reflection draw updates its semantic controls");

    const IndexedStaticMeshDrawResult result =
        device.draw_indexed_static_mesh_and_readback(*target.texture, request);
    if (!result.ok())
      throw std::runtime_error(
          "Reflection draw failed: " + result.diagnostic.code + ": " +
          result.diagnostic.message);

    StockMultiMapReflectionInput expected_input;
    expected_input.controls.fresnel_and_additive = reflection_values;
    expected_input.surface_normal = {-1.0F, 0.0F, 0.0F};
    expected_input.camera_to_surface = {1.0F, 0.0F, 0.0F};
    expected_input.maps_specular_exponent = 0.5F;
    expected_input.ks_specular_exponent = 255.0F;
    expected_input.maps_reflection = 0.8F;
    expected_input.lit_rgb = {0.2F, 0.2F, 0.2F};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
      expected_input.cube_rgb[channel] =
          static_cast<float>(std::to_integer<std::uint8_t>(pixel[channel])) /
          255.0F;
    const StockMultiMapReflectionResult expected =
        evaluate_stock_multimap_reflection(expected_input);
    require(expected.ready && expected.cube_direction[0U] == 1.0F,
            "reflection oracle resolves the positive-X cube face");

    std::array<float, 4U> expected_rgba = {expected.rgb[0U], expected.rgb[1U],
                                           expected.rgb[2U], expected.alpha};
    const std::size_t center = (4U * 8U + 4U) * 4U;
    for (std::size_t channel = 0U; channel < expected_rgba.size(); ++channel) {
      const long rounded =
          std::lround(std::clamp(expected_rgba[channel], 0.0F, 1.0F) * 255.0F);
      const int actual =
          std::to_integer<std::uint8_t>(result.rgba8[center + channel]);
      require(std::abs(actual - static_cast<int>(rounded)) <= 1,
              "reflection draw matches recovered branch arithmetic");
    }
  }

  TextureDescription capture_description = target_description;
  capture_description.format = TextureFormat::rgba16_sfloat;
  capture_description.mip_levels = 4U;
  capture_description.usage =
      TextureUsage::sampled | TextureUsage::color_attachment |
      TextureUsage::transfer_source;
  capture_description.shape = TextureShape::texture_cube;
  capture_description.access_policy = TextureAccessPolicy::render_then_sample;
  TextureResult capture_target = device.create_texture(capture_description);
  require(capture_target.ok(),
          "reflection draw creates an RGBA16F cube capture target");
  const TextureUpdateResult empty_generation =
      device.generate_texture_mips(*capture_target.texture);
  require(!empty_generation.ok() &&
              empty_generation.status == TextureStatus::invalid_description &&
              empty_generation.diagnostic.code ==
                  "texture_mip_generation_base_uninitialized",
          "cube mip generation rejects an uninitialized base level");

  PipelineProgram capture_pipeline = pipeline;
  capture_pipeline.targets.colors.front().format =
      PipelineRenderTargetFormat::rgba16_float;
  IndexedStaticMeshDrawRequest capture_request = request;
  capture_request.pipeline = &capture_pipeline;
  const std::array<IndexedStaticMeshDrawRequest, 1U> capture_draws = {
      capture_request};
  IndexedStaticMeshBatchDescription capture_batch;
  capture_batch.draws = capture_draws;
  capture_batch.capture_rgba8 = false;
  capture_batch.target_subresource.cube_face = CubeFace::negative_z;

  const std::array<float, 4U> capture_values = {0.1F, 5.0F, 0.5F, 1.0F};
  std::fill(reflection.begin(), reflection.end(), std::byte{});
  std::memcpy(reflection.data(), capture_values.data(), sizeof(capture_values));
  require(device.update_buffer(*reflection_buffer.buffer, 0U, reflection).ok(),
          "cube-face draw updates its semantic controls");

  const IndexedStaticMeshBatchResult captured =
      device.draw_indexed_static_mesh_batch_and_readback(
          *capture_target.texture, capture_batch);
  if (!captured.ok())
    throw std::runtime_error(
        "Cube-face reflection draw failed: " + captured.diagnostic.code + ": " +
        captured.diagnostic.message);
  const TextureUpdateResult partial_generation =
      device.generate_texture_mips(*capture_target.texture);
  require(!partial_generation.ok() &&
              partial_generation.diagnostic.code ==
                  "texture_mip_generation_base_uninitialized",
          "cube mip generation requires every base face before execution");

  StockMultiMapReflectionInput capture_expected_input;
  capture_expected_input.controls.fresnel_and_additive = capture_values;
  capture_expected_input.surface_normal = {-1.0F, 0.0F, 0.0F};
  capture_expected_input.camera_to_surface = {1.0F, 0.0F, 0.0F};
  capture_expected_input.maps_specular_exponent = 0.5F;
  capture_expected_input.ks_specular_exponent = 255.0F;
  capture_expected_input.maps_reflection = 0.8F;
  capture_expected_input.lit_rgb = {0.2F, 0.2F, 0.2F};
  for (std::size_t channel = 0U; channel < 3U; ++channel)
    capture_expected_input.cube_rgb[channel] =
        static_cast<float>(std::to_integer<std::uint8_t>(pixel[channel])) /
        255.0F;
  const StockMultiMapReflectionResult capture_expected =
      evaluate_stock_multimap_reflection(capture_expected_input);
  require(capture_expected.ready,
          "cube-face draw resolves its recovered expected value");

  const std::array<float, 4U> expected_capture_rgba = {
      capture_expected.rgb[0U], capture_expected.rgb[1U],
      capture_expected.rgb[2U], capture_expected.alpha};
  const std::size_t capture_center = (4U * 8U + 4U) * 4U;
  require(captured.rgba8.empty(),
          "RGBA16F cube capture stays GPU-resident without RGBA8 readback");

  capture_batch.target_subresource.cube_face = CubeFace::positive_x;
  const IndexedStaticMeshBatchResult captured_positive_x =
      device.draw_indexed_static_mesh_batch_and_readback(
          *capture_target.texture, capture_batch);
  if (!captured_positive_x.ok())
    throw std::runtime_error(
        "Positive-X cube-face draw failed: " +
        captured_positive_x.diagnostic.code + ": " +
        captured_positive_x.diagnostic.message);

  constexpr std::array<CubeFace, 4U> remaining_faces = {
      CubeFace::negative_x, CubeFace::positive_y, CubeFace::negative_y,
      CubeFace::positive_z};
  for (const CubeFace face : remaining_faces) {
    capture_batch.target_subresource.cube_face = face;
    const IndexedStaticMeshBatchResult captured_face =
        device.draw_indexed_static_mesh_batch_and_readback(
            *capture_target.texture, capture_batch);
    if (!captured_face.ok())
      throw std::runtime_error(
          "Remaining RGBA16F cube-face draw failed: " +
          captured_face.diagnostic.code + ": " +
          captured_face.diagnostic.message);
  }
  const TextureUpdateResult generated =
      device.generate_texture_mips(*capture_target.texture);
  if (!generated.ok())
    throw std::runtime_error(
        "RGBA16F cube mip generation failed: " +
        generated.diagnostic.code + ": " + generated.diagnostic.message);

  request.multimap_reflection_binding.cube.texture = capture_target.texture.get();
  const IndexedStaticMeshDrawResult sampled_capture =
      device.draw_indexed_static_mesh_and_readback(*target.texture, request);
  if (!sampled_capture.ok())
    throw std::runtime_error(
        "Render-then-sample reflection draw failed: " +
        sampled_capture.diagnostic.code + ": " +
        sampled_capture.diagnostic.message);

  StockMultiMapReflectionInput sampled_capture_input = capture_expected_input;
  for (std::size_t channel = 0U; channel < 3U; ++channel)
    sampled_capture_input.cube_rgb[channel] = expected_capture_rgba[channel];
  const StockMultiMapReflectionResult sampled_capture_expected =
      evaluate_stock_multimap_reflection(sampled_capture_input);
  require(sampled_capture_expected.ready,
          "render-then-sample resolves its recovered expected value");
  const std::array<float, 4U> expected_sampled_capture_rgba = {
      sampled_capture_expected.rgb[0U], sampled_capture_expected.rgb[1U],
      sampled_capture_expected.rgb[2U], sampled_capture_expected.alpha};
  for (std::size_t channel = 0U;
       channel < expected_sampled_capture_rgba.size(); ++channel) {
    const long rounded =
        std::lround(std::clamp(expected_sampled_capture_rgba[channel], 0.0F,
                              1.0F) *
                    255.0F);
    const int actual = std::to_integer<std::uint8_t>(
        sampled_capture.rgba8[capture_center + channel]);
    require(std::abs(actual - static_cast<int>(rounded)) <= 2,
            "a rendered cube face is sampled after its final transition");
  }
}

#if defined(_WIN32)
void exercise_d3d12_warp_mip_generation(Device &device) {
  require(device.info().backend == Backend::D3D12,
          "D3D12 mip test receives a D3D12 device");
  require(device.info().software,
          "D3D12 mip test selects the software WARP adapter");

  constexpr std::uint32_t width = 8U;
  constexpr std::uint32_t height = 8U;
  constexpr std::size_t texel_count = width * height;
  using HalfRgba = std::array<std::uint16_t, texel_count * 4U>;
  // These values are exactly representable in binary16.  A checkerboard and
  // two green stripes make a copy-or-point implementation observably differ
  // from the required filtered mip result.
  constexpr std::uint16_t half_zero = 0x0000U;
  constexpr std::uint16_t half_one = 0x3c00U;
  constexpr std::uint16_t half_quarter = 0x3400U;
  constexpr std::uint16_t half_three_quarters = 0x3a00U;
  constexpr std::uint16_t half_eighth = 0x3000U;
  std::array<HalfRgba, texture_cube_face_count> faces{};
  for (std::uint32_t face = 0U; face < texture_cube_face_count; ++face) {
    for (std::uint32_t y = 0U; y < height; ++y) {
      for (std::uint32_t x = 0U; x < width; ++x) {
        const std::size_t texel =
            (static_cast<std::size_t>(y) * width + x) * 4U;
        if (face == 0U) {
          faces[face][texel + 0U] = ((x + y) & 1U) != 0U ? half_one : half_zero;
          faces[face][texel + 1U] = x < width / 2U ? half_quarter
                                                  : half_three_quarters;
          faces[face][texel + 2U] = half_eighth;
        } else {
          faces[face][texel + 0U] = half_zero;
          faces[face][texel + 1U] = half_zero;
          faces[face][texel + 2U] = half_zero;
        }
        faces[face][texel + 3U] = half_one;
      }
    }
  }
  const auto face_bytes = [](const HalfRgba &face) {
    return std::as_bytes(std::span(face));
  };

  TextureDescription description;
  description.width = width;
  description.height = height;
  description.mip_levels = 4U;
  description.array_layers = 1U;
  description.format = TextureFormat::rgba16_sfloat;
  description.usage = TextureUsage::sampled | TextureUsage::color_attachment |
                       TextureUsage::transfer_source;
  description.mutability = TextureMutability::mutable_data;
  description.shape = TextureShape::texture_cube;
  description.access_policy = TextureAccessPolicy::render_then_sample;

  // Create with one face only.  The backend must retain per-physical-layer
  // initialization state and reject generation until the other five arrive.
  TextureUploadPlan partial;
  partial.subresources.push_back(
      {0U, 0U, width, height, width * 4U * sizeof(std::uint16_t),
       face_bytes(faces[0U]), CubeFace::positive_x});
  TextureResult cube = device.create_texture(description, partial);
  require(cube.ok(), "D3D12 WARP creates a partially initialized RGBA16F cube");
  const TextureUpdateResult partial_generation =
      device.generate_texture_mips(*cube.texture);
  require(!partial_generation.ok() &&
              partial_generation.diagnostic.code ==
                  "texture_mip_generation_base_uninitialized",
          "D3D12 WARP rejects mip generation for a partial cube base");

  TextureUploadPlan remaining;
  remaining.subresources.reserve(texture_cube_face_count - 1U);
  for (std::uint32_t face = 1U; face < texture_cube_face_count; ++face) {
    remaining.subresources.push_back(
        {0U, 0U, width, height, width * 4U * sizeof(std::uint16_t),
         face_bytes(faces[face]), cube_faces[face]});
  }
  const TextureUpdateResult uploaded =
      device.update_texture(*cube.texture, remaining);
  require(uploaded.ok(), "D3D12 WARP uploads the remaining cube faces");
  const TextureUpdateResult generated =
      device.generate_texture_mips(*cube.texture);
  if (!generated.ok())
    throw std::runtime_error(
        "D3D12 WARP RGBA16F cube mip generation failed: " +
        generated.diagnostic.code + ": " + generated.diagnostic.message);

  // Bind the generated cube to a real indexed draw.  Sampling mip three
  // reaches the one-by-one level, whose expected result is the arithmetic
  // average of the nonconstant positive-X base face: (0.5, 0.5, 0.125, 1).
  TextureDescription sampled_description;
  sampled_description.width = 1U;
  sampled_description.height = 1U;
  sampled_description.format = TextureFormat::rgba8_unorm;
  sampled_description.usage = TextureUsage::sampled;
  const std::array<std::byte, 4U> sampled_pixel = {
      std::byte{255U}, std::byte{255U}, std::byte{255U}, std::byte{255U}};
  const TextureUploadPlan sampled_uploads{{
      {0U, 0U, 1U, 1U, 4U, sampled_pixel}}};
  TextureResult sampled =
      device.create_texture(sampled_description, sampled_uploads);
  require(sampled.ok(), "D3D12 WARP creates the mip-test material texture");
  SamplerResult sampler = device.create_sampler({});
  require(sampler.ok(), "D3D12 WARP creates the mip-test sampler");

  std::array<float, 33U> vertices{};
  vertices[0U] = -1.0F;
  vertices[1U] = -1.0F;
  vertices[11U] = 3.0F;
  vertices[12U] = -1.0F;
  vertices[22U] = -1.0F;
  vertices[23U] = 3.0F;
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
          "D3D12 WARP creates mip-test indexed geometry");

  std::array<std::byte, portable_material_buffer_view_bytes> material{};
  std::array<std::byte, portable_frame_buffer_view_bytes> frame{};
  const auto make_uniform = [&](const std::span<const std::byte> bytes) {
    return device.create_buffer({bytes.size(), BufferUsage::uniform,
                                 BufferMemory::host_visible,
                                 BufferMutability::immutable},
                                bytes);
  };
  BufferResult material_buffer = make_uniform(material);
  BufferResult frame_buffer = make_uniform(frame);
  std::array<std::byte, portable_multimap_reflection_buffer_view_bytes>
      reflection{};
  BufferResult reflection_buffer = device.create_buffer(
      {reflection.size(), BufferUsage::uniform, BufferMemory::host_visible,
       BufferMutability::mutable_data},
      reflection);
  require(material_buffer.ok() && frame_buffer.ok() && reflection_buffer.ok(),
          "D3D12 WARP creates mip-test padded uniform views");

  PipelineProgram pipeline;
  pipeline.name = "d3d12-warp-mip-filter-readback";
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
  constexpr std::string_view vertex_source =
      "cbuffer DrawMatrices : register(b0) { column_major float4x4 world; "
      "column_major float4x4 viewProjection; };"
      "float4 main(float3 position : POSITION) : SV_Position { return "
      "mul(viewProjection, mul(world, float4(position, 1.0))); }";
  constexpr std::string_view fragment_source =
      "TextureCube reflectionCube : register(t21);"
      "SamplerState reflectionSampler : register(s22);"
      "float4 main() : SV_Target { return reflectionCube.SampleLevel("
      "reflectionSampler, float3(1.0, 0.0, 0.0), 3.0); }";
  pipeline.shaders = {
      {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
       d3d_shader(vertex_source, "vs_5_0")},
      {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
       d3d_shader(fragment_source, "ps_5_1")},
  };
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
       portable_multimap_reflection_constants_binding, "reflectionConstants"},
  };

  TextureDescription target_description;
  target_description.width = width;
  target_description.height = height;
  target_description.format = TextureFormat::rgba8_unorm;
  target_description.usage =
      TextureUsage::color_attachment | TextureUsage::transfer_source;
  target_description.mutability = TextureMutability::mutable_data;
  TextureResult target = device.create_texture(target_description);
  require(target.ok(), "D3D12 WARP creates the mip-test readback target");

  DrawPacket packet;
  packet.primitive = DrawPrimitiveKind::static_mesh;
  packet.vertex_count = 3U;
  packet.index_count = 3U;
  packet.vertex_stride_floats = 11U;
  packet.shader_execution_supported = true;
  packet.flags.depth_test = false;
  packet.flags.depth_write = false;
  CameraFrame camera;
  camera.clip_space = CameraClipSpace::d3d12;
  IndexedStaticMeshDrawRequest request;
  request.packet = &packet;
  request.pipeline = &pipeline;
  request.vertex_buffer = vertex_buffer.buffer.get();
  request.index_buffer = index_buffer.buffer.get();
  request.camera_frame = camera;
  request.resource_authority = IndexedResourceAuthority::explicit_bindings;
  request.sampled_binding = {sampled.texture.get(), sampler.sampler.get()};
  request.normal_binding = request.sampled_binding;
  request.maps_binding = request.sampled_binding;
  request.material_binding = {material_buffer.buffer.get(), 0U,
                              portable_material_buffer_view_bytes};
  request.frame_binding = {frame_buffer.buffer.get(), 0U,
                           portable_frame_buffer_view_bytes};
  request.multimap_reflection_binding = {
      {cube.texture.get(), sampler.sampler.get()},
      {reflection_buffer.buffer.get(), 0U,
       portable_multimap_reflection_buffer_view_bytes}};
  const IndexedStaticMeshDrawResult readback =
      device.draw_indexed_static_mesh_and_readback(*target.texture, request);
  if (!readback.ok())
    throw std::runtime_error(
        "D3D12 WARP mip sample failed: " + readback.diagnostic.code +
        ": " + readback.diagnostic.message);
  const std::size_t center = (4U * width + 4U) * 4U;
  require(readback.rgba8.size() >= center + 4U,
          "D3D12 WARP mip sample returned a complete readback");
  const std::array<int, 4U> expected = {128, 128, 32, 255};
  for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
    const int actual = std::to_integer<std::uint8_t>(
        readback.rgba8[center + channel]);
    require(std::abs(actual - expected[channel]) <= 3,
            "D3D12 WARP mip sample proves filtered RGBA16F output");
  }
}
#endif

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
#if defined(_WIN32)
  if (backend == Backend::D3D12)
    exercise_d3d12_warp_mip_generation(*created.device);
#endif
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
