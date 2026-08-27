#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/stock_tyres_source.hpp"
#include "test_environment.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename T, std::size_t Size>
std::span<const std::byte> bytes_of(const std::array<T, Size>& values) {
    return {reinterpret_cast<const std::byte*>(values.data()), sizeof(values)};
}

Backend requested_backend() {
    return apex::tests::environment_value("APEX_RENDER_BACKEND") == "d3d12"
               ? Backend::D3D12
               : Backend::Vulkan;
}

TextureResult make_texture(Device& device,
                           std::array<std::byte, 4U> pixel) {
    TextureDescription description;
    description.width = 1U;
    description.height = 1U;
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::sampled;
    description.mutability = TextureMutability::immutable;
    TextureUploadPlan upload;
    upload.subresources.push_back({0U, 0U, 1U, 1U, 4U, pixel});
    return device.create_texture(description, upload);
}

TextureResult make_cube(Device& device) {
    TextureDescription description;
    description.width = 1U;
    description.height = 1U;
    description.array_layers = 1U;
    description.shape = TextureShape::texture_cube;
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::sampled;
    description.mutability = TextureMutability::immutable;
    TextureUploadPlan upload;
    static constexpr std::array<std::byte, 4U> black = {
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};
    constexpr std::array<CubeFace, texture_cube_face_count> faces = {
        CubeFace::positive_x, CubeFace::negative_x, CubeFace::positive_y,
        CubeFace::negative_y, CubeFace::positive_z, CubeFace::negative_z};
    for (std::uint32_t face = 0U; face < texture_cube_face_count; ++face)
        upload.subresources.push_back(
            {0U, 0U, 1U, 1U, 4U, black, faces[face]});
    return device.create_texture(description, upload);
}

template <typename Constants, std::size_t Size>
BufferResult make_constants(Device& device, const Constants& constants) {
    std::array<std::byte, Size> bytes{};
    std::memcpy(bytes.data(), &constants, sizeof(constants));
    return device.create_buffer(
        {bytes.size(), BufferUsage::uniform, BufferMemory::host_visible,
         BufferMutability::immutable},
        bytes);
}

