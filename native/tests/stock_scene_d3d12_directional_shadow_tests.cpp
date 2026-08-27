#include "apex/render/camera.hpp"
#include "apex/render/directional_shadow.hpp"
#include "apex/render/stock_scene_execution.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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

#if defined(_WIN32)

void require(const bool condition, const std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> compile_shader(const std::string_view source,
                                         const std::string_view profile) {
  using Microsoft::WRL::ComPtr;
  ComPtr<ID3DBlob> bytecode;
  ComPtr<ID3DBlob> errors;
  const HRESULT result =
      D3DCompile(source.data(), source.size(), "stock_scene_directional_shadow",
                 nullptr, nullptr, "main", profile.data(),
                 D3DCOMPILE_ENABLE_STRICTNESS, 0U, &bytecode, &errors);
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

struct Fixture {
  apex::formats::Kn5File model;
  apex::scene::SceneSnapshot scene;
  std::array<PipelineShaderModule, 2U> shaders;
  StockMaterialShaderModules shader_set;
};

Fixture make_fixture() {
  constexpr std::string_view vertex_source = R"(
cbuffer DrawMatrices : register(b0) {
  column_major float4x4 world;
  column_major float4x4 viewProjection;
};
struct Input {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float2 texcoord : TEXCOORD0;
  float3 tangent : TANGENT;
};
struct Output {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float3 worldPosition : TEXCOORD1;
  float2 texcoord : TEXCOORD0;
};
Output main(Input input) {
  Output output;
  const float4 worldPosition = mul(world, float4(input.position, 1.0));
  output.position = mul(viewProjection, worldPosition);
  output.normal = mul((float3x3)world, input.normal);
  output.worldPosition = worldPosition.xyz;
  output.texcoord = input.texcoord;
  return output;
}
)";
  constexpr std::string_view fragment_source = R"(
Texture2D diffuseTexture : register(t0);
SamplerState diffuseSampler : register(s1);
cbuffer Material : register(b2) {
  float4 lighting;
  float4 fresnel;
  float4 emissive;
};
cbuffer Frame : register(b3) {
  float4 sunDirection;
  float4 sunColor;
  float4 ambientColor;
  float4 frameCameraPosition;
  float4 horizonColor;
  float4 skyColor;
  float4 fogColor;
  float4 fog;
};
Texture2D<float> txShadow0 : register(t16);
Texture2D<float> txShadow1 : register(t17);
Texture2D<float> txShadow2 : register(t18);
SamplerState shadowSampler : register(s19);
cbuffer ShadowReceiver : register(b20) {
  column_major float4x4 shadowMatrices[3];
  float4 splitDistances;
  float4 depthBiases;
  float4 cameraPosition;
  float4 cameraForward;
};

float sampleShadow(Texture2D<float> shadowMap, float4x4 shadowMatrix,
                   float bias, float3 worldPosition) {
  const float4 projected = mul(shadowMatrix, float4(worldPosition, 1.0));
  if (projected.w <= 0.0) return 1.0;
  float3 coordinate = projected.xyz / projected.w;
  coordinate.xy = coordinate.xy * 0.5 + 0.5;
  if (coordinate.x <= 0.0 || coordinate.x >= 1.0 ||
      coordinate.y <= 0.0 || coordinate.y >= 1.0 ||
      coordinate.z <= 0.0 || coordinate.z >= 1.0) return 1.0;
  uint width = 0;
  uint height = 0;
  shadowMap.GetDimensions(width, height);
  const float2 texel = 1.0 / float2(width, height);
  float lit = 0.0;
  [unroll]
  for (int y = -1; y <= 1; ++y) {
    [unroll]
    for (int x = -1; x <= 1; ++x) {
      const float storedDepth = shadowMap.SampleLevel(
          shadowSampler, coordinate.xy + float2(x, y) * texel, 0.0);
      lit += coordinate.z - bias <= storedDepth ? 1.0 : 0.0;
    }
  }
  return lit / 9.0;
}

float sunShadow(float3 worldPosition) {
  const float depth = dot(worldPosition - cameraPosition.xyz,
                          cameraForward.xyz);
  if (depth <= splitDistances.x)
    return sampleShadow(txShadow0, shadowMatrices[0], depthBiases.x,
                        worldPosition);
  if (depth <= splitDistances.y)
    return sampleShadow(txShadow1, shadowMatrices[1], depthBiases.y,
                        worldPosition);
  if (depth <= splitDistances.z)
    return sampleShadow(txShadow2, shadowMatrices[2], depthBiases.z,
                        worldPosition);
  return 1.0;
}

float4 main(float4 position : SV_Position, float3 normal : NORMAL,
            float3 worldPosition : TEXCOORD1,
            float2 texcoord : TEXCOORD0) : SV_Target {
  const float3 texel = diffuseTexture.Sample(diffuseSampler, texcoord).rgb;
  const float3 n = normalize(normal);
  const float3 l = normalize(sunDirection.xyz);
  const float3 v = normalize(frameCameraPosition.xyz - worldPosition);
  const float ndl = max(dot(n, l), 0.0);
  const float mappedPower = max(1.0, lighting.w + 1.0);
  const float shadow = sunShadow(worldPosition);
  const float3 specular = sunColor.rgb *
      (pow(max(dot(n, normalize(l + v)), 0.0), mappedPower) *
       lighting.z * shadow);
  const float3 lit = texel *
      (ambientColor.rgb * lighting.x +
       sunColor.rgb * lighting.y * ndl * shadow + emissive.rgb) + specular;
  const float fogAmount = fog.z > 0.5
      ? pow(saturate(distance(frameCameraPosition.xyz, worldPosition) /
                     max(1.0, fog.x)), max(0.05, fog.y))
      : 0.0;
  return float4(max(lerp(lit, fogColor.rgb, fogAmount), 0.0), 1.0);
}
)";

  Fixture result;
  result.shaders = {
      PipelineShaderModule{PipelineShaderStage::vertex,
                           PipelineShaderFormat::dxbc,
                           compile_shader(vertex_source, "vs_5_1")},
      PipelineShaderModule{PipelineShaderStage::fragment,
                           PipelineShaderFormat::dxbc,
                           compile_shader(fragment_source, "ps_5_1")},
  };
  result.shader_set = {
      StockMaterialShaderKeyKind::shader_family,
      "ksPerPixel",
      std::span<const PipelineShaderModule>(result.shaders),
      StockMaterialShaderVariant::standard,
      true,
  };

  result.model.source = "stock-scene-directional-shadow.kn5";
  result.model.root.type = 1U;
  result.model.root.kind = "node";
  result.model.root.name = "ROOT";
  apex::formats::Kn5Node mesh;
  mesh.type = 2U;
  mesh.kind = "mesh";
  mesh.name = "RECEIVER";
  mesh.active = true;
  mesh.renderable = true;
  mesh.visible = true;
  mesh.castShadows = true;
  mesh.materialId = 0U;
  mesh.vertexStride = 11U;
  mesh.vertices = {
      -0.8F, -0.8F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.8F,  -0.8F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F,  0.8F,  0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F,
  };
  mesh.indices = {0U, 1U, 2U};
  result.model.root.children.push_back(std::move(mesh));

  result.model.materials.push_back({});
  result.model.materials.front().name = "body";
  result.model.materials.front().shader = "ksPerPixel";
  result.model.materials.front().depthMode = 2U;
  result.model.materials.front().properties = {
      {"ksAmbient", 0.5F, {}, {}, {}},
      {"ksDiffuse", 0.75F, {}, {}, {}},
      {"ksSpecular", 0.0F, {}, {}, {}},
      {"ksSpecularEXP", 0.0F, {}, {}, {}},
      {"ksEmissive", 0.0F, {}, {0.1F, 0.05F, 0.02F}, {}},
  };
  result.model.materials.front().resources.push_back(
      {"txDiffuse", 0U, "diffuse"});
  result.model.textures.push_back({true, "diffuse", 4U, {}, std::nullopt});

  apex::scene::SceneNode root;
  root.name = "ROOT";
  root.kind = apex::scene::NodeKind::node;
  root.active = true;
  root.visible = true;
  root.renderable = false;
  const apex::scene::NodeId root_id = result.scene.add_node(std::move(root));
  (void)result.scene.add_material(
      {"body", "ksPerPixel", apex::scene::BlendMode::opaque});
  apex::scene::SceneNode scene_mesh;
  scene_mesh.name = "RECEIVER";
  scene_mesh.kind = apex::scene::NodeKind::mesh;
  scene_mesh.active = true;
  scene_mesh.visible = true;
  scene_mesh.renderable = true;
  scene_mesh.cast_shadows = true;
  scene_mesh.material = 0U;
  scene_mesh.bounds_center = {0.0F, 0.0F, 0.0F};
  scene_mesh.bounds_radius = 1.0F;
  (void)result.scene.add_node(std::move(scene_mesh), root_id);
  return result;
}

