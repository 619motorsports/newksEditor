#include "apex/render/device.hpp"
#include "backend_internal.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace apex::render {

namespace {

AdapterResult unavailable(const char* code, std::string message) {
    AdapterResult result;
    result.status = DeviceStatus::unavailable;
    result.diagnostic = {code, std::move(message)};
    return result;
}

AdapterResult invalid_options(const char* code, std::string message) {
    AdapterResult result;
    result.status = DeviceStatus::invalid_options;
    result.diagnostic = {code, std::move(message)};
    return result;
}

DeviceResult unavailable_device(const char* code, std::string message) {
    DeviceResult result;
    result.status = DeviceStatus::unavailable;
    result.diagnostic = {code, std::move(message)};
    return result;
}

DeviceResult invalid_device_options(const char* code, std::string message) {
    DeviceResult result;
    result.status = DeviceStatus::invalid_options;
    result.diagnostic = {code, std::move(message)};
    return result;
}

} // namespace

bool valid_buffer_usage(BufferUsage usage) noexcept {
    constexpr auto known = BufferUsage::transfer_source | BufferUsage::transfer_destination |
                           BufferUsage::vertex | BufferUsage::index | BufferUsage::uniform |
                           BufferUsage::storage;
    const auto raw = static_cast<std::uint32_t>(usage);
    return raw != 0U && (raw & ~static_cast<std::uint32_t>(known)) == 0U;
}

BufferStatus validate_buffer_description(const BufferDescription& description,
                                         std::size_t initial_data_size,
                                         Diagnostic& diagnostic) {
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (description.size_bytes == 0U) {
        diagnostic = {"buffer_size_zero", "A buffer must contain at least one byte"};
        return BufferStatus::invalid_description;
    }
    if (description.size_bytes > max_size_t) {
        diagnostic = {"buffer_size_platform_limit", "The buffer size cannot be represented by this platform"};
        return BufferStatus::invalid_description;
    }
    if (description.size_bytes > max_buffer_bytes) {
        diagnostic = {"buffer_size_limit", "The buffer size exceeds the backend-neutral safety limit"};
        return BufferStatus::invalid_description;
    }
    if (!valid_buffer_usage(description.usage)) {
        diagnostic = {"buffer_usage_invalid", "A buffer must specify only known non-empty usage bits"};
        return BufferStatus::invalid_description;
    }
    if (static_cast<std::uint64_t>(initial_data_size) > description.size_bytes) {
        diagnostic = {"buffer_initial_data_too_large", "Initial buffer data exceeds the declared buffer size"};
        return BufferStatus::invalid_description;
    }
    return BufferStatus::ready;
}

BufferStatus validate_buffer_update(const Buffer& buffer,
                                    std::uint64_t offset,
                                    std::size_t data_size,
                                    Diagnostic& diagnostic) {
    const BufferDescription& description = buffer.info().description;
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (description.size_bytes > max_size_t || offset > max_size_t) {
        diagnostic = {"buffer_update_platform_limit", "The buffer update range cannot be represented by this platform"};
        return BufferStatus::invalid_description;
    }
    if (description.mutability != BufferMutability::mutable_data) {
        diagnostic = {"buffer_immutable", "Immutable buffers cannot be updated"};
        return BufferStatus::invalid_description;
    }
    if (offset > description.size_bytes || static_cast<std::uint64_t>(data_size) > description.size_bytes - offset) {
        diagnostic = {"buffer_update_out_of_range", "The buffer update range exceeds the declared buffer size"};
        return BufferStatus::invalid_description;
    }
    if (data_size == 0U) {
        diagnostic = {"buffer_update_empty", "A buffer update must contain at least one byte"};
        return BufferStatus::invalid_description;
    }
    return BufferStatus::ready;
}

bool valid_buffer_description(const BufferDescription& description,
                              std::size_t initial_data_size,
                              Diagnostic& diagnostic) {
    return validate_buffer_description(description, initial_data_size, diagnostic) == BufferStatus::ready;
}

