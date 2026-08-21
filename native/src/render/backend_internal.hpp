#pragma once

#include "apex/render/device.hpp"

#include <cstddef>
#include <string>

namespace apex::render {

[[nodiscard]] AdapterResult enumerate_vulkan_adapters(const DeviceOptions& options);
[[nodiscard]] DeviceResult create_vulkan_device(const DeviceOptions& options);

[[nodiscard]] AdapterResult enumerate_d3d12_adapters(const DeviceOptions& options);
[[nodiscard]] DeviceResult create_d3d12_device(const DeviceOptions& options);

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

} // namespace apex::render