PipelineProgram
make_caster_pipeline(const PipelineShaderModule &vertex_shader) {
  PipelineProgram pipeline;
  pipeline.name = "portable-stock-directional-shadow-caster";
  pipeline.shaders = {vertex_shader};
  pipeline.vertex_layout.stride = 44U;
  pipeline.vertex_layout.attributes = {
      {PipelineVertexSemantic::position,
       PipelineVertexAttributeFormat::float32x3, 0U, 0U},
      {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3,
       1U, 12U},
      {PipelineVertexSemantic::texcoord0,
       PipelineVertexAttributeFormat::float32x2, 2U, 24U},
      {PipelineVertexSemantic::tangent,
       PipelineVertexAttributeFormat::float32x3, 3U, 32U},
  };
  pipeline.targets.has_depth = true;
  pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
  pipeline.raster.cull = PipelineCullMode::back;
  pipeline.depth.test_enabled = true;
  pipeline.depth.write_enabled = true;
  pipeline.depth.compare = PipelineCompareOperation::less;
  pipeline.transform_contract = PipelineTransformContract::draw_matrices;
  return pipeline;
}

std::unique_ptr<Device> make_warp_device() {
  DeviceOptions options;
  options.headless = true;
  options.enable_validation = true;
  options.allow_software = true;
  options.prefer_software = true;
  const AdapterResult adapters = enumerate_adapters(Backend::D3D12, options);
  DeviceResult device = create_device(Backend::D3D12, options);
  if (!adapters.ok() || !device.ok()) {
    const Diagnostic &diagnostic =
        !device.ok() ? device.diagnostic : adapters.diagnostic;
    std::cout << "SKIP D3D12 WARP: " << diagnostic.code << ": "
              << diagnostic.message << '\n';
    return nullptr;
  }
  require(device.device->info().backend == Backend::D3D12,
          "WARP device uses the D3D12 backend");
  require(device.device->info().software,
          "D3D12 test selected the requested software adapter");
  return std::move(device.device);
}

