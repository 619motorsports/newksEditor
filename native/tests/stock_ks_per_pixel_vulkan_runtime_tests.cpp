#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan_abi.hpp"

#include "stock_vulkan_abi_probe_spirv.hpp"

#include <array>
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
#include <vector>

namespace {

using namespace apex::render;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> decode_hex(std::string_view hex) {
    require(hex.size() % 2U == 0U, "SPIR-V fixture has complete bytes");
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F')
            return static_cast<std::uint8_t>(value - 'A' + 10);
        throw std::runtime_error("SPIR-V fixture contains non-hex text");
    };
    std::vector<std::uint8_t> bytes(hex.size() / 2U);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (nibble(hex[index * 2U]) << 4U) | nibble(hex[index * 2U + 1U]));
    }
    return bytes;
}

PipelineResourceKind probe_resource_kind(StockShaderDescriptorKind kind) {
    switch (kind) {
    case StockShaderDescriptorKind::uniform_buffer:
        return PipelineResourceKind::uniform_buffer;
    case StockShaderDescriptorKind::sampled_image:
        return PipelineResourceKind::sampled_texture;
    case StockShaderDescriptorKind::sampler:
        return PipelineResourceKind::sampler;
    }
    return PipelineResourceKind::storage_buffer;
}

PipelineProgram probe_pipeline() {
    PipelineProgram pipeline;
    pipeline.name = "stock-ks-per-pixel-vulkan-abi-runtime-probe";
    pipeline.shaders = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
         decode_hex(test::stock_vulkan_abi_probe_vertex_spirv_hex),
         PipelineShaderProvenance::native_abi_probe},
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
         decode_hex(test::stock_vulkan_abi_probe_fragment_spirv_hex),
         PipelineShaderProvenance::native_abi_probe},
    };
    pipeline.vertex_layout.stride = stock_ks_per_pixel_vertex_stride_bytes;
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0,
         PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent,
         PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    pipeline.targets.colors.push_back(
        {PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.depth.compare = PipelineCompareOperation::less;
    pipeline.transform_contract = PipelineTransformContract::none;
    for (const StockKsPerPixelBackendBinding& binding :
         stock_ks_per_pixel_vulkan_abi_manifest.bindings) {
        pipeline.resources.push_back(
            {probe_resource_kind(binding.descriptor_kind), binding.vulkan_set,
             binding.vulkan_binding, std::string(binding.name)});
    }
    return pipeline;
}

template <typename T, std::size_t Size>
std::span<const std::byte> bytes_of(const std::array<T, Size>& values) {
    return {reinterpret_cast<const std::byte*>(values.data()), sizeof(values)};
}

int run_runtime_probe() {
    DeviceOptions options;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    DeviceResult device_result = create_device(Backend::Vulkan, options);
    if (!device_result.ok()) {
        require(!device_result.diagnostic.code.empty() &&
                    !device_result.diagnostic.message.empty(),
                "unavailable Vulkan device has an actionable diagnostic");
        std::cout << "SKIP Vulkan ABI runtime probe: "
                  << device_result.diagnostic.code << ": "
                  << device_result.diagnostic.message << '\n';
        return 77;
    }
    Device& device = *device_result.device;

    TextureDescription target_description;
    target_description.width = 32U;
    target_description.height = 32U;
    target_description.format = TextureFormat::rgba8_unorm;
    target_description.usage =
        TextureUsage::color_attachment | TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TextureResult target = device.create_texture(target_description);
    require(target.ok(), "Vulkan ABI probe target creation");

    const std::array<float, 33U> vertices = {
        -0.75F, -0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         0.75F, -0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         0.0F,   0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F,
    };
    const std::array<std::uint16_t, 3U> indices = {0U, 1U, 2U};
    BufferResult vertex_buffer = device.create_buffer(
        {sizeof(vertices), BufferUsage::vertex, BufferMemory::device_local,
         BufferMutability::immutable},
        bytes_of(vertices));
    require(vertex_buffer.ok(), "Vulkan ABI probe vertex-buffer creation");
    BufferResult index_buffer = device.create_buffer(
        {sizeof(indices), BufferUsage::index, BufferMemory::device_local,
         BufferMutability::immutable},
        bytes_of(indices));
    require(index_buffer.ok(), "Vulkan ABI probe index-buffer creation");

    const std::array<std::byte, 256U> zero_uniform{};
    std::array<std::unique_ptr<Buffer>, 5U> uniform_buffers;
    for (std::unique_ptr<Buffer>& uniform : uniform_buffers) {
        BufferResult result = device.create_buffer(
            {zero_uniform.size(), BufferUsage::uniform,
             BufferMemory::host_visible, BufferMutability::mutable_data},
            zero_uniform);
        require(result.ok(), "Vulkan ABI probe uniform-buffer creation");
        uniform = std::move(result.buffer);
    }

    const std::array<std::byte, 16U> red_pixels = {
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
    };
    TextureDescription diffuse_description;
    diffuse_description.width = 2U;
    diffuse_description.height = 2U;
    diffuse_description.format = TextureFormat::rgba8_unorm;
    diffuse_description.usage =
        TextureUsage::sampled | TextureUsage::transfer_destination;
    diffuse_description.mutability = TextureMutability::immutable;
    TextureUploadPlan diffuse_upload;
    diffuse_upload.subresources.push_back(
        {0U, 0U, 2U, 2U, 8U, red_pixels});
    TextureResult diffuse =
        device.create_texture(diffuse_description, diffuse_upload);
    require(diffuse.ok(), "Vulkan ABI probe diffuse upload");

    const DepthAttachmentDescription shadow_description{
        32U, 32U, 1U, DepthAttachmentFormat::d32_float, true};
    std::array<std::unique_ptr<DepthAttachment>, 3U> shadows;
    for (std::unique_ptr<DepthAttachment>& shadow : shadows) {
        DepthAttachmentResult result =
            device.create_depth_attachment(shadow_description);
        require(result.ok(), "Vulkan ABI probe shadow-map creation");
        shadow = std::move(result.attachment);
        const DepthOnlyIndexedStaticMeshBatchResult clear =
            device.draw_depth_only_indexed_static_mesh_batch(
                {{}, shadow.get(), true, 1.0F});
        require(clear.ok(), "Vulkan ABI probe shadow-map initialization");
    }

    SamplerDescription linear_description;
    linear_description.min_filter = SamplerFilter::anisotropic;
    linear_description.mag_filter = SamplerFilter::anisotropic;
    linear_description.mip_filter = SamplerFilter::anisotropic;
    linear_description.max_anisotropy = 4.0F;
    linear_description.max_lod = std::numeric_limits<float>::max();
    SamplerResult linear_sampler = device.create_sampler(linear_description);
    require(linear_sampler.ok(), "Vulkan ABI probe linear sampler creation");

    SamplerDescription shadow_sampler_description;
    shadow_sampler_description.min_filter = SamplerFilter::linear;
    shadow_sampler_description.mag_filter = SamplerFilter::linear;
    shadow_sampler_description.mip_filter = SamplerFilter::nearest;
    shadow_sampler_description.address_u = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.address_v = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.address_w = SamplerAddressMode::clamp_to_edge;
    shadow_sampler_description.compare = SamplerCompare::less;
    shadow_sampler_description.max_lod = std::numeric_limits<float>::max();
    SamplerResult shadow_sampler =
        device.create_sampler(shadow_sampler_description);
    require(shadow_sampler.ok(), "Vulkan ABI probe comparison sampler creation");

    StockKsPerPixelVulkanAbiProbeDrawBinding binding;
    for (std::size_t index = 0U; index < uniform_buffers.size(); ++index)
        binding.uniform_buffers[index] = {uniform_buffers[index].get(), 0U, 256U};
    binding.diffuse_texture = diffuse.texture.get();
    binding.shadow_maps = {shadows[0].get(), shadows[1].get(), shadows[2].get()};
    binding.linear_sampler = linear_sampler.sampler.get();
    binding.shadow_sampler = shadow_sampler.sampler.get();

    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.flags.depth_test = false;
    packet.flags.depth_write = false;
    packet.shader_execution_supported = false;
    PipelineProgram pipeline = probe_pipeline();
    require(validate_stock_ks_per_pixel_vulkan_abi(pipeline) ==
                StockKsPerPixelVulkanAbiStatus::ready,
            "Vulkan ABI runtime probe pipeline matches recovered manifest");

    IndexedStaticMeshDrawRequest request;
    request.packet = &packet;
    request.pipeline = &pipeline;
    request.vertex_buffer = vertex_buffer.buffer.get();
    request.index_buffer = index_buffer.buffer.get();
    request.shader_authority = IndexedShaderAuthority::
        explicit_stock_ks_per_pixel_vulkan_abi_probe;
    request.stock_ks_per_pixel_vulkan_abi_probe = &binding;

    IndexedStaticMeshDrawResult draw =
        device.draw_indexed_static_mesh_and_readback(*target.texture, request);
    require(draw.ok(), draw.diagnostic.code.empty()
                           ? "Vulkan ABI runtime probe draw"
                           : draw.diagnostic.code);
    require(draw.rgba8.size() == 32U * 32U * 4U,
            "Vulkan ABI runtime probe readback size");
    const std::size_t center = (16U * 32U + 16U) * 4U;
    require(draw.rgba8[center] == std::byte{255} &&
                draw.rgba8[center + 1U] == std::byte{0} &&
                draw.rgba8[center + 2U] == std::byte{0} &&
                draw.rgba8[center + 3U] == std::byte{255},
            "Vulkan ABI runtime probe samples red at triangle center");
    require(draw.rgba8[0] == std::byte{0} && draw.rgba8[1] == std::byte{0} &&
                draw.rgba8[2] == std::byte{0} &&
                draw.rgba8[3] == std::byte{255},
            "Vulkan ABI runtime probe preserves the black clear outside");

    DepthAttachmentResult uninitialized_shadow =
        device.create_depth_attachment(shadow_description);
    require(uninitialized_shadow.ok(),
            "Vulkan ABI probe uninitialized-shadow fixture creation");
    binding.shadow_maps[2] = uninitialized_shadow.attachment.get();
    IndexedStaticMeshDrawResult rejected =
        device.draw_indexed_static_mesh_and_readback(*target.texture, request);
    require(rejected.status == IndexedStaticMeshDrawStatus::execution_failed &&
                rejected.diagnostic.code ==
                    "vulkan_stock_abi_probe_shadow_resource_invalid",
            "Vulkan ABI runtime rejects an uninitialized shadow map");
    binding.shadow_maps[2] = shadows[2].get();

    DeviceResult peer_result = create_device(Backend::Vulkan, options);
    require(peer_result.ok(), "Vulkan ABI probe peer-device creation");
    BufferResult foreign_uniform = peer_result.device->create_buffer(
        {zero_uniform.size(), BufferUsage::uniform, BufferMemory::host_visible,
         BufferMutability::mutable_data},
        zero_uniform);
    require(foreign_uniform.ok(), "Vulkan ABI probe foreign-uniform creation");
    binding.uniform_buffers[0].buffer = foreign_uniform.buffer.get();
    rejected = device.draw_indexed_static_mesh_and_readback(*target.texture, request);
    require(rejected.status == IndexedStaticMeshDrawStatus::execution_failed &&
                rejected.diagnostic.code ==
                    "vulkan_stock_abi_probe_context_mismatch",
            "Vulkan ABI runtime rejects a foreign-device descriptor resource");
    binding.uniform_buffers[0].buffer = uniform_buffers[0].get();

    device.wait_idle();
    peer_result.device->wait_idle();
    std::cout << "stock ksPerPixel Vulkan ABI runtime transport probe passed\n";
    return 0;
}

} // namespace

int main() {
    try {
        return run_runtime_probe();
    } catch (const std::exception& error) {
        std::cerr << "stock ksPerPixel Vulkan ABI runtime transport probe failed: "
                  << error.what() << '\n';
        return 1;
    }
}
