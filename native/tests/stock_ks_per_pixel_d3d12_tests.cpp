#include "apex/render/device.hpp"
#include "apex/render/material_profile.hpp"
#include "apex/render/static_mesh_upload.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace apex::render;

constexpr std::size_t max_package_bytes = 16U * 1024U * 1024U;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::filesystem::path shader_path(const std::filesystem::path &root,
                                  std::string_view file_name) {
  return root / "sdk" / "editor" / "system" / "shaders" /
         std::filesystem::path(std::string(file_name));
}

std::vector<std::uint8_t> read_bounded(const std::filesystem::path &path) {
  std::error_code error;
  require(std::filesystem::is_regular_file(path, error),
          "installed shader package is not a regular file");
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  require(!error && size <= max_package_bytes,
          "installed shader package exceeds the bounded test input size");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream input(path.string(), std::ios::binary);
  require(static_cast<bool>(input), "installed shader package opens");
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    require(input.gcount() == static_cast<std::streamsize>(bytes.size()),
            "installed shader package read is complete");
  }
  return bytes;
}

StockKsPerPixelNativeConstantData native_constants(const CameraFrame &camera) {
  StockKsPerPixelNativeConstantData constants;
  constants.camera = make_stock_ks_per_pixel_camera_constants(
      camera.view, camera.projection, apex::scene::identity_matrix,
      {camera.position[0], camera.position[1], camera.position[2]},
      camera.near_plane, camera.far_plane, camera.fov_radians);
  constants.object =
      make_stock_ks_per_pixel_object_constants(apex::scene::identity_matrix);
  constants.lighting.light_direction = {0.0F, -1.0F, 0.0F};
  constants.lighting.ambient_color = {0.2F, 0.3F, 0.4F, 1.0F};
  constants.lighting.light_color = {1.0F, 1.0F, 1.0F};
  constants.lighting.horizon_color[3U] = 1.0F;
  constants.lighting.zenith_color[3U] = 1.0F;
  constants.shadow_maps = make_stock_directional_shadow_receiver_constants(
      {apex::scene::identity_matrix, apex::scene::identity_matrix,
       apex::scene::identity_matrix},
      {0.000002F, 0.000015F, 0.0003F}, 32U);
  constants.material.ambient = 0.35F;
  constants.material.diffuse = 0.80F;
  constants.material.specular = 0.20F;
  constants.material.specular_exponent = 30.0F;
  constants.material.emissive = {0.1F, 0.2F, 0.3F};
  constants.material.alpha_reference = 0.0F;
  return constants;
}

PipelineProgram native_pipeline() {
  PipelineProgram pipeline;
  pipeline.name = "installed-ksPerPixel-native-warp";
  pipeline.targets.colors.push_back(
      {PipelineRenderTargetFormat::rgba8_unorm, 1U});
  pipeline.raster.cull = PipelineCullMode::none;
  pipeline.depth.test_enabled = false;
  pipeline.depth.write_enabled = false;
  pipeline.vertex_layout.stride = stock_ks_per_pixel_vertex_stride_bytes;
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
  return pipeline;
}

apex::formats::Kn5Node triangle_mesh() {
  apex::formats::Kn5Node mesh;
  mesh.type = 2U;
  mesh.kind = "mesh";
  mesh.vertexStride = 11U;
  mesh.vertices = {
      -0.75F, -0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.75F,  -0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F,   0.75F,  0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F,
  };
  mesh.indices = {0U, 1U, 2U};
  return mesh;
}