void clear_shadow_maps(
    Device &device, DirectionalShadowMapResources &maps,
    const std::array<float, directional_shadow_cascade_count> &values) {
  for (std::size_t cascade = 0U; cascade < directional_shadow_cascade_count;
       ++cascade) {
    DepthOnlyIndexedStaticMeshBatchDescription clear;
    clear.depth_attachment = &maps.attachment(cascade);
    clear.clear_depth = true;
    clear.depth_clear_value = values[cascade];
    require(device.draw_depth_only_indexed_static_mesh_batch(clear).ok(),
            "D32 cascade clear succeeds");
  }
}

void require_pixel(const std::vector<std::byte> &pixels,
                   const std::array<std::uint8_t, 4U> &expected,
                   const std::string_view message) {
  require(pixels.size() == 32U * 32U * 4U, "scene readback is bounded RGBA8");
  const std::size_t center = (16U * 32U + 16U) * 4U;
  for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
    const int actual = std::to_integer<unsigned char>(pixels[center + channel]);
    require(std::abs(actual - static_cast<int>(expected[channel])) <= 1,
            message);
  }
}

int run_test() {
  std::unique_ptr<Device> device = make_warp_device();
  if (device == nullptr)
    return 77;
  Fixture fixture = make_fixture();

  StockSceneExecutionRequest request;
  request.model = &fixture.model;
  request.scene = &fixture.scene;
  request.shader_modules =
      std::span<const StockMaterialShaderModules>(&fixture.shader_set, 1U);
  request.targets.colors.push_back(
      {PipelineRenderTargetFormat::rgba8_unorm, 1U});
  request.targets.has_depth = true;
  request.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
  request.directional_shadow_receiver = true;
  const StockSceneExecutionResult prepared =
      prepare_stock_scene_execution(*device, request);
  require(prepared.ok(), prepared.diagnostic.message);
  require(prepared.resources->owns_directional_shadow_receiver(),
          "stock scene retains its directional receiver owner");

  CameraFrameRequest camera_request;
  camera_request.eye = {0.0F, 0.0F, 2.0F};
  camera_request.target = {0.0F, 0.0F, 0.0F};
  camera_request.up = {0.0F, 1.0F, 0.0F};
  camera_request.aspect = 1.0F;
  camera_request.near_plane = 0.1F;
  camera_request.far_plane = 100.0F;
  camera_request.clip_space = CameraClipSpace::d3d12;
  const CameraFrameResult camera = build_camera_frame(camera_request);
  require(camera.ok(), "D3D12 camera frame builds");

  DirectionalShadowMapRequest shadow_request;
  shadow_request.lighting.eye = camera_request.eye;
  shadow_request.lighting.target = camera_request.target;
  shadow_request.lighting.up = camera_request.up;
  shadow_request.lighting.sun_direction = {0.35F, 0.82F, 0.42F};
  shadow_request.lighting.fov_radians = camera_request.fov_radians;
  shadow_request.lighting.aspect = camera_request.aspect;
  shadow_request.lighting.near_plane = camera_request.near_plane;
  shadow_request.lighting.far_plane = camera_request.far_plane;
  shadow_request.lighting.scene_radius = 1.0F;
  shadow_request.lighting.map_size = 32U;
  const DirectionalShadowMapResult shadow_maps =
      prepare_directional_shadow_maps(*device, shadow_request);
  require(shadow_maps.ok(), shadow_maps.diagnostic.message);

  const std::array<std::byte, 4U> diffuse_pixel = {
      std::byte{17}, std::byte{201}, std::byte{83}, std::byte{255}};
  TextureDescription diffuse_description;
  diffuse_description.width = 1U;
  diffuse_description.height = 1U;
  diffuse_description.format = TextureFormat::rgba8_unorm;
  diffuse_description.usage = TextureUsage::sampled;
  TextureUploadPlan diffuse_uploads;
  diffuse_uploads.subresources.push_back({0U, 0U, 1U, 1U, 4U, diffuse_pixel});
  TextureResult diffuse =
      device->create_texture(diffuse_description, diffuse_uploads);
  require(diffuse.ok(), "diffuse texture allocation succeeds");
  SamplerResult sampler = device->create_sampler(SamplerDescription{});
  require(sampler.ok(), "diffuse sampler allocation succeeds");

  TextureDescription target_description;
  target_description.width = 32U;
  target_description.height = 32U;
  target_description.format = TextureFormat::rgba8_unorm;
  target_description.usage =
      TextureUsage::color_attachment | TextureUsage::transfer_source;
  target_description.mutability = TextureMutability::mutable_data;
  TextureResult target = device->create_texture(target_description);
  require(target.ok(), "scene color target allocation succeeds");
  DepthAttachmentResult depth = device->create_depth_attachment(
      {32U, 32U, 1U, DepthAttachmentFormat::d32_float, false});
  require(depth.ok(), "scene depth allocation succeeds");

  const std::array<const Texture *, 1U> textures = {diffuse.texture.get()};
  const std::array<const Sampler *, 1U> samplers = {sampler.sampler.get()};
  StaticSceneFrameDescription frame;
  frame.camera = *camera.frame;
  frame.depth_attachment = depth.attachment.get();
  frame.clear_depth = true;
  KsPerPixelFrameConstants frame_constants;
  frame_constants.sun_direction = {0.0F, 1.0F, 0.0F, 0.0F};
  frame_constants.sun_color = {1.0F, 0.5F, 0.25F, 0.0F};
  frame_constants.ambient_color = {0.2F, 0.3F, 0.4F, 0.0F};
  frame_constants.camera_position = {0.0F, 0.0F, 2.0F, 0.0F};
  frame.frame_constants = frame_constants;
  frame.textures_by_global_index = textures;
  frame.samplers_by_global_index = samplers;
  frame.directional_shadow_maps = shadow_maps.resources.get();

  const std::array<float, directional_shadow_cascade_count> shadowed_depths = {
      0.0F, 0.0F, 0.0F};
  clear_shadow_maps(*device, *shadow_maps.resources, shadowed_depths);
  const IndexedStaticMeshBatchResult shadowed =
      prepared.resources->draw_and_readback(*device, *target.texture, frame);
  require(shadowed.ok(), shadowed.diagnostic.message);
  require_pixel(shadowed.rgba8, {3U, 40U, 18U, 255U},
                "zero-depth cascades attenuate direct stock lighting");
  for (std::size_t cascade = 0U; cascade < directional_shadow_cascade_count;
       ++cascade) {
    const auto readback = device->read_depth_attachment(
        shadow_maps.resources->attachment(cascade), {32U, 32U});
    require(readback.ok() && readback.depth.size() == 32U * 32U &&
                std::all_of(readback.depth.begin(), readback.depth.end(),
                            [](float value) {
                              return std::isfinite(value) &&
                                     std::abs(value) < 0.0001F;
                            }),
            "sampled zero-depth cascade restores its readable state");
  }

  const std::array<float, directional_shadow_cascade_count> lit_depths = {
      1.0F, 1.0F, 1.0F};
  clear_shadow_maps(*device, *shadow_maps.resources, lit_depths);
  const IndexedStaticMeshBatchResult lit =
      prepared.resources->draw_and_readback(*device, *target.texture, frame);
  require(lit.ok(), lit.diagnostic.message);
  require_pixel(lit.rgba8, {16U, 116U, 34U, 255U},
                "one-depth cascades preserve direct stock lighting");

  PipelineProgram caster_pipeline = make_caster_pipeline(fixture.shaders[0U]);
  StaticSceneDirectionalShadowFrameDescription caster_frame;
  caster_frame.maps = shadow_maps.resources.get();
  caster_frame.opaque_pipeline = &caster_pipeline;
  caster_frame.textures_by_global_index = textures;
  caster_frame.samplers_by_global_index = samplers;
  const StaticSceneDirectionalShadowResult caster =
      prepared.resources->draw_opaque_directional_shadows(*device,
                                                          caster_frame);
  require(caster.ok() &&
              caster.cascades_completed == directional_shadow_cascade_count &&
              caster.opaque_casters == 1U,
          caster.diagnostic.message);
  for (std::size_t cascade = 0U; cascade < directional_shadow_cascade_count;
       ++cascade) {
    const auto readback = device->read_depth_attachment(
        shadow_maps.resources->attachment(cascade), {32U, 32U});
    require(readback.ok() && !readback.depth.empty() &&
                std::all_of(readback.depth.begin(), readback.depth.end(),
                            [](float value) { return std::isfinite(value); }) &&
                *std::min_element(readback.depth.begin(),
                                  readback.depth.end()) < 0.99F,
            "opaque caster rewrites each sampled cascade");
  }
  const IndexedStaticMeshBatchResult sampled_after_caster =
      prepared.resources->draw_and_readback(*device, *target.texture, frame);
  require(sampled_after_caster.ok(), sampled_after_caster.diagnostic.message);
  return 0;
}

#endif

} // namespace

int main() {
#if !defined(_WIN32)
  std::cout << "SKIP D3D12 WARP: Windows is required\n";
  return 77;
#else
  try {
    return run_test();
  } catch (const std::exception &error) {
    std::cerr << "stock_scene_d3d12_directional_shadow_tests: " << error.what()
              << '\n';
    return 1;
  }
#endif
}