bool valid_buffer_update(const Buffer& buffer,
                         std::uint64_t offset,
                         std::size_t data_size,
                         Diagnostic& diagnostic) {
    return validate_buffer_update(buffer, offset, data_size, diagnostic) == BufferStatus::ready;
}

TextureStatus validate_texture_upload_plan(const TextureDescription& description,
                                           const TextureUploadPlan& uploads,
                                           Diagnostic& diagnostic) {
    if (description.width == 0U || description.height == 0U || description.mip_levels == 0U ||
        description.array_layers == 0U) {
        diagnostic = {"texture_dimensions_invalid", "Texture dimensions, mip levels, and array layers must be non-zero"};
        return TextureStatus::invalid_description;
    }
    if (description.width > max_texture_dimension || description.height > max_texture_dimension ||
        description.mip_levels > 32U) {
        diagnostic = {"texture_dimension_limit", "Texture upload dimensions exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    if (description.array_layers > 2048U ||
        static_cast<std::uint64_t>(description.mip_levels) * description.array_layers > 65535U) {
        diagnostic = {"texture_subresource_limit", "Texture upload subresources exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    if (!texture_format_is_known(description.format)) {
        diagnostic = {"texture_format_unknown", "The texture format is not recognized by the native contract"};
        return TextureStatus::unsupported;
    }
    if (!texture_format_cpu_upload_supported(description.format)) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "Block-compressed texture resources require a GPU upload path"};
        return TextureStatus::unsupported;
    }
    std::uint32_t largest_dimension = std::max(description.width, description.height);
    std::uint32_t possible_mips = 1U;
    while (largest_dimension > 1U) {
        largest_dimension >>= 1U;
        ++possible_mips;
    }
    if (description.mip_levels > possible_mips) {
        diagnostic = {"texture_mip_limit", "The texture has too many mip levels"};
        return TextureStatus::invalid_description;
    }
    const std::size_t bytes_per_pixel = texture_format_bytes_per_pixel(description.format);
    std::set<std::uint64_t> seen;
    std::uint64_t upload_total = 0U;
    for (const TextureUpload& upload : uploads.subresources) {
        if (upload.mip_level >= description.mip_levels || upload.array_layer >= description.array_layers) {
            diagnostic = {"texture_subresource_out_of_range", "Texture upload subresource is outside the description"};
            return TextureStatus::invalid_description;
        }
        const std::uint32_t expected_width = std::max(1U, description.width >> upload.mip_level);
        const std::uint32_t expected_height = std::max(1U, description.height >> upload.mip_level);
        if (upload.width != expected_width || upload.height != expected_height) {
            diagnostic = {"texture_upload_dimensions", "Texture upload dimensions do not match the mip level"};
            return TextureStatus::invalid_description;
        }
        const std::uint64_t minimum_pitch = static_cast<std::uint64_t>(expected_width) * bytes_per_pixel;
        if (upload.row_pitch < minimum_pitch) {
            diagnostic = {"texture_row_pitch_too_small", "Texture upload row pitch is smaller than one packed row"};
            return TextureStatus::invalid_description;
        }
        if (static_cast<std::uint64_t>(upload.row_pitch) % bytes_per_pixel != 0U) {
            diagnostic = {"texture_row_pitch_alignment", "Texture upload row pitch must align to the format texel size"};
            return TextureStatus::invalid_description;
        }
        const std::uint64_t upload_bytes = static_cast<std::uint64_t>(upload.row_pitch) * upload.height;
        if (upload_bytes > static_cast<std::uint64_t>(upload.data.size())) {
            diagnostic = {"texture_upload_truncated", "Texture upload data is shorter than its row-pitch footprint"};
            return TextureStatus::invalid_description;
        }
        constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
        const auto upload_size = static_cast<std::uint64_t>(upload.data.size());
        if (upload_size > max_size_t || upload_total > max_size_t - upload_size) {
            diagnostic = {"texture_upload_platform_limit",
                          "Texture upload data cannot be represented by this platform"};
            return TextureStatus::invalid_description;
        }
        if (upload_size > max_texture_bytes - upload_total) {
            diagnostic = {"texture_upload_size_limit", "Texture upload data exceeds the backend-neutral byte budget"};
            return TextureStatus::invalid_description;
        }
        upload_total += upload_size;
        const std::uint64_t key = (static_cast<std::uint64_t>(upload.array_layer) << 32U) | upload.mip_level;
        if (!seen.insert(key).second) {
            diagnostic = {"texture_subresource_duplicate", "Texture upload plan contains a duplicate subresource"};
            return TextureStatus::invalid_description;
        }
    }
    return TextureStatus::ready;
}

