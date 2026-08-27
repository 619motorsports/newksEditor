#include "apex/render/directional_shadow_source.hpp"
#include "apex/render/draw_packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>

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

int run_runtime_test(Backend backend) {
    DeviceOptions options;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    DeviceResult device_result = create_device(backend, options);
    if (!device_result.ok()) {
        std::cout << "SKIP directional shadow source runtime: " << device_result.diagnostic.code
                  << ": " << device_result.diagnostic.message << '\n';
        return 77;
    }
    Device& device = *device_result.device;

    auto source = create_builtin_directional_shadow_source_program(
        backend, DirectionalShadowSourceRole::cpu_skinned);
    require(source.ok(), "directional-shadow runtime program creation");

    const std::array<float, 57U> vertices = {
        -0.75F, -0.75F, 0.0F, 0.0F,  0.0F, 1.0F, 0.0F, 0.0F,  1.0F,   0.0F, 0.0F, 1.0F,
        0.0F,   0.0F,   0.0F, 0.0F,  0.0F, 0.0F, 0.0F, 0.75F, -0.75F, 0.0F, 0.0F, 0.0F,
        1.0F,   1.0F,   0.0F, 1.0F,  0.0F, 0.0F, 1.0F, 0.0F,  0.0F,   0.0F, 0.0F, 0.0F,
        0.0F,   0.0F,   0.0F, 0.75F, 0.0F, 0.0F, 0.0F, 1.0F,  0.5F,   1.0F, 1.0F, 0.0F,
        0.0F,   1.0F,   0.0F, 0.0F,  0.0F, 0.0F, 0.0F, 0.0F,  0.0F,
    };
    const std::array<std::uint16_t, 3U> indices = {0U, 2U, 1U};
    BufferResult vertex_buffer =
        device.create_buffer({sizeof(vertices), BufferUsage::vertex, BufferMemory::device_local,
                              BufferMutability::mutable_data},
                             bytes_of(vertices));
    BufferResult index_buffer =
        device.create_buffer({sizeof(indices), BufferUsage::index, BufferMemory::device_local,
                              BufferMutability::immutable},
                             bytes_of(indices));
    require(vertex_buffer.ok() && index_buffer.ok(), "directional-shadow runtime geometry upload");

    DepthAttachmentDescription depth_description;
    depth_description.width = 32U;
    depth_description.height = 32U;
    depth_description.samples = 1U;
    depth_description.format = DepthAttachmentFormat::d32_float;
    DepthAttachmentResult depth = device.create_depth_attachment(depth_description);
    require(depth.ok(), "directional-shadow runtime D32 creation");

    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::skinned_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 19U;
    packet.flags.depth_test = true;
    packet.flags.depth_write = true;
    packet.shader_execution_supported = true;
    packet.bone_palette = {apex::scene::identity_matrix};

    CameraFrame camera;
    camera.clip_space =
        backend == Backend::Vulkan ? CameraClipSpace::vulkan : CameraClipSpace::d3d12;
    DepthOnlyIndexedStaticMeshDrawRequest request;
    request.packet = &packet;
    request.pipeline = &*source.program;
    request.vertex_buffer = vertex_buffer.buffer.get();
    request.index_buffer = index_buffer.buffer.get();
    request.camera_frame = camera;
    request.clear_depth = true;
    const auto draw = device.draw_depth_only_indexed_static_mesh(*depth.attachment, request);
    require(draw.ok(), draw.diagnostic.code.empty() ? "directional-shadow runtime depth draw"
                                                    : draw.diagnostic.code);
    const auto readback = device.read_depth_attachment(*depth.attachment, {32U, 32U});
    require(readback.ok() && readback.depth.size() == 32U * 32U,
            "directional-shadow runtime depth readback");
    const float center = readback.depth[16U * 32U + 16U];
    const float outside = readback.depth.front();
    require(center < 0.99F && outside > 0.99F &&
                *std::min_element(readback.depth.begin(), readback.depth.end()) < 0.99F,
            "translated CPU-skinned caster writes bounded depth");

    auto alpha_source = create_builtin_directional_shadow_source_program(
        backend, DirectionalShadowSourceRole::alpha_tested_static);
    require(alpha_source.ok(), "alpha directional-shadow runtime program creation");
    const std::array<float, 33U> static_vertices = {
        -0.75F, -0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.75F,  -0.75F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F,   0.75F,  0.0F, 0.0F, 0.0F, 1.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F,
    };
    auto static_vertex_buffer =
        device.create_buffer({sizeof(static_vertices), BufferUsage::vertex,
                              BufferMemory::device_local, BufferMutability::immutable},
                             bytes_of(static_vertices));
    require(static_vertex_buffer.ok(), "alpha shadow geometry upload");

    const auto make_texture = [&](std::byte alpha) {
        const std::array<std::byte, 4U> pixel = {std::byte{255}, std::byte{255}, std::byte{255},
                                                 alpha};
        TextureDescription description;
        description.width = 1U;
        description.height = 1U;
        description.format = TextureFormat::rgba8_unorm;
        description.usage = TextureUsage::sampled;
        description.mutability = TextureMutability::immutable;
        TextureUploadPlan upload;
        upload.subresources.push_back({0U, 0U, 1U, 1U, 4U, pixel});
        return device.create_texture(description, upload);
    };
    auto transparent = make_texture(std::byte{0});
    auto opaque = make_texture(std::byte{255});
    SamplerDescription sampler_description;
    sampler_description.min_filter = SamplerFilter::nearest;
    sampler_description.mag_filter = SamplerFilter::nearest;
    sampler_description.mip_filter = SamplerFilter::nearest;
    auto sampler = device.create_sampler(sampler_description);
    require(transparent.ok() && opaque.ok() && sampler.ok(), "alpha shadow sampled resources");

    StockShadowCasterMaterialConstants material;
    material.emissive_and_alpha_ref[3U] = 0.5F;
    std::array<std::byte, stock_shadow_caster_buffer_alignment> material_bytes{};
    std::memcpy(material_bytes.data(), &material, sizeof(material));
    auto material_buffer =
        device.create_buffer({material_bytes.size(), BufferUsage::uniform,
                              BufferMemory::host_visible, BufferMutability::immutable},
                             material_bytes);
    require(material_buffer.ok(), "alpha shadow constant upload");

    DrawPacket alpha_packet;
    alpha_packet.primitive = DrawPrimitiveKind::static_mesh;
    alpha_packet.vertex_count = 3U;
    alpha_packet.index_count = 3U;
    alpha_packet.vertex_stride_floats = 11U;
    alpha_packet.flags.depth_test = true;
    alpha_packet.flags.depth_write = true;
    alpha_packet.shader_execution_supported = true;
    alpha_packet.material_profile.shadow_alpha_tested = true;
    const auto draw_alpha = [&](Texture& texture) {
        auto alpha_depth = device.create_depth_attachment(depth_description);
        require(alpha_depth.ok(), "alpha shadow D32 creation");
        DepthOnlyIndexedStaticMeshDrawRequest alpha_request;
        alpha_request.packet = &alpha_packet;
        alpha_request.pipeline = &*alpha_source.program;
        alpha_request.vertex_buffer = static_vertex_buffer.buffer.get();
        alpha_request.index_buffer = index_buffer.buffer.get();
        alpha_request.camera_frame = camera;
        alpha_request.clear_depth = true;
        alpha_request.material_mode =
            DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::stock_alpha_tested;
        alpha_request.resource_authority = IndexedResourceAuthority::explicit_bindings;
        alpha_request.alpha_tested_diffuse_binding = {&texture, sampler.sampler.get()};
        alpha_request.alpha_tested_material_binding = {material_buffer.buffer.get(), 0U,
                                                       stock_shadow_caster_material_bytes};
        const auto alpha_draw =
            device.draw_depth_only_indexed_static_mesh(*alpha_depth.attachment, alpha_request);
        require(alpha_draw.ok(), "alpha shadow depth draw");
        auto alpha_readback = device.read_depth_attachment(*alpha_depth.attachment, {32U, 32U});
        require(alpha_readback.ok(), "alpha shadow depth readback");
        return alpha_readback.depth[16U * 32U + 16U];
    };
    require(draw_alpha(*transparent.texture) > 0.99F && draw_alpha(*opaque.texture) < 0.99F,
            "alpha shadow discards below ksAlphaRef and accepts above it");
    return 0;
}

} // namespace

int main() {
    try {
        const int result = run_runtime_test(requested_backend());
        if (result == 0)
            std::cout << "directional shadow source runtime test passed\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "directional shadow source runtime test failed: " << error.what() << '\n';
        return 1;
    }
}
