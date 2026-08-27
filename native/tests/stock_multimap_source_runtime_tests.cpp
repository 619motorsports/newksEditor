#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/stock_multimap_source.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename T, std::size_t Size>
std::span<const std::byte> bytes_of(const std::array<T, Size>& values) {
    return {reinterpret_cast<const std::byte*>(values.data()), sizeof(values)};
}

Backend requested_backend() {
    const char* value = std::getenv("APEX_RENDER_BACKEND");
    if (value != nullptr && std::string_view(value) == "d3d12")
        return Backend::D3D12;
    return Backend::Vulkan;
}

TextureResult make_texture(Device& device, std::array<std::byte, 4U> pixel) {
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

int run_runtime_test(Backend backend) {
    DeviceOptions options;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    DeviceResult device_result = create_device(backend, options);
    if (!device_result.ok()) {
        std::cout << "SKIP MultiMap source runtime: " << device_result.diagnostic.code << ": "
                  << device_result.diagnostic.message << '\n';
        return 77;
    }
    Device& device = *device_result.device;

    StockMultiMapSourceProgramResult source = create_builtin_stock_multimap_source_program(
        backend, StockMultiMapSourceVariant::normal_detail);
    require(source.ok(), "MultiMap source runtime program creation");
    PipelineProgram& pipeline = *source.program;

    TextureDescription target_description;
    target_description.width = 32U;
    target_description.height = 32U;
    target_description.format = TextureFormat::rgba8_unorm;
    target_description.usage = TextureUsage::color_attachment | TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TextureResult target = device.create_texture(target_description);
    require(target.ok(), "MultiMap source runtime target creation");

    const std::array<float, 33U> vertices = {
        -0.75F, -0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.75F,  -0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F,   0.75F,  0.0F, 0.0F, 0.0F, 1.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F,
    };
    const std::array<std::uint16_t, 3U> indices = {0U, 1U, 2U};
    BufferResult vertex_buffer =
        device.create_buffer({sizeof(vertices), BufferUsage::vertex, BufferMemory::device_local,
                              BufferMutability::immutable},
                             bytes_of(vertices));
    BufferResult index_buffer =
        device.create_buffer({sizeof(indices), BufferUsage::index, BufferMemory::device_local,
                              BufferMutability::immutable},
                             bytes_of(indices));
    require(vertex_buffer.ok() && index_buffer.ok(), "MultiMap source runtime geometry upload");

    KsPerPixelMaterialConstants material;
    material.lighting = {1.0F, 0.0F, 0.0F, 1.0F};
    material.emissive = {0.0F, 0.0F, 0.0F, 0.0F};
    material.detail = {1.0F, 1.0F, 1.0F, 0.0F};
    std::array<std::byte, portable_material_buffer_view_bytes> material_bytes{};
    std::memcpy(material_bytes.data(), &material, sizeof(material));
    BufferResult material_buffer =
        device.create_buffer({material_bytes.size(), BufferUsage::uniform,
                              BufferMemory::host_visible, BufferMutability::immutable},
                             material_bytes);

    KsPerPixelFrameConstants frame;
    frame.sun_direction = {0.0F, 0.0F, 1.0F, 0.0F};
    frame.sun_color = {1.0F, 1.0F, 1.0F, 0.0F};
    frame.ambient_color = {1.0F, 1.0F, 1.0F, 0.0F};
    frame.camera_position = {0.0F, 0.0F, 2.0F, 0.0F};
    std::array<std::byte, portable_frame_buffer_view_bytes> frame_bytes{};
    std::memcpy(frame_bytes.data(), &frame, sizeof(frame));
    BufferResult frame_buffer =
        device.create_buffer({frame_bytes.size(), BufferUsage::uniform, BufferMemory::host_visible,
                              BufferMutability::immutable},
                             frame_bytes);
    require(material_buffer.ok() && frame_buffer.ok(), "MultiMap source runtime constant upload");

    TextureResult diffuse =
        make_texture(device, {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{0}});
    TextureResult normal =
        make_texture(device, {std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}});
    TextureResult maps =
        make_texture(device, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}});
    TextureResult detail =
        make_texture(device, {std::byte{128}, std::byte{255}, std::byte{255}, std::byte{255}});
    TextureResult normal_detail =
        make_texture(device, {std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}});
    require(diffuse.ok() && normal.ok() && maps.ok() && detail.ok() && normal_detail.ok(),
            "MultiMap source runtime texture upload");

    SamplerDescription sampler_description;
    sampler_description.min_filter = SamplerFilter::nearest;
    sampler_description.mag_filter = SamplerFilter::nearest;
    sampler_description.mip_filter = SamplerFilter::nearest;
    sampler_description.address_u = SamplerAddressMode::repeat;
    sampler_description.address_v = SamplerAddressMode::repeat;
    sampler_description.address_w = SamplerAddressMode::repeat;
    sampler_description.max_lod = std::numeric_limits<float>::max();
    std::array<std::unique_ptr<Sampler>, 5U> samplers;
    for (std::unique_ptr<Sampler>& sampler : samplers) {
        SamplerResult result = device.create_sampler(sampler_description);
        require(result.ok(), "MultiMap source runtime sampler creation");
        sampler = std::move(result.sampler);
    }

    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.flags.depth_test = false;
    packet.flags.depth_write = false;
    packet.shader_execution_supported = true;

    CameraFrame camera;
    camera.clip_space =
        backend == Backend::Vulkan ? CameraClipSpace::vulkan : CameraClipSpace::d3d12;
    IndexedStaticMeshDrawRequest request;
    request.packet = &packet;
    request.pipeline = &pipeline;
    request.vertex_buffer = vertex_buffer.buffer.get();
    request.index_buffer = index_buffer.buffer.get();
    request.camera_frame = camera;
    request.resource_authority = IndexedResourceAuthority::explicit_bindings;
    request.sampled_binding = {diffuse.texture.get(), samplers[0].get()};
    request.normal_binding = {normal.texture.get(), samplers[1].get()};
    request.maps_binding = {maps.texture.get(), samplers[2].get()};
    request.detail_binding = {detail.texture.get(), samplers[3].get()};
    request.normal_detail_binding = {normal_detail.texture.get(), samplers[4].get()};
    request.material_binding = {material_buffer.buffer.get(), 0U,
                                portable_material_buffer_view_bytes};
    request.frame_binding = {frame_buffer.buffer.get(), 0U, portable_frame_buffer_view_bytes};

    const IndexedStaticMeshDrawResult draw =
        device.draw_indexed_static_mesh_and_readback(*target.texture, request);
    require(draw.ok(),
            draw.diagnostic.code.empty() ? "MultiMap source runtime draw" : draw.diagnostic.code);
    require(draw.rgba8.size() == 32U * 32U * 4U, "MultiMap source runtime readback size");
    const std::size_t center = (16U * 32U + 16U) * 4U;
    const int red = std::to_integer<std::uint8_t>(draw.rgba8[center]);
    require(std::abs(red - 128) <= 2 && draw.rgba8[center + 1U] == std::byte{0} &&
                draw.rgba8[center + 2U] == std::byte{0} && draw.rgba8[center + 3U] == std::byte{0},
            "MultiMap source runtime applies the detail mask and keeps diffuse "
            "alpha");
    require(draw.rgba8[0] == std::byte{0} && draw.rgba8[1] == std::byte{0} &&
                draw.rgba8[2] == std::byte{0} && draw.rgba8[3] == std::byte{255},
            "MultiMap source runtime keeps the black clear outside the mesh");
    return 0;
}

} // namespace

int main() {
    try {
        const Backend backend = requested_backend();
        const int result = run_runtime_test(backend);
        if (result == 0)
            std::cout << "stock MultiMap source runtime test passed\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "stock MultiMap source runtime test failed: " << error.what() << '\n';
        return 1;
    }
}