TextureStatus validate_texture_description(const TextureDescription& description,
                                           const TextureUploadPlan& initial_uploads,
                                           Diagnostic& diagnostic) {
    if (description.width == 0U || description.height == 0U || description.mip_levels == 0U ||
        description.array_layers == 0U) {
        diagnostic = {"texture_dimensions_invalid", "Texture dimensions, mip levels, and array layers must be non-zero"};
        return TextureStatus::invalid_description;
    }
    if (description.width > max_texture_dimension || description.height > max_texture_dimension) {
        diagnostic = {"texture_dimension_limit", "Texture dimensions exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    if (description.mip_levels > 32U) {
        diagnostic = {"texture_mip_limit", "The texture has too many mip levels"};
        return TextureStatus::invalid_description;
    }
    if (description.array_layers > 2048U) {
        diagnostic = {"texture_layer_limit", "Texture array layers exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    if (static_cast<std::uint64_t>(description.mip_levels) * description.array_layers > 65535U) {
        diagnostic = {"texture_subresource_limit", "Texture subresources exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    if (!texture_format_is_known(description.format)) {
        diagnostic = {"texture_format_unknown", "The texture format is not recognized by the native contract"};
        return TextureStatus::unsupported;
    }
    if (!texture_format_cpu_upload_supported(description.format)) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "Block-compressed texture resources require a GPU upload path"};
        return TextureStatus::unsupported;
    }
    const auto usage = static_cast<std::uint32_t>(description.usage);
    constexpr std::uint32_t known_usage = static_cast<std::uint32_t>(TextureUsage::sampled) |
                                           static_cast<std::uint32_t>(TextureUsage::transfer_source) |
                                           static_cast<std::uint32_t>(TextureUsage::transfer_destination) |
                                           static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                                           static_cast<std::uint32_t>(TextureUsage::storage);
    if (usage == 0U || (usage & ~known_usage) != 0U) {
        diagnostic = {"texture_usage_invalid", "A texture must specify only known non-empty usage bits"};
        return TextureStatus::invalid_description;
    }
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    std::uint64_t total_bytes = 0U;
    for (std::uint32_t layer = 0; layer < description.array_layers; ++layer) {
        (void)layer;
        for (std::uint32_t mip = 0; mip < description.mip_levels; ++mip) {
            const std::uint64_t width = std::max(1U, description.width >> mip);
            const std::uint64_t height = std::max(1U, description.height >> mip);
            const std::uint64_t level_bytes = width * height * texture_format_bytes_per_pixel(description.format);
            if (level_bytes > max_size_t || total_bytes > max_size_t - level_bytes) {
                diagnostic = {"texture_size_platform_limit",
                              "The texture byte size cannot be represented by this platform"};
                return TextureStatus::invalid_description;
            }
            if (level_bytes > max_texture_bytes - total_bytes) {
                diagnostic = {"texture_size_limit", "The texture byte budget exceeds the backend-neutral safety limit"};
                return TextureStatus::invalid_description;
            }
            total_bytes += level_bytes;
        }
    }
    return validate_texture_upload_plan(description, initial_uploads, diagnostic);
}

bool valid_texture_description(const TextureDescription& description,
                               const TextureUploadPlan& initial_uploads,
                               Diagnostic& diagnostic) {
    return validate_texture_description(description, initial_uploads, diagnostic) == TextureStatus::ready;
}

TextureStatus validate_texture_update(const Texture& texture,
                                      const TextureUploadPlan& uploads,
                                      Diagnostic& diagnostic) {
    if (texture.info().description.mutability != TextureMutability::mutable_data) {
        diagnostic = {"texture_immutable", "Immutable textures cannot be updated"};
        return TextureStatus::invalid_description;
    }
    if (uploads.subresources.empty()) {
        diagnostic = {"texture_update_empty", "A texture update must contain at least one subresource"};
        return TextureStatus::invalid_description;
    }
    return validate_texture_upload_plan(texture.info().description, uploads, diagnostic);
}

bool valid_texture_update(const Texture& texture,
                          const TextureUploadPlan& uploads,
                          Diagnostic& diagnostic) {
    return validate_texture_update(texture, uploads, diagnostic) == TextureStatus::ready;
}

const char* backend_name(Backend backend) noexcept {
    switch (backend) {
    case Backend::Vulkan:
        return "Vulkan";
    case Backend::D3D12:
        return "Direct3D 12";
    }
    return "Unknown";
}

const char* device_status_name(DeviceStatus status) noexcept {
    switch (status) {
    case DeviceStatus::ready:
        return "ready";
    case DeviceStatus::unavailable:
        return "unavailable";
    case DeviceStatus::invalid_options:
        return "invalid_options";
    case DeviceStatus::initialization_failed:
        return "initialization_failed";
    }
    return "unknown";
}

const char* buffer_status_name(BufferStatus status) noexcept {
    switch (status) {
    case BufferStatus::ready:
        return "ready";
    case BufferStatus::invalid_description:
        return "invalid_description";
    case BufferStatus::unsupported:
        return "unsupported";
    case BufferStatus::allocation_failed:
        return "allocation_failed";
    case BufferStatus::upload_failed:
        return "upload_failed";
    }
    return "unknown";
}

const char* texture_status_name(TextureStatus status) noexcept {
    switch (status) {
    case TextureStatus::ready:
        return "ready";
    case TextureStatus::invalid_description:
        return "invalid_description";
    case TextureStatus::unsupported:
        return "unsupported";
    case TextureStatus::allocation_failed:
        return "allocation_failed";
    case TextureStatus::upload_failed:
        return "upload_failed";
    }
    return "unknown";
}

AdapterResult enumerate_adapters(Backend backend, const DeviceOptions& options) {
    if (!options.headless) {
        return invalid_options("headless_required",
                               "The native renderer device contract only supports headless initialization");
    }
    if (options.prefer_software && !options.allow_software) {
        return invalid_options("software_adapter_disallowed",
                               "A software adapter cannot be preferred when software adapters are disabled");
    }

    switch (backend) {
    case Backend::Vulkan:
        return enumerate_vulkan_adapters(options);
    case Backend::D3D12:
        return enumerate_d3d12_adapters(options);
    }
    return unavailable("unknown_backend", "Unknown renderer backend");
}

DeviceResult create_device(Backend backend, const DeviceOptions& options) {
    if (!options.headless) {
        return invalid_device_options("headless_required",
                                      "The native renderer device contract only supports headless initialization");
    }
    if (options.prefer_software && !options.allow_software) {
        return invalid_device_options("software_adapter_disallowed",
                                      "A software adapter cannot be preferred when software adapters are disabled");
    }

    switch (backend) {
    case Backend::Vulkan:
        return create_vulkan_device(options);
    case Backend::D3D12:
        return create_d3d12_device(options);
    }
    return unavailable_device("unknown_backend", "Unknown renderer backend");
}

} // namespace apex::render
