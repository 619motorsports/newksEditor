#pragma once

#include "apex/render/device.hpp"

namespace apex::render {

[[nodiscard]] AdapterResult enumerate_vulkan_adapters(const DeviceOptions& options);
[[nodiscard]] DeviceResult create_vulkan_device(const DeviceOptions& options);

[[nodiscard]] AdapterResult enumerate_d3d12_adapters(const DeviceOptions& options);
[[nodiscard]] DeviceResult create_d3d12_device(const DeviceOptions& options);

} // namespace apex::render
