#include "apex/render/directional_shadow_source.hpp"
#include "apex/render/draw_packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
