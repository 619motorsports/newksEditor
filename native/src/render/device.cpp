#include "apex/render/device.hpp"
#include "apex/render/selected_mesh.hpp"
#include "apex/render/draw_packet.hpp"
#include "backend_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace apex::render {

namespace {

inline constexpr std::uint32_t max_spirv_id_bound = 1U << 24U;
inline constexpr std::uint32_t max_dxbc_chunks = 4096U;
inline constexpr std::size_t max_native_surface_extensions = 64U;
inline constexpr std::size_t max_native_surface_extension_name = 255U;

bool valid_native_surface_source(const platform::NativeSurfaceSource& source,
                                 Diagnostic& diagnostic) {
    if (source.createVulkanSurface == nullptr || source.destroyVulkanSurface == nullptr) {
        diagnostic = {"native_surface_callbacks_missing",
                      "A native surface source requires both Vulkan create and destroy callbacks"};
        return false;
    }
    if (source.vulkanInstanceExtensions.empty() ||
        source.vulkanInstanceExtensions.size() > max_native_surface_extensions) {
        diagnostic = {"native_surface_extensions_invalid",
                      "A native surface source must provide between one and 64 bounded Vulkan instance extensions"};
        return false;
    }
    bool has_surface_extension = false;
    for (std::size_t index = 0U; index < source.vulkanInstanceExtensions.size(); ++index) {
        const std::string& extension = source.vulkanInstanceExtensions[index];
        if (extension.empty() || extension.size() > max_native_surface_extension_name ||
            extension.find('\0') != std::string::npos) {
            diagnostic = {"native_surface_extension_name_invalid",
                          "Native Vulkan instance extension names must be non-empty and bounded"};
            return false;
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (source.vulkanInstanceExtensions[previous] == extension) {
                diagnostic = {"native_surface_extension_duplicate",
                              "Native Vulkan instance extension names must be unique"};
                return false;
            }
        }
        if (extension == "VK_KHR_surface") has_surface_extension = true;
    }
    if (!has_surface_extension) {
        diagnostic = {"native_surface_extension_missing",
                      "A native Vulkan surface source must request VK_KHR_surface"};
        return false;
    }
    return true;
}

bool valid_render_sample_count(std::uint32_t samples) noexcept {
    return samples == 1U || samples == 4U;
}

bool checked_texture_size_multiply(std::uint64_t left, std::uint64_t right,
                                   std::uint64_t& output) noexcept {
    if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right) return false;
    output = left * right;
    return true;
}

bool checked_texture_size_add(std::uint64_t left, std::uint64_t right,
                              std::uint64_t& output) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) return false;
    output = left + right;
    return true;
}

TextureStatus validate_texture_block_upload_contract(const TextureDescription& description,
                                                     Diagnostic& diagnostic) {
    if (!texture_format_is_compressed(description.format)) return TextureStatus::ready;
    if (!texture_format_neutral_block_upload_supported(description.format)) {
        diagnostic = {"texture_compressed_format_unsupported",
                      "This block-compressed texture format is not supported by the neutral upload contract"};
        return TextureStatus::unsupported;
    }
    if (description.samples != 1U || description.mutability != TextureMutability::immutable ||
        description.array_layers != 1U || description.usage != TextureUsage::sampled) {
        diagnostic = {"texture_compressed_upload_unsupported",
                      "BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 uploads require one-layer, one-sample immutable sampled texture resources"};
        return TextureStatus::unsupported;
    }
    return TextureStatus::ready;
}

TextureStatus validate_texture_scalar_float_contract(const TextureDescription& description,
                                                      Diagnostic& diagnostic) {
    if (description.format != TextureFormat::r32_sfloat) return TextureStatus::ready;
    if (description.samples != 1U || description.mutability != TextureMutability::immutable ||
        description.array_layers != 1U || description.usage != TextureUsage::sampled) {
        diagnostic = {"texture_scalar_float_upload_unsupported",
                      "R32_SFLOAT textures require one-layer, one-sample immutable sampled resources"};
        return TextureStatus::unsupported;
    }
    return TextureStatus::ready;
}

bool texture_upload_layout(TextureFormat format, std::uint32_t width, std::uint32_t height,
                           std::uint64_t& row_bytes, std::uint64_t& row_count,
                           std::uint64_t& level_bytes) noexcept {
    const auto info = texture_format_info(format);
    if (info.classification == TextureFormatClass::block_compressed) {
        const std::uint64_t block_width = info.block_width;
        const std::uint64_t block_height = info.block_height;
        std::uint64_t width_rounded = 0U;
        std::uint64_t height_rounded = 0U;
        if (!checked_texture_size_add(width, block_width - 1U, width_rounded) ||
            !checked_texture_size_add(height, block_height - 1U, height_rounded))
            return false;
        const std::uint64_t blocks_w = width_rounded / block_width;
        const std::uint64_t blocks_h = height_rounded / block_height;
        if (!checked_texture_size_multiply(blocks_w, info.block_bytes, row_bytes)) return false;
        row_count = blocks_h;
    } else {
        if (!checked_texture_size_multiply(width, info.bytes_per_pixel, row_bytes)) return false;
        row_count = height;
    }
    return checked_texture_size_multiply(row_bytes, row_count, level_bytes);
}

bool portable_sampled_color_format(TextureFormat format, bool allow_srgb) noexcept {
    if (format == TextureFormat::rgba8_unorm || format == TextureFormat::bgra8_unorm ||
        format == TextureFormat::bc1_unorm || format == TextureFormat::bc3_unorm ||
        format == TextureFormat::bc7_unorm)
        return true;
    return allow_srgb &&
           (format == TextureFormat::rgba8_srgb || format == TextureFormat::bgra8_srgb ||
            format == TextureFormat::bc1_srgb || format == TextureFormat::bc3_srgb ||
            format == TextureFormat::bc7_srgb);
}

TextureStatus validate_texture_sample_contract(const TextureDescription& description,
                                               bool has_uploads,
                                               Diagnostic& diagnostic) {
    if (!valid_render_sample_count(description.samples)) {
        diagnostic = {"texture_samples_unsupported",
                      "Texture sample count must be exactly 1 or 4"};
        return TextureStatus::unsupported;
    }
    if (description.samples != 1U && has_uploads) {
        diagnostic = {"texture_multisample_upload_unsupported",
                      "Multisampled textures cannot have initial or update uploads"};
        return TextureStatus::unsupported;
    }
    constexpr std::uint32_t multisample_forbidden_usage =
        static_cast<std::uint32_t>(TextureUsage::sampled) |
        static_cast<std::uint32_t>(TextureUsage::transfer_destination) |
        static_cast<std::uint32_t>(TextureUsage::storage);
    if (description.samples != 1U &&
        (static_cast<std::uint32_t>(description.usage) & multisample_forbidden_usage) != 0U) {
        diagnostic = {"texture_multisample_usage_unsupported",
                      "Multisampled textures cannot be sampled, uploaded, or used as storage"};
        return TextureStatus::unsupported;
    }
    return TextureStatus::ready;
}

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

PresentationTargetStatus validate_presentation_target_description(
    const PresentationTargetDescription& description,
    Diagnostic& diagnostic) {
    if (description.width == 0U || description.height == 0U) {
        diagnostic = {"presentation_dimensions_invalid",
                      "Presentation target dimensions must be non-zero"};
        return PresentationTargetStatus::invalid_description;
    }
    if (description.width > max_texture_dimension ||
        description.height > max_texture_dimension) {
        diagnostic = {"presentation_dimension_limit",
                      "Presentation target dimensions exceed the backend-neutral safety limit"};
        return PresentationTargetStatus::invalid_description;
    }
    if (description.image_count < 2U ||
        description.image_count > max_presentation_image_count) {
        diagnostic = {"presentation_image_count_invalid",
                      "Presentation image count must be between 2 and 8"};
        return PresentationTargetStatus::invalid_description;
    }
    if (description.format != TextureFormat::rgba8_unorm &&
        description.format != TextureFormat::rgba8_srgb &&
        description.format != TextureFormat::bgra8_unorm &&
        description.format != TextureFormat::bgra8_srgb) {
        diagnostic = {"presentation_format_unsupported",
                      "Presentation targets require RGBA8 or BGRA8 UNORM or sRGB data"};
        return PresentationTargetStatus::unsupported;
    }
    diagnostic = {};
    return PresentationTargetStatus::ready;
}

