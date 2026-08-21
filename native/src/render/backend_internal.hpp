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
[[nodiscard]] bool valid_texture_clear_readback(const Texture& texture,
                                                const TextureClearReadbackRequest& request,
                                                Diagnostic& diagnostic);
[[nodiscard]] bool valid_sampler_description(const SamplerDescription& description,
                                             Diagnostic& diagnostic);
[[nodiscard]] bool valid_shader_module_description(const ShaderModuleDescription& description,
                                                   Diagnostic& diagnostic);
[[nodiscard]] bool shader_bytecode_format(std::span<const std::byte> bytecode,
                                          ShaderBytecodeFormat& format) noexcept;

} // namespace apex::render
