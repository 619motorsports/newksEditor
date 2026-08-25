#pragma once

#include "apex/render/device.hpp"

#include <cstddef>
#include <limits>
#include <string>

namespace apex::render {

[[nodiscard]] AdapterResult enumerate_vulkan_adapters(const DeviceOptions& options);
[[nodiscard]] DeviceResult create_vulkan_device(const DeviceOptions& options);

[[nodiscard]] AdapterResult enumerate_d3d12_adapters(const DeviceOptions& options);
[[nodiscard]] DeviceResult create_d3d12_device(const DeviceOptions& options);

[[nodiscard]] bool valid_d3d12_native_window(
    const apex::platform::NativeSurfaceSource& source, Diagnostic& diagnostic);

[[nodiscard]] bool valid_buffer_usage(BufferUsage usage) noexcept;
[[nodiscard]] bool valid_buffer_description(const BufferDescription& description,
                                             std::size_t initial_data_size,
                                             Diagnostic& diagnostic);
[[nodiscard]] bool valid_buffer_update(const Buffer& buffer,
                                       std::uint64_t offset,
                                       std::size_t data_size,
                                       Diagnostic& diagnostic);
[[nodiscard]] bool valid_texture_description(const TextureDescription& description,
                                              const TextureUploadPlan& initial_uploads,
                                              Diagnostic& diagnostic);
[[nodiscard]] bool valid_texture_update(const Texture& texture,
                                        const TextureUploadPlan& uploads,
                                        Diagnostic& diagnostic);
[[nodiscard]] bool valid_texture_clear_readback(const Texture& texture,
                                                const TextureClearReadbackRequest& request,
                                                Diagnostic& diagnostic);
[[nodiscard]] bool valid_sampler_description(const SamplerDescription& description,
                                             Diagnostic& diagnostic);
[[nodiscard]] bool valid_shader_module_description(const ShaderModuleDescription& description,
                                                   Diagnostic& diagnostic);
[[nodiscard]] bool shader_bytecode_format(std::span<const std::byte> bytecode,
                                          ShaderBytecodeFormat& format) noexcept;

// Visit one validated batch in its backend-neutral execution order. The
// bounded scans keep selected draws before line draws at an equal scene
// position. The ordinary scene draw at that position executes last.
template <typename SceneDraw, typename SelectedDraw, typename OverlayDraw>
[[nodiscard]] bool visit_indexed_static_mesh_batch_draws(
    const IndexedStaticMeshBatchDescription& description,
    SceneDraw&& visit_scene, SelectedDraw&& visit_selected,
    OverlayDraw&& visit_overlay) {
    const auto normalized_position = [&](std::uint32_t position) {
        return position == std::numeric_limits<std::uint32_t>::max()
                   ? description.draws.size()
                   : static_cast<std::size_t>(position);
    };
    for (std::size_t scene_position = 0U;
         scene_position <= description.draws.size(); ++scene_position) {
        for (const SelectedMeshDrawRequest& request :
             description.selected_mesh_draws) {
            if (normalized_position(request.scene_position) == scene_position &&
                !visit_selected(request))
                return false;
        }
        for (const OverlayLineDrawRequest& request : description.overlay_draws) {
            if (normalized_position(request.scene_position) == scene_position &&
                !visit_overlay(request))
                return false;
        }
        if (scene_position < description.draws.size() &&
            !visit_scene(description.draws[scene_position]))
            return false;
    }
    return true;
}

} // namespace apex::render