TextureStatus validate_texture_upload_plan(const TextureDescription& description,
                                           const TextureUploadPlan& uploads,
                                           Diagnostic& diagnostic) {
    if (description.width == 0U || description.height == 0U || description.mip_levels == 0U ||
        description.array_layers == 0U) {
        diagnostic = {"texture_dimensions_invalid", "Texture dimensions, mip levels, and array layers must be non-zero"};
        return TextureStatus::invalid_description;
    }
    const TextureStatus sample_status =
        validate_texture_sample_contract(description, !uploads.subresources.empty(), diagnostic);
    if (sample_status != TextureStatus::ready) return sample_status;
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
    if (uploads.subresources.size() > 65535U) {
        diagnostic = {"texture_upload_subresource_count",
                      "The texture upload plan exceeds the bounded subresource-entry limit"};
        return TextureStatus::invalid_description;
    }
    if (!texture_format_is_known(description.format)) {
        diagnostic = {"texture_format_unknown", "The texture format is not recognized by the native contract"};
        return TextureStatus::unsupported;
    }
    const TextureStatus block_status = validate_texture_block_upload_contract(description, diagnostic);
    if (block_status != TextureStatus::ready) return block_status;
    const TextureStatus scalar_float_status =
        validate_texture_scalar_float_contract(description, diagnostic);
    if (scalar_float_status != TextureStatus::ready) return scalar_float_status;
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
    const auto format_info = texture_format_info(description.format);
    const bool block_compressed = format_info.classification == TextureFormatClass::block_compressed;
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
        std::uint64_t minimum_pitch = 0U;
        std::uint64_t expected_rows = 0U;
        std::uint64_t level_bytes = 0U;
        if (!texture_upload_layout(description.format, expected_width, expected_height,
                                   minimum_pitch, expected_rows, level_bytes)) {
            diagnostic = {"texture_upload_size_overflow",
                          "Texture upload block dimensions or footprint overflow"};
            return TextureStatus::invalid_description;
        }
        (void)level_bytes;
        if (upload.row_pitch < minimum_pitch) {
            diagnostic = {"texture_row_pitch_too_small", "Texture upload row pitch is smaller than one packed row"};
            return TextureStatus::invalid_description;
        }
        const std::uint64_t row_alignment = block_compressed ? format_info.block_bytes : format_info.bytes_per_pixel;
        if (row_alignment == 0U || static_cast<std::uint64_t>(upload.row_pitch) % row_alignment != 0U) {
            diagnostic = {"texture_row_pitch_alignment", "Texture upload row pitch must align to the format texel or block size"};
            return TextureStatus::invalid_description;
        }
        std::uint64_t upload_bytes = 0U;
        if (!checked_texture_size_multiply(upload.row_pitch, expected_rows, upload_bytes)) {
            diagnostic = {"texture_upload_size_overflow", "Texture upload row-pitch footprint overflows"};
            return TextureStatus::invalid_description;
        }
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
    const TextureStatus sample_status =
        validate_texture_sample_contract(description, !initial_uploads.subresources.empty(), diagnostic);
    if (sample_status != TextureStatus::ready) return sample_status;
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
    const TextureStatus block_status = validate_texture_block_upload_contract(description, diagnostic);
    if (block_status != TextureStatus::ready) return block_status;
    const TextureStatus scalar_float_status =
        validate_texture_scalar_float_contract(description, diagnostic);
    if (scalar_float_status != TextureStatus::ready) return scalar_float_status;
    if (texture_format_is_compressed(description.format) &&
        initial_uploads.subresources.size() !=
            static_cast<std::size_t>(description.mip_levels) * description.array_layers) {
        diagnostic = {"texture_compressed_upload_incomplete",
                      "Immutable BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 textures require one upload for every subresource"};
        return TextureStatus::invalid_description;
    }
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    std::uint64_t total_bytes = 0U;
    for (std::uint32_t layer = 0; layer < description.array_layers; ++layer) {
        (void)layer;
        for (std::uint32_t mip = 0; mip < description.mip_levels; ++mip) {
            const std::uint64_t width = std::max(1U, description.width >> mip);
            const std::uint64_t height = std::max(1U, description.height >> mip);
            std::uint64_t row_bytes = 0U;
            std::uint64_t row_count = 0U;
            std::uint64_t level_bytes = 0U;
            if (!texture_upload_layout(description.format, static_cast<std::uint32_t>(width),
                                       static_cast<std::uint32_t>(height), row_bytes, row_count, level_bytes)) {
                diagnostic = {"texture_size_platform_limit",
                              "The texture byte size cannot be represented by this platform"};
                return TextureStatus::invalid_description;
            }
            (void)row_bytes;
            (void)row_count;
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

DepthAttachmentStatus validate_depth_attachment_description(
    const DepthAttachmentDescription& description, Diagnostic& diagnostic) {
    if (description.format != DepthAttachmentFormat::d32_float) {
        diagnostic = {"depth_attachment_format_unsupported",
                      "Only D32 depth attachments are supported by the neutral contract"};
        return DepthAttachmentStatus::unsupported;
    }
    if (description.shader_readable && description.samples != 1U) {
        diagnostic = {"depth_attachment_sampled_multisample_unsupported",
                      "Shader-readable D32 attachments require exactly one sample"};
        return DepthAttachmentStatus::unsupported;
    }
    if (description.width == 0U || description.height == 0U) {
        diagnostic = {"depth_attachment_dimensions_invalid",
                      "Depth attachment dimensions must be non-zero"};
        return DepthAttachmentStatus::invalid_description;
    }
    if (description.width > max_texture_dimension || description.height > max_texture_dimension) {
        diagnostic = {"depth_attachment_dimension_limit",
                      "Depth attachment dimensions exceed the backend-neutral safety limit"};
        return DepthAttachmentStatus::invalid_description;
    }
    if (!valid_render_sample_count(description.samples)) {
        diagnostic = {"depth_attachment_samples_unsupported",
                      "Depth attachment sample count must be exactly 1 or 4"};
        return DepthAttachmentStatus::unsupported;
    }
    constexpr std::uint64_t max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    const std::uint64_t width = description.width;
    const std::uint64_t height = description.height;
    if (width > std::numeric_limits<std::uint64_t>::max() / height) {
        diagnostic = {"depth_attachment_size_overflow",
                      "Depth attachment dimensions overflow byte arithmetic"};
        return DepthAttachmentStatus::invalid_description;
    }
    const std::uint64_t texels = width * height;
    if (texels > std::numeric_limits<std::uint64_t>::max() / sizeof(float)) {
        diagnostic = {"depth_attachment_size_overflow",
                      "Depth attachment byte size overflows byte arithmetic"};
        return DepthAttachmentStatus::invalid_description;
    }
    const std::uint64_t bytes = texels * sizeof(float);
    if (bytes > max_size_t) {
        diagnostic = {"depth_attachment_size_platform_limit",
                      "Depth attachment size cannot be represented by this platform"};
        return DepthAttachmentStatus::invalid_description;
    }
    if (bytes > max_texture_bytes) {
        diagnostic = {"depth_attachment_size_limit",
                      "Depth attachment exceeds the backend-neutral safety limit"};
        return DepthAttachmentStatus::invalid_description;
    }
    diagnostic = {};
    return DepthAttachmentStatus::ready;
}

DepthAttachmentReadbackStatus validate_depth_attachment_readback(
    const DepthAttachment& attachment,
    const DepthAttachmentReadbackRequest& request,
    Diagnostic& diagnostic) {
    const DepthAttachmentDescription& description = attachment.info().description;
    Diagnostic description_diagnostic;
    const DepthAttachmentStatus description_status =
        validate_depth_attachment_description(description, description_diagnostic);
    if (description_status != DepthAttachmentStatus::ready) {
        diagnostic = {description_diagnostic.code.empty() ? "depth_attachment_invalid"
                                                            : description_diagnostic.code,
                      description_diagnostic.message};
        return description_status == DepthAttachmentStatus::unsupported
                   ? DepthAttachmentReadbackStatus::unsupported
                   : DepthAttachmentReadbackStatus::invalid_request;
    }
    if (description.samples != 1U) {
        diagnostic = {"depth_attachment_readback_multisample_unsupported",
                      "D32 depth readback supports only single-sample attachments"};
        return DepthAttachmentReadbackStatus::unsupported;
    }
    if (request.output_width != description.width || request.output_height != description.height) {
        diagnostic = {"depth_attachment_readback_dimensions_mismatch",
                      "D32 depth readback dimensions must match the attachment exactly"};
        return DepthAttachmentReadbackStatus::invalid_request;
    }
    const std::uint64_t width = request.output_width;
    const std::uint64_t height = request.output_height;
    if (height != 0U && width > std::numeric_limits<std::uint64_t>::max() / height) {
        diagnostic = {"depth_attachment_readback_size_overflow",
                      "D32 depth readback dimensions overflow byte arithmetic"};
        return DepthAttachmentReadbackStatus::invalid_request;
    }
    const std::uint64_t texels = width * height;
    if (texels > std::numeric_limits<std::uint64_t>::max() / sizeof(float)) {
        diagnostic = {"depth_attachment_readback_size_overflow",
                      "D32 depth readback byte size overflows byte arithmetic"};
        return DepthAttachmentReadbackStatus::invalid_request;
    }
    const std::uint64_t bytes = texels * sizeof(float);
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        diagnostic = {"depth_attachment_readback_size_platform_limit",
                      "D32 depth readback cannot be represented by this platform"};
        return DepthAttachmentReadbackStatus::invalid_request;
    }
    if (bytes > max_texture_readback_bytes) {
        diagnostic = {"depth_attachment_readback_size_limit",
                      "D32 depth readback exceeds the backend-neutral output limit"};
        return DepthAttachmentReadbackStatus::invalid_request;
    }
    diagnostic = {};
    return DepthAttachmentReadbackStatus::ready;
}

TextureReadbackStatus validate_texture_clear_readback(
    const Texture& texture, const TextureClearReadbackRequest& request, Diagnostic& diagnostic) {
    const TextureDescription& description = texture.info().description;
    if (description.samples != 1U) {
        diagnostic = {"texture_readback_multisample_unsupported",
                      "Texture clear/readback supports only single-sample textures"};
        return TextureReadbackStatus::unsupported;
    }
    for (const float component : request.clear_color) {
        if (!std::isfinite(component)) {
            diagnostic = {"texture_clear_color_non_finite", "Texture clear color components must be finite"};
            return TextureReadbackStatus::invalid_request;
        }
    }
    if (description.width == 0U || description.height == 0U || description.mip_levels == 0U ||
        description.array_layers == 0U || request.mip_level >= description.mip_levels || request.mip_level >= 32U ||
        request.array_layer >= description.array_layers) {
        diagnostic = {"texture_readback_subresource_out_of_range", "Texture readback mip or array layer is out of range"};
        return TextureReadbackStatus::invalid_request;
    }
    const std::uint32_t expected_width = std::max(1U, description.width >> request.mip_level);
    const std::uint32_t expected_height = std::max(1U, description.height >> request.mip_level);
    if (request.output_width != expected_width || request.output_height != expected_height) {
        diagnostic = {"texture_readback_dimensions", "Texture readback output dimensions do not match the mip level"};
        return TextureReadbackStatus::invalid_request;
    }
    const auto usage = static_cast<std::uint32_t>(description.usage);
    const auto color_attachment = static_cast<std::uint32_t>(TextureUsage::color_attachment);
    const auto transfer_source = static_cast<std::uint32_t>(TextureUsage::transfer_source);
    if ((usage & color_attachment) == 0U || (usage & transfer_source) == 0U) {
        diagnostic = {"texture_readback_usage_invalid",
                      "Texture clear/readback requires color-attachment and transfer-source usage"};
        return TextureReadbackStatus::invalid_request;
    }
    if (description.format != TextureFormat::rgba8_unorm &&
        description.format != TextureFormat::rgba8_srgb &&
        description.format != TextureFormat::bgra8_unorm &&
        description.format != TextureFormat::bgra8_srgb) {
        diagnostic = {"texture_readback_format_unsupported",
                      "Texture clear/readback currently supports only RGBA8 and BGRA8 formats"};
        return TextureReadbackStatus::unsupported;
    }
    constexpr std::uint64_t max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    const std::uint64_t width = request.output_width;
    const std::uint64_t height = request.output_height;
    if (width == 0U || height == 0U || width > std::numeric_limits<std::uint64_t>::max() / 4U ||
        height > (std::numeric_limits<std::uint64_t>::max() / 4U) / width) {
        diagnostic = {"texture_readback_size_limit", "Texture readback output size overflows the byte budget"};
        return TextureReadbackStatus::invalid_request;
    }
    const std::uint64_t byte_count = width * height * 4U;
    if (byte_count > max_size_t || byte_count > max_texture_readback_bytes) {
        diagnostic = {"texture_readback_size_limit", "Texture readback output exceeds the backend-neutral byte limit"};
        return TextureReadbackStatus::invalid_request;
    }
    return TextureReadbackStatus::ready;
}

TriangleDrawStatus validate_triangle_draw_request(const Texture& texture,
                                                  const TriangleDrawRequest& request,
                                                  Diagnostic& diagnostic) {
    if (request.pipeline == nullptr) {
        diagnostic = {"triangle_pipeline_missing", "Triangle drawing requires a pipeline program"};
        return TriangleDrawStatus::invalid_request;
    }
    const TextureDescription& description = texture.info().description;
    if (description.samples != 1U) {
        diagnostic = {"triangle_target_samples_unsupported",
                      "The fixed triangle path supports only single-sample targets"};
        return TriangleDrawStatus::unsupported;
    }
    for (const float component : request.clear_color) {
        if (!std::isfinite(component)) {
            diagnostic = {"triangle_clear_color_non_finite", "Triangle clear color components must be finite"};
            return TriangleDrawStatus::invalid_request;
        }
    }
    if (request.mip_level != 0U || request.array_layer != 0U || description.mip_levels != 1U ||
        description.array_layers != 1U) {
        diagnostic = {"triangle_subresource_unsupported",
                      "Triangle drawing currently targets the only mip and array layer"};
        return TriangleDrawStatus::unsupported;
    }
    const auto usage = static_cast<std::uint32_t>(description.usage);
    const auto required_usage = static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                                static_cast<std::uint32_t>(TextureUsage::transfer_source);
    if ((usage & required_usage) != required_usage) {
        diagnostic = {"triangle_target_usage_invalid", "Triangle drawing requires color attachment and transfer-source usage"};
        return TriangleDrawStatus::invalid_request;
    }
    if (description.width == 0U || description.height == 0U || description.mip_levels == 0U ||
        description.array_layers == 0U || description.format == TextureFormat::r8_unorm ||
        description.format == TextureFormat::r5g6b5_unorm) {
        diagnostic = {"triangle_target_invalid", "Triangle drawing target dimensions or format are invalid"};
        return TriangleDrawStatus::invalid_request;
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(description.width) * 4U;
    const std::uint64_t target_bytes = static_cast<std::uint64_t>(description.height) > 0U &&
                                               row_bytes > max_texture_readback_bytes /
                                                               static_cast<std::uint64_t>(description.height)
                                           ? max_texture_readback_bytes + 1U
                                           : row_bytes * static_cast<std::uint64_t>(description.height);
    if (target_bytes > max_texture_readback_bytes ||
        target_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        diagnostic = {"triangle_target_size_limit", "Triangle target exceeds the bounded readback size"};
        return TriangleDrawStatus::invalid_request;
    }
    if (request.vertex_count != 3U || request.pipeline->vertex_layout.stride == 0U ||
        request.pipeline->vertex_layout.stride > 4096U ||
        request.pipeline->vertex_layout.attributes.empty() ||
        request.pipeline->vertex_layout.attributes.size() > 16U) {
        diagnostic = {"triangle_vertex_layout_invalid", "Triangle vertex layout or vertex count is invalid"};
        return TriangleDrawStatus::invalid_request;
    }
    const std::size_t stride = request.pipeline->vertex_layout.stride;
    if (stride > std::numeric_limits<std::size_t>::max() / request.vertex_count ||
        request.vertex_data.size() != stride * request.vertex_count ||
        request.vertex_data.size() > 1U * 1024U * 1024U) {
        diagnostic = {"triangle_vertex_data_invalid", "Triangle vertex data exceeds the bounded layout"};
        return TriangleDrawStatus::invalid_request;
    }
    std::array<bool, 32> locations{};
    for (const PipelineVertexAttribute& attribute : request.pipeline->vertex_layout.attributes) {
        std::uint32_t attribute_bytes = 0U;
        switch (attribute.format) {
        case PipelineVertexAttributeFormat::float32: attribute_bytes = 4U; break;
        case PipelineVertexAttributeFormat::float32x2: attribute_bytes = 8U; break;
        case PipelineVertexAttributeFormat::float32x3: attribute_bytes = 12U; break;
        case PipelineVertexAttributeFormat::float32x4: attribute_bytes = 16U; break;
        case PipelineVertexAttributeFormat::uint32x4: attribute_bytes = 16U; break;
        default:
            diagnostic = {"triangle_vertex_attribute_invalid", "Triangle vertex attribute format is unknown"};
            return TriangleDrawStatus::invalid_request;
        }
        if (attribute.location >= locations.size() || locations[attribute.location] ||
            attribute.offset > request.pipeline->vertex_layout.stride ||
            attribute_bytes > request.pipeline->vertex_layout.stride - attribute.offset) {
            diagnostic = {"triangle_vertex_attribute_invalid", "Triangle vertex attribute exceeds its vertex stride"};
            return TriangleDrawStatus::invalid_request;
        }
        locations[attribute.location] = true;
    }
    const PipelineProgram& pipeline = *request.pipeline;
    const PipelineValidationResult pipeline_validation = validate_pipeline(pipeline);
    if (!pipeline_validation.valid) {
        if (!pipeline_validation.diagnostics.empty()) {
            const PipelineDiagnostic& failure = pipeline_validation.diagnostics.front();
            diagnostic = {"triangle_pipeline_" + failure.code, failure.message};
        } else {
            diagnostic = {"triangle_pipeline_invalid", "Pipeline validation failed without a diagnostic"};
        }
        return TriangleDrawStatus::invalid_request;
    }
    for (const PipelineShaderModule& shader : pipeline.shaders) {
        if (shader.stage == PipelineShaderStage::geometry) continue;
        const ShaderStage stage = shader.stage == PipelineShaderStage::vertex ? ShaderStage::vertex : ShaderStage::fragment;
        const std::span<const std::byte> bytecode(reinterpret_cast<const std::byte*>(shader.bytes.data()),
                                                   shader.bytes.size());
        Diagnostic shader_diagnostic;
        const ShaderModuleStatus shader_status =
            validate_shader_module_description({stage, bytecode}, shader_diagnostic);
        if (shader_status != ShaderModuleStatus::ready) {
            const std::string code = shader_diagnostic.code.empty()
                                         ? "triangle_shader_invalid"
                                         : "triangle_shader_" + shader_diagnostic.code;
            diagnostic = {code, shader_diagnostic.message};
            return shader_status == ShaderModuleStatus::unsupported ? TriangleDrawStatus::unsupported
                                                                      : TriangleDrawStatus::invalid_request;
        }
    }
    if (pipeline.targets.colors.size() != 1U || pipeline.targets.has_depth ||
        pipeline.targets.colors[0].samples != 1U || pipeline.resources.size() != 0U ||
        pipeline.transform_contract != PipelineTransformContract::none) {
        diagnostic = {"triangle_pipeline_unsupported",
                      "Triangle drawing supports one single-sample color target without resources or transforms"};
        return TriangleDrawStatus::unsupported;
    }
    const auto expected_target = [&] {
        switch (description.format) {
        case TextureFormat::rgba8_unorm: return PipelineRenderTargetFormat::rgba8_unorm;
        case TextureFormat::rgba8_srgb: return PipelineRenderTargetFormat::rgba8_srgb;
        case TextureFormat::bgra8_unorm: return PipelineRenderTargetFormat::bgra8_unorm;
        case TextureFormat::bgra8_srgb: return PipelineRenderTargetFormat::bgra8_srgb;
        default: return PipelineRenderTargetFormat::unknown;
        }
    }();
    if (pipeline.targets.colors[0].format != expected_target) {
        diagnostic = {"triangle_target_format_mismatch", "Pipeline color format does not match the target texture"};
        return TriangleDrawStatus::invalid_request;
    }
    bool has_vertex = false;
    bool has_fragment = false;
    std::size_t shader_bytes = 0U;
    for (const PipelineShaderModule& shader : pipeline.shaders) {
        if (shader.bytes.empty() || shader.bytes.size() > max_shader_module_bytes ||
            shader_bytes > std::numeric_limits<std::size_t>::max() - shader.bytes.size()) {
            diagnostic = {"triangle_shader_data_invalid", "Triangle shader bytecode exceeds the bounded module limit"};
            return TriangleDrawStatus::invalid_request;
        }
        shader_bytes += shader.bytes.size();
        if (shader_bytes > 32U * 1024U * 1024U) {
            diagnostic = {"triangle_shader_size_limit", "Triangle shader bytecode exceeds the total module limit"};
            return TriangleDrawStatus::invalid_request;
        }
        if (shader.stage == PipelineShaderStage::vertex) has_vertex = true;
        else if (shader.stage == PipelineShaderStage::fragment) has_fragment = true;
        else {
            diagnostic = {"triangle_shader_stage_unsupported", "Triangle drawing supports only vertex and fragment shaders"};
            return TriangleDrawStatus::unsupported;
        }
    }
    if (pipeline.shaders.size() != 2U || !has_vertex || !has_fragment) {
        diagnostic = {"triangle_shader_pair_invalid", "Triangle drawing requires one vertex and one fragment shader"};
        return TriangleDrawStatus::invalid_request;
    }
    if (pipeline.raster.fill != PipelineFillMode::solid || pipeline.blend.enabled ||
        pipeline.blend.alpha_to_coverage ||
        pipeline.depth.test_enabled || pipeline.depth.write_enabled) {
        diagnostic = {"triangle_pipeline_state_unsupported", "Triangle drawing supports solid opaque rasterization"};
        return TriangleDrawStatus::unsupported;
    }
    return TriangleDrawStatus::ready;
}

bool pipeline_declares_directional_shadow_receiver(
    const PipelineProgram& pipeline) noexcept {
    std::array<bool, 5> found{};
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set != 0U || resource.binding < 16U || resource.binding > 20U)
            continue;
        const std::size_t index = resource.binding - 16U;
        const PipelineResourceKind expected = resource.binding <= 18U
                                                  ? PipelineResourceKind::sampled_texture
                                              : resource.binding == 19U
                                                  ? PipelineResourceKind::sampler
                                                  : PipelineResourceKind::uniform_buffer;
        if (resource.kind != expected || found[index]) return false;
        found[index] = true;
    }
    return std::all_of(found.begin(), found.end(), [](bool value) { return value; });
}

IndexedPortableResourceLayout classify_indexed_portable_resource_layout(
    const PipelineProgram& pipeline) noexcept {
    if (pipeline.resources.empty())
        return IndexedPortableResourceLayout::resource_free;
    const bool has_receiver = pipeline_declares_directional_shadow_receiver(pipeline);
    const bool has_receiver_range = std::any_of(
        pipeline.resources.begin(), pipeline.resources.end(),
        [](const PipelineResourceBinding& resource) {
            return resource.set == 0U && resource.binding >= 16U &&
                   resource.binding <= 20U;
        });
    if (has_receiver_range && !has_receiver)
        return IndexedPortableResourceLayout::unsupported;
    const std::size_t material_resource_count =
        pipeline.resources.size() - (has_receiver ? 5U : 0U);
    if (material_resource_count != 2U && material_resource_count != 3U &&
        material_resource_count != 4U && material_resource_count != 6U &&
        material_resource_count != 8U && material_resource_count != 12U &&
        material_resource_count != 14U)
        return IndexedPortableResourceLayout::unsupported;
    bool sampled_texture = false;
    bool sampler = false;
    bool material_constants = false;
    bool frame_constants = false;
    bool normal_texture = false;
    bool normal_sampler = false;
    bool maps_texture = false;
    bool maps_sampler = false;
    bool detail_texture = false;
    bool detail_sampler = false;
    bool normal_detail_texture = false;
    bool normal_detail_sampler = false;
    bool damage_texture = false;
    bool damage_sampler = false;
    bool damage_mask_texture = false;
    bool damage_mask_sampler = false;
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set == 0U && resource.binding >= 16U &&
            resource.binding <= 20U && has_receiver)
            continue;
        if (resource.set == 0U && resource.binding == 0U &&
            resource.kind == PipelineResourceKind::sampled_texture) {
            if (sampled_texture) return IndexedPortableResourceLayout::unsupported;
            sampled_texture = true;
        } else if (resource.set == 0U && resource.binding == 1U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (sampler) return IndexedPortableResourceLayout::unsupported;
            sampler = true;
        } else if (resource.set == 0U && resource.binding == 2U &&
                   resource.kind == PipelineResourceKind::uniform_buffer) {
            if (material_constants) return IndexedPortableResourceLayout::unsupported;
            material_constants = true;
        } else if (resource.set == 0U && resource.binding == 3U &&
                   resource.kind == PipelineResourceKind::uniform_buffer) {
            if (frame_constants) return IndexedPortableResourceLayout::unsupported;
            frame_constants = true;
        } else if (resource.set == 0U && resource.binding == 4U &&
                   resource.kind == PipelineResourceKind::sampled_texture) {
            if (normal_texture) return IndexedPortableResourceLayout::unsupported;
            normal_texture = true;
        } else if (resource.set == 0U && resource.binding == 5U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (normal_sampler) return IndexedPortableResourceLayout::unsupported;
            normal_sampler = true;
        } else if (resource.set == 0U && resource.binding == 6U &&
                   resource.kind == PipelineResourceKind::sampled_texture) {
            if (maps_texture) return IndexedPortableResourceLayout::unsupported;
            maps_texture = true;
        } else if (resource.set == 0U && resource.binding == 7U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (maps_sampler) return IndexedPortableResourceLayout::unsupported;
            maps_sampler = true;
        } else if (resource.set == 0U && resource.binding == 8U &&
                   resource.kind == PipelineResourceKind::sampled_texture) {
            if (detail_texture) return IndexedPortableResourceLayout::unsupported;
            detail_texture = true;
        } else if (resource.set == 0U && resource.binding == 9U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (detail_sampler) return IndexedPortableResourceLayout::unsupported;
            detail_sampler = true;
        } else if (resource.set == 0U && resource.binding == 10U &&
                   resource.kind == PipelineResourceKind::sampled_texture) {
            if (normal_detail_texture) return IndexedPortableResourceLayout::unsupported;
            normal_detail_texture = true;
        } else if (resource.set == 0U && resource.binding == 11U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (normal_detail_sampler) return IndexedPortableResourceLayout::unsupported;
            normal_detail_sampler = true;
        } else if (resource.set == 0U && resource.binding == 12U &&
                   resource.kind == PipelineResourceKind::sampled_texture) {
            if (damage_texture) return IndexedPortableResourceLayout::unsupported;
            damage_texture = true;
        } else if (resource.set == 0U && resource.binding == 13U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (damage_sampler) return IndexedPortableResourceLayout::unsupported;
            damage_sampler = true;
        } else if (resource.set == 0U && resource.binding == 14U &&
                   resource.kind == PipelineResourceKind::sampled_texture) {
            if (damage_mask_texture) return IndexedPortableResourceLayout::unsupported;
            damage_mask_texture = true;
        } else if (resource.set == 0U && resource.binding == 15U &&
                   resource.kind == PipelineResourceKind::sampler) {
            if (damage_mask_sampler) return IndexedPortableResourceLayout::unsupported;
            damage_mask_sampler = true;
        } else {
            return IndexedPortableResourceLayout::unsupported;
        }
    }
    if (!sampled_texture || !sampler || normal_texture != normal_sampler ||
        maps_texture != maps_sampler || detail_texture != detail_sampler ||
        normal_detail_texture != normal_detail_sampler || damage_texture != damage_sampler ||
        damage_mask_texture != damage_mask_sampler)
        return IndexedPortableResourceLayout::unsupported;
    if (material_resource_count == 2U && !material_constants && !frame_constants)
        return IndexedPortableResourceLayout::diffuse;
    if (material_resource_count == 3U && material_constants && !frame_constants)
        return IndexedPortableResourceLayout::diffuse_with_constants;
    if (material_resource_count == 3U && !material_constants && frame_constants)
        return IndexedPortableResourceLayout::diffuse_with_frame;
    if (material_resource_count == 4U && material_constants && frame_constants)
        return IndexedPortableResourceLayout::diffuse_with_constants_and_frame;
    if (material_resource_count == 6U && material_constants && frame_constants &&
        normal_texture && normal_sampler)
        return IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame;
    if (material_resource_count == 8U && material_constants && frame_constants &&
        normal_texture && normal_sampler && maps_texture && maps_sampler)
        return IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame;
    if (material_resource_count == 12U && material_constants && frame_constants &&
        normal_texture && normal_sampler && maps_texture && maps_sampler &&
        detail_texture && detail_sampler && normal_detail_texture && normal_detail_sampler)
        return IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame;
    if (material_resource_count == 12U && material_constants && frame_constants &&
        normal_texture && normal_sampler && maps_texture && maps_sampler &&
        damage_texture && damage_sampler && damage_mask_texture && damage_mask_sampler)
        return IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame;
    if (material_resource_count == 14U && material_constants && frame_constants &&
        normal_texture && normal_sampler && maps_texture && maps_sampler &&
        detail_texture && detail_sampler && damage_texture && damage_sampler &&
        damage_mask_texture && damage_mask_sampler)
        return IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
    return IndexedPortableResourceLayout::unsupported;
}

