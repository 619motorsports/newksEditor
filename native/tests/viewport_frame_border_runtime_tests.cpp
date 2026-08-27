#include "apex/render/viewport_frame_border.hpp"

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
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename T, std::size_t Size>
std::span<const std::byte> bytes_of(const std::array<T, Size>& values) {
    return {reinterpret_cast<const std::byte*>(values.data()), sizeof(values)};
}

Backend requested_backend() {
    const char* value = std::getenv("APEX_RENDER_BACKEND");
    return value != nullptr && std::string_view(value) == "d3d12"
        ? Backend::D3D12 : Backend::Vulkan;
}

int run(Backend backend) {
    DeviceOptions options;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    auto created_device = create_device(backend, options);
    if (!created_device.ok()) {
        std::cout << "SKIP viewport frame runtime: "
                  << created_device.diagnostic.code << ": "
                  << created_device.diagnostic.message << '\n';
        return 77;
    }
    Device& device = *created_device.device;
    constexpr std::uint32_t width = 8U;
    constexpr std::uint32_t height = 8U;

    PipelineRenderTargets targets;
    targets.colors = {{PipelineRenderTargetFormat::rgba8_unorm, 1U}};
    targets.has_depth = true;
    targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    auto program = create_viewport_frame_border_program(backend, targets);
    const auto geometry = build_viewport_frame_border(width, height, true, backend);
    require(program.ok() && geometry.ok(), "runtime frame package builds");

    auto vertices = device.create_buffer(
        {sizeof(geometry.vertices), BufferUsage::vertex,
         BufferMemory::device_local, BufferMutability::immutable},
        bytes_of(geometry.vertices));
    auto indices = device.create_buffer(
        {sizeof(geometry.indices), BufferUsage::index,
         BufferMemory::device_local, BufferMutability::immutable},
        bytes_of(geometry.indices));
    require(vertices.ok() && indices.ok(), "runtime frame geometry uploads");

    TextureDescription color_description;
    color_description.width = width;
    color_description.height = height;
    color_description.format = TextureFormat::rgba8_unorm;
    color_description.usage = TextureUsage::color_attachment |
                              TextureUsage::transfer_source;
    color_description.mutability = TextureMutability::mutable_data;
    auto color = device.create_texture(color_description, {});
    DepthAttachmentDescription depth_description;
    depth_description.width = width;
    depth_description.height = height;
    depth_description.samples = 1U;
    depth_description.format = DepthAttachmentFormat::d32_float;
    auto depth = device.create_depth_attachment(depth_description);
    require(color.ok() && depth.ok(), "runtime frame targets allocate");

    ViewportFrameBorderDrawRequest frame{
        &*program.program, vertices.buffer.get(), indices.buffer.get(),
        geometry.matrices};
    IndexedStaticMeshBatchDescription batch;
    batch.depth_attachment = depth.attachment.get();
    batch.clear_depth = true;
    batch.capture_rgba8 = true;
    batch.viewport_frame_border = frame;
    const auto drawn = device.draw_indexed_static_mesh_batch_and_readback(
        *color.texture, batch);
    require(drawn.ok() && drawn.rgba8.size() == width * height * 4U,
            drawn.diagnostic.code.empty() ? "runtime frame draw succeeds"
                                          : drawn.diagnostic.code);
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return std::span<const std::byte, 4U>(
            drawn.rgba8.data() + (y * width + x) * 4U, 4U);
    };
    const auto is_green = [&](std::uint32_t x, std::uint32_t y) {
        const auto value = pixel(x, y);
        return std::to_integer<std::uint8_t>(value[0U]) <= 1U &&
               std::to_integer<std::uint8_t>(value[1U]) >= 177U &&
               std::to_integer<std::uint8_t>(value[1U]) <= 180U &&
               std::to_integer<std::uint8_t>(value[2U]) <= 1U &&
               std::to_integer<std::uint8_t>(value[3U]) == 255U;
    };
    require(is_green(0U, 0U) && is_green(1U, 4U) &&
                is_green(6U, 4U) && is_green(4U, 7U),
            "both backends rasterize all four recovered 2-pixel edges");
    const auto center = pixel(4U, 4U);
    require(std::to_integer<std::uint8_t>(center[0U]) == 0U &&
                std::to_integer<std::uint8_t>(center[1U]) == 0U &&
                std::to_integer<std::uint8_t>(center[2U]) == 0U &&
                std::to_integer<std::uint8_t>(center[3U]) == 255U,
            "recovered frame leaves the viewport interior unchanged");
    return 0;
}

} // namespace

int main() {
    try {
        const int result = run(requested_backend());
        if (result == 0) std::cout << "viewport frame runtime test passed\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "viewport frame runtime test failed: " << error.what() << '\n';
        return 1;
    }
}