[[maybe_unused]] int run_test(const std::filesystem::path &root) {
  const std::filesystem::path base_path =
      shader_path(root, "ksPerPixel.shader");
  const std::filesystem::path alpha_path =
      shader_path(root, "ksPerPixelAT.shader");
  std::error_code base_error;
  std::error_code alpha_error;
  if (!std::filesystem::is_regular_file(base_path, base_error) ||
      !std::filesystem::is_regular_file(alpha_path, alpha_error)) {
    std::cout << "SKIP D3D12 WARP: installed ksPerPixel packages are "
                 "unavailable\n";
    return 77;
  }
  const std::vector<std::uint8_t> base_bytes = read_bounded(base_path);
  const std::vector<std::uint8_t> alpha_bytes = read_bounded(alpha_path);
  require(base_bytes.size() == 11'254U && alpha_bytes.size() == 11'330U,
          "installed native packages have the recovered bounded sizes");

  StockShaderContainer base_container =
      parse_stock_shader_container(base_bytes);
  StockShaderContainer alpha_container =
      parse_stock_shader_container(alpha_bytes);
  require(base_container.header.vertex_layout == "mesh" &&
              !base_container.header.alpha_tested &&
              base_container.geometry_shader.empty(),
          "base installed package has the exact native container shape");
  require(alpha_container.header.vertex_layout == "mesh" &&
              alpha_container.header.alpha_tested &&
              alpha_container.geometry_shader.empty(),
          "alpha installed package has the exact native container shape");
  require(base_container.vertex_shader.size() == 3'728U &&
              base_container.pixel_shader.size() == 7'508U,
          "base installed package has recovered stage sizes");
  require(alpha_container.vertex_shader.size() == 3'728U &&
              alpha_container.pixel_shader.size() == 7'584U,
          "alpha installed package has recovered stage sizes");

  auto base_program_result = create_validated_stock_ks_per_pixel_native_program(
      std::move(base_container), StockKsPerPixelVariant::base);
  require(base_program_result.ok() && base_program_result.program.has_value(),
          "base installed package passes the complete native ABI gate");
  auto alpha_program_result =
      create_validated_stock_ks_per_pixel_native_program(
          std::move(alpha_container),
          StockKsPerPixelVariant::alpha_to_coverage);
  require(alpha_program_result.ok() && alpha_program_result.program.has_value(),
          "alpha installed package passes the complete native ABI gate");

  DeviceOptions options;
  options.headless = true;
  options.enable_validation = true;
  options.allow_software = true;
  options.prefer_software = true;
  const AdapterResult adapters = enumerate_adapters(Backend::D3D12, options);
  const DeviceResult device_result = create_device(Backend::D3D12, options);
  if (!adapters.ok() || !device_result.ok()) {
    const Diagnostic &diagnostic =
        !device_result.ok() ? device_result.diagnostic : adapters.diagnostic;
    std::cout << "SKIP D3D12 WARP: " << diagnostic.code << ": "
              << diagnostic.message << '\n';
    return 77;
  }
  require(device_result.device->info().backend == Backend::D3D12,
          "created device is D3D12");
  require(device_result.device->info().software,
          "created D3D12 device is the requested software adapter");

  auto native_shaders = allocate_stock_ks_per_pixel_native_shaders(
      *device_result.device, std::move(*base_program_result.program));
  require(native_shaders.ok() && native_shaders.program != nullptr,
          "D3D12 owns the validated base DXBC shader pair");
  auto alpha_native_shaders = allocate_stock_ks_per_pixel_native_shaders(
      *device_result.device, std::move(*alpha_program_result.program));
  require(alpha_native_shaders.ok() && alpha_native_shaders.program != nullptr,
          "D3D12 owns the validated alpha-to-coverage DXBC shader pair");
  require(alpha_native_shaders.program->source().variant() ==
              StockKsPerPixelVariant::alpha_to_coverage,
          "D3D12 native owner retains the exact A2C shader variant");
  require(
      native_shaders.program->source().variant() ==
              StockKsPerPixelVariant::base &&
          native_shaders.program->vertex_shader().backend() == Backend::D3D12 &&
          native_shaders.program->pixel_shader().backend() == Backend::D3D12 &&
          native_shaders.program->vertex_shader().info().stage ==
              ShaderStage::vertex &&
          native_shaders.program->pixel_shader().info().stage ==
              ShaderStage::fragment &&
          native_shaders.program->vertex_shader().info().format ==
              ShaderBytecodeFormat::dxbc &&
          native_shaders.program->pixel_shader().info().format ==
              ShaderBytecodeFormat::dxbc,
      "D3D12 native owner retains exact stage metadata");

  CameraFrameRequest camera_request;
  camera_request.eye = {0.0F, 0.0F, 2.0F};
  camera_request.target = {0.0F, 0.0F, 0.0F};
  camera_request.fov_radians = 1.5707963267948966F;
  camera_request.aspect = 1.0F;
  camera_request.near_plane = 0.1F;
  camera_request.far_plane = 10.0F;
  camera_request.clip_space = CameraClipSpace::d3d12;
  const CameraFrameResult camera = build_camera_frame(camera_request);
  require(camera.ok() && camera.frame.has_value(), "native D3D12 camera frame");

  auto constants = allocate_stock_ks_per_pixel_native_constant_buffers(
      *device_result.device, native_constants(*camera.frame));
  auto samplers =
      allocate_stock_ks_per_pixel_native_samplers(*device_result.device);
  require(constants.ok() && constants.buffers->ready() && samplers.ok() &&
              samplers.samplers->ready(),
          "D3D12 owns all exact native constant and sampler resources");
  for (std::size_t index = 0U;
       index <
       static_cast<std::size_t>(StockKsPerPixelNativeConstantSlot::count);
       ++index) {
    const auto *buffer = constants.buffers->buffer(
        static_cast<StockKsPerPixelNativeConstantSlot>(index));
    require(buffer != nullptr && buffer->backend() == Backend::D3D12 &&
                buffer->info().description.size_bytes ==
                    stock_ks_per_pixel_native_constant_buffer_view_bytes,
            "native constant slot has the exact D3D12-aligned view");
  }
  require(
      samplers.samplers->sampler(StockKsPerPixelNativeSamplerSlot::linear)
                  ->backend() == Backend::D3D12 &&
          samplers.samplers->sampler(StockKsPerPixelNativeSamplerSlot::shadow)
                  ->backend() == Backend::D3D12,
      "native s0 and s1 samplers belong to D3D12");

  TextureDescription target_description;
  target_description.width = 32U;
  target_description.height = 32U;
  target_description.format = TextureFormat::rgba8_unorm;
  target_description.usage =
      TextureUsage::color_attachment | TextureUsage::transfer_source;
  TextureResult target =
      device_result.device->create_texture(target_description);
  require(target.ok(), "native target allocation");
  TextureDescription alpha_target_description = target_description;
  alpha_target_description.samples = 4U;
  TextureResult alpha_target =
      device_result.device->create_texture(alpha_target_description);
  require(alpha_target.ok(), "native 4x alpha-to-coverage target allocation");

  const std::array<std::byte, 4U> diffuse_pixel = {
      std::byte{180}, std::byte{120}, std::byte{80}, std::byte{255}};
  TextureDescription diffuse_description;
  diffuse_description.width = 1U;
  diffuse_description.height = 1U;
  diffuse_description.format = TextureFormat::rgba8_unorm;
  diffuse_description.usage = TextureUsage::sampled;
  TextureUploadPlan diffuse_uploads;
  diffuse_uploads.subresources.push_back({0U, 0U, 1U, 1U, 4U, diffuse_pixel});
  TextureResult diffuse = device_result.device->create_texture(
      diffuse_description, diffuse_uploads);
  require(diffuse.ok(), "native diffuse allocation and upload");
  const std::array<std::byte, 4U> partial_alpha_diffuse_pixel = {
      std::byte{180}, std::byte{120}, std::byte{80}, std::byte{128}};
  TextureUploadPlan partial_alpha_diffuse_uploads;
  partial_alpha_diffuse_uploads.subresources.push_back(
      {0U, 0U, 1U, 1U, 4U, partial_alpha_diffuse_pixel});
  TextureResult partial_alpha_diffuse = device_result.device->create_texture(
      diffuse_description, partial_alpha_diffuse_uploads);
  require(partial_alpha_diffuse.ok(),
          "native partial-alpha diffuse allocation and upload");

  std::array<std::unique_ptr<DepthAttachment>,
             stock_ks_per_pixel_shadow_cascade_count>
      shadows;
  for (auto &shadow : shadows) {
    DepthAttachmentResult result =
        device_result.device->create_depth_attachment(
            {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    require(result.ok(), "native shader-readable shadow allocation");
    shadow = std::move(result.attachment);
  }
  for (const auto &shadow : shadows) {
    DepthOnlyIndexedStaticMeshBatchDescription clear;
    clear.depth_attachment = shadow.get();
    clear.clear_depth = true;
    clear.depth_clear_value = 1.0F;
    const DepthOnlyIndexedStaticMeshBatchResult cleared =
        device_result.device->draw_depth_only_indexed_static_mesh_batch(clear);
    require(cleared.ok(), "native shadow clear before sampling");
  }

  StaticMeshUploadResult mesh_upload =
      upload_static_mesh(*device_result.device, triangle_mesh(), [] {
        DrawPacket packet;
        packet.primitive = DrawPrimitiveKind::static_mesh;
        packet.vertex_count = 3U;
        packet.index_count = 3U;
        packet.vertex_stride_floats = 11U;
        packet.flags.depth_test = false;
        packet.flags.depth_write = false;
        return packet;
      }());
  require(mesh_upload.ok(), "native triangle geometry allocation");

  PipelineProgram pipeline = native_pipeline();
  IndexedStaticMeshDrawRequest request =
      mesh_upload.upload->make_request(pipeline, *camera.frame);
  const StockKsPerPixelNativeDrawBinding binding{
      native_shaders.program.get(),
      constants.buffers.get(),
      samplers.samplers.get(),
      diffuse.texture.get(),
      {shadows[0].get(), shadows[1].get(), shadows[2].get()}};
  request.shader_authority =
      IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
  request.stock_ks_per_pixel_native = &binding;
  request.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
  const IndexedStaticMeshDrawResult draw =
      device_result.device->draw_indexed_static_mesh_and_readback(
          *target.texture, request);
  require(draw.ok() && draw.rgba8.size() == 32U * 32U * 4U,
          draw.diagnostic.code.empty() ? "native ksPerPixel WARP draw/readback"
                                       : draw.diagnostic.code);
  const std::size_t center = (16U * 32U + 16U) * 4U;
  require(draw.rgba8[center] != std::byte{0} ||
              draw.rgba8[center + 1U] != std::byte{0} ||
              draw.rgba8[center + 2U] != std::byte{0},
          "native ksPerPixel WARP readback is not the clear color");

  StaticMeshUploadResult alpha_mesh_upload =
      upload_static_mesh(*device_result.device, triangle_mesh(), [] {
        DrawPacket packet;
        packet.primitive = DrawPrimitiveKind::static_mesh;
        packet.vertex_count = 3U;
        packet.index_count = 3U;
        packet.vertex_stride_floats = 11U;
        packet.flags.alpha_to_coverage = true;
        packet.flags.depth_test = false;
        packet.flags.depth_write = false;
        return packet;
      }());
  require(alpha_mesh_upload.ok(),
          "native alpha-to-coverage triangle geometry allocation");
  PipelineProgram alpha_pipeline = native_pipeline();
  alpha_pipeline.targets.colors[0].samples = 4U;
  alpha_pipeline.blend.alpha_to_coverage = true;
  IndexedStaticMeshDrawRequest alpha_request =
      alpha_mesh_upload.upload->make_request(alpha_pipeline, *camera.frame);
  const StockKsPerPixelNativeDrawBinding alpha_binding{
      alpha_native_shaders.program.get(),
      constants.buffers.get(),
      samplers.samplers.get(),
      partial_alpha_diffuse.texture.get(),
      {shadows[0].get(), shadows[1].get(), shadows[2].get()}};
  alpha_request.shader_authority =
      IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
  alpha_request.stock_ks_per_pixel_native = &alpha_binding;
  alpha_request.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
  const IndexedStaticMeshDrawResult alpha_draw =
      device_result.device->draw_indexed_static_mesh_and_readback(
          *alpha_target.texture, alpha_request);
  require(alpha_draw.ok() && alpha_draw.rgba8.size() == 32U * 32U * 4U,
          alpha_draw.diagnostic.code.empty()
              ? "native ksPerPixel A2C WARP draw/readback"
              : alpha_draw.diagnostic.code);
  require(alpha_draw.rgba8[center] != std::byte{0} ||
              alpha_draw.rgba8[center + 1U] != std::byte{0} ||
              alpha_draw.rgba8[center + 2U] != std::byte{0},
          "native ksPerPixel A2C WARP readback is not the clear color");
  require(alpha_draw.rgba8[center] < draw.rgba8[center],
          "native ksPerPixel A2C resolves partial sample coverage");
  device_result.device->wait_idle();
  return 0;
}

} // namespace

int main() {
#if !defined(_WIN32)
  std::cout << "SKIP D3D12 WARP: Windows is required\n";
  return 77;
#else
  try {
    const char *root = std::getenv("ASSETTO_CORSA_ROOT");
    if (root == nullptr || *root == '\0') {
      std::cout << "SKIP D3D12 WARP: ASSETTO_CORSA_ROOT is not set\n";
      return 77;
    }
    return run_test(std::filesystem::path(root));
  } catch (const MaterialProfileError &error) {
    std::cerr << "FAIL D3D12 WARP: malformed installed shader at offset "
              << error.offset() << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "FAIL D3D12 WARP: " << error.what() << '\n';
    return 1;
  }
#endif
}