int run(Backend backend) {
    DeviceOptions options;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    DeviceResult device_result = create_device(backend, options);
    if (!device_result.ok()) {
        std::cout << "SKIP stock tyre runtime: "
                  << device_result.diagnostic.code << ": "
                  << device_result.diagnostic.message << '\n';
        return 77;
    }
    Device& device = *device_result.device;

    auto source = create_builtin_stock_tyres_source_program(
        backend, StockTyresSourceVariant::base);
    require(source.ok(), "stock tyre runtime program creation");

    TextureDescription target_description;
    target_description.width = 32U;
    target_description.height = 32U;
    target_description.format = TextureFormat::rgba8_unorm;
    target_description.usage = TextureUsage::color_attachment |
                               TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TextureResult target = device.create_texture(target_description);
    require(target.ok(), "stock tyre runtime target creation");

    const std::array<float, 33U> vertices = {
        -0.75F, -0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         0.75F, -0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         0.0F,   0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F};
    const std::array<std::uint16_t, 3U> indices = {0U, 1U, 2U};
    BufferResult vertex = device.create_buffer(
        {sizeof(vertices), BufferUsage::vertex, BufferMemory::device_local,
         BufferMutability::immutable}, bytes_of(vertices));
    BufferResult index = device.create_buffer(
        {sizeof(indices), BufferUsage::index, BufferMemory::device_local,
         BufferMutability::immutable}, bytes_of(indices));
    require(vertex.ok() && index.ok(), "stock tyre runtime geometry upload");

    StockTyresSourceMaterialConstants material;
    material.ambient = 1.0F;
    material.diffuse = 0.0F;
    material.specular = 0.0F;
    KsPerPixelFrameConstants frame;
    frame.sun_direction = {0.0F, 0.0F, 1.0F, 0.0F};
    frame.ambient_color = {1.0F, 1.0F, 1.0F, 0.0F};
    frame.camera_position = {0.0F, 0.0F, 2.0F, 0.0F};
    StockTyresSourceConstants controls;
    controls.blur_level = 0.5F;
    controls.dirty_level = 1.0F;
    controls.fresnel_max_level = 0.0F;
    BufferResult material_buffer = make_constants<
        StockTyresSourceMaterialConstants,
        portable_stock_tyres_material_buffer_view_bytes>(device, material);
    BufferResult frame_buffer = make_constants<
        KsPerPixelFrameConstants, portable_frame_buffer_view_bytes>(device, frame);
    BufferResult controls_buffer = make_constants<
        StockTyresSourceConstants, portable_stock_tyres_buffer_view_bytes>(
            device, controls);
    require(material_buffer.ok() && frame_buffer.ok() && controls_buffer.ok(),
            "stock tyre runtime constant upload");

    TextureResult diffuse = make_texture(
        device, {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}});
    TextureResult normal = make_texture(
        device, {std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}});
    TextureResult dirty = make_texture(
        device, {std::byte{0}, std::byte{255}, std::byte{0}, std::byte{128}});
    TextureResult blur = make_texture(
        device, {std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}});
    TextureResult normal_blur = make_texture(
        device, {std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}});
    TextureResult cube = make_cube(device);
    const auto require_texture = [](const TextureResult& result,
                                    const char* role) {
        if (!result.ok())
            throw std::runtime_error(
                std::string("stock tyre runtime ") + role +
                " texture upload: " + result.diagnostic.code + " " +
                result.diagnostic.message);
    };
    require_texture(diffuse, "diffuse");
    require_texture(normal, "normal");
    require_texture(dirty, "dirty");
    require_texture(blur, "blur");
    require_texture(normal_blur, "normal-blur");
    require_texture(cube, "cube");

    SamplerDescription sampler_description;
    sampler_description.min_filter = SamplerFilter::nearest;
    sampler_description.mag_filter = SamplerFilter::nearest;
    sampler_description.mip_filter = SamplerFilter::nearest;
    sampler_description.max_lod = std::numeric_limits<float>::max();
    SamplerResult sampler = device.create_sampler(sampler_description);
    require(sampler.ok(), "stock tyre runtime sampler creation");

    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.flags.depth_test = false;
    packet.flags.depth_write = false;
    packet.shader_execution_supported = true;

    CameraFrame camera;
    camera.clip_space = backend == Backend::Vulkan
                            ? CameraClipSpace::vulkan
                            : CameraClipSpace::d3d12;
    IndexedStaticMeshDrawRequest request;
    request.packet = &packet;
    request.pipeline = &*source.program;
    request.vertex_buffer = vertex.buffer.get();
    request.index_buffer = index.buffer.get();
    request.camera_frame = camera;
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {diffuse.texture.get(), sampler.sampler.get()};
    request.normal_binding = {normal.texture.get(), sampler.sampler.get()};
    request.tyre_dirty_binding = {dirty.texture.get(), sampler.sampler.get()};
    request.tyre_blur_binding = {blur.texture.get(), sampler.sampler.get()};
    request.tyre_normal_blur_binding = {
        normal_blur.texture.get(), sampler.sampler.get()};
    request.material_binding = {material_buffer.buffer.get(), 0U,
                                portable_stock_tyres_material_buffer_view_bytes};
    request.frame_binding = {frame_buffer.buffer.get(), 0U,
                             portable_frame_buffer_view_bytes};
    request.multimap_reflection_binding = {
        {cube.texture.get(), sampler.sampler.get()},
        {controls_buffer.buffer.get(), 0U,
         portable_stock_tyres_buffer_view_bytes}};

    const auto draw = device.draw_indexed_static_mesh_and_readback(
        *target.texture, request);
    require(draw.ok(), draw.diagnostic.code.empty()
                           ? "stock tyre runtime draw"
                           : draw.diagnostic.code);
    const std::size_t center = (16U * 32U + 16U) * 4U;
    const auto channel = [&](std::size_t offset) {
        return std::to_integer<int>(draw.rgba8[center + offset]);
    };
    require(std::abs(channel(0U) - 64) <= 3 &&
                std::abs(channel(1U) - 128) <= 3 &&
                std::abs(channel(2U) - 64) <= 3 && channel(3U) == 255,
            "stock tyre runtime preserves blur/dirt RGB math and output alpha");
    return 0;
}

}  // namespace

int main() {
    try {
        const int result = run(requested_backend());
        if (result == 0) std::cout << "stock tyre source runtime test passed\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "stock tyre source runtime test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