namespace {

IndexedStaticMeshDrawStatus validate_indexed_color_target(
    const TextureDescription& target, Diagnostic& diagnostic) {
    if (!valid_render_sample_count(target.samples)) {
        diagnostic = {"indexed_static_mesh_target_samples_unsupported",
                      "Indexed static-mesh color targets require exactly 1 or 4 samples"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const auto target_usage = static_cast<std::uint32_t>(target.usage);
    const auto required_usage =
        static_cast<std::uint32_t>(TextureUsage::color_attachment) |
        static_cast<std::uint32_t>(TextureUsage::transfer_source);
    if ((target_usage & required_usage) != required_usage) {
        diagnostic = {"indexed_static_mesh_target_usage_invalid",
                      "Indexed static-mesh drawing requires color-attachment and transfer-source usage"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (target.width == 0U || target.height == 0U ||
        target.mip_levels != 1U || target.array_layers != 1U) {
        diagnostic = {"indexed_static_mesh_target_invalid",
                      "Indexed static-mesh target dimensions or subresource count are invalid"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const std::uint64_t row_bytes =
        static_cast<std::uint64_t>(target.width) * 4U;
    const std::uint64_t height = target.height;
    if (row_bytes > max_texture_readback_bytes / height ||
        row_bytes * height >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        diagnostic = {"indexed_static_mesh_target_size_limit",
                      "Indexed static-mesh target exceeds the bounded readback size"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    switch (target.format) {
    case TextureFormat::rgba8_unorm:
    case TextureFormat::rgba8_srgb:
    case TextureFormat::bgra8_unorm:
    case TextureFormat::bgra8_srgb:
        return IndexedStaticMeshDrawStatus::ready;
    default:
        diagnostic = {"indexed_static_mesh_target_format_unsupported",
                      "Indexed static-mesh drawing supports only RGBA8 and BGRA8 targets"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
}

} // namespace

IndexedStaticMeshDrawStatus validate_indexed_static_mesh_draw_request(
    const Texture& texture, const IndexedStaticMeshDrawRequest& request, Diagnostic& diagnostic) {
    if (request.packet == nullptr || request.pipeline == nullptr || request.vertex_buffer == nullptr ||
        request.index_buffer == nullptr) {
        diagnostic = {"indexed_static_mesh_handle_missing",
                      "Indexed static-mesh drawing requires a packet, pipeline, vertex buffer, and index buffer"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    for (const float component : request.clear_color) {
        if (!std::isfinite(component)) {
            diagnostic = {"indexed_static_mesh_clear_color_non_finite",
                          "Indexed static-mesh clear color components must be finite"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    if (!std::isfinite(request.depth_clear_value) || request.depth_clear_value < 0.0F ||
        request.depth_clear_value > 1.0F) {
        diagnostic = {"indexed_static_mesh_depth_clear_invalid",
                      "Indexed static-mesh depth clear value must be finite and within [0, 1]"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (request.index_type != StaticMeshIndexType::uint16) {
        diagnostic = {"indexed_static_mesh_index_type_unsupported",
                      "Only uint16 indexed static-mesh draws are supported"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const TextureDescription& target = texture.info().description;
    const IndexedStaticMeshDrawStatus target_status =
        validate_indexed_color_target(target, diagnostic);
    if (target_status != IndexedStaticMeshDrawStatus::ready)
        return target_status;
    if (request.depth_attachment != nullptr) {
        const DepthAttachment& depth = *request.depth_attachment;
        if (depth.backend() != texture.backend()) {
            diagnostic = {"indexed_depth_attachment_backend_mismatch",
                          "Color target and depth attachment must belong to the same backend"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        Diagnostic depth_diagnostic;
        const DepthAttachmentStatus depth_status =
            validate_depth_attachment_description(depth.info().description, depth_diagnostic);
        if (depth_status != DepthAttachmentStatus::ready) {
            diagnostic = {depth_diagnostic.code.empty() ? "indexed_depth_attachment_invalid"
                                                         : depth_diagnostic.code,
                          depth_diagnostic.message};
            return depth_status == DepthAttachmentStatus::unsupported
                       ? IndexedStaticMeshDrawStatus::unsupported
                       : IndexedStaticMeshDrawStatus::invalid_request;
        }
        const DepthAttachmentDescription& depth_description = depth.info().description;
        if (depth_description.width != target.width || depth_description.height != target.height ||
            depth_description.samples != target.samples) {
            diagnostic = {"indexed_depth_attachment_dimensions_mismatch",
                          "Depth attachment dimensions and samples must match the color target"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    if (request.mip_level != 0U || request.array_layer != 0U) {
        diagnostic = {"indexed_static_mesh_subresource_unsupported",
                      "Indexed static-mesh drawing targets mip zero and array layer zero"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const auto expected_format = [&] {
        switch (target.format) {
        case TextureFormat::rgba8_unorm: return PipelineRenderTargetFormat::rgba8_unorm;
        case TextureFormat::rgba8_srgb: return PipelineRenderTargetFormat::rgba8_srgb;
        case TextureFormat::bgra8_unorm: return PipelineRenderTargetFormat::bgra8_unorm;
        case TextureFormat::bgra8_srgb: return PipelineRenderTargetFormat::bgra8_srgb;
        default: return PipelineRenderTargetFormat::unknown;
        }
    }();
    // validate_indexed_color_target already rejected unknown target formats.

    const DrawPacket& packet = *request.packet;
    const bool static_mesh = packet.primitive == DrawPrimitiveKind::static_mesh;
    const bool skinned_mesh = packet.primitive == DrawPrimitiveKind::skinned_mesh;
    if (!static_mesh && !skinned_mesh) {
        diagnostic = {"indexed_static_mesh_primitive_unsupported",
                      "Indexed drawing requires a static or skinned mesh primitive"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (!packet.shader_execution_supported &&
        request.shader_authority != IndexedShaderAuthority::explicit_pipeline) {
        diagnostic = {"indexed_shader_execution_staged",
                      "The draw packet does not contain an executable shader contract"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (static_mesh && !packet.bone_palette.empty()) {
        diagnostic = {"indexed_static_mesh_skinning_unsupported",
                      "Static indexed drawing requires an empty bone palette"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (skinned_mesh && packet.bone_palette.empty()) {
        diagnostic = {"indexed_skinned_mesh_palette_missing",
                      "Skinned indexed drawing requires a non-empty bone palette"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (request.pipeline->transform_contract != PipelineTransformContract::draw_matrices) {
        diagnostic = {"indexed_transform_contract_required",
                      "Indexed static-mesh execution requires the draw-matrices shader contract"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (!request.camera_frame.has_value()) {
        diagnostic = {"indexed_camera_frame_missing",
                      "Indexed static-mesh transform execution requires a camera frame"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const CameraFrame& camera = *request.camera_frame;
    const CameraClipSpace expected_clip_space = texture.backend() == Backend::Vulkan
                                                    ? CameraClipSpace::vulkan
                                                    : CameraClipSpace::d3d12;
    if (camera.clip_space != expected_clip_space) {
        diagnostic = {"indexed_camera_clip_space_mismatch",
                      "Camera clip space does not match the indexed draw backend"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    for (const float component : packet.world_matrix) {
        if (!std::isfinite(component)) {
            diagnostic = {"indexed_static_mesh_world_matrix_non_finite",
                          "Indexed static-mesh world matrix must be finite"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    for (const float component : camera.view_projection) {
        if (!std::isfinite(component)) {
            diagnostic = {"indexed_camera_view_projection_non_finite",
                          "Camera view-projection matrix must be finite"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    if (packet.flags.alpha_to_coverage && target.samples != 4U) {
        diagnostic = {"indexed_alpha_to_coverage_sample_count",
                      "Alpha-to-coverage requires a 4x indexed color target"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (packet.flags.depth_write && !packet.flags.depth_test) {
        diagnostic = {"indexed_depth_write_without_test",
                      "Indexed static-mesh depth writes require depth testing"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (request.clear_depth && request.depth_attachment == nullptr) {
        diagnostic = {"indexed_depth_attachment_missing",
                      "A depth clear requires a persistent depth attachment"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (packet.vertex_count == 0U || packet.vertex_count > max_indexed_static_mesh_vertices ||
        packet.index_count == 0U || packet.index_count > max_indexed_static_mesh_indices ||
        packet.index_count % 3U != 0U) {
        diagnostic = {"indexed_static_mesh_range_invalid",
                      "Indexed static-mesh vertex/index counts are outside bounded triangle-list limits"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }

    const PipelineProgram& pipeline = *request.pipeline;
    const PipelineValidationResult pipeline_validation = validate_pipeline(pipeline);
    if (!pipeline_validation.valid) {
        if (!pipeline_validation.diagnostics.empty()) {
            const PipelineDiagnostic& failure = pipeline_validation.diagnostics.front();
            diagnostic = {"indexed_pipeline_" + failure.code, failure.message};
        } else {
            diagnostic = {"indexed_pipeline_invalid", "Pipeline validation failed without a diagnostic"};
        }
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.targets.colors.size() != 1U) {
        diagnostic = {"indexed_pipeline_state_unsupported",
                      "Indexed static-mesh execution requires one color target"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (!valid_render_sample_count(pipeline.targets.colors[0].samples)) {
        diagnostic = {"indexed_pipeline_target_samples_unsupported",
                      "Indexed pipeline color targets require exactly 1 or 4 samples"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (pipeline.targets.colors[0].samples != target.samples) {
        diagnostic = {"indexed_pipeline_target_samples_mismatch",
                      "Indexed pipeline color samples must match the supplied color target"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.targets.colors[0].format != expected_format) {
        diagnostic = {"indexed_pipeline_state_unsupported",
                      "Indexed pipeline color format must match the supplied color target"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (pipeline.blend.alpha_to_coverage && target.samples != 4U) {
        diagnostic = {"indexed_alpha_to_coverage_sample_count",
                      "Alpha-to-coverage requires a 4x indexed color target"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const bool pipeline_wireframe = pipeline.raster.fill == PipelineFillMode::wireframe;
    if (pipeline_wireframe != packet.flags.wireframe) {
        diagnostic = {"indexed_fill_state_mismatch",
                      "Pipeline fill state must match the draw packet"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.blend.enabled != packet.flags.blend_enabled ||
        pipeline.blend.alpha_to_coverage != packet.flags.alpha_to_coverage) {
        diagnostic = {"indexed_blend_state_mismatch",
                      "Pipeline blend state must match the draw packet"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const bool has_sampled_texture = request.sampled_binding.texture != nullptr;
    const bool has_sampler = request.sampled_binding.sampler != nullptr;
    const bool has_normal_texture = request.normal_binding.texture != nullptr;
    const bool has_normal_sampler = request.normal_binding.sampler != nullptr;
    const bool has_maps_texture = request.maps_binding.texture != nullptr;
    const bool has_maps_sampler = request.maps_binding.sampler != nullptr;
    const bool has_damage_texture = request.damage_binding.texture != nullptr;
    const bool has_damage_sampler = request.damage_binding.sampler != nullptr;
    const bool has_damage_mask_texture = request.damage_mask_binding.texture != nullptr;
    const bool has_damage_mask_sampler = request.damage_mask_binding.sampler != nullptr;
    const bool has_material_buffer = request.material_binding.buffer != nullptr;
    const bool has_material_range = request.material_binding.offset_bytes != 0U ||
                                    request.material_binding.range_bytes != 0U;
    const bool has_frame_buffer = request.frame_binding.buffer != nullptr;
    const bool has_frame_range = request.frame_binding.offset_bytes != 0U ||
                                 request.frame_binding.range_bytes != 0U;
    const bool has_shadow_map = std::any_of(
        request.directional_shadow_binding.maps.begin(),
        request.directional_shadow_binding.maps.end(),
        [](const DepthAttachment* map) { return map != nullptr; });
    const bool has_shadow_sampler = request.directional_shadow_binding.sampler != nullptr;
    const bool has_shadow_constants = request.directional_shadow_binding.constants != nullptr;
    const bool has_shadow_range =
        request.directional_shadow_binding.constants_offset_bytes != 0U ||
        request.directional_shadow_binding.constants_range_bytes != 0U;
    const bool shadow_declaration =
        pipeline_declares_directional_shadow_receiver(pipeline);
    const bool has_directional_shadow_binding =
        has_shadow_map || has_shadow_sampler || has_shadow_constants || has_shadow_range;
    // Check this before classifying the material layout. A partial receiver
    // declaration is intentionally unsupported, but a valid non-receiver
    // pipeline must fail closed when a caller supplies even one directional
    // shadow resource. Otherwise the error can be obscured by the unrelated
    // material-layout classifier and a caller may accidentally treat the
    // shadow binding as staged rather than rejected.
    if (!shadow_declaration && has_directional_shadow_binding) {
        diagnostic = {"indexed_directional_shadow_binding_unexpected",
                      "A pipeline without the receiver extension cannot receive directional-shadow resources"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const IndexedPortableResourceLayout resource_layout =
        classify_indexed_portable_resource_layout(pipeline);
    if (resource_layout == IndexedPortableResourceLayout::resource_free) {
        if (has_sampled_texture || has_sampler || has_normal_texture || has_normal_sampler ||
            has_maps_texture || has_maps_sampler ||
            request.detail_binding.texture != nullptr || request.detail_binding.sampler != nullptr ||
            request.normal_detail_binding.texture != nullptr ||
            request.normal_detail_binding.sampler != nullptr ||
            has_damage_texture || has_damage_sampler || has_damage_mask_texture ||
            has_damage_mask_sampler ||
            has_material_buffer || has_material_range ||
            has_frame_buffer || has_frame_range ||
            has_shadow_map || has_shadow_sampler || has_shadow_constants || has_shadow_range ||
            request.resource_authority != IndexedResourceAuthority::packet_contract) {
            diagnostic = {"indexed_resource_binding_unexpected",
                          "A resource-free pipeline cannot receive explicit material bindings"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (!packet.resources.empty()) {
            diagnostic = {"indexed_static_mesh_resources_unsupported",
                          "The resource-free indexed baseline cannot execute material packet resources"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
    } else {
        if (resource_layout == IndexedPortableResourceLayout::unsupported) {
            diagnostic = {"indexed_resource_layout_unsupported",
                          "The portable material ABI requires the bounded diffuse, normal, txMaps, detail-stack, or damage layout"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const bool material_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_with_constants ||
            resource_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        const bool frame_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_with_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        const bool normal_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        const bool maps_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        const bool detail_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        const bool normal_detail_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_detail_stack_with_constants_and_frame;
        const bool damage_declaration =
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_with_constants_and_frame ||
            resource_layout == IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
        if (request.resource_authority != IndexedResourceAuthority::explicit_bindings) {
            diagnostic = {"indexed_resource_execution_staged",
                          "Material resources require explicit request-local binding authority"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        if (!has_sampled_texture || !has_sampler) {
            diagnostic = {"indexed_resource_binding_missing",
                          "The portable diffuse baseline requires both a sampled texture and sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (normal_declaration != has_normal_texture ||
            normal_declaration != has_normal_sampler) {
            diagnostic = {normal_declaration ? "indexed_normal_binding_missing"
                                             : "indexed_normal_binding_unexpected",
                          normal_declaration
                              ? "The portable ksPerPixelNM ABI requires its normal texture and sampler"
                              : "The diffuse ABI cannot receive a normal texture or sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (maps_declaration != has_maps_texture || maps_declaration != has_maps_sampler) {
            diagnostic = {maps_declaration ? "indexed_maps_binding_missing"
                                           : "indexed_maps_binding_unexpected",
                          maps_declaration
                              ? "The portable txMaps ABI requires its maps texture and sampler"
                              : "The diffuse and normal ABIs cannot receive a maps texture or sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const bool has_detail_texture = request.detail_binding.texture != nullptr;
        const bool has_detail_sampler = request.detail_binding.sampler != nullptr;
        if (detail_declaration != has_detail_texture || detail_declaration != has_detail_sampler) {
            diagnostic = {detail_declaration ? "indexed_detail_binding_missing"
                                             : "indexed_detail_binding_unexpected",
                          detail_declaration
                              ? "The portable detail-stack ABI requires its detail texture and sampler"
                              : "The diffuse, normal, and txMaps ABIs cannot receive a detail texture or sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const bool has_normal_detail_texture = request.normal_detail_binding.texture != nullptr;
        const bool has_normal_detail_sampler = request.normal_detail_binding.sampler != nullptr;
        if (normal_detail_declaration != has_normal_detail_texture ||
            normal_detail_declaration != has_normal_detail_sampler) {
            diagnostic = {normal_detail_declaration ? "indexed_normal_detail_binding_missing"
                                                     : "indexed_normal_detail_binding_unexpected",
                          normal_detail_declaration
                              ? "The portable detail-stack ABI requires its normal-detail texture and sampler"
                              : "The diffuse, normal, and txMaps ABIs cannot receive a normal-detail texture or sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (damage_declaration != has_damage_texture || damage_declaration != has_damage_sampler) {
            diagnostic = {damage_declaration ? "indexed_damage_binding_missing"
                                             : "indexed_damage_binding_unexpected",
                          damage_declaration
                              ? "The portable damage ABI requires its damage texture and sampler"
                              : "The non-damage ABIs cannot receive a damage texture or sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (damage_declaration != has_damage_mask_texture ||
            damage_declaration != has_damage_mask_sampler) {
            diagnostic = {damage_declaration ? "indexed_damage_mask_binding_missing"
                                             : "indexed_damage_mask_binding_unexpected",
                          damage_declaration
                              ? "The portable damage ABI requires its damage-mask texture and sampler"
                              : "The non-damage ABIs cannot receive a damage-mask texture or sampler"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (material_declaration != has_material_buffer ||
            (!material_declaration && has_material_range)) {
            diagnostic = {material_declaration ? "indexed_material_buffer_missing"
                                               : "indexed_material_buffer_unexpected",
                          material_declaration
                              ? "The portable material ABI requires its constants buffer"
                              : "The diffuse-only ABI cannot receive a constants buffer"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (material_declaration) {
            const Buffer& material_buffer = *request.material_binding.buffer;
            if (material_buffer.backend() != texture.backend()) {
                diagnostic = {"indexed_material_buffer_backend_mismatch",
                              "Material constants and the color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const BufferDescription& material = material_buffer.info().description;
            if (material.usage != BufferUsage::uniform) {
                diagnostic = {"indexed_material_buffer_usage_invalid",
                              "The portable material buffer requires exclusive uniform usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (request.material_binding.offset_bytes % portable_material_buffer_view_bytes != 0U ||
                request.material_binding.range_bytes != portable_material_buffer_view_bytes) {
                diagnostic = {"indexed_material_buffer_alignment_invalid",
                              "Material buffer views require a 256-byte aligned offset and 256-byte range"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (request.material_binding.offset_bytes > material.size_bytes ||
                static_cast<std::uint64_t>(request.material_binding.range_bytes) >
                    material.size_bytes - request.material_binding.offset_bytes) {
                diagnostic = {"indexed_material_buffer_range_invalid",
                              "The material buffer view exceeds the declared buffer size"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        if (frame_declaration != has_frame_buffer || (!frame_declaration && has_frame_range)) {
            diagnostic = {frame_declaration ? "indexed_frame_buffer_missing"
                                            : "indexed_frame_buffer_unexpected",
                          frame_declaration
                              ? "The portable lighting ABI requires its frame constants buffer"
                              : "The diffuse/material ABI cannot receive a frame constants buffer"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (frame_declaration) {
            const Buffer& frame_buffer = *request.frame_binding.buffer;
            if (frame_buffer.backend() != texture.backend()) {
                diagnostic = {"indexed_frame_buffer_backend_mismatch",
                              "Frame constants and the color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const BufferDescription& frame = frame_buffer.info().description;
            if (frame.usage != BufferUsage::uniform) {
                diagnostic = {"indexed_frame_buffer_usage_invalid",
                              "The portable frame constants buffer requires exclusive uniform usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (request.frame_binding.offset_bytes % portable_frame_buffer_view_bytes != 0U ||
                request.frame_binding.range_bytes != portable_frame_buffer_view_bytes) {
                diagnostic = {"indexed_frame_buffer_alignment_invalid",
                              "Frame constants buffer views require a 256-byte aligned offset and 256-byte range"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (request.frame_binding.offset_bytes > frame.size_bytes ||
                static_cast<std::uint64_t>(request.frame_binding.range_bytes) >
                    frame.size_bytes - request.frame_binding.offset_bytes) {
                diagnostic = {"indexed_frame_buffer_range_invalid",
                              "The frame constants buffer view exceeds the declared buffer size"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        const bool has_all_shadow_maps = std::all_of(
            request.directional_shadow_binding.maps.begin(),
            request.directional_shadow_binding.maps.end(),
            [](const DepthAttachment* map) { return map != nullptr; });
        if (shadow_declaration != has_all_shadow_maps ||
            shadow_declaration != has_shadow_sampler ||
            shadow_declaration != has_shadow_constants ||
            (!shadow_declaration && has_shadow_range)) {
            diagnostic = {shadow_declaration
                              ? "indexed_directional_shadow_binding_missing"
                              : "indexed_directional_shadow_binding_unexpected",
                          shadow_declaration
                              ? "The directional-shadow receiver requires exactly three maps, one sampler, and one constants buffer"
                              : "A pipeline without the receiver extension cannot receive directional-shadow resources"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (shadow_declaration) {
            const DepthAttachmentDescription* first_description = nullptr;
            for (std::size_t map_index = 0U;
                 map_index < request.directional_shadow_binding.maps.size();
                 ++map_index) {
                const DepthAttachment* map =
                    request.directional_shadow_binding.maps[map_index];
                for (std::size_t previous = 0U; previous < map_index; ++previous) {
                    if (map == request.directional_shadow_binding.maps[previous]) {
                        diagnostic = {"indexed_directional_shadow_map_duplicate",
                                      "Directional-shadow receiver cascades must use three distinct maps"};
                        return IndexedStaticMeshDrawStatus::invalid_request;
                    }
                }
                if (map->backend() != texture.backend()) {
                    diagnostic = {"indexed_directional_shadow_backend_mismatch",
                                  "Directional-shadow maps and the color target must use the same backend"};
                    return IndexedStaticMeshDrawStatus::unsupported;
                }
                if (map == request.depth_attachment) {
                    diagnostic = {"indexed_directional_shadow_feedback_loop",
                                  "A writable main-pass depth attachment cannot be sampled as a directional shadow"};
                    return IndexedStaticMeshDrawStatus::invalid_request;
                }
                const DepthAttachmentDescription& description = map->info().description;
                if (!description.shader_readable || description.samples != 1U ||
                    description.format != DepthAttachmentFormat::d32_float) {
                    diagnostic = {"indexed_directional_shadow_description_unsupported",
                                  "Directional-shadow receiver maps require shader-readable single-sample D32 attachments"};
                    return IndexedStaticMeshDrawStatus::unsupported;
                }
                if (first_description != nullptr &&
                    (description.width != first_description->width ||
                     description.height != first_description->height)) {
                    diagnostic = {"indexed_directional_shadow_dimensions_mismatch",
                                  "All directional-shadow receiver maps must have equal dimensions"};
                    return IndexedStaticMeshDrawStatus::invalid_request;
                }
                first_description = &description;
            }
            const Sampler& shadow_sampler =
                *request.directional_shadow_binding.sampler;
            if (shadow_sampler.backend() != texture.backend()) {
                diagnostic = {"indexed_directional_shadow_sampler_backend_mismatch",
                              "The directional-shadow sampler and color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            Diagnostic shadow_sampler_diagnostic;
            if (validate_sampler_description(shadow_sampler.info().description,
                                             shadow_sampler_diagnostic) !=
                SamplerStatus::ready) {
                diagnostic = {shadow_sampler_diagnostic.code.empty()
                                  ? "indexed_directional_shadow_sampler_invalid"
                                  : "indexed_directional_shadow_" +
                                        shadow_sampler_diagnostic.code,
                              shadow_sampler_diagnostic.message};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            const SamplerDescription& sampler_description =
                shadow_sampler.info().description;
            if (sampler_description.min_filter != SamplerFilter::nearest ||
                sampler_description.mag_filter != SamplerFilter::nearest ||
                sampler_description.mip_filter != SamplerFilter::nearest ||
                sampler_description.address_u != SamplerAddressMode::clamp_to_edge ||
                sampler_description.address_v != SamplerAddressMode::clamp_to_edge ||
                sampler_description.address_w != SamplerAddressMode::clamp_to_edge ||
                sampler_description.compare != SamplerCompare::disabled) {
                diagnostic = {"indexed_directional_shadow_sampler_contract_invalid",
                              "Directional-shadow PCF requires nearest clamp-to-edge sampling with comparison disabled"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            const Buffer& shadow_constants =
                *request.directional_shadow_binding.constants;
            if (shadow_constants.backend() != texture.backend()) {
                diagnostic = {"indexed_directional_shadow_constants_backend_mismatch",
                              "Directional-shadow constants and the color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const BufferDescription& constants_description =
                shadow_constants.info().description;
            if (constants_description.usage != BufferUsage::uniform) {
                diagnostic = {"indexed_directional_shadow_constants_usage_invalid",
                              "Directional-shadow constants require exclusive uniform-buffer usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            const std::uint64_t shadow_offset =
                request.directional_shadow_binding.constants_offset_bytes;
            const std::uint64_t shadow_range =
                request.directional_shadow_binding.constants_range_bytes;
            if (shadow_offset % portable_directional_shadow_buffer_view_bytes != 0U ||
                (shadow_range != portable_directional_shadow_buffer_view_bytes &&
                 shadow_range != stock_directional_shadow_buffer_view_bytes)) {
                diagnostic = {"indexed_directional_shadow_constants_alignment_invalid",
                              "Directional-shadow constants require a 256-byte aligned offset and either the portable 256-byte or recovered stock 208-byte range"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (shadow_offset > constants_description.size_bytes ||
                shadow_range > constants_description.size_bytes - shadow_offset) {
                diagnostic = {"indexed_directional_shadow_constants_range_invalid",
                              "Directional-shadow constants exceed the declared buffer size"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        const Texture& sampled_texture = *request.sampled_binding.texture;
        const Sampler& sampler = *request.sampled_binding.sampler;
        if (&sampled_texture == &texture) {
            diagnostic = {"indexed_resource_feedback_loop",
                          "The color target cannot also be sampled by the same draw"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (sampled_texture.backend() != texture.backend() || sampler.backend() != texture.backend()) {
            diagnostic = {"indexed_resource_backend_mismatch",
                          "Sampled texture, sampler, and color target must use the same backend"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const TextureDescription& sampled = sampled_texture.info().description;
        const auto sampled_usage = static_cast<std::uint32_t>(sampled.usage);
        const auto forbidden_usage = static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                                     static_cast<std::uint32_t>(TextureUsage::storage) |
                                     static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((sampled_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
            diagnostic = {"indexed_resource_texture_usage_invalid",
                          "The diffuse texture requires sampled usage"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if ((sampled_usage & forbidden_usage) != 0U) {
            diagnostic = {"indexed_resource_texture_usage_unsupported",
                          "The portable diffuse baseline rejects writable or attachment texture usage"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        if (sampled.width == 0U || sampled.height == 0U || sampled.mip_levels == 0U ||
            sampled.array_layers != 1U || sampled.samples != 1U ||
            !portable_sampled_color_format(sampled.format, true) ||
            (texture_format_is_compressed(sampled.format) &&
             sampled.mutability != TextureMutability::immutable)) {
            diagnostic = {"indexed_resource_texture_description_unsupported",
                          "The portable diffuse baseline requires one-layer RGBA8, BGRA8, BC1, BC3, or BC7 texture data"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        Diagnostic sampler_diagnostic;
        const SamplerStatus sampler_status =
            validate_sampler_description(sampler.info().description, sampler_diagnostic);
        if (sampler_status != SamplerStatus::ready) {
            diagnostic = {sampler_diagnostic.code.empty() ? "indexed_resource_sampler_invalid"
                                                           : "indexed_resource_" + sampler_diagnostic.code,
                          sampler_diagnostic.message};
            return sampler_status == SamplerStatus::unsupported
                       ? IndexedStaticMeshDrawStatus::unsupported
                       : IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (normal_declaration) {
            const Texture& normal_texture = *request.normal_binding.texture;
            const Sampler& normal_sampler = *request.normal_binding.sampler;
            if (&normal_texture == &texture) {
                diagnostic = {"indexed_normal_feedback_loop",
                              "The color target cannot also be the normal texture"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (normal_texture.backend() != texture.backend() ||
                normal_sampler.backend() != texture.backend()) {
                diagnostic = {"indexed_normal_backend_mismatch",
                              "Normal texture, sampler, and color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const TextureDescription& normal = normal_texture.info().description;
            const auto normal_usage = static_cast<std::uint32_t>(normal.usage);
            if ((normal_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                diagnostic = {"indexed_normal_texture_usage_invalid",
                              "The normal texture requires sampled usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if ((normal_usage & forbidden_usage) != 0U) {
                diagnostic = {"indexed_normal_texture_usage_unsupported",
                              "The portable normal path rejects writable or attachment texture usage"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            if (normal.width == 0U || normal.height == 0U || normal.mip_levels == 0U ||
                normal.array_layers != 1U || normal.samples != 1U ||
                (normal.format != TextureFormat::rgba8_unorm &&
                 normal.format != TextureFormat::rgba8_srgb &&
                 normal.format != TextureFormat::bgra8_unorm &&
                 normal.format != TextureFormat::bgra8_srgb)) {
                diagnostic = {"indexed_normal_texture_description_unsupported",
                              "The portable normal path requires one-layer RGBA8 or BGRA8 texture data"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            Diagnostic normal_sampler_diagnostic;
            const SamplerStatus normal_sampler_status = validate_sampler_description(
                normal_sampler.info().description, normal_sampler_diagnostic);
            if (normal_sampler_status != SamplerStatus::ready) {
                diagnostic = {normal_sampler_diagnostic.code.empty()
                                  ? "indexed_normal_sampler_invalid"
                                  : "indexed_normal_" + normal_sampler_diagnostic.code,
                              normal_sampler_diagnostic.message};
                return normal_sampler_status == SamplerStatus::unsupported
                           ? IndexedStaticMeshDrawStatus::unsupported
                           : IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        if (maps_declaration) {
            const Texture& maps_texture = *request.maps_binding.texture;
            const Sampler& maps_sampler = *request.maps_binding.sampler;
            if (&maps_texture == &texture) {
                diagnostic = {"indexed_maps_feedback_loop",
                              "The color target cannot also be the maps texture"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (maps_texture.backend() != texture.backend() ||
                maps_sampler.backend() != texture.backend()) {
                diagnostic = {"indexed_maps_backend_mismatch",
                              "Maps texture, sampler, and color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const TextureDescription& maps = maps_texture.info().description;
            const auto maps_usage = static_cast<std::uint32_t>(maps.usage);
            if ((maps_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                diagnostic = {"indexed_maps_texture_usage_invalid",
                              "The maps texture requires sampled usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if ((maps_usage & forbidden_usage) != 0U) {
                diagnostic = {"indexed_maps_texture_usage_unsupported",
                              "The portable maps path rejects writable or attachment texture usage"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            if (maps.width == 0U || maps.height == 0U || maps.mip_levels == 0U ||
                maps.array_layers != 1U || maps.samples != 1U ||
                !portable_sampled_color_format(maps.format, false) ||
                (texture_format_is_compressed(maps.format) &&
                 maps.mutability != TextureMutability::immutable)) {
                diagnostic = {"indexed_maps_texture_description_unsupported",
                              "The portable maps path requires one-layer linear RGBA8, BGRA8, BC1, BC3, or BC7 UNORM texture data"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            Diagnostic maps_sampler_diagnostic;
            const SamplerStatus maps_sampler_status = validate_sampler_description(
                maps_sampler.info().description, maps_sampler_diagnostic);
            if (maps_sampler_status != SamplerStatus::ready) {
                diagnostic = {maps_sampler_diagnostic.code.empty()
                                  ? "indexed_maps_sampler_invalid"
                                  : "indexed_maps_" + maps_sampler_diagnostic.code,
                              maps_sampler_diagnostic.message};
                return maps_sampler_status == SamplerStatus::unsupported
                           ? IndexedStaticMeshDrawStatus::unsupported
                           : IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        if (detail_declaration) {
            const Texture& detail_texture = *request.detail_binding.texture;
            const Sampler& detail_sampler = *request.detail_binding.sampler;
            if (&detail_texture == &texture) {
                diagnostic = {"indexed_detail_feedback_loop",
                              "The color target cannot also be the detail texture"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (detail_texture.backend() != texture.backend() ||
                detail_sampler.backend() != texture.backend()) {
                diagnostic = {"indexed_detail_backend_mismatch",
                              "Detail texture, sampler, and color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const TextureDescription& detail = detail_texture.info().description;
            const auto detail_usage = static_cast<std::uint32_t>(detail.usage);
            if ((detail_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                diagnostic = {"indexed_detail_texture_usage_invalid",
                              "The detail texture requires sampled usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if ((detail_usage & forbidden_usage) != 0U) {
                diagnostic = {"indexed_detail_texture_usage_unsupported",
                              "The portable detail path rejects writable or attachment texture usage"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            if (detail.width == 0U || detail.height == 0U || detail.mip_levels == 0U ||
                detail.array_layers != 1U || detail.samples != 1U ||
                !portable_sampled_color_format(detail.format, true) ||
                (texture_format_is_compressed(detail.format) &&
                 detail.mutability != TextureMutability::immutable)) {
                diagnostic = {"indexed_detail_texture_description_unsupported",
                              "The portable detail path requires one-layer RGBA8, BGRA8, BC1, BC3, or BC7 texture data"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            Diagnostic detail_sampler_diagnostic;
            const SamplerStatus detail_sampler_status = validate_sampler_description(
                detail_sampler.info().description, detail_sampler_diagnostic);
            if (detail_sampler_status != SamplerStatus::ready) {
                diagnostic = {detail_sampler_diagnostic.code.empty()
                                  ? "indexed_detail_sampler_invalid"
                                  : "indexed_detail_" + detail_sampler_diagnostic.code,
                              detail_sampler_diagnostic.message};
                return detail_sampler_status == SamplerStatus::unsupported
                           ? IndexedStaticMeshDrawStatus::unsupported
                           : IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        if (normal_detail_declaration) {
            const Texture& normal_detail_texture = *request.normal_detail_binding.texture;
            const Sampler& normal_detail_sampler = *request.normal_detail_binding.sampler;
            if (&normal_detail_texture == &texture) {
                diagnostic = {"indexed_normal_detail_feedback_loop",
                              "The color target cannot also be the normal-detail texture"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (normal_detail_texture.backend() != texture.backend() ||
                normal_detail_sampler.backend() != texture.backend()) {
                diagnostic = {"indexed_normal_detail_backend_mismatch",
                              "Normal-detail texture, sampler, and color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const TextureDescription& normal_detail = normal_detail_texture.info().description;
            const auto normal_detail_usage = static_cast<std::uint32_t>(normal_detail.usage);
            if ((normal_detail_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                diagnostic = {"indexed_normal_detail_texture_usage_invalid",
                              "The normal-detail texture requires sampled usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if ((normal_detail_usage & forbidden_usage) != 0U) {
                diagnostic = {"indexed_normal_detail_texture_usage_unsupported",
                              "The portable normal-detail path rejects writable or attachment texture usage"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            if (normal_detail.width == 0U || normal_detail.height == 0U ||
                normal_detail.mip_levels == 0U || normal_detail.array_layers != 1U ||
                normal_detail.samples != 1U ||
                (normal_detail.format != TextureFormat::rgba8_unorm &&
                 normal_detail.format != TextureFormat::bgra8_unorm)) {
                diagnostic = {"indexed_normal_detail_texture_description_unsupported",
                              "The portable normal-detail path requires one-layer linear RGBA8 or BGRA8 UNORM data"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            Diagnostic normal_detail_sampler_diagnostic;
            const SamplerStatus normal_detail_sampler_status = validate_sampler_description(
                normal_detail_sampler.info().description, normal_detail_sampler_diagnostic);
            if (normal_detail_sampler_status != SamplerStatus::ready) {
                diagnostic = {normal_detail_sampler_diagnostic.code.empty()
                                  ? "indexed_normal_detail_sampler_invalid"
                                  : "indexed_normal_detail_" + normal_detail_sampler_diagnostic.code,
                              normal_detail_sampler_diagnostic.message};
                return normal_detail_sampler_status == SamplerStatus::unsupported
                           ? IndexedStaticMeshDrawStatus::unsupported
                           : IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        const auto validate_damage_texture =
            [&](const IndexedSampledTextureBinding& binding, const char* role,
                bool allow_srgb) -> IndexedStaticMeshDrawStatus {
                const Texture& source_texture = *binding.texture;
                const Sampler& source_sampler = *binding.sampler;
                const std::string code_prefix = std::string("indexed_") + role;
                if (&source_texture == &texture) {
                    diagnostic = {code_prefix + "_feedback_loop",
                                  "The color target cannot also be a damage input texture"};
                    return IndexedStaticMeshDrawStatus::invalid_request;
                }
                if (source_texture.backend() != texture.backend() ||
                    source_sampler.backend() != texture.backend()) {
                    diagnostic = {code_prefix + "_backend_mismatch",
                                  "Damage input texture, sampler, and color target must use the same backend"};
                    return IndexedStaticMeshDrawStatus::unsupported;
                }
                const TextureDescription& source = source_texture.info().description;
                const auto source_usage = static_cast<std::uint32_t>(source.usage);
                if ((source_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                    diagnostic = {code_prefix + "_texture_usage_invalid",
                                  "A damage input texture requires sampled usage"};
                    return IndexedStaticMeshDrawStatus::invalid_request;
                }
                if ((source_usage & forbidden_usage) != 0U) {
                    diagnostic = {code_prefix + "_texture_usage_unsupported",
                                  "The portable damage path rejects writable or attachment texture usage"};
                    return IndexedStaticMeshDrawStatus::unsupported;
                }
                if (source.width == 0U || source.height == 0U || source.mip_levels == 0U ||
                    source.array_layers != 1U || source.samples != 1U ||
                    !portable_sampled_color_format(source.format, allow_srgb) ||
                    (texture_format_is_compressed(source.format) &&
                     source.mutability != TextureMutability::immutable)) {
                    diagnostic = {code_prefix + "_texture_description_unsupported",
                                  "The portable damage path requires one-layer RGBA8, BGRA8, BC1, BC3, or BC7 texture data"};
                    return IndexedStaticMeshDrawStatus::unsupported;
                }
                Diagnostic damage_sampler_diagnostic;
                const SamplerStatus damage_sampler_status = validate_sampler_description(
                    source_sampler.info().description, damage_sampler_diagnostic);
                if (damage_sampler_status != SamplerStatus::ready) {
                    diagnostic = {damage_sampler_diagnostic.code.empty()
                                      ? code_prefix + "_sampler_invalid"
                                      : code_prefix + "_" + damage_sampler_diagnostic.code,
                                  damage_sampler_diagnostic.message};
                    return damage_sampler_status == SamplerStatus::unsupported
                               ? IndexedStaticMeshDrawStatus::unsupported
                               : IndexedStaticMeshDrawStatus::invalid_request;
                }
                return IndexedStaticMeshDrawStatus::ready;
            };
        if (damage_declaration) {
            const IndexedStaticMeshDrawStatus damage_status =
                validate_damage_texture(request.damage_binding, "damage", true);
            if (damage_status != IndexedStaticMeshDrawStatus::ready) return damage_status;
            const IndexedStaticMeshDrawStatus mask_status =
                validate_damage_texture(request.damage_mask_binding, "damage_mask", false);
            if (mask_status != IndexedStaticMeshDrawStatus::ready) return mask_status;
        }
    }
    if (pipeline.depth.test_enabled != packet.flags.depth_test ||
        pipeline.depth.write_enabled != packet.flags.depth_write) {
        diagnostic = {"indexed_depth_state_mismatch",
                      "Pipeline depth test/write state must match the draw packet"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.depth.test_enabled && request.depth_attachment == nullptr) {
        diagnostic = {"indexed_depth_attachment_missing",
                      "Depth-enabled indexed static-mesh drawing requires a depth attachment"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.targets.has_depth != (request.depth_attachment != nullptr)) {
        diagnostic = {"indexed_depth_target_binding_mismatch",
                      "Pipeline depth target declaration must match the supplied depth attachment"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.targets.has_depth &&
        (pipeline.targets.depth.format != PipelineRenderTargetFormat::depth32_float ||
         pipeline.targets.depth.samples != target.samples)) {
        diagnostic = {"indexed_pipeline_depth_target_invalid",
                      "Indexed pipeline depth samples must match the supplied color target and use D32"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (pipeline.depth.test_enabled) {
        if (pipeline.depth.compare != PipelineCompareOperation::less) {
            diagnostic = {"indexed_depth_compare_unsupported",
                          "Indexed static-mesh execution requires the source-evidenced LESS depth comparison"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
    }
    bool has_vertex = false;
    bool has_fragment = false;
    for (const PipelineShaderModule& shader : pipeline.shaders) {
        if (shader.stage == PipelineShaderStage::vertex) has_vertex = true;
        else if (shader.stage == PipelineShaderStage::fragment) has_fragment = true;
        else {
            diagnostic = {"indexed_shader_stage_unsupported",
                          "Indexed static-mesh baseline supports only vertex and fragment shaders"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const ShaderStage stage = shader.stage == PipelineShaderStage::vertex ? ShaderStage::vertex : ShaderStage::fragment;
        const std::span<const std::byte> bytecode(reinterpret_cast<const std::byte*>(shader.bytes.data()),
                                                   shader.bytes.size());
        Diagnostic shader_diagnostic;
        const ShaderModuleStatus shader_status =
            validate_shader_module_description({stage, bytecode}, shader_diagnostic);
        if (shader_status != ShaderModuleStatus::ready) {
            const std::string code = shader_diagnostic.code.empty()
                                         ? "indexed_shader_invalid"
                                         : "indexed_shader_" + shader_diagnostic.code;
            diagnostic = {code, shader_diagnostic.message};
            return shader_status == ShaderModuleStatus::unsupported
                       ? IndexedStaticMeshDrawStatus::unsupported
                       : IndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    if (pipeline.shaders.size() != 2U || !has_vertex || !has_fragment) {
        diagnostic = {"indexed_shader_pair_invalid",
                      "Indexed static-mesh baseline requires one vertex and one fragment shader"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const std::uint32_t expected_stride_floats = static_mesh ? 11U : 19U;
    if (packet.vertex_stride_floats != expected_stride_floats ||
        static_cast<std::uint64_t>(packet.vertex_stride_floats) * sizeof(float) !=
            pipeline.vertex_layout.stride) {
        diagnostic = {"indexed_static_mesh_stride_mismatch",
                      "Draw packet stride must be exactly 11 or 19 float32 values and match the executable pipeline byte stride"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const Buffer& vertex_buffer = *request.vertex_buffer;
    const Buffer& index_buffer = *request.index_buffer;
    if (texture.backend() != vertex_buffer.backend() || texture.backend() != index_buffer.backend()) {
        diagnostic = {"indexed_static_mesh_backend_mismatch",
                      "Target and indexed static-mesh buffers must belong to the same backend"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const std::uint32_t vertex_usage = static_cast<std::uint32_t>(vertex_buffer.info().description.usage);
    const std::uint32_t index_usage = static_cast<std::uint32_t>(index_buffer.info().description.usage);
    if ((static_mesh && vertex_buffer.info().description.mutability != BufferMutability::immutable) ||
        (skinned_mesh && vertex_buffer.info().description.mutability != BufferMutability::mutable_data) ||
        index_buffer.info().description.mutability != BufferMutability::immutable) {
        diagnostic = {"indexed_static_mesh_buffer_mutable",
                      "Indexed drawing requires an immutable index buffer, an immutable static vertex buffer, and a mutable skinned vertex buffer"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if ((vertex_usage & static_cast<std::uint32_t>(BufferUsage::vertex)) == 0U) {
        diagnostic = {"indexed_static_mesh_vertex_usage_invalid",
                      "Vertex buffer lacks vertex usage"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if ((index_usage & static_cast<std::uint32_t>(BufferUsage::index)) == 0U) {
        diagnostic = {"indexed_static_mesh_index_usage_invalid",
                      "Index buffer lacks index usage"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const std::uint64_t stride = pipeline.vertex_layout.stride;
    const std::uint64_t vertex_offset = static_cast<std::uint64_t>(packet.vertex_offset);
    const std::uint64_t index_offset = static_cast<std::uint64_t>(packet.index_offset);
    if (vertex_offset > std::numeric_limits<std::uint64_t>::max() / stride ||
        index_offset > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
        diagnostic = {"indexed_static_mesh_offset_overflow",
                      "Indexed static-mesh element offsets overflow byte arithmetic"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const std::uint64_t vertex_bytes = vertex_offset * stride;
    const std::uint64_t index_bytes = index_offset * sizeof(std::uint16_t);
    const std::uint64_t vertex_span = static_cast<std::uint64_t>(packet.vertex_count) * stride;
    const std::uint64_t index_span = static_cast<std::uint64_t>(packet.index_count) * sizeof(std::uint16_t);
    if (vertex_bytes > vertex_buffer.info().description.size_bytes ||
        vertex_span > vertex_buffer.info().description.size_bytes - vertex_bytes ||
        index_bytes > index_buffer.info().description.size_bytes ||
        index_span > index_buffer.info().description.size_bytes - index_bytes) {
        diagnostic = {"indexed_static_mesh_buffer_range_invalid",
                      "Indexed static-mesh draw range exceeds its buffer"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    return IndexedStaticMeshDrawStatus::ready;
}

IndexedStaticMeshBatchStatus validate_overlay_line_draw_request(
    const Texture& texture, const OverlayLineDrawRequest& request,
    Diagnostic& diagnostic) {
    if (request.pipeline == nullptr || request.vertex_buffer == nullptr) {
        diagnostic = {"overlay_line_handle_missing",
                      "Overlay line drawing requires a pipeline and vertex buffer"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (request.vertex_count < 2U || request.vertex_count % 2U != 0U) {
        diagnostic = {"overlay_line_vertex_count_invalid",
                      "Overlay line vertex count must contain complete line pairs"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (request.vertex_count > max_overlay_line_vertices) {
        diagnostic = {"overlay_line_vertex_limit",
                      "Overlay line vertex count exceeds the bounded limit"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const PipelineProgram& pipeline = *request.pipeline;
    const PipelineValidationResult pipeline_validation = validate_pipeline(pipeline);
    if (!pipeline_validation.valid) {
        diagnostic = pipeline_validation.diagnostics.empty()
                         ? Diagnostic{"overlay_line_pipeline_invalid",
                                      "Overlay line pipeline validation failed"}
                         : Diagnostic{"overlay_line_pipeline_" +
                                          pipeline_validation.diagnostics.front().code,
                                      pipeline_validation.diagnostics.front().message};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.shaders.size() != 2U ||
        std::count_if(pipeline.shaders.begin(), pipeline.shaders.end(),
                      [](const PipelineShaderModule& shader) {
                          return shader.stage == PipelineShaderStage::vertex;
                      }) != 1 ||
        std::count_if(pipeline.shaders.begin(), pipeline.shaders.end(),
                      [](const PipelineShaderModule& shader) {
                          return shader.stage == PipelineShaderStage::fragment;
                      }) != 1) {
        diagnostic = {"overlay_line_shader_pair_invalid",
                      "Overlay line pipelines require one vertex and one fragment shader"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.vertex_layout.stride != sizeof(OverlayLineVertex) ||
        pipeline.vertex_layout.attributes.size() != 2U) {
        diagnostic = {"overlay_line_vertex_layout_invalid",
                      "Overlay line pipelines require the fixed position-color vertex ABI"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const PipelineVertexAttribute expected_position{
        PipelineVertexSemantic::position,
        PipelineVertexAttributeFormat::float32x3, 0U, 0U};
    const PipelineVertexAttribute expected_color{
        PipelineVertexSemantic::color,
        PipelineVertexAttributeFormat::float32x3, 1U,
        static_cast<std::uint32_t>(3U * sizeof(float))};
    const auto same_attribute = [](const PipelineVertexAttribute& left,
                                   const PipelineVertexAttribute& right) {
        return left.semantic == right.semantic && left.format == right.format &&
               left.location == right.location && left.offset == right.offset;
    };
    if (!same_attribute(pipeline.vertex_layout.attributes[0], expected_position) ||
        !same_attribute(pipeline.vertex_layout.attributes[1], expected_color)) {
        diagnostic = {"overlay_line_vertex_layout_invalid",
                      "Overlay line attributes must be position float3 at 0 and color float3 at 1"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const TextureDescription& target = texture.info().description;
    const auto expected_format = [&] {
        switch (target.format) {
        case TextureFormat::rgba8_unorm: return PipelineRenderTargetFormat::rgba8_unorm;
        case TextureFormat::rgba8_srgb: return PipelineRenderTargetFormat::rgba8_srgb;
        case TextureFormat::bgra8_unorm: return PipelineRenderTargetFormat::bgra8_unorm;
        case TextureFormat::bgra8_srgb: return PipelineRenderTargetFormat::bgra8_srgb;
        default: return PipelineRenderTargetFormat::unknown;
        }
    }();
    if (expected_format == PipelineRenderTargetFormat::unknown) {
        diagnostic = {"overlay_line_target_format_unsupported",
                      "Overlay lines support only RGBA8 and BGRA8 color targets"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    if (pipeline.targets.colors.size() != 1U ||
        pipeline.targets.colors[0].format != expected_format ||
        pipeline.targets.colors[0].samples != target.samples) {
        diagnostic = {"overlay_line_pipeline_target_mismatch",
                      "Overlay line pipeline target must match the batch color target"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.transform_contract != PipelineTransformContract::draw_matrices) {
        diagnostic = {"overlay_line_transform_contract_invalid",
                      "Overlay lines require the draw-matrices transform contract"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.raster.fill != PipelineFillMode::wireframe ||
        pipeline.raster.cull != PipelineCullMode::none) {
        diagnostic = {"overlay_line_topology_invalid",
                      "Overlay lines require line-list topology with culling disabled"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.depth.test_enabled || pipeline.depth.write_enabled) {
        diagnostic = {"overlay_line_depth_state_invalid",
                      "Overlay line depth testing and writes must be disabled"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.blend.enabled || pipeline.blend.alpha_to_coverage) {
        diagnostic = {"overlay_line_blend_state_invalid",
                      "Overlay line blending and alpha-to-coverage must be disabled"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (!pipeline.resources.empty()) {
        diagnostic = {"overlay_line_resources_invalid",
                      "Overlay line pipelines must be resource-free"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (request.vertex_buffer->backend() != texture.backend()) {
        diagnostic = {"overlay_line_backend_mismatch",
                      "Overlay line buffer and color target must use the same backend"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    const BufferDescription& buffer = request.vertex_buffer->info().description;
    if (!any(buffer.usage & BufferUsage::vertex)) {
        diagnostic = {"overlay_line_vertex_usage_invalid",
                      "Overlay line buffer lacks vertex usage"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const std::uint64_t vertex_bytes =
        static_cast<std::uint64_t>(request.vertex_count) * sizeof(OverlayLineVertex);
    if (request.vertex_offset_bytes > buffer.size_bytes ||
        vertex_bytes > buffer.size_bytes - request.vertex_offset_bytes) {
        diagnostic = {"overlay_line_vertex_range_invalid",
                      "Overlay line vertex range exceeds its buffer"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    for (const float component : request.matrices.world) {
        if (!std::isfinite(component)) {
            diagnostic = {"overlay_line_matrix_non_finite",
                          "Overlay line matrices must contain only finite values"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    for (const float component : request.matrices.view_projection) {
        if (!std::isfinite(component)) {
            diagnostic = {"overlay_line_matrix_non_finite",
                          "Overlay line matrices must contain only finite values"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    diagnostic = {};
    return IndexedStaticMeshBatchStatus::ready;
}

IndexedStaticMeshBatchStatus validate_selected_mesh_draw_request(
    const Texture& texture, const SelectedMeshDrawRequest& request,
    Diagnostic& diagnostic) {
    if (request.packet == nullptr || request.pipeline == nullptr ||
        request.vertex_buffer == nullptr || request.index_buffer == nullptr ||
        request.color_buffer == nullptr) {
        diagnostic = {"selected_mesh_handle_missing",
                      "Selected-mesh drawing requires geometry, pipeline, and color-buffer handles"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const DrawPacket& packet = *request.packet;
    const PipelineProgram& pipeline = *request.pipeline;
    if (packet.primitive != DrawPrimitiveKind::static_mesh ||
        packet.vertex_stride_floats != 11U) {
        diagnostic = {"selected_mesh_static_contract_required",
                      "Selected-mesh drawing requires the recovered static 44-byte vertex contract"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    if (!packet.flags.selected) {
        diagnostic = {"selected_mesh_packet_not_selected",
                      "Selected-mesh drawing requires an explicitly selected packet"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (packet.vertex_count == 0U || packet.index_count == 0U ||
        packet.index_count % 3U != 0U ||
        packet.vertex_count > max_indexed_static_mesh_vertices ||
        packet.index_count > max_indexed_static_mesh_indices) {
        diagnostic = {"selected_mesh_geometry_count_invalid",
                      "Selected-mesh geometry must contain bounded complete indexed triangles"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (request.index_type != StaticMeshIndexType::uint16) {
        diagnostic = {"selected_mesh_index_type_unsupported",
                      "Selected-mesh drawing supports only uint16 indices"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    const PipelineValidationResult pipeline_validation = validate_pipeline(pipeline);
    if (!pipeline_validation.valid) {
        diagnostic = pipeline_validation.diagnostics.empty()
                         ? Diagnostic{"selected_mesh_pipeline_invalid",
                                      "Selected-mesh pipeline validation failed"}
                         : Diagnostic{"selected_mesh_pipeline_" +
                                          pipeline_validation.diagnostics.front().code,
                                      pipeline_validation.diagnostics.front().message};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.shaders.size() != 2U ||
        std::count_if(pipeline.shaders.begin(), pipeline.shaders.end(),
                      [](const PipelineShaderModule& shader) {
                          return shader.stage == PipelineShaderStage::vertex;
                      }) != 1 ||
        std::count_if(pipeline.shaders.begin(), pipeline.shaders.end(),
                      [](const PipelineShaderModule& shader) {
                          return shader.stage == PipelineShaderStage::fragment;
                      }) != 1) {
        diagnostic = {"selected_mesh_shader_pair_invalid",
                      "Selected-mesh pipelines require one vertex and one fragment shader"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const PipelineVertexAttribute expected_position{
        PipelineVertexSemantic::position,
        PipelineVertexAttributeFormat::float32x3, 0U, 0U};
    const auto& attributes = pipeline.vertex_layout.attributes;
    if (pipeline.vertex_layout.stride != 11U * sizeof(float) ||
        attributes.size() != 1U ||
        attributes[0].semantic != expected_position.semantic ||
        attributes[0].format != expected_position.format ||
        attributes[0].location != expected_position.location ||
        attributes[0].offset != expected_position.offset) {
        diagnostic = {"selected_mesh_vertex_layout_invalid",
                      "Selected-mesh pipelines require position float3 at offset zero in a 44-byte vertex"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const TextureDescription& target = texture.info().description;
    const auto expected_format = [&] {
        switch (target.format) {
        case TextureFormat::rgba8_unorm: return PipelineRenderTargetFormat::rgba8_unorm;
        case TextureFormat::rgba8_srgb: return PipelineRenderTargetFormat::rgba8_srgb;
        case TextureFormat::bgra8_unorm: return PipelineRenderTargetFormat::bgra8_unorm;
        case TextureFormat::bgra8_srgb: return PipelineRenderTargetFormat::bgra8_srgb;
        default: return PipelineRenderTargetFormat::unknown;
        }
    }();
    if (expected_format == PipelineRenderTargetFormat::unknown) {
        diagnostic = {"selected_mesh_target_format_unsupported",
                      "Selected meshes support only RGBA8 and BGRA8 color targets"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    if (pipeline.targets.colors.size() != 1U ||
        pipeline.targets.colors[0].format != expected_format ||
        pipeline.targets.colors[0].samples != target.samples) {
        diagnostic = {"selected_mesh_pipeline_target_mismatch",
                      "Selected-mesh pipeline target metadata must match the batch color target"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.transform_contract != PipelineTransformContract::selected_mesh) {
        diagnostic = {"selected_mesh_transform_contract_invalid",
                      "Selected meshes require draw matrices and fragment color constants"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.raster.fill != PipelineFillMode::solid ||
        pipeline.raster.cull != PipelineCullMode::front) {
        diagnostic = {"selected_mesh_raster_state_invalid",
                      "Selected meshes require recovered solid fill and front-face culling"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.depth.test_enabled || pipeline.depth.write_enabled) {
        diagnostic = {"selected_mesh_depth_state_invalid",
                      "Selected meshes require recovered depth mode off"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (pipeline.blend.enabled || pipeline.blend.alpha_to_coverage) {
        diagnostic = {"selected_mesh_blend_state_invalid",
                      "Selected meshes require recovered opaque blend state"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (!pipeline.resources.empty()) {
        diagnostic = {"selected_mesh_resources_invalid",
                      "Selected-mesh pipelines use only stage-visible constants"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (request.vertex_buffer->backend() != texture.backend() ||
        request.index_buffer->backend() != texture.backend() ||
        request.color_buffer->backend() != texture.backend()) {
        diagnostic = {"selected_mesh_backend_mismatch",
                      "Selected-mesh buffers and color target must use one backend"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    const BufferDescription& vertices = request.vertex_buffer->info().description;
    const BufferDescription& indices = request.index_buffer->info().description;
    const BufferDescription& color = request.color_buffer->info().description;
    if (vertices.mutability != BufferMutability::immutable ||
        indices.mutability != BufferMutability::immutable) {
        diagnostic = {"selected_mesh_buffer_mutable",
                      "Selected-mesh drawing requires immutable static vertex and index buffers"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    if (!any(vertices.usage & BufferUsage::vertex) ||
        !any(indices.usage & BufferUsage::index)) {
        diagnostic = {"selected_mesh_buffer_usage_invalid",
                      "Selected-mesh buffers require vertex and index usage"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (color.usage != BufferUsage::uniform ||
        color.mutability != BufferMutability::mutable_data ||
        request.color_range_bytes != selected_mesh_color_view_bytes ||
        request.color_offset_bytes % selected_mesh_color_view_bytes != 0U ||
        request.color_offset_bytes > color.size_bytes ||
        request.color_range_bytes >
            color.size_bytes - request.color_offset_bytes) {
        diagnostic = {"selected_mesh_color_buffer_invalid",
                      "Selected-mesh color requires a mutable aligned 256-byte uniform view"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    constexpr std::uint64_t stride = 11U * sizeof(float);
    const std::uint64_t vertex_offset = packet.vertex_offset;
    const std::uint64_t index_offset = packet.index_offset;
    if (vertex_offset > std::numeric_limits<std::uint64_t>::max() / stride ||
        index_offset > std::numeric_limits<std::uint64_t>::max() /
                           sizeof(std::uint16_t)) {
        diagnostic = {"selected_mesh_offset_overflow",
                      "Selected-mesh offsets overflow byte arithmetic"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const std::uint64_t vertex_bytes = vertex_offset * stride;
    const std::uint64_t index_bytes = index_offset * sizeof(std::uint16_t);
    const std::uint64_t vertex_span =
        static_cast<std::uint64_t>(packet.vertex_count) * stride;
    const std::uint64_t index_span =
        static_cast<std::uint64_t>(packet.index_count) * sizeof(std::uint16_t);
    if (vertex_bytes > vertices.size_bytes ||
        vertex_span > vertices.size_bytes - vertex_bytes ||
        index_bytes > indices.size_bytes ||
        index_span > indices.size_bytes - index_bytes) {
        diagnostic = {"selected_mesh_buffer_range_invalid",
                      "Selected-mesh draw range exceeds its buffers"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    for (const float value : request.matrices.world)
        if (!std::isfinite(value)) {
            diagnostic = {"selected_mesh_matrix_non_finite",
                          "Selected-mesh matrices must contain only finite values"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    for (const float value : request.matrices.view_projection)
        if (!std::isfinite(value)) {
            diagnostic = {"selected_mesh_matrix_non_finite",
                          "Selected-mesh matrices must contain only finite values"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    diagnostic = {};
    return IndexedStaticMeshBatchStatus::ready;
}

IndexedStaticMeshBatchStatus validate_indexed_static_mesh_batch_description(
    const Texture& texture, const IndexedStaticMeshBatchDescription& description,
    Diagnostic& diagnostic) {
    if (description.draws.size() > max_indexed_static_mesh_batch_draws) {
        diagnostic = {"indexed_static_mesh_batch_limit",
                      "Indexed static-mesh batch exceeds the bounded draw limit"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.overlay_draws.size() > max_overlay_line_draws) {
        diagnostic = {"overlay_line_batch_limit",
                      "Overlay line batch exceeds the bounded draw limit"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.selected_mesh_draws.size() > max_selected_mesh_draws) {
        diagnostic = {"selected_mesh_batch_limit",
                      "Selected-mesh batch exceeds the recovered single-selection limit"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.selected_mesh_draws.size() >
        max_indexed_static_mesh_batch_draws - description.draws.size()) {
        diagnostic = {"indexed_static_mesh_batch_limit",
                      "Merged scene and selected-mesh draws exceed the bounded draw limit"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const IndexedStaticMeshDrawStatus target_status =
        validate_indexed_color_target(texture.info().description, diagnostic);
    if (target_status != IndexedStaticMeshDrawStatus::ready)
        return target_status == IndexedStaticMeshDrawStatus::unsupported
                   ? IndexedStaticMeshBatchStatus::unsupported
                   : IndexedStaticMeshBatchStatus::invalid_request;
    std::uint32_t previous_selected_position = 0U;
    bool has_previous_selected_position = false;
    for (const SelectedMeshDrawRequest& selected :
         description.selected_mesh_draws) {
        const std::size_t normalized =
            selected.scene_position == std::numeric_limits<std::uint32_t>::max()
                ? description.draws.size()
                : static_cast<std::size_t>(selected.scene_position);
        if (normalized > description.draws.size()) {
            diagnostic = {"selected_mesh_scene_position_invalid",
                          "Selected-mesh scene position exceeds the ordinary draw count"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        const std::uint32_t position = static_cast<std::uint32_t>(normalized);
        if (has_previous_selected_position &&
            position < previous_selected_position) {
            diagnostic = {"selected_mesh_scene_order_invalid",
                          "Selected-mesh scene positions must be nondecreasing"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        previous_selected_position = position;
        has_previous_selected_position = true;
        Diagnostic selected_diagnostic;
        const IndexedStaticMeshBatchStatus selected_status =
            validate_selected_mesh_draw_request(texture, selected,
                                                selected_diagnostic);
        if (selected_status != IndexedStaticMeshBatchStatus::ready) {
            diagnostic = std::move(selected_diagnostic);
            return selected_status;
        }
        const bool batch_has_depth = description.depth_attachment != nullptr;
        if (selected.pipeline->targets.has_depth != batch_has_depth ||
            (batch_has_depth &&
             (selected.pipeline->targets.depth.format !=
                  PipelineRenderTargetFormat::depth32_float ||
              selected.pipeline->targets.depth.samples !=
                  texture.info().description.samples))) {
            diagnostic = {"selected_mesh_pipeline_depth_target_mismatch",
                          "Selected-mesh depth target metadata must match the batch render pass"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    std::uint64_t overlay_vertices = 0U;
    std::uint32_t previous_overlay_position = 0U;
    bool has_previous_overlay_position = false;
    for (const OverlayLineDrawRequest& overlay : description.overlay_draws) {
        const std::size_t normalized =
            overlay.scene_position == std::numeric_limits<std::uint32_t>::max()
                ? description.draws.size()
                : static_cast<std::size_t>(overlay.scene_position);
        if (normalized > description.draws.size()) {
            diagnostic = {"overlay_line_scene_position_invalid",
                          "Overlay line scene position exceeds the ordinary draw count"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        const std::uint32_t position = static_cast<std::uint32_t>(normalized);
        if (has_previous_overlay_position &&
            position < previous_overlay_position) {
            diagnostic = {"overlay_line_scene_order_invalid",
                          "Overlay line scene positions must be nondecreasing"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        previous_overlay_position = position;
        has_previous_overlay_position = true;
        overlay_vertices += overlay.vertex_count;
        if (overlay_vertices > max_overlay_line_total_vertices) {
            diagnostic = {"overlay_line_total_vertex_limit",
                          "Overlay line batch exceeds the bounded total vertex limit"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        Diagnostic overlay_diagnostic;
        const IndexedStaticMeshBatchStatus overlay_status =
            validate_overlay_line_draw_request(texture, overlay, overlay_diagnostic);
        if (overlay_status != IndexedStaticMeshBatchStatus::ready) {
            diagnostic = std::move(overlay_diagnostic);
            return overlay_status;
        }
        const bool batch_has_depth = description.depth_attachment != nullptr;
        if (overlay.pipeline->targets.has_depth != batch_has_depth ||
            (batch_has_depth &&
             (overlay.pipeline->targets.depth.format !=
                  PipelineRenderTargetFormat::depth32_float ||
              overlay.pipeline->targets.depth.samples !=
                  texture.info().description.samples))) {
            diagnostic = {"overlay_line_pipeline_depth_target_mismatch",
                          "Overlay line depth target metadata must match the batch render pass"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    for (const float component : description.clear_color) {
        if (!std::isfinite(component)) {
            diagnostic = {"indexed_static_mesh_batch_clear_color_non_finite",
                          "Indexed static-mesh batch clear color components must be finite"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    if (!std::isfinite(description.depth_clear_value) || description.depth_clear_value < 0.0F ||
        description.depth_clear_value > 1.0F) {
        diagnostic = {"indexed_static_mesh_batch_depth_clear_invalid",
                      "Indexed static-mesh batch depth clear value must be finite and within [0, 1]"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.clear_depth && description.depth_attachment == nullptr) {
        diagnostic = {"indexed_static_mesh_batch_depth_attachment_missing",
                      "A batch depth clear requires a persistent depth attachment"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    const TextureDescription& target = texture.info().description;
    if (description.resolve_target != nullptr) {
        if (description.resolve_target == &texture) {
            diagnostic = {"indexed_static_mesh_batch_resolve_alias",
                          "Batch color and resolve targets must be different textures"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (target.samples != 4U) {
            diagnostic = {"indexed_static_mesh_batch_resolve_source_samples_invalid",
                          "A retained batch resolve requires a four-sample color target"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        const auto target_usage = static_cast<std::uint32_t>(target.usage);
        const auto required_target_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::transfer_source);
        const bool supported_format =
            target.format == TextureFormat::rgba8_unorm ||
            target.format == TextureFormat::rgba8_srgb ||
            target.format == TextureFormat::bgra8_unorm ||
            target.format == TextureFormat::bgra8_srgb;
        if (target.width == 0U || target.height == 0U ||
            target.mip_levels != 1U || target.array_layers != 1U ||
            (target_usage & required_target_usage) != required_target_usage) {
            diagnostic = {"indexed_static_mesh_batch_resolve_source_invalid",
                          "Batch resolve source must be a bounded color attachment and transfer source"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (!supported_format) {
            diagnostic = {"indexed_static_mesh_batch_resolve_format_unsupported",
                          "Batch resolve supports only RGBA8 and BGRA8 color formats"};
            return IndexedStaticMeshBatchStatus::unsupported;
        }
        if (description.resolve_target->backend() != texture.backend()) {
            diagnostic = {"indexed_static_mesh_batch_resolve_backend_mismatch",
                          "Batch color and resolve targets must use the same backend"};
            return IndexedStaticMeshBatchStatus::unsupported;
        }
        const TextureDescription& resolved =
            description.resolve_target->info().description;
        const auto resolve_usage = static_cast<std::uint32_t>(resolved.usage);
        const auto color_usage = static_cast<std::uint32_t>(TextureUsage::color_attachment);
        const auto source_usage = static_cast<std::uint32_t>(TextureUsage::transfer_source);
        if (resolved.width != target.width || resolved.height != target.height ||
            resolved.format != target.format || resolved.mip_levels != 1U ||
            resolved.array_layers != 1U || resolved.samples != 1U) {
            diagnostic = {"indexed_static_mesh_batch_resolve_description_mismatch",
                          "Batch resolve target must be a matching single-sample 2D texture"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if ((resolve_usage & color_usage) == 0U ||
            (resolve_usage & source_usage) == 0U ||
            resolved.mutability != TextureMutability::mutable_data) {
            diagnostic = {"indexed_static_mesh_batch_resolve_usage_invalid",
                          "Batch resolve target must be a mutable color attachment and transfer source"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    } else if (!description.capture_rgba8 && target.samples == 4U) {
        diagnostic = {"indexed_static_mesh_batch_resolve_target_missing",
                      "A four-sample batch without CPU capture requires a resolve target"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.depth_attachment != nullptr) {
        const DepthAttachment& depth = *description.depth_attachment;
        if (depth.backend() != texture.backend()) {
            diagnostic = {"indexed_static_mesh_batch_depth_backend_mismatch",
                          "Batch color target and depth attachment must belong to the same backend"};
            return IndexedStaticMeshBatchStatus::unsupported;
        }
        Diagnostic depth_diagnostic;
        const DepthAttachmentStatus depth_status =
            validate_depth_attachment_description(depth.info().description, depth_diagnostic);
        if (depth_status != DepthAttachmentStatus::ready) {
            diagnostic = {depth_diagnostic.code.empty() ? "indexed_static_mesh_batch_depth_invalid"
                                                         : depth_diagnostic.code,
                          depth_diagnostic.message};
            return depth_status == DepthAttachmentStatus::unsupported
                       ? IndexedStaticMeshBatchStatus::unsupported
                       : IndexedStaticMeshBatchStatus::invalid_request;
        }
        const DepthAttachmentDescription& depth_description = depth.info().description;
        if (depth_description.width != target.width || depth_description.height != target.height ||
            depth_description.samples != target.samples) {
            diagnostic = {"indexed_static_mesh_batch_depth_dimensions_mismatch",
                          "Batch depth attachment dimensions and samples must match the color target"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }

    constexpr std::array<float, 4> default_clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    for (std::size_t index = 0U; index < description.draws.size(); ++index) {
        const IndexedStaticMeshDrawRequest& source = description.draws[index];
        if (source.depth_attachment != nullptr || source.load_color || source.clear_depth ||
            source.depth_clear_value != 1.0F || source.clear_color != default_clear_color) {
            diagnostic = {"indexed_static_mesh_batch_draw_override",
                          "Batch attachment, load, and clear state must be specified only once on the batch"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }

        // Preflight a local request. The caller's ordered span and its request
        // objects remain unchanged; execution backends receive the batch state
        // through this same effective single-draw contract.
        std::size_t merged_index = index;
        for (const SelectedMeshDrawRequest& selected :
             description.selected_mesh_draws) {
            const std::size_t position =
                selected.scene_position ==
                        std::numeric_limits<std::uint32_t>::max()
                    ? description.draws.size()
                    : static_cast<std::size_t>(selected.scene_position);
            if (position <= index) ++merged_index;
        }
        for (const OverlayLineDrawRequest& overlay :
             description.overlay_draws) {
            const std::size_t position =
                overlay.scene_position ==
                        std::numeric_limits<std::uint32_t>::max()
                    ? description.draws.size()
                    : static_cast<std::size_t>(overlay.scene_position);
            if (position <= index) ++merged_index;
        }
        IndexedStaticMeshDrawRequest effective = source;
        effective.depth_attachment = description.depth_attachment;
        effective.load_color = description.load_color || merged_index != 0U;
        effective.clear_color = description.clear_color;
        effective.clear_depth = description.clear_depth && merged_index == 0U;
        effective.depth_clear_value = description.depth_clear_value;
        Diagnostic draw_diagnostic;
        const IndexedStaticMeshDrawStatus draw_status =
            validate_indexed_static_mesh_draw_request(texture, effective, draw_diagnostic);
        if (draw_status != IndexedStaticMeshDrawStatus::ready) {
            diagnostic = std::move(draw_diagnostic);
            return draw_status == IndexedStaticMeshDrawStatus::unsupported
                       ? IndexedStaticMeshBatchStatus::unsupported
                       : IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    diagnostic = {};
    return IndexedStaticMeshBatchStatus::ready;
}

DepthOnlyIndexedStaticMeshDrawStatus
validate_depth_only_indexed_static_mesh_draw_request(
    const DepthAttachment& depth,
    const DepthOnlyIndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic) {
    if (request.packet == nullptr || request.pipeline == nullptr ||
        request.vertex_buffer == nullptr || request.index_buffer == nullptr) {
        diagnostic = {"depth_only_indexed_static_mesh_handle_missing",
                      "Depth-only indexed drawing requires a packet, pipeline, vertex buffer, and index buffer"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (!std::isfinite(request.depth_clear_value) || request.depth_clear_value < 0.0F ||
        request.depth_clear_value > 1.0F) {
        diagnostic = {"depth_only_indexed_static_mesh_depth_clear_invalid",
                      "Depth-only indexed depth clear value must be finite and within [0, 1]"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (request.index_type != StaticMeshIndexType::uint16) {
        diagnostic = {"depth_only_indexed_static_mesh_index_type_unsupported",
                      "Depth-only indexed drawing supports only uint16 indices"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }
    Diagnostic depth_diagnostic;
    const DepthAttachmentStatus depth_status =
        validate_depth_attachment_description(depth.info().description, depth_diagnostic);
    if (depth_status != DepthAttachmentStatus::ready) {
        diagnostic = {depth_diagnostic.code.empty() ? "depth_only_indexed_static_mesh_depth_invalid"
                                                     : depth_diagnostic.code,
                      depth_diagnostic.message};
        return depth_status == DepthAttachmentStatus::unsupported
                   ? DepthOnlyIndexedStaticMeshDrawStatus::unsupported
                   : DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    const auto& depth_description = depth.info().description;
    if (depth_description.format != DepthAttachmentFormat::d32_float ||
        depth_description.samples != 1U) {
        diagnostic = {"depth_only_indexed_static_mesh_depth_target_unsupported",
                      "Depth-only indexed drawing requires a single-sample D32 attachment"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }

    const DrawPacket& packet = *request.packet;
    const bool skinned = packet.primitive == DrawPrimitiveKind::skinned_mesh;
    const bool alpha_tested =
        request.material_mode == DepthOnlyIndexedStaticMeshDrawRequest::MaterialMode::stock_alpha_tested;
    const bool has_alpha_bindings =
        request.alpha_tested_diffuse_binding.texture != nullptr ||
        request.alpha_tested_diffuse_binding.sampler != nullptr ||
        request.alpha_tested_material_binding.buffer != nullptr ||
        request.alpha_tested_material_binding.offset_bytes != 0U ||
        request.alpha_tested_material_binding.range_bytes != 0U;
    if (!alpha_tested &&
        (request.resource_authority != IndexedResourceAuthority::packet_contract ||
         has_alpha_bindings)) {
        diagnostic = {"depth_only_indexed_resource_binding_unexpected",
                      "Opaque depth-only drawing cannot receive alpha-tested resource bindings"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (alpha_tested && skinned) {
        diagnostic = {"depth_only_indexed_alpha_tested_static_only",
                      "Stock alpha-tested depth-only drawing currently accepts static meshes only"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }
    if (!skinned && packet.primitive != DrawPrimitiveKind::static_mesh) {
        diagnostic = {"depth_only_indexed_static_mesh_primitive_unsupported",
                      "Depth-only indexed drawing accepts static or skinned mesh packets only"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }
    if (!skinned && !packet.bone_palette.empty()) {
        diagnostic = {"depth_only_indexed_static_mesh_skinning_unsupported",
                      "Depth-only indexed static meshes must not carry a bone palette"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }
    if (skinned && packet.bone_palette.empty()) {
        diagnostic = {"depth_only_indexed_skinned_mesh_palette_missing",
                      "Depth-only indexed skinned meshes require a bone palette"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (packet.vertex_count == 0U || packet.vertex_count > max_indexed_static_mesh_vertices ||
        packet.index_count == 0U || packet.index_count > max_indexed_static_mesh_indices ||
        packet.index_count % 3U != 0U) {
        diagnostic = {"depth_only_indexed_static_mesh_range_invalid",
                      "Depth-only indexed ranges must contain bounded non-empty triangles"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    for (const float value : packet.world_matrix) {
        if (!std::isfinite(value)) {
            diagnostic = {"depth_only_indexed_static_mesh_world_matrix_non_finite",
                          "Depth-only indexed world matrix must be finite"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    if (!request.camera_frame.has_value()) {
        diagnostic = {"depth_only_indexed_static_mesh_camera_missing",
                      "Depth-only indexed transform execution requires a camera frame"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    const CameraFrame& camera = *request.camera_frame;
    const CameraClipSpace expected_clip = depth.backend() == Backend::Vulkan
                                              ? CameraClipSpace::vulkan
                                              : CameraClipSpace::d3d12;
    if (camera.clip_space != expected_clip) {
        diagnostic = {"depth_only_indexed_static_mesh_camera_clip_space_mismatch",
                      "Depth-only indexed camera clip space does not match the backend"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    for (const float value : camera.view_projection) {
        if (!std::isfinite(value)) {
            diagnostic = {"depth_only_indexed_static_mesh_view_projection_non_finite",
                          "Depth-only indexed view-projection matrix must be finite"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
    }

    const PipelineProgram& pipeline = *request.pipeline;
    const PipelineValidationResult pipeline_validation = validate_pipeline(pipeline);
    if (!pipeline_validation.valid) {
        if (!pipeline_validation.diagnostics.empty()) {
            const auto& failure = pipeline_validation.diagnostics.front();
            diagnostic = {"depth_only_indexed_pipeline_" + failure.code, failure.message};
        } else {
            diagnostic = {"depth_only_indexed_pipeline_invalid",
                          "Depth-only indexed pipeline validation failed without a diagnostic"};
        }
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (!pipeline.targets.colors.empty() || !pipeline.targets.has_depth ||
        pipeline.targets.depth.format != PipelineRenderTargetFormat::depth32_float ||
        pipeline.targets.depth.samples != 1U) {
        diagnostic = {"depth_only_indexed_pipeline_target_invalid",
                      "Depth-only indexed pipelines require exactly one single-sample D32 depth target and no color targets"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    const bool valid_alpha_resources =
        pipeline.resources.size() == 3U &&
        std::all_of(pipeline.resources.begin(), pipeline.resources.end(),
                    [](const PipelineResourceBinding& resource) {
                        if (resource.set != 0U) return false;
                        if (resource.binding == 0U)
                            return resource.kind == PipelineResourceKind::sampled_texture;
                        if (resource.binding == 3U)
                            return resource.kind == PipelineResourceKind::sampler;
                        if (resource.binding == 4U)
                            return resource.kind == PipelineResourceKind::uniform_buffer;
                        return false;
                    }) &&
        std::any_of(pipeline.resources.begin(), pipeline.resources.end(),
                    [](const PipelineResourceBinding& resource) {
                        return resource.binding == 0U;
                    }) &&
        std::any_of(pipeline.resources.begin(), pipeline.resources.end(),
                    [](const PipelineResourceBinding& resource) {
                        return resource.binding == 3U;
                    }) &&
        std::any_of(pipeline.resources.begin(), pipeline.resources.end(),
                    [](const PipelineResourceBinding& resource) {
                        return resource.binding == 4U;
                    });
    if ((!alpha_tested && pipeline.resources.size() != 0U) ||
        (alpha_tested && !valid_alpha_resources) || pipeline.blend.enabled ||
        pipeline.blend.alpha_to_coverage ||
        pipeline.transform_contract != PipelineTransformContract::draw_matrices ||
        !pipeline.depth.test_enabled || !pipeline.depth.write_enabled ||
        pipeline.depth.compare != PipelineCompareOperation::less) {
        diagnostic = {"depth_only_indexed_pipeline_state_invalid",
                      alpha_tested
                          ? "Stock alpha-tested depth-only pipelines require exact t0/s3/b4 resources, draw matrices, depth LESS test/write, and no blend"
                          : "Depth-only indexed pipelines require draw matrices, depth LESS test/write, no blend, and no resources"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (packet.flags.depth_test != pipeline.depth.test_enabled ||
        packet.flags.depth_write != pipeline.depth.write_enabled) {
        diagnostic = {"depth_only_indexed_packet_depth_state_mismatch",
                      "Depth-only packet depth flags must match the executable pipeline"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if ((!alpha_tested && (pipeline.shaders.size() != 1U ||
                           pipeline.shaders.front().stage != PipelineShaderStage::vertex)) ||
        (alpha_tested &&
         (pipeline.shaders.size() != 2U ||
          std::count_if(pipeline.shaders.begin(), pipeline.shaders.end(),
                        [](const PipelineShaderModule& shader) {
                            return shader.stage == PipelineShaderStage::vertex;
                        }) != 1 ||
          std::count_if(pipeline.shaders.begin(), pipeline.shaders.end(),
                        [](const PipelineShaderModule& shader) {
                            return shader.stage == PipelineShaderStage::fragment;
                        }) != 1))) {
        diagnostic = {"depth_only_indexed_shader_pair_invalid",
                      alpha_tested
                          ? "Stock alpha-tested depth-only pipelines require exactly one vertex and one fragment shader"
                          : "Depth-only indexed pipelines require exactly one vertex shader and no fragment shader"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    const PipelineShaderFormat expected_shader = depth.backend() == Backend::Vulkan
                                                     ? PipelineShaderFormat::spirv
                                                     : PipelineShaderFormat::dxil;
    for (const auto& shader : pipeline.shaders) {
        if (shader.format != expected_shader) {
            diagnostic = {"depth_only_indexed_shader_format_mismatch",
                          alpha_tested
                              ? "Stock alpha-tested depth-only shader formats must match the backend"
                              : "Depth-only indexed vertex shader format does not match the backend"};
            return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
        }
        const ShaderStage shader_stage = shader.stage == PipelineShaderStage::fragment
                                             ? ShaderStage::fragment
                                             : ShaderStage::vertex;
        const std::span<const std::byte> bytecode(
            reinterpret_cast<const std::byte*>(shader.bytes.data()), shader.bytes.size());
        Diagnostic shader_diagnostic;
        const ShaderModuleStatus shader_status =
            validate_shader_module_description({shader_stage, bytecode}, shader_diagnostic);
        if (shader_status != ShaderModuleStatus::ready) {
            diagnostic = {shader_diagnostic.code.empty() ? "depth_only_indexed_shader_invalid"
                                                          : "depth_only_indexed_shader_" + shader_diagnostic.code,
                          shader_diagnostic.message};
            return shader_status == ShaderModuleStatus::unsupported
                       ? DepthOnlyIndexedStaticMeshDrawStatus::unsupported
                       : DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    const std::uint32_t expected_stride_floats = skinned ? 19U : 11U;
    if (packet.vertex_stride_floats != expected_stride_floats ||
        static_cast<std::uint64_t>(packet.vertex_stride_floats) * sizeof(float) !=
            pipeline.vertex_layout.stride) {
        diagnostic = {skinned ? "depth_only_indexed_skinned_mesh_stride_mismatch"
                              : "depth_only_indexed_static_mesh_stride_mismatch",
                      skinned
                          ? "Depth-only indexed skinned meshes require a 19-float stride matching the pipeline"
                          : "Depth-only indexed static mesh requires an 11-float stride matching the pipeline"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }

    const Buffer& vertex_buffer = *request.vertex_buffer;
    const Buffer& index_buffer = *request.index_buffer;
    if (depth.backend() != vertex_buffer.backend() ||
        depth.backend() != index_buffer.backend()) {
        diagnostic = {"depth_only_indexed_static_mesh_backend_mismatch",
                      "Depth attachment and indexed buffers must belong to the same backend"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }
    if (alpha_tested) {
        if (request.resource_authority != IndexedResourceAuthority::explicit_bindings) {
            diagnostic = {"depth_only_indexed_alpha_tested_resource_authority_required",
                          "Stock alpha-tested depth-only drawing requires explicit request-local resource bindings"};
            return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
        }
        const IndexedSampledTextureBinding& diffuse = request.alpha_tested_diffuse_binding;
        const StockShadowCasterMaterialBinding& material =
            request.alpha_tested_material_binding;
        if (diffuse.texture == nullptr || diffuse.sampler == nullptr ||
            material.buffer == nullptr) {
            diagnostic = {"depth_only_indexed_alpha_tested_binding_missing",
                          "Stock alpha-tested depth-only drawing requires a diffuse texture, sampler, and material buffer"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
        if (diffuse.texture->backend() != depth.backend() ||
            diffuse.sampler->backend() != depth.backend() ||
            material.buffer->backend() != depth.backend()) {
            diagnostic = {"depth_only_indexed_alpha_tested_backend_mismatch",
                          "Stock alpha-tested resources and the depth attachment must use the same backend"};
            return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
        }
        const TextureDescription& texture = diffuse.texture->info().description;
        const std::uint32_t texture_usage = static_cast<std::uint32_t>(texture.usage);
        constexpr std::uint32_t sampled_usage =
            static_cast<std::uint32_t>(TextureUsage::sampled);
        constexpr std::uint32_t forbidden_usage =
            static_cast<std::uint32_t>(TextureUsage::color_attachment) |
            static_cast<std::uint32_t>(TextureUsage::storage) |
            static_cast<std::uint32_t>(TextureUsage::transfer_destination);
        if ((texture_usage & sampled_usage) == 0U) {
            diagnostic = {"depth_only_indexed_alpha_tested_texture_usage_invalid",
                          "Stock alpha-tested diffuse textures require sampled usage"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
        if ((texture_usage & forbidden_usage) != 0U ||
            texture.mutability != TextureMutability::immutable ||
            texture.width == 0U || texture.height == 0U || texture.mip_levels == 0U ||
            texture.array_layers != 1U || texture.samples != 1U ||
            !portable_sampled_color_format(texture.format, true)) {
            diagnostic = {"depth_only_indexed_alpha_tested_texture_description_invalid",
                          "Stock alpha-tested diffuse textures require one-layer immutable sampled RGBA8, BGRA8, BC1, BC3, or BC7 data"};
            return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
        }
        Diagnostic sampler_diagnostic;
        const SamplerStatus sampler_status =
            validate_sampler_description(diffuse.sampler->info().description,
                                         sampler_diagnostic);
        if (sampler_status != SamplerStatus::ready) {
            diagnostic = {sampler_diagnostic.code.empty()
                              ? "depth_only_indexed_alpha_tested_sampler_invalid"
                              : "depth_only_indexed_alpha_tested_" + sampler_diagnostic.code,
                          sampler_diagnostic.message};
            return sampler_status == SamplerStatus::unsupported
                       ? DepthOnlyIndexedStaticMeshDrawStatus::unsupported
                       : DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
        const BufferDescription& material_description =
            material.buffer->info().description;
        if (material_description.usage != BufferUsage::uniform) {
            diagnostic = {"depth_only_indexed_alpha_tested_material_usage_invalid",
                          "Stock alpha-tested material buffers require exclusive uniform usage"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
        if (material.offset_bytes % stock_shadow_caster_buffer_alignment != 0U ||
            material.range_bytes != stock_shadow_caster_material_bytes) {
            diagnostic = {"depth_only_indexed_alpha_tested_material_alignment_invalid",
                          "Stock alpha-tested material views require a 256-byte aligned offset and a 32-byte logical range"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
        if (material.offset_bytes > material_description.size_bytes ||
            static_cast<std::uint64_t>(material.range_bytes) >
                material_description.size_bytes - material.offset_bytes) {
            diagnostic = {"depth_only_indexed_alpha_tested_material_range_invalid",
                          "Stock alpha-tested material view exceeds the declared buffer size"};
            return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    const auto& vertex_description = vertex_buffer.info().description;
    const auto& index_description = index_buffer.info().description;
    if (vertex_description.usage != BufferUsage::vertex) {
        diagnostic = {"depth_only_indexed_vertex_buffer_usage_invalid",
                      "Depth-only indexed vertex buffers require exclusive vertex usage"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (index_description.usage != BufferUsage::index) {
        diagnostic = {"depth_only_indexed_index_buffer_usage_invalid",
                      "Depth-only indexed index buffers require exclusive index usage"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    if (vertex_description.memory != BufferMemory::device_local ||
        index_description.memory != BufferMemory::device_local ||
        (skinned ? vertex_description.mutability != BufferMutability::mutable_data
                  : vertex_description.mutability != BufferMutability::immutable) ||
        index_description.mutability != BufferMutability::immutable) {
        diagnostic = {"depth_only_indexed_buffer_ownership_invalid",
                      skinned
                          ? "Depth-only indexed skinned meshes require a mutable device-local vertex buffer and immutable device-local indices"
                          : "Depth-only indexed buffers require immutable device-local ownership"};
        return DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    }
    const std::uint64_t stride = pipeline.vertex_layout.stride;
    const std::uint64_t vertex_offset = packet.vertex_offset;
    const std::uint64_t index_offset = packet.index_offset;
    if (vertex_offset > std::numeric_limits<std::uint64_t>::max() / stride ||
        index_offset > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
        diagnostic = {"depth_only_indexed_static_mesh_offset_overflow",
                      "Depth-only indexed buffer offsets overflow byte arithmetic"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    const std::uint64_t vertex_bytes = vertex_offset * stride;
    const std::uint64_t index_bytes = index_offset * sizeof(std::uint16_t);
    const std::uint64_t vertex_span = static_cast<std::uint64_t>(packet.vertex_count) * stride;
    const std::uint64_t index_span = static_cast<std::uint64_t>(packet.index_count) * sizeof(std::uint16_t);
    if (vertex_bytes > vertex_description.size_bytes ||
        vertex_span > vertex_description.size_bytes - vertex_bytes ||
        index_bytes > index_description.size_bytes ||
        index_span > index_description.size_bytes - index_bytes) {
        diagnostic = {"depth_only_indexed_static_mesh_buffer_range_invalid",
                      "Depth-only indexed draw ranges exceed their owned buffers"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    diagnostic = {};
    return DepthOnlyIndexedStaticMeshDrawStatus::ready;
}

DepthOnlyIndexedStaticMeshBatchStatus
validate_depth_only_indexed_static_mesh_batch_description(
    const DepthOnlyIndexedStaticMeshBatchDescription& description,
    Diagnostic& diagnostic) {
    if (description.draws.empty() && !description.clear_depth) {
        diagnostic = {"depth_only_indexed_static_mesh_batch_empty",
                      "An empty depth-only indexed batch must clear its attachment"};
        return DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.draws.size() > max_indexed_static_mesh_batch_draws) {
        diagnostic = {"depth_only_indexed_static_mesh_batch_limit",
                      "Depth-only indexed batch exceeds the bounded draw limit"};
        return DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
    }
    if (description.depth_attachment == nullptr) {
        diagnostic = {"depth_only_indexed_static_mesh_batch_depth_missing",
                      "A depth-only indexed batch requires a persistent depth attachment"};
        return DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
    }
    if (!std::isfinite(description.depth_clear_value) ||
        description.depth_clear_value < 0.0F || description.depth_clear_value > 1.0F) {
        diagnostic = {"depth_only_indexed_static_mesh_batch_depth_clear_invalid",
                      "Depth-only indexed batch depth clear value must be finite and within [0, 1]"};
        return DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
    }
    Diagnostic depth_diagnostic;
    const DepthAttachmentStatus depth_status = validate_depth_attachment_description(
        description.depth_attachment->info().description, depth_diagnostic);
    if (depth_status != DepthAttachmentStatus::ready) {
        diagnostic = {depth_diagnostic.code.empty() ? "depth_only_indexed_static_mesh_batch_depth_invalid"
                                                     : depth_diagnostic.code,
                      depth_diagnostic.message};
        return depth_status == DepthAttachmentStatus::unsupported
                   ? DepthOnlyIndexedStaticMeshBatchStatus::unsupported
                   : DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
    }
    const auto& depth_description = description.depth_attachment->info().description;
    if (depth_description.format != DepthAttachmentFormat::d32_float ||
        depth_description.samples != 1U) {
        diagnostic = {"depth_only_indexed_static_mesh_batch_depth_target_unsupported",
                      "Depth-only indexed batches require a single-sample D32 attachment"};
        return DepthOnlyIndexedStaticMeshBatchStatus::unsupported;
    }
    for (std::size_t index = 0U; index < description.draws.size(); ++index) {
        const auto& source = description.draws[index];
        if (source.clear_depth || source.depth_clear_value != 1.0F) {
            diagnostic = {"depth_only_indexed_static_mesh_batch_draw_override",
                          "Depth clear state must be specified only on the batch"};
            return DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
        }
        auto effective = source;
        effective.clear_depth = description.clear_depth && index == 0U;
        effective.depth_clear_value = description.depth_clear_value;
        Diagnostic draw_diagnostic;
        const auto draw_status = validate_depth_only_indexed_static_mesh_draw_request(
            *description.depth_attachment, effective, draw_diagnostic);
        if (draw_status != DepthOnlyIndexedStaticMeshDrawStatus::ready) {
            diagnostic = std::move(draw_diagnostic);
            return draw_status == DepthOnlyIndexedStaticMeshDrawStatus::unsupported
                       ? DepthOnlyIndexedStaticMeshBatchStatus::unsupported
                       : DepthOnlyIndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    diagnostic = {};
    return DepthOnlyIndexedStaticMeshBatchStatus::ready;
}

SamplerStatus validate_sampler_description(const SamplerDescription& description,
                                           Diagnostic& diagnostic) {
    const auto valid_filter = [](SamplerFilter filter) {
        return filter == SamplerFilter::nearest || filter == SamplerFilter::linear ||
               filter == SamplerFilter::anisotropic;
    };
    const auto valid_address = [](SamplerAddressMode mode) {
        return mode == SamplerAddressMode::repeat || mode == SamplerAddressMode::mirrored_repeat ||
               mode == SamplerAddressMode::clamp_to_edge || mode == SamplerAddressMode::clamp_to_border;
    };
    const auto valid_compare = [](SamplerCompare compare) {
        return compare == SamplerCompare::disabled || compare == SamplerCompare::less ||
               compare == SamplerCompare::less_equal || compare == SamplerCompare::greater ||
               compare == SamplerCompare::greater_equal || compare == SamplerCompare::equal ||
               compare == SamplerCompare::not_equal || compare == SamplerCompare::always ||
               compare == SamplerCompare::never;
    };
    if (!valid_filter(description.min_filter) || !valid_filter(description.mag_filter) ||
        !valid_filter(description.mip_filter)) {
        diagnostic = {"sampler_filter_invalid", "Sampler filters contain an unknown value"};
        return SamplerStatus::invalid_description;
    }
    if (!valid_address(description.address_u) || !valid_address(description.address_v) ||
        !valid_address(description.address_w)) {
        diagnostic = {"sampler_address_invalid", "Sampler address modes contain an unknown value"};
        return SamplerStatus::invalid_description;
    }
    if (!valid_compare(description.compare)) {
        diagnostic = {"sampler_compare_invalid", "Sampler comparison mode contains an unknown value"};
        return SamplerStatus::invalid_description;
    }
    if (!std::isfinite(description.max_anisotropy) || description.max_anisotropy < 1.0F ||
        description.max_anisotropy > 16.0F || std::trunc(description.max_anisotropy) != description.max_anisotropy) {
        diagnostic = {"sampler_anisotropy_invalid", "Sampler anisotropy must be an integer between 1 and 16"};
        return SamplerStatus::invalid_description;
    }
    if (description.min_filter == SamplerFilter::anisotropic || description.mag_filter == SamplerFilter::anisotropic ||
        description.mip_filter == SamplerFilter::anisotropic) {
        if (description.max_anisotropy <= 1.0F) {
            diagnostic = {"sampler_anisotropy_invalid", "Anisotropic filtering requires anisotropy greater than one"};
            return SamplerStatus::invalid_description;
        }
    } else if (description.max_anisotropy != 1.0F) {
        diagnostic = {"sampler_anisotropy_unused", "Non-anisotropic filtering must use anisotropy one"};
        return SamplerStatus::invalid_description;
    }
    if (!std::isfinite(description.min_lod) || !std::isfinite(description.max_lod) || description.min_lod < 0.0F ||
        description.max_lod < description.min_lod || description.max_lod > 16384.0F) {
        diagnostic = {"sampler_lod_invalid", "Sampler LOD values are outside the supported finite range"};
        return SamplerStatus::invalid_description;
    }
    return SamplerStatus::ready;
}

namespace {

std::uint32_t shader_word(std::span<const std::byte> bytes) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3])) << 24U);
}

bool read_shader_word(std::span<const std::byte> bytes, std::size_t offset,
                      std::uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) return false;
    value = shader_word(bytes.subspan(offset, sizeof(std::uint32_t)));
    return true;
}

enum class ContainerValidation : std::uint8_t {
    valid,
    invalid,
    unsupported,
};

ContainerValidation validate_dxbc_container(std::span<const std::byte> bytes,
                                             Diagnostic& diagnostic) {
    if (bytes.size() < 32U) {
        diagnostic = {"shader_dxbc_header_invalid", "DXBC bytecode is shorter than its container header"};
        return ContainerValidation::invalid;
    }
    std::uint32_t header_version = 0;
    std::uint32_t total_size = 0;
    std::uint32_t chunk_count = 0;
    if (!read_shader_word(bytes, 20U, header_version) || !read_shader_word(bytes, 24U, total_size) ||
        !read_shader_word(bytes, 28U, chunk_count) || header_version != 1U) {
        diagnostic = {"shader_dxbc_header_invalid", "DXBC container header fields are invalid"};
        return ContainerValidation::invalid;
    }
    if (total_size != bytes.size() || total_size < 32U) {
        diagnostic = {"shader_dxbc_size_invalid", "DXBC container size does not match the bytecode span"};
        return ContainerValidation::invalid;
    }
    if (chunk_count == 0U || chunk_count > max_dxbc_chunks ||
        static_cast<std::size_t>(chunk_count) > (bytes.size() - 32U) / sizeof(std::uint32_t)) {
        diagnostic = {"shader_dxbc_chunk_count_invalid", "DXBC chunk count exceeds the bounded container table"};
        return ContainerValidation::invalid;
    }
    const std::size_t table_end = 32U + static_cast<std::size_t>(chunk_count) * sizeof(std::uint32_t);
    std::set<std::pair<std::size_t, std::size_t>> ranges;
    bool supported_chunk = false;
    for (std::uint32_t index = 0; index < chunk_count; ++index) {
        std::uint32_t offset_word = 0;
        const std::size_t table_offset = 32U + static_cast<std::size_t>(index) * sizeof(std::uint32_t);
        if (!read_shader_word(bytes, table_offset, offset_word)) {
            diagnostic = {"shader_dxbc_offset_invalid", "DXBC chunk offset table is truncated"};
            return ContainerValidation::invalid;
        }
        const std::size_t offset = static_cast<std::size_t>(offset_word);
        if (offset < table_end || offset > bytes.size() || bytes.size() - offset < 8U) {
            diagnostic = {"shader_dxbc_offset_invalid", "DXBC chunk offset is outside the container"};
            return ContainerValidation::invalid;
        }
        std::uint32_t chunk_fourcc = 0;
        std::uint32_t chunk_size = 0;
        if (!read_shader_word(bytes, offset, chunk_fourcc) ||
            !read_shader_word(bytes, offset + 4U, chunk_size) ||
            static_cast<std::size_t>(chunk_size) > bytes.size() - offset - 8U) {
            diagnostic = {"shader_dxbc_chunk_invalid", "DXBC chunk size exceeds the container"};
            return ContainerValidation::invalid;
        }
        const std::size_t end = offset + 8U + static_cast<std::size_t>(chunk_size);
        if (!ranges.insert({offset, end}).second) {
            diagnostic = {"shader_dxbc_chunk_overlap", "DXBC chunk offsets contain a duplicate range"};
            return ContainerValidation::invalid;
        }
        if (chunk_fourcc == 0x4C495844U || chunk_fourcc == 0x58454853U || chunk_fourcc == 0x52444853U) {
            if (chunk_size < sizeof(std::uint32_t)) {
                diagnostic = {"shader_dxbc_chunk_invalid", "DXBC shader chunk is truncated"};
                return ContainerValidation::invalid;
            }
            supported_chunk = true;
        }
    }
    std::size_t previous_end = table_end;
    for (const auto& range : ranges) {
        if (range.first < previous_end) {
            diagnostic = {"shader_dxbc_chunk_overlap", "DXBC chunks overlap"};
            return ContainerValidation::invalid;
        }
        previous_end = range.second;
    }
    if (!supported_chunk) {
        diagnostic = {"shader_dxbc_chunk_unsupported", "DXBC contains no supported shader chunk"};
        return ContainerValidation::unsupported;
    }
    return ContainerValidation::valid;
}

} // namespace

bool shader_bytecode_format(std::span<const std::byte> bytecode,
                            ShaderBytecodeFormat& format) noexcept {
    if (bytecode.size() < 4U || bytecode.size() % 4U != 0U) return false;
    const auto signature = shader_word(bytecode);
    if (signature == 0x07230203U) {
        format = ShaderBytecodeFormat::spirv;
        return true;
    }
    if (signature == 0x43425844U) {
        format = ShaderBytecodeFormat::dxil;
        return true;
    }
    return false;
}

ShaderModuleStatus validate_shader_module_description(const ShaderModuleDescription& description,
                                                      Diagnostic& diagnostic) {
    if (description.stage != ShaderStage::vertex && description.stage != ShaderStage::fragment &&
        description.stage != ShaderStage::compute) {
        diagnostic = {"shader_stage_invalid", "Shader module stage contains an unknown value"};
        return ShaderModuleStatus::invalid_description;
    }
    if (description.bytecode.empty()) {
        diagnostic = {"shader_bytecode_empty", "Shader module bytecode must not be empty"};
        return ShaderModuleStatus::invalid_description;
    }
    if (description.bytecode.size() > max_shader_module_bytes) {
        diagnostic = {"shader_bytecode_size_limit", "Shader module bytecode exceeds the safety limit"};
        return ShaderModuleStatus::invalid_description;
    }
    if (description.bytecode.size() % 4U != 0U) {
        diagnostic = {"shader_bytecode_alignment", "Shader module bytecode size must be four-byte aligned"};
        return ShaderModuleStatus::invalid_description;
    }
    ShaderBytecodeFormat format{};
    if (!shader_bytecode_format(description.bytecode, format)) {
        diagnostic = {"shader_bytecode_signature", "Shader module bytecode has no supported SPIR-V or DXIL signature"};
        return ShaderModuleStatus::unsupported;
    }
    if (format == ShaderBytecodeFormat::spirv) {
        if (description.bytecode.size() < 20U) {
            diagnostic = {"shader_spirv_header_invalid", "SPIR-V bytecode has an invalid header"};
            return ShaderModuleStatus::invalid_description;
        }
        const auto version = shader_word(description.bytecode.subspan(4U));
        if (version < 0x00010000U || version > 0x00010600U) {
            diagnostic = {"shader_spirv_version_unsupported", "SPIR-V bytecode version is unsupported"};
            return ShaderModuleStatus::unsupported;
        }
        const auto bound = shader_word(description.bytecode.subspan(12U));
        if (bound == 0U || bound > max_spirv_id_bound) {
            diagnostic = {"shader_spirv_bound_invalid", "SPIR-V ID bound is outside the bounded module limit"};
            return ShaderModuleStatus::invalid_description;
        }
        if (shader_word(description.bytecode.subspan(16U)) != 0U) {
            diagnostic = {"shader_spirv_schema_invalid", "SPIR-V reserved schema word must be zero"};
            return ShaderModuleStatus::invalid_description;
        }
        const std::size_t word_count = description.bytecode.size() / sizeof(std::uint32_t);
        std::size_t cursor = 5U;
        if (cursor == word_count) {
            diagnostic = {"shader_spirv_instruction_missing", "SPIR-V module has no instructions"};
            return ShaderModuleStatus::invalid_description;
        }
        while (cursor < word_count) {
            const std::uint32_t instruction = shader_word(
                description.bytecode.subspan(cursor * sizeof(std::uint32_t), sizeof(std::uint32_t)));
            const std::size_t instruction_words = static_cast<std::size_t>(instruction >> 16U);
            if (instruction_words == 0U || instruction_words > word_count - cursor) {
                diagnostic = {"shader_spirv_instruction_truncated", "SPIR-V instruction length exceeds the module"};
                return ShaderModuleStatus::invalid_description;
            }
            cursor += instruction_words;
        }
    } else {
        const ContainerValidation container = validate_dxbc_container(description.bytecode, diagnostic);
        if (container == ContainerValidation::invalid) return ShaderModuleStatus::invalid_description;
        if (container == ContainerValidation::unsupported) return ShaderModuleStatus::unsupported;
    }
    return ShaderModuleStatus::ready;
}

bool valid_sampler_description(const SamplerDescription& description,
                               Diagnostic& diagnostic) {
    return validate_sampler_description(description, diagnostic) == SamplerStatus::ready;
}

bool valid_shader_module_description(const ShaderModuleDescription& description,
                                     Diagnostic& diagnostic) {
    return validate_shader_module_description(description, diagnostic) == ShaderModuleStatus::ready;
}

bool valid_texture_update(const Texture& texture,
                          const TextureUploadPlan& uploads,
                          Diagnostic& diagnostic) {
    return validate_texture_update(texture, uploads, diagnostic) == TextureStatus::ready;
}

bool valid_texture_clear_readback(const Texture& texture,
                                  const TextureClearReadbackRequest& request,
                                  Diagnostic& diagnostic) {
    return validate_texture_clear_readback(texture, request, diagnostic) == TextureReadbackStatus::ready;
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

const char* presentation_target_status_name(
    PresentationTargetStatus status) noexcept {
    switch (status) {
    case PresentationTargetStatus::ready:
        return "ready";
    case PresentationTargetStatus::invalid_description:
        return "invalid_description";
    case PresentationTargetStatus::unsupported:
        return "unsupported";
    case PresentationTargetStatus::allocation_failed:
        return "allocation_failed";
    case PresentationTargetStatus::execution_failed:
        return "execution_failed";
    }
    return "unknown";
}

const char* presentation_frame_status_name(
    PresentationFrameStatus status) noexcept {
    switch (status) {
    case PresentationFrameStatus::ready:
        return "ready";
    case PresentationFrameStatus::invalid_request:
        return "invalid_request";
    case PresentationFrameStatus::unsupported:
        return "unsupported";
    case PresentationFrameStatus::execution_failed:
        return "execution_failed";
    }
    return "unknown";
}

const char* depth_attachment_status_name(DepthAttachmentStatus status) noexcept {
    switch (status) {
    case DepthAttachmentStatus::ready:
        return "ready";
    case DepthAttachmentStatus::invalid_description:
        return "invalid_description";
    case DepthAttachmentStatus::unsupported:
        return "unsupported";
    case DepthAttachmentStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unknown";
}

const char* depth_attachment_readback_status_name(
    DepthAttachmentReadbackStatus status) noexcept {
    switch (status) {
    case DepthAttachmentReadbackStatus::ready:
        return "ready";
    case DepthAttachmentReadbackStatus::invalid_request:
        return "invalid_request";
    case DepthAttachmentReadbackStatus::unsupported:
        return "unsupported";
    case DepthAttachmentReadbackStatus::execution_failed:
        return "execution_failed";
    }
    return "unknown";
}

const char* texture_readback_status_name(TextureReadbackStatus status) noexcept {
    switch (status) {
    case TextureReadbackStatus::ready:
        return "ready";
    case TextureReadbackStatus::invalid_request:
        return "invalid_request";
    case TextureReadbackStatus::unsupported:
        return "unsupported";
    case TextureReadbackStatus::execution_failed:
        return "execution_failed";
    }
    return "unknown";
}

const char* triangle_draw_status_name(TriangleDrawStatus status) noexcept {
    switch (status) {
    case TriangleDrawStatus::ready: return "ready";
    case TriangleDrawStatus::invalid_request: return "invalid_request";
    case TriangleDrawStatus::unsupported: return "unsupported";
    case TriangleDrawStatus::execution_failed: return "execution_failed";
    }
    return "unknown";
}

const char* indexed_static_mesh_draw_status_name(IndexedStaticMeshDrawStatus status) noexcept {
    switch (status) {
    case IndexedStaticMeshDrawStatus::ready: return "ready";
    case IndexedStaticMeshDrawStatus::invalid_request: return "invalid_request";
    case IndexedStaticMeshDrawStatus::unsupported: return "unsupported";
    case IndexedStaticMeshDrawStatus::execution_failed: return "execution_failed";
    }
    return "unknown";
}

const char* indexed_static_mesh_batch_status_name(IndexedStaticMeshBatchStatus status) noexcept {
    switch (status) {
    case IndexedStaticMeshBatchStatus::ready: return "ready";
    case IndexedStaticMeshBatchStatus::invalid_request: return "invalid_request";
    case IndexedStaticMeshBatchStatus::unsupported: return "unsupported";
    case IndexedStaticMeshBatchStatus::execution_failed: return "execution_failed";
    }
    return "unknown";
}

const char* depth_only_indexed_static_mesh_draw_status_name(
    DepthOnlyIndexedStaticMeshDrawStatus status) noexcept {
    switch (status) {
    case DepthOnlyIndexedStaticMeshDrawStatus::ready: return "ready";
    case DepthOnlyIndexedStaticMeshDrawStatus::invalid_request: return "invalid_request";
    case DepthOnlyIndexedStaticMeshDrawStatus::unsupported: return "unsupported";
    case DepthOnlyIndexedStaticMeshDrawStatus::execution_failed: return "execution_failed";
    }
    return "unknown";
}

const char* depth_only_indexed_static_mesh_batch_status_name(
    DepthOnlyIndexedStaticMeshBatchStatus status) noexcept {
    switch (status) {
    case DepthOnlyIndexedStaticMeshBatchStatus::ready: return "ready";
    case DepthOnlyIndexedStaticMeshBatchStatus::invalid_request: return "invalid_request";
    case DepthOnlyIndexedStaticMeshBatchStatus::unsupported: return "unsupported";
    case DepthOnlyIndexedStaticMeshBatchStatus::execution_failed: return "execution_failed";
    }
    return "unknown";
}

const char* sampler_status_name(SamplerStatus status) noexcept {
    switch (status) {
    case SamplerStatus::ready:
        return "ready";
    case SamplerStatus::invalid_description:
        return "invalid_description";
    case SamplerStatus::unsupported:
        return "unsupported";
    case SamplerStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unknown";
}

const char* shader_module_status_name(ShaderModuleStatus status) noexcept {
    switch (status) {
    case ShaderModuleStatus::ready:
        return "ready";
    case ShaderModuleStatus::invalid_description:
        return "invalid_description";
    case ShaderModuleStatus::unsupported:
        return "unsupported";
    case ShaderModuleStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unknown";
}

AdapterResult enumerate_adapters(Backend backend, const DeviceOptions& options) {
    if ((!options.headless && options.enable_headless_presentation) ||
        (options.headless && options.native_surface.has_value())) {
        return invalid_options("presentation_mode_conflict",
                               "Headless and native presentation modes are mutually exclusive");
    }
    if (!options.headless) {
        if (!options.native_surface.has_value()) {
            return invalid_options("native_surface_required",
                                   "Non-headless initialization requires a native presentation source");
        }
        Diagnostic diagnostic;
        if (backend == Backend::D3D12) {
            if (!valid_d3d12_native_window(*options.native_surface, diagnostic))
                return invalid_options(diagnostic.code.c_str(), std::move(diagnostic.message));
        } else if (!valid_native_surface_source(*options.native_surface, diagnostic)) {
            return invalid_options(diagnostic.code.c_str(), std::move(diagnostic.message));
        }
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
    if ((!options.headless && options.enable_headless_presentation) ||
        (options.headless && options.native_surface.has_value())) {
        return invalid_device_options("presentation_mode_conflict",
                                      "Headless and native presentation modes are mutually exclusive");
    }
    if (!options.headless) {
        if (!options.native_surface.has_value()) {
            return invalid_device_options("native_surface_required",
                                          "Non-headless initialization requires a native presentation source");
        }
        Diagnostic diagnostic;
        if (backend == Backend::D3D12) {
            if (!valid_d3d12_native_window(*options.native_surface, diagnostic))
                return invalid_device_options(diagnostic.code.c_str(), std::move(diagnostic.message));
        } else if (!valid_native_surface_source(*options.native_surface, diagnostic)) {
            return invalid_device_options(diagnostic.code.c_str(), std::move(diagnostic.message));
        }
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
