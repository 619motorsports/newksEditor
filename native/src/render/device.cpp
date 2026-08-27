#include "apex/render/device.hpp"
#include "apex/render/selected_mesh.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan.hpp"
#include "apex/render/stock_ks_per_pixel_vulkan_abi.hpp"
#include "backend_internal.hpp"
#include "dxbc_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
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

bool valid_texture_mip_count(const TextureDescription& description) noexcept {
    if (description.width == 0U || description.height == 0U ||
        description.mip_levels == 0U)
        return false;
    std::uint32_t largest_dimension =
        std::max(description.width, description.height);
    std::uint32_t full_chain_levels = 1U;
    while (largest_dimension > 1U) {
        largest_dimension >>= 1U;
        ++full_chain_levels;
    }
    return description.mip_levels <= full_chain_levels;
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
    const bool supported_layers = description.shape == TextureShape::texture_cube ||
                                  description.array_layers == 1U;
    if (description.samples != 1U || description.mutability != TextureMutability::immutable ||
        !supported_layers || description.usage != TextureUsage::sampled) {
        diagnostic = {"texture_compressed_upload_unsupported",
                      "BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 uploads require one-layer 2D or explicit cube, one-sample immutable sampled resources"};
        return TextureStatus::unsupported;
    }
    return TextureStatus::ready;
}

TextureStatus validate_texture_shape_contract(const TextureDescription& description,
                                               Diagnostic& diagnostic) {
    if (description.shape != TextureShape::texture_2d &&
        description.shape != TextureShape::texture_cube) {
        diagnostic = {"texture_shape_unknown", "The texture shape is not recognized by the native contract"};
        return TextureStatus::invalid_description;
    }
    if (description.shape == TextureShape::texture_cube && description.width != description.height) {
        diagnostic = {"texture_cube_dimensions_invalid", "Cube textures require equal width and height"};
        return TextureStatus::invalid_description;
    }
    if (description.shape == TextureShape::texture_cube && description.samples != 1U) {
        diagnostic = {"texture_cube_samples_invalid", "Cube textures require one sample per texel"};
        return TextureStatus::invalid_description;
    }
    return TextureStatus::ready;
}

TextureStatus validate_texture_access_policy_contract(
    const TextureDescription& description, Diagnostic& diagnostic) {
    switch (description.access_policy) {
    case TextureAccessPolicy::fixed_usage:
        return TextureStatus::ready;
    case TextureAccessPolicy::render_then_sample:
        break;
    default:
        diagnostic = {"texture_access_policy_unknown",
                      "The texture access policy is not recognized by the native contract"};
        return TextureStatus::invalid_description;
    }

    constexpr std::uint32_t required_usage =
        static_cast<std::uint32_t>(TextureUsage::sampled) |
        static_cast<std::uint32_t>(TextureUsage::color_attachment) |
        static_cast<std::uint32_t>(TextureUsage::transfer_source);
    if (static_cast<std::uint32_t>(description.usage) != required_usage) {
        diagnostic = {"texture_access_policy_usage_invalid",
                      "The render-then-sample policy requires exactly sampled, color-attachment, and transfer-source usage"};
        return TextureStatus::invalid_description;
    }
    if (description.mutability != TextureMutability::mutable_data) {
        diagnostic = {"texture_access_policy_mutability_invalid",
                      "The render-then-sample policy requires mutable texture data"};
        return TextureStatus::invalid_description;
    }
    if (description.samples != 1U) {
        diagnostic = {"texture_access_policy_samples_invalid",
                      "The render-then-sample policy requires a single-sample texture"};
        return TextureStatus::invalid_description;
    }
    const TextureFormatInfo format = texture_format_info(description.format);
    if (!texture_format_is_known(description.format) ||
        format.classification != TextureFormatClass::uncompressed ||
        format.bytes_per_pixel == 0U) {
        diagnostic = {"texture_access_policy_format_invalid",
                      "The render-then-sample policy requires a known uncompressed color format"};
        return TextureStatus::invalid_description;
    }
    return TextureStatus::ready;
}

TextureStatus validate_texture_scalar_float_contract(const TextureDescription& description,
                                                      Diagnostic& diagnostic) {
    if (description.format != TextureFormat::r32_sfloat) return TextureStatus::ready;
    if (description.samples != 1U || description.mutability != TextureMutability::immutable ||
        description.shape != TextureShape::texture_2d || description.array_layers != 1U ||
        description.usage != TextureUsage::sampled) {
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
    if (format == TextureFormat::rgba16_sfloat ||
        format == TextureFormat::rgba8_unorm || format == TextureFormat::bgra8_unorm ||
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
    const TextureStatus shape_status = validate_texture_shape_contract(description, diagnostic);
    if (shape_status != TextureStatus::ready) return shape_status;
    const TextureStatus access_policy_status =
        validate_texture_access_policy_contract(description, diagnostic);
    if (access_policy_status != TextureStatus::ready) return access_policy_status;
    const TextureStatus sample_status =
        validate_texture_sample_contract(description, !uploads.subresources.empty(), diagnostic);
    if (sample_status != TextureStatus::ready) return sample_status;
    if (description.width > max_texture_dimension || description.height > max_texture_dimension ||
        description.mip_levels > 32U) {
        diagnostic = {"texture_dimension_limit", "Texture upload dimensions exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    const std::uint64_t physical_layers = texture_physical_array_layers(description);
    if (physical_layers > 2048U ||
        static_cast<std::uint64_t>(description.mip_levels) * physical_layers > 65535U) {
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
        if (description.shape == TextureShape::texture_2d && upload.cube_face != CubeFace::none) {
            diagnostic = {"texture_cube_face_unexpected", "A 2D texture upload cannot name a cube face"};
            return TextureStatus::invalid_description;
        }
        if (description.shape == TextureShape::texture_cube && !is_cube_face(upload.cube_face)) {
            diagnostic = {"texture_cube_face_missing", "Each cube texture upload must name one of six faces"};
            return TextureStatus::invalid_description;
        }
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
        const std::uint64_t physical_layer = texture_upload_physical_array_layer(description, upload);
        const std::uint64_t key = (physical_layer << 32U) | upload.mip_level;
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
    const TextureStatus shape_status = validate_texture_shape_contract(description, diagnostic);
    if (shape_status != TextureStatus::ready) return shape_status;
    const TextureStatus access_policy_status =
        validate_texture_access_policy_contract(description, diagnostic);
    if (access_policy_status != TextureStatus::ready) return access_policy_status;
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
    const std::uint64_t physical_layers = texture_physical_array_layers(description);
    if (physical_layers > 2048U) {
        diagnostic = {"texture_layer_limit", "Texture array layers exceed the backend-neutral safety limit"};
        return TextureStatus::invalid_description;
    }
    if (static_cast<std::uint64_t>(description.mip_levels) * physical_layers > 65535U) {
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
            static_cast<std::size_t>(description.mip_levels) * physical_layers) {
        diagnostic = {"texture_compressed_upload_incomplete",
                      "Immutable BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 textures require one upload for every subresource"};
        return TextureStatus::invalid_description;
    }
    constexpr auto max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    std::uint64_t total_bytes = 0U;
    for (std::uint64_t layer = 0; layer < physical_layers; ++layer) {
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

TextureStatus validate_texture_mip_generation_description(
    const TextureDescription& description, Diagnostic& diagnostic) {
    if (description.mip_levels < 2U) {
        diagnostic = {"texture_mip_generation_levels_invalid",
                      "Texture mip generation requires at least two mip levels"};
        return TextureStatus::invalid_description;
    }
    std::uint32_t maximum_dimension =
        std::max(description.width, description.height);
    std::uint32_t full_chain_levels = 0U;
    while (maximum_dimension != 0U) {
        ++full_chain_levels;
        maximum_dimension >>= 1U;
    }
    if (description.mip_levels > full_chain_levels) {
        diagnostic = {"texture_mip_generation_levels_excessive",
                      "Texture mip generation cannot extend beyond the full dimension chain"};
        return TextureStatus::invalid_description;
    }
    if (description.access_policy != TextureAccessPolicy::render_then_sample) {
        diagnostic = {"texture_mip_generation_access_policy_invalid",
                      "Texture mip generation requires the render-then-sample access policy"};
        return TextureStatus::invalid_description;
    }
    Diagnostic description_diagnostic;
    const TextureStatus description_status =
        validate_texture_description(description, {}, description_diagnostic);
    if (description_status != TextureStatus::ready) {
        diagnostic = std::move(description_diagnostic);
        return description_status;
    }
    diagnostic = {};
    return TextureStatus::ready;
}

HdrToneMapStatus validate_hdr_tone_map_parameters(
    const HdrToneMapParameters& parameters, Diagnostic& diagnostic) {
    if (!std::isfinite(parameters.exposure) || parameters.exposure < 0.0F ||
        !std::isfinite(parameters.gamma) || parameters.gamma <= 0.0F ||
        !std::isfinite(parameters.saturation) || parameters.saturation < 0.0F ||
        !std::isfinite(parameters.curve_scale) || parameters.curve_scale < 0.0F ||
        !std::isfinite(parameters.curve_shoulder) ||
        !std::isfinite(parameters.dither_scale) ||
        parameters.dither_scale < 0.0F) {
        diagnostic = {"hdr_tone_map_parameters_invalid",
                      "HDR tone-map parameters must be finite. Gain and dither "
                      "values must be nonnegative, and gamma must be positive"};
        return HdrToneMapStatus::invalid_request;
    }
    diagnostic = {};
    return HdrToneMapStatus::ready;
}

HdrLuminanceStatus validate_hdr_luminance_request(
    const Texture& source, Diagnostic& diagnostic) {
    const TextureDescription& description = source.info().description;
    if (description.width == 0U || description.height == 0U) {
        diagnostic = {"hdr_luminance_dimensions_invalid",
                      "HDR luminance measurement dimensions must be non-zero"};
        return HdrLuminanceStatus::invalid_request;
    }
    if (description.width > max_texture_dimension ||
        description.height > max_texture_dimension) {
        diagnostic = {"hdr_luminance_dimension_limit",
                      "HDR luminance measurement dimensions exceed the backend-neutral safety limit"};
        return HdrLuminanceStatus::invalid_request;
    }
    if (description.format != TextureFormat::rgba16_sfloat) {
        diagnostic = {"hdr_luminance_source_format_unsupported",
                      "HDR luminance measurement requires an RGBA16F source texture"};
        return HdrLuminanceStatus::unsupported;
    }
    if (description.shape != TextureShape::texture_2d ||
        description.array_layers != 1U || description.samples != 1U) {
        diagnostic = {"hdr_luminance_source_shape_invalid",
                      "HDR luminance measurement requires a one-layer, single-sample 2D source"};
        return HdrLuminanceStatus::invalid_request;
    }
    if (description.mip_levels == 0U) {
        diagnostic = {"hdr_luminance_mip_chain_invalid",
                      "HDR luminance measurement requires a non-empty mip chain"};
        return HdrLuminanceStatus::invalid_request;
    }
    std::uint32_t largest_dimension =
        std::max(description.width, description.height);
    std::uint32_t full_chain_levels = 1U;
    while (largest_dimension > 1U) {
        largest_dimension >>= 1U;
        ++full_chain_levels;
    }
    if (description.mip_levels != full_chain_levels) {
        diagnostic = {"hdr_luminance_mip_chain_invalid",
                      "HDR luminance measurement requires the exact full mip chain ending at 1x1"};
        return HdrLuminanceStatus::invalid_request;
    }
    constexpr std::uint32_t required_usage =
        static_cast<std::uint32_t>(TextureUsage::sampled) |
        static_cast<std::uint32_t>(TextureUsage::color_attachment) |
        static_cast<std::uint32_t>(TextureUsage::transfer_source);
    if (static_cast<std::uint32_t>(description.usage) != required_usage) {
        diagnostic = {"hdr_luminance_source_usage_invalid",
                      "HDR luminance measurement requires sampled, color-attachment, and transfer-source usage"};
        return HdrLuminanceStatus::invalid_request;
    }
    if (description.mutability != TextureMutability::mutable_data) {
        diagnostic = {"hdr_luminance_source_mutability_invalid",
                      "HDR luminance measurement requires a mutable source texture"};
        return HdrLuminanceStatus::invalid_request;
    }
    if (description.access_policy != TextureAccessPolicy::render_then_sample) {
        diagnostic = {"hdr_luminance_source_access_policy_invalid",
                      "HDR luminance measurement requires render-then-sample source access"};
        return HdrLuminanceStatus::invalid_request;
    }
    Diagnostic description_diagnostic;
    const TextureStatus description_status =
        validate_texture_description(description, {}, description_diagnostic);
    if (description_status != TextureStatus::ready) {
        diagnostic = {description_diagnostic.code.empty()
                          ? "hdr_luminance_source_invalid"
                          : description_diagnostic.code,
                      description_diagnostic.message};
        return description_status == TextureStatus::unsupported
                   ? HdrLuminanceStatus::unsupported
                   : HdrLuminanceStatus::invalid_request;
    }
    diagnostic = {};
    return HdrLuminanceStatus::ready;
}

HdrToneMapStatus validate_hdr_tone_map_request(
    const Texture& source, const Texture& destination,
    const HdrToneMapParameters& parameters, Diagnostic& diagnostic) {
    if (&source == &destination) {
        diagnostic = {"hdr_tone_map_alias_invalid",
                      "HDR tone mapping requires different source and destination textures"};
        return HdrToneMapStatus::invalid_request;
    }
    if (source.backend() != destination.backend()) {
        diagnostic = {"hdr_tone_map_backend_mismatch",
                      "HDR tone-map textures must use the same graphics backend"};
        return HdrToneMapStatus::unsupported;
    }
    const TextureDescription& input = source.info().description;
    const TextureDescription& output = destination.info().description;
    if (input.width == 0U || input.height == 0U || output.width == 0U ||
        output.height == 0U) {
        diagnostic = {"hdr_tone_map_dimensions_invalid",
                      "HDR tone-map texture dimensions must be non-zero"};
        return HdrToneMapStatus::invalid_request;
    }
    if (input.format != TextureFormat::rgba16_sfloat) {
        diagnostic = {"hdr_tone_map_source_format_unsupported",
                      "HDR tone mapping requires an RGBA16F source texture"};
        return HdrToneMapStatus::unsupported;
    }
    const bool output_format_supported =
        output.format == TextureFormat::rgba8_unorm ||
        output.format == TextureFormat::rgba8_srgb ||
        output.format == TextureFormat::bgra8_unorm ||
        output.format == TextureFormat::bgra8_srgb;
    if (!output_format_supported) {
        diagnostic = {"hdr_tone_map_destination_format_unsupported",
                      "HDR tone mapping requires an RGBA8 or BGRA8 destination texture"};
        return HdrToneMapStatus::unsupported;
    }
    const auto valid_source_image = [&](const TextureDescription& description) {
        return description.shape == TextureShape::texture_2d &&
               description.array_layers == 1U &&
               valid_texture_mip_count(description) && description.samples == 1U;
    };
    const auto valid_destination_image = [](const TextureDescription& description) {
        return description.shape == TextureShape::texture_2d &&
               description.array_layers == 1U &&
               description.mip_levels == 1U && description.samples == 1U;
    };
    if (!valid_source_image(input)) {
        diagnostic = {"hdr_tone_map_source_shape_invalid",
                      "HDR tone mapping requires a one-layer, valid-mip-chain, single-sample 2D source"};
        return HdrToneMapStatus::invalid_request;
    }
    if (!valid_destination_image(output)) {
        diagnostic = {"hdr_tone_map_destination_shape_invalid",
                      "HDR tone mapping requires a one-layer, one-mip, single-sample 2D destination"};
        return HdrToneMapStatus::invalid_request;
    }
    if (input.width != output.width || input.height != output.height) {
        diagnostic = {"hdr_tone_map_dimensions_mismatch",
                      "HDR tone-map source and destination dimensions must match"};
        return HdrToneMapStatus::invalid_request;
    }
    const auto has_usage = [](TextureUsage usage, TextureUsage bit) {
        return static_cast<std::uint32_t>(usage & bit) != 0U;
    };
    if (!has_usage(input.usage, TextureUsage::sampled)) {
        diagnostic = {"hdr_tone_map_source_usage_invalid",
                      "HDR tone mapping requires sampled source usage"};
        return HdrToneMapStatus::invalid_request;
    }
    if (!has_usage(output.usage, TextureUsage::color_attachment) ||
        !has_usage(output.usage, TextureUsage::transfer_source)) {
        diagnostic = {"hdr_tone_map_destination_usage_invalid",
                      "HDR tone mapping requires color-attachment and transfer-source destination usage"};
        return HdrToneMapStatus::invalid_request;
    }
    if (output.mutability != TextureMutability::mutable_data) {
        diagnostic = {"hdr_tone_map_destination_mutability_invalid",
                      "HDR tone mapping requires a mutable destination texture"};
        return HdrToneMapStatus::invalid_request;
    }
    return validate_hdr_tone_map_parameters(parameters, diagnostic);
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
    if (description.shape != TextureShape::texture_2d) {
        diagnostic = {"texture_readback_shape_unsupported",
                      "Texture clear/readback requires an explicit 2D texture"};
        return TextureReadbackStatus::unsupported;
    }
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
        description.array_layers != 1U || description.shape != TextureShape::texture_2d) {
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
        case TextureFormat::rgba16_sfloat: return PipelineRenderTargetFormat::rgba16_float;
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

bool pipeline_declares_multimap_reflection(
    const PipelineProgram& pipeline) noexcept {
    std::array<bool, 3U> found{};
    for (const PipelineResourceBinding& resource : pipeline.resources) {
        if (resource.set != 0U ||
            resource.binding < portable_multimap_cube_texture_binding ||
            resource.binding > portable_multimap_reflection_constants_binding)
            continue;
        const std::size_t index =
            resource.binding - portable_multimap_cube_texture_binding;
        const PipelineResourceKind expected =
            resource.binding == portable_multimap_cube_texture_binding
                ? PipelineResourceKind::sampled_texture
            : resource.binding == portable_multimap_cube_sampler_binding
                ? PipelineResourceKind::sampler
                : PipelineResourceKind::uniform_buffer;
        if (resource.kind != expected || found[index]) return false;
        found[index] = true;
    }
    return std::all_of(found.begin(), found.end(),
                       [](const bool value) { return value; });
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
    const bool has_reflection = pipeline_declares_multimap_reflection(pipeline);
    const bool has_reflection_range = std::any_of(
        pipeline.resources.begin(), pipeline.resources.end(),
        [](const PipelineResourceBinding& resource) {
            return resource.set == 0U &&
                   resource.binding >= portable_multimap_cube_texture_binding &&
                   resource.binding <=
                       portable_multimap_reflection_constants_binding;
        });
    if (has_reflection_range && !has_reflection)
        return IndexedPortableResourceLayout::unsupported;
    const std::size_t material_resource_count =
        pipeline.resources.size() - (has_receiver ? 5U : 0U) -
        (has_reflection ? 3U : 0U);
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
        if (resource.set == 0U &&
            resource.binding >= portable_multimap_cube_texture_binding &&
            resource.binding <= portable_multimap_reflection_constants_binding &&
            has_reflection)
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
    const TextureDescription& target, Diagnostic& diagnostic,
    const bool allow_cube_target = false,
    const bool allow_float_target = false) {
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
    constexpr auto multisample_forbidden_usage =
        static_cast<std::uint32_t>(TextureUsage::sampled) |
        static_cast<std::uint32_t>(TextureUsage::transfer_destination) |
        static_cast<std::uint32_t>(TextureUsage::storage);
    if (target.samples != 1U &&
        ((target_usage & multisample_forbidden_usage) != 0U ||
         target.access_policy != TextureAccessPolicy::fixed_usage)) {
        diagnostic = {
            "indexed_static_mesh_target_multisample_usage_invalid",
            "A multisample color target cannot be sampled, uploaded, used as storage, or use a changing access policy"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const bool valid_shape = target.shape == TextureShape::texture_2d ||
                             (allow_cube_target && target.shape == TextureShape::texture_cube);
    const bool allow_single_sample_hdr_mips =
        allow_float_target && target.format == TextureFormat::rgba16_sfloat &&
        target.samples == 1U && target.shape == TextureShape::texture_2d;
    const bool valid_subresource_count = target.shape == TextureShape::texture_cube
                                             ? target.mip_levels != 0U && target.array_layers != 0U
                                             : target.array_layers == 1U &&
                                                   (allow_single_sample_hdr_mips
                                                        ? valid_texture_mip_count(target)
                                                        : target.mip_levels == 1U);
    if (target.width == 0U || target.height == 0U || !valid_subresource_count || !valid_shape) {
        diagnostic = {"indexed_static_mesh_target_invalid",
                      "Indexed static-mesh target dimensions or subresource count are invalid"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (target.shape == TextureShape::texture_cube && target.samples != 1U) {
        diagnostic = {"indexed_static_mesh_target_cube_samples_unsupported",
                      "Cube indexed static-mesh targets require exactly one sample"};
        return IndexedStaticMeshDrawStatus::unsupported;
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
    if (target.format == TextureFormat::rgba16_sfloat && allow_float_target)
        return IndexedStaticMeshDrawStatus::ready;
    switch (target.format) {
    case TextureFormat::rgba8_unorm:
    case TextureFormat::rgba8_srgb:
    case TextureFormat::bgra8_unorm:
    case TextureFormat::bgra8_srgb:
        return IndexedStaticMeshDrawStatus::ready;
    default:
        diagnostic = {"indexed_static_mesh_target_format_unsupported",
                      "Indexed static-mesh drawing supports RGBA8 and BGRA8 targets; non-readback batches also support RGBA16F"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
}

bool native_sampler_matches(const SamplerDescription& actual,
                            const SamplerDescription& expected) noexcept {
    return actual.min_filter == expected.min_filter &&
           actual.mag_filter == expected.mag_filter &&
           actual.mip_filter == expected.mip_filter &&
           actual.address_u == expected.address_u &&
           actual.address_v == expected.address_v &&
           actual.address_w == expected.address_w &&
           actual.compare == expected.compare &&
           actual.mip_lod_bias == expected.mip_lod_bias &&
           actual.max_anisotropy == expected.max_anisotropy &&
           actual.min_lod == expected.min_lod &&
           actual.max_lod == expected.max_lod;
}

IndexedStaticMeshDrawStatus validate_native_ks_per_pixel_binding(
    const Texture& target, const IndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic) {
    const auto* binding = request.stock_ks_per_pixel_native;
    const auto* resources = binding != nullptr ? binding->resources : nullptr;
    if (resources == nullptr || !resources->ready() ||
        binding->diffuse_texture == nullptr ||
        std::any_of(binding->shadow_maps.begin(), binding->shadow_maps.end(),
                    [](const DepthAttachment* map) { return map == nullptr; })) {
        diagnostic = {"indexed_stock_native_binding_missing",
                      "Native ksPerPixel drawing requires its shader, constants, samplers, diffuse texture, and three shadow maps"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (target.backend() != Backend::D3D12) {
        diagnostic = {"indexed_stock_native_backend_unsupported",
                      "Installed ksPerPixel DXBC execution requires D3D12"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const auto& shaders = resources->shader_program();
    if (shaders.source().validation_status() !=
        StockKsPerPixelNativeProgramStatus::ready) {
        diagnostic = {"indexed_stock_native_shader_invalid",
                      "Native ksPerPixel drawing requires an intact validated shader owner"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (shaders.vertex_shader().backend() != Backend::D3D12 ||
        shaders.pixel_shader().backend() != Backend::D3D12 ||
        shaders.vertex_shader().info().stage != ShaderStage::vertex ||
        shaders.pixel_shader().info().stage != ShaderStage::fragment ||
        shaders.vertex_shader().info().format != ShaderBytecodeFormat::dxbc ||
        shaders.pixel_shader().info().format != ShaderBytecodeFormat::dxbc) {
        diagnostic = {"indexed_stock_native_shader_backend_mismatch",
                      "Native ksPerPixel shader modules must be D3D12 DXBC vertex and pixel stages"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    for (std::size_t index = 0U; index < static_cast<std::size_t>(
             StockKsPerPixelNativeConstantSlot::count); ++index) {
        const Buffer* buffer = resources->constant_buffers().buffer(
            static_cast<StockKsPerPixelNativeConstantSlot>(index));
        if (buffer == nullptr || buffer->backend() != Backend::D3D12) {
            diagnostic = {"indexed_stock_native_constant_backend_mismatch",
                          "Every native ksPerPixel constant buffer must belong to D3D12"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const auto& description = buffer->info().description;
        if (description.size_bytes != stock_ks_per_pixel_native_constant_buffer_view_bytes ||
            description.usage != BufferUsage::uniform ||
            description.memory != BufferMemory::host_visible ||
            description.mutability != BufferMutability::mutable_data) {
            diagnostic = {"indexed_stock_native_constant_description_invalid",
                          "Every native ksPerPixel constant buffer must preserve its 256-byte mutable uniform view"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
    }
    const Sampler* linear = resources->samplers().sampler(
        StockKsPerPixelNativeSamplerSlot::linear);
    const Sampler* shadow = resources->samplers().sampler(
        StockKsPerPixelNativeSamplerSlot::shadow);
    if (linear == nullptr || shadow == nullptr ||
        linear->backend() != Backend::D3D12 ||
        shadow->backend() != Backend::D3D12) {
        diagnostic = {"indexed_stock_native_sampler_backend_mismatch",
                      "Both native ksPerPixel samplers must belong to D3D12"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    SamplerDescription expected_linear;
    expected_linear.min_filter = SamplerFilter::anisotropic;
    expected_linear.mag_filter = SamplerFilter::anisotropic;
    expected_linear.mip_filter = SamplerFilter::anisotropic;
    expected_linear.max_anisotropy = linear->info().description.max_anisotropy;
    expected_linear.mip_lod_bias = linear->info().description.mip_lod_bias;
    expected_linear.max_lod = std::numeric_limits<float>::max();
    SamplerDescription expected_shadow;
    expected_shadow.min_filter = SamplerFilter::linear;
    expected_shadow.mag_filter = SamplerFilter::linear;
    expected_shadow.mip_filter = SamplerFilter::nearest;
    expected_shadow.address_u = SamplerAddressMode::clamp_to_edge;
    expected_shadow.address_v = SamplerAddressMode::clamp_to_edge;
    expected_shadow.address_w = SamplerAddressMode::clamp_to_edge;
    expected_shadow.compare = SamplerCompare::less;
    expected_shadow.max_lod = std::numeric_limits<float>::max();
    if (linear->info().description.max_anisotropy < 2.0F ||
        !native_sampler_matches(linear->info().description, expected_linear) ||
        !native_sampler_matches(shadow->info().description, expected_shadow)) {
        diagnostic = {"indexed_stock_native_sampler_contract_invalid",
                      "Native ksPerPixel samplers do not match the recovered s0 and s1 contract"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const Texture& diffuse = *binding->diffuse_texture;
    const auto& diffuse_description = diffuse.info().description;
    if (&diffuse == &target) {
        diagnostic = {"indexed_stock_native_diffuse_feedback_loop",
                      "The native diffuse texture cannot also be the color target"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (diffuse.backend() != Backend::D3D12) {
        diagnostic = {"indexed_stock_native_diffuse_backend_mismatch",
                      "The native diffuse texture must belong to D3D12"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if ((static_cast<std::uint32_t>(diffuse_description.usage) &
         static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
        diffuse_description.width == 0U || diffuse_description.height == 0U ||
        diffuse_description.mip_levels == 0U ||
        diffuse_description.array_layers != 1U || diffuse_description.samples != 1U ||
        diffuse_description.shape != TextureShape::texture_2d ||
        !portable_sampled_color_format(diffuse_description.format, true)) {
        diagnostic = {"indexed_stock_native_diffuse_description_unsupported",
                      "The native diffuse binding requires a one-layer sampled color texture"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const DepthAttachmentDescription* first_shadow = nullptr;
    for (std::size_t index = 0U; index < binding->shadow_maps.size(); ++index) {
        const DepthAttachment* map = binding->shadow_maps[index];
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (map == binding->shadow_maps[previous]) {
                diagnostic = {"indexed_stock_native_shadow_map_duplicate",
                              "Native ksPerPixel shadow slots require three distinct maps"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        if (map == request.depth_attachment) {
            diagnostic = {"indexed_stock_native_shadow_feedback_loop",
                          "A writable main-pass depth attachment cannot be sampled as a native shadow map"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (map->backend() != Backend::D3D12) {
            diagnostic = {"indexed_stock_native_shadow_backend_mismatch",
                          "Every native shadow map must belong to D3D12"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const auto& description = map->info().description;
        if (!description.shader_readable || description.samples != 1U ||
            description.format != DepthAttachmentFormat::d32_float) {
            diagnostic = {"indexed_stock_native_shadow_description_unsupported",
                          "Native ksPerPixel shadows require shader-readable single-sample D32 maps"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        if (first_shadow != nullptr &&
            (description.width != first_shadow->width ||
             description.height != first_shadow->height)) {
            diagnostic = {"indexed_stock_native_shadow_dimensions_mismatch",
                          "All native ksPerPixel shadow maps must have equal dimensions"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        first_shadow = &description;
    }
    return IndexedStaticMeshDrawStatus::ready;
}

bool has_portable_stock_resource_binding(
    const IndexedStaticMeshDrawRequest& request,
    bool include_packet_resources = true) noexcept {
    return request.sampled_binding.texture != nullptr ||
           request.sampled_binding.sampler != nullptr ||
           request.normal_binding.texture != nullptr ||
           request.normal_binding.sampler != nullptr ||
           request.maps_binding.texture != nullptr ||
           request.maps_binding.sampler != nullptr ||
           request.detail_binding.texture != nullptr ||
           request.detail_binding.sampler != nullptr ||
           request.normal_detail_binding.texture != nullptr ||
           request.normal_detail_binding.sampler != nullptr ||
           request.damage_binding.texture != nullptr ||
           request.damage_binding.sampler != nullptr ||
           request.damage_mask_binding.texture != nullptr ||
           request.damage_mask_binding.sampler != nullptr ||
           request.material_binding.buffer != nullptr ||
           request.material_binding.offset_bytes != 0U ||
           request.material_binding.range_bytes != 0U ||
           request.frame_binding.buffer != nullptr ||
           request.frame_binding.offset_bytes != 0U ||
           request.frame_binding.range_bytes != 0U ||
           std::any_of(request.directional_shadow_binding.maps.begin(),
                       request.directional_shadow_binding.maps.end(),
                       [](const DepthAttachment* map) {
                           return map != nullptr;
                       }) ||
           request.directional_shadow_binding.sampler != nullptr ||
           request.directional_shadow_binding.constants != nullptr ||
           request.directional_shadow_binding.constants_offset_bytes != 0U ||
           request.directional_shadow_binding.constants_range_bytes != 0U ||
           (include_packet_resources && !request.packet->resources.empty()) ||
           request.resource_authority !=
               IndexedResourceAuthority::packet_contract;
}

IndexedStaticMeshDrawStatus validate_stock_ks_per_pixel_vulkan_native_abi_binding(
    const Texture& target, const IndexedStaticMeshDrawRequest& request,
    const StockKsPerPixelVulkanNativeAbiDrawBinding* binding,
    Diagnostic& diagnostic) {
    if (binding == nullptr) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_binding_missing",
                      "Vulkan ABI probe drawing requires its complete non-owning binding"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (target.backend() != Backend::Vulkan) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_backend_unsupported",
                      "The recovered Vulkan ABI probe requires a Vulkan color target"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    constexpr std::array<std::uint32_t, 5U> record_bytes = {
        224U, 64U, 160U, 208U, 32U};
    for (std::size_t index = 0U; index < binding->uniform_buffers.size(); ++index) {
        const StockKsPerPixelVulkanAbiUniformBufferView& view =
            binding->uniform_buffers[index];
        if (view.buffer == nullptr) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_uniform_missing",
                          "The Vulkan ABI probe requires all five uniform-buffer views"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (view.buffer->backend() != Backend::Vulkan) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_uniform_backend_mismatch",
                          "Vulkan ABI probe uniform buffers must belong to Vulkan"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const BufferDescription& description = view.buffer->info().description;
        if (description.usage != BufferUsage::uniform ||
            view.offset_bytes % stock_ks_per_pixel_native_constant_buffer_view_bytes != 0U ||
            view.range_bytes != stock_ks_per_pixel_native_constant_buffer_view_bytes ||
            view.range_bytes < record_bytes[index] ||
            view.offset_bytes > description.size_bytes ||
            static_cast<std::uint64_t>(view.range_bytes) >
                description.size_bytes - view.offset_bytes) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_uniform_view_invalid",
                          "Vulkan ABI probe uniform views require aligned bounded 256-byte uniform ranges"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
    }

    const Texture* diffuse = binding->diffuse_texture;
    if (diffuse == nullptr) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_diffuse_missing",
                      "Vulkan ABI probe drawing requires a diffuse texture"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (diffuse == &target) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_diffuse_feedback_loop",
                      "The Vulkan ABI probe diffuse texture cannot also be the color target"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (diffuse->backend() != Backend::Vulkan) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_diffuse_backend_mismatch",
                      "The Vulkan ABI probe diffuse texture must belong to Vulkan"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const TextureDescription& diffuse_description = diffuse->info().description;
    const std::uint32_t diffuse_usage =
        static_cast<std::uint32_t>(diffuse_description.usage);
    if ((diffuse_usage & static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U ||
        (diffuse_usage & (static_cast<std::uint32_t>(TextureUsage::color_attachment) |
                          static_cast<std::uint32_t>(TextureUsage::storage))) != 0U ||
        diffuse_description.width == 0U || diffuse_description.height == 0U ||
        diffuse_description.mip_levels == 0U || diffuse_description.array_layers != 1U ||
        diffuse_description.shape != TextureShape::texture_2d ||
        diffuse_description.samples != 1U ||
        !portable_sampled_color_format(diffuse_description.format, true)) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_diffuse_invalid",
                      "The Vulkan ABI probe diffuse texture must be a one-layer sampled color image"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }

    const DepthAttachmentDescription* first_shadow = nullptr;
    for (std::size_t index = 0U; index < binding->shadow_maps.size(); ++index) {
        const DepthAttachment* shadow = binding->shadow_maps[index];
        if (shadow == nullptr) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_shadow_missing",
                          "The Vulkan ABI probe requires all three shadow maps"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (shadow == binding->shadow_maps[previous]) {
                diagnostic = {"indexed_stock_vulkan_abi_probe_shadow_duplicate",
                              "Vulkan ABI probe shadow slots require three distinct maps"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
        if (shadow == request.depth_attachment) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_shadow_feedback_loop",
                          "A writable main-pass depth attachment cannot be a Vulkan ABI probe shadow map"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (shadow->backend() != Backend::Vulkan) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_shadow_backend_mismatch",
                          "Vulkan ABI probe shadow maps must belong to Vulkan"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
        const DepthAttachmentDescription& description = shadow->info().description;
        if (!description.shader_readable || description.samples != 1U ||
            description.format != DepthAttachmentFormat::d32_float) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_shadow_invalid",
                          "Vulkan ABI probe shadows require shader-readable single-sample D32 maps"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (first_shadow != nullptr &&
            (description.width != first_shadow->width ||
             description.height != first_shadow->height)) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_shadow_dimensions_mismatch",
                          "Vulkan ABI probe shadow maps must have equal dimensions"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        first_shadow = &description;
    }

    const Sampler* linear = binding->linear_sampler;
    const Sampler* shadow = binding->shadow_sampler;
    if (linear == nullptr || shadow == nullptr) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_sampler_missing",
                      "Vulkan ABI probe drawing requires linear and comparison samplers"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (linear->backend() != Backend::Vulkan || shadow->backend() != Backend::Vulkan) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_sampler_backend_mismatch",
                      "Vulkan ABI probe samplers must belong to Vulkan"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    Diagnostic sampler_diagnostic;
    if (validate_sampler_description(linear->info().description, sampler_diagnostic) !=
            SamplerStatus::ready ||
        validate_sampler_description(shadow->info().description, sampler_diagnostic) !=
            SamplerStatus::ready) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_sampler_invalid",
                      "Vulkan ABI probe samplers must pass sampler description validation"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const SamplerDescription& linear_description = linear->info().description;
    const SamplerDescription& shadow_description = shadow->info().description;
    const SamplerDescription expected_linear = [] {
        SamplerDescription description;
        description.min_filter = SamplerFilter::anisotropic;
        description.mag_filter = SamplerFilter::anisotropic;
        description.mip_filter = SamplerFilter::anisotropic;
        description.max_lod = std::numeric_limits<float>::max();
        return description;
    }();
    SamplerDescription linear_contract = expected_linear;
    linear_contract.max_anisotropy = linear_description.max_anisotropy;
    linear_contract.mip_lod_bias = linear_description.mip_lod_bias;
    SamplerDescription expected_shadow;
    expected_shadow.min_filter = SamplerFilter::linear;
    expected_shadow.mag_filter = SamplerFilter::linear;
    expected_shadow.mip_filter = SamplerFilter::nearest;
    expected_shadow.address_u = SamplerAddressMode::clamp_to_edge;
    expected_shadow.address_v = SamplerAddressMode::clamp_to_edge;
    expected_shadow.address_w = SamplerAddressMode::clamp_to_edge;
    expected_shadow.compare = SamplerCompare::less;
    expected_shadow.max_lod = std::numeric_limits<float>::max();
    if (!std::isfinite(linear_description.max_anisotropy) ||
        linear_description.max_anisotropy < 2.0F ||
        linear_description.max_anisotropy > 16.0F ||
        std::trunc(linear_description.max_anisotropy) !=
            linear_description.max_anisotropy ||
        !std::isfinite(linear_description.mip_lod_bias) ||
        linear_description.mip_lod_bias < -16.0F ||
        linear_description.mip_lod_bias > 16.0F ||
        !native_sampler_matches(linear_description, linear_contract) ||
        !native_sampler_matches(shadow_description, expected_shadow)) {
        diagnostic = {"indexed_stock_vulkan_abi_probe_sampler_contract_invalid",
                      "Vulkan ABI probe samplers do not match the recovered s0 and s1 contract"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    return IndexedStaticMeshDrawStatus::ready;
}

IndexedStaticMeshDrawStatus validate_stock_ks_per_pixel_vulkan_abi_probe_binding(
    const Texture& target, const IndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic) {
    return validate_stock_ks_per_pixel_vulkan_native_abi_binding(
        target, request, request.stock_ks_per_pixel_vulkan_abi_probe,
        diagnostic);
}

IndexedStaticMeshDrawStatus validate_stock_ks_per_pixel_vulkan_source_binding(
    const Texture& target, const IndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic) {
    const StockKsPerPixelVulkanSourceDrawBinding* binding =
        request.stock_ks_per_pixel_vulkan_source;
    if (binding == nullptr || binding->program == nullptr) {
        diagnostic = {
            "indexed_stock_vulkan_source_binding_missing",
            "Vulkan source-equivalent drawing requires its validated owner and native ABI resources"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (&binding->program->pipeline() != request.pipeline) {
        diagnostic = {
            "indexed_stock_vulkan_source_owner_mismatch",
            "The Vulkan source-equivalent request must use its owner's immutable pipeline"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const StockKsPerPixelVulkanSourceStatus source_status =
        binding->program->validation_status();
    if (source_status != StockKsPerPixelVulkanSourceStatus::ready) {
        diagnostic = {
            "indexed_stock_vulkan_source_program_" + std::string(
                stock_ks_per_pixel_vulkan_source_status_name(source_status)),
            "The Vulkan source-equivalent owner no longer passes its allocation gate"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const StockKsPerPixelVariant variant = binding->program->variant();
    const bool alpha_to_coverage =
        variant == StockKsPerPixelVariant::alpha_to_coverage;
    if (request.packet->flags.alpha_to_coverage != alpha_to_coverage ||
        request.pipeline->blend.alpha_to_coverage != alpha_to_coverage) {
        diagnostic = {
            "indexed_stock_vulkan_source_variant_state_mismatch",
            "The Vulkan source-equivalent owner, packet, and pipeline A2C state must match"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    const std::uint32_t target_samples =
        target.info().description.samples;
    if ((alpha_to_coverage && target_samples != 4U) ||
        (!alpha_to_coverage && target_samples != 1U &&
         target_samples != 4U)) {
        diagnostic = {
            alpha_to_coverage
                ? "indexed_stock_vulkan_source_a2c_target_samples_invalid"
                : "indexed_stock_vulkan_source_base_target_samples_invalid",
            alpha_to_coverage
                ? "Vulkan source-equivalent alpha-to-coverage requires a 4x target"
                : "Vulkan source-equivalent base rendering requires a 1x or 4x target"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    const IndexedStaticMeshDrawStatus status =
        validate_stock_ks_per_pixel_vulkan_native_abi_binding(
            target, request, &binding->resources, diagnostic);
    if (status != IndexedStaticMeshDrawStatus::ready) {
        constexpr std::string_view probe_prefix =
            "indexed_stock_vulkan_abi_probe_";
        if (diagnostic.code.starts_with(probe_prefix)) {
            diagnostic.code = "indexed_stock_vulkan_source_" +
                diagnostic.code.substr(probe_prefix.size());
        }
        constexpr std::string_view probe_name = "Vulkan ABI probe";
        const std::size_t name = diagnostic.message.find(probe_name);
        if (name != std::string::npos) {
            diagnostic.message.replace(
                name, probe_name.size(), "Vulkan source-equivalent draw");
        }
    }
    return status;
}

IndexedStaticMeshBatchStatus validate_indexed_target_subresource(
    const TextureDescription& target, const TextureTargetSubresource& subresource,
    Diagnostic& diagnostic) {
    if (target.shape == TextureShape::texture_cube) {
        if (subresource.cube_face == CubeFace::none) {
            diagnostic = {"indexed_static_mesh_target_cube_face_missing",
                          "Cube indexed static-mesh targets require an explicit face"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (!is_cube_face(subresource.cube_face)) {
            diagnostic = {"indexed_static_mesh_target_cube_face_invalid",
                          "Cube indexed static-mesh target face is invalid"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (subresource.array_layer >= target.array_layers) {
            diagnostic = {"indexed_static_mesh_target_cube_layer_out_of_range",
                          "Cube indexed static-mesh target layer is out of range"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (subresource.mip_level >= target.mip_levels) {
            diagnostic = {"indexed_static_mesh_target_cube_mip_out_of_range",
                          "Cube indexed static-mesh target mip is out of range"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (subresource.mip_level != 0U) {
            diagnostic = {"indexed_static_mesh_target_cube_mip_unsupported",
                          "Cube indexed static-mesh targets support only mip zero"};
            return IndexedStaticMeshBatchStatus::unsupported;
        }
        diagnostic = {};
        return IndexedStaticMeshBatchStatus::ready;
    }
    if (subresource.cube_face != CubeFace::none) {
        diagnostic = {"indexed_static_mesh_target_cube_face_unexpected",
                      "2D indexed static-mesh targets cannot specify a cube face"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (subresource.mip_level != 0U) {
        diagnostic = {"indexed_static_mesh_target_subresource_mip_out_of_range",
                      "2D indexed static-mesh target mip is out of range"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    if (subresource.array_layer != 0U) {
        diagnostic = {"indexed_static_mesh_target_subresource_layer_out_of_range",
                      "2D indexed static-mesh target layer is out of range"};
        return IndexedStaticMeshBatchStatus::invalid_request;
    }
    diagnostic = {};
    return IndexedStaticMeshBatchStatus::ready;
}

} // namespace

static IndexedStaticMeshDrawStatus validate_indexed_static_mesh_draw_request_internal(
    const Texture& texture, const IndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic, const bool allow_cube_target,
    const bool allow_float_target) {
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
        validate_indexed_color_target(target, diagnostic, allow_cube_target,
                                      allow_float_target);
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
        case TextureFormat::rgba16_sfloat: return PipelineRenderTargetFormat::rgba16_float;
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
    const bool stock_native = request.shader_authority ==
        IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
    const bool stock_vulkan_source = request.shader_authority ==
        IndexedShaderAuthority::
            explicit_stock_ks_per_pixel_vulkan_source_equivalent;
    const bool stock_vulkan_abi_probe = request.shader_authority ==
        IndexedShaderAuthority::explicit_stock_ks_per_pixel_vulkan_abi_probe;
    if (!static_mesh && !skinned_mesh) {
        diagnostic = {"indexed_static_mesh_primitive_unsupported",
                      "Indexed drawing requires a static or skinned mesh primitive"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (!packet.shader_execution_supported &&
        request.shader_authority != IndexedShaderAuthority::explicit_pipeline &&
        !stock_native && !stock_vulkan_source && !stock_vulkan_abi_probe) {
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
    if ((stock_native || stock_vulkan_source || stock_vulkan_abi_probe) &&
        !static_mesh) {
        diagnostic = {stock_vulkan_source
                          ? "indexed_stock_vulkan_source_primitive_unsupported"
                      : stock_vulkan_abi_probe
                          ? "indexed_stock_vulkan_abi_probe_primitive_unsupported"
                          : "indexed_stock_native_primitive_unsupported",
                      "Native ksPerPixel ABI execution supports only static mesh draws"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (!stock_native && !stock_vulkan_source && !stock_vulkan_abi_probe &&
        request.pipeline->transform_contract !=
                             PipelineTransformContract::draw_matrices) {
        diagnostic = {"indexed_transform_contract_required",
                      "Indexed static-mesh execution requires the draw-matrices shader contract"};
        return IndexedStaticMeshDrawStatus::unsupported;
    }
    if (!stock_native && !stock_vulkan_source && !stock_vulkan_abi_probe &&
        !request.camera_frame.has_value()) {
        diagnostic = {"indexed_camera_frame_missing",
                      "Indexed static-mesh transform execution requires a camera frame"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    if (!stock_native && !stock_vulkan_source && !stock_vulkan_abi_probe) {
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
    if (stock_native) {
        if (request.stock_ks_per_pixel_vulkan_abi_probe != nullptr ||
            request.stock_ks_per_pixel_vulkan_source != nullptr) {
            diagnostic = request.stock_ks_per_pixel_vulkan_source != nullptr
                             ? Diagnostic{
                                   "indexed_stock_vulkan_source_binding_unexpected",
                                   "The Vulkan source-equivalent binding requires its matching shader authority"}
                             : Diagnostic{
                                   "indexed_stock_vulkan_abi_probe_binding_unexpected",
                                   "The Vulkan ABI probe binding requires its matching shader authority"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const IndexedStaticMeshDrawStatus native_status =
            validate_native_ks_per_pixel_binding(texture, request, diagnostic);
        if (native_status != IndexedStaticMeshDrawStatus::ready)
            return native_status;
    } else if (stock_vulkan_abi_probe) {
        if (request.stock_ks_per_pixel_native != nullptr ||
            request.stock_ks_per_pixel_vulkan_source != nullptr) {
            diagnostic = request.stock_ks_per_pixel_vulkan_source != nullptr
                             ? Diagnostic{
                                   "indexed_stock_vulkan_source_binding_unexpected",
                                   "The Vulkan source-equivalent binding requires its matching shader authority"}
                             : Diagnostic{
                                   "indexed_stock_native_binding_unexpected",
                                   "The installed-native binding requires its matching shader authority"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (has_portable_stock_resource_binding(request)) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_portable_binding_overlap",
                          "The Vulkan ABI probe cannot receive portable material bindings"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const StockKsPerPixelVulkanAbiStatus abi_status =
            validate_stock_ks_per_pixel_vulkan_abi(*request.pipeline);
        if (abi_status != StockKsPerPixelVulkanAbiStatus::ready) {
            diagnostic = {"indexed_stock_vulkan_abi_probe_pipeline_" +
                              std::string(stock_ks_per_pixel_vulkan_abi_status_name(abi_status)),
                          "Vulkan ABI probe pipeline does not match the recovered manifest"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const IndexedStaticMeshDrawStatus probe_status =
            validate_stock_ks_per_pixel_vulkan_abi_probe_binding(texture, request, diagnostic);
        if (probe_status != IndexedStaticMeshDrawStatus::ready)
            return probe_status;
    } else if (stock_vulkan_source) {
        if (request.stock_ks_per_pixel_native != nullptr ||
            request.stock_ks_per_pixel_vulkan_abi_probe != nullptr) {
            diagnostic = {
                "indexed_stock_vulkan_source_binding_overlap",
                "The Vulkan source-equivalent authority cannot receive installed-native or probe bindings"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        // Source preparation retains packet texture identity as declarative
        // metadata. The actual GPU resources still come exclusively from the
        // recovered native ABI binding below.
        if (has_portable_stock_resource_binding(request, false)) {
            diagnostic = {
                "indexed_stock_vulkan_source_portable_binding_overlap",
                "The Vulkan source-equivalent authority cannot receive portable material bindings"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const IndexedStaticMeshDrawStatus source_status =
            validate_stock_ks_per_pixel_vulkan_source_binding(
                texture, request, diagnostic);
        if (source_status != IndexedStaticMeshDrawStatus::ready)
            return source_status;
    } else if (request.stock_ks_per_pixel_native != nullptr ||
               request.stock_ks_per_pixel_vulkan_abi_probe != nullptr ||
               request.stock_ks_per_pixel_vulkan_source != nullptr) {
        diagnostic = request.stock_ks_per_pixel_native != nullptr
                         ? Diagnostic{
                               "indexed_stock_native_binding_unexpected",
                               "A native ksPerPixel binding requires native shader authority"}
                     : request.stock_ks_per_pixel_vulkan_source != nullptr
                         ? Diagnostic{
                               "indexed_stock_vulkan_source_binding_unexpected",
                               "A Vulkan source-equivalent binding requires its matching shader authority"}
                         : Diagnostic{
                               "indexed_stock_vulkan_abi_probe_binding_unexpected",
                               "The Vulkan ABI probe binding requires its matching shader authority"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }

    PipelineProgram native_pipeline;
    const PipelineProgram* effective_pipeline = request.pipeline;
    if (stock_native) {
        native_pipeline = *request.pipeline;
        const auto& source =
            request.stock_ks_per_pixel_native->resources->shader_program().source();
        native_pipeline.shaders = {
            {PipelineShaderStage::vertex, PipelineShaderFormat::dxbc,
             {source.vertex_shader().begin(), source.vertex_shader().end()}},
            {PipelineShaderStage::fragment, PipelineShaderFormat::dxbc,
             {source.pixel_shader().begin(), source.pixel_shader().end()}},
        };
        native_pipeline.transform_contract = PipelineTransformContract::none;
        native_pipeline.resources.clear();
        effective_pipeline = &native_pipeline;
    }
    const PipelineProgram& pipeline = *effective_pipeline;
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
    if (stock_native || stock_vulkan_source || stock_vulkan_abi_probe) {
        const bool invalid_pipeline_contract =
            (stock_native && !request.pipeline->resources.empty()) ||
            request.pipeline->transform_contract != PipelineTransformContract::none;
        if (invalid_pipeline_contract) {
            diagnostic = {stock_vulkan_source
                              ? "indexed_stock_vulkan_source_pipeline_contract_invalid"
                          : stock_vulkan_abi_probe
                              ? "indexed_stock_vulkan_abi_probe_pipeline_contract_invalid"
                              : "indexed_stock_native_pipeline_contract_invalid",
                          stock_vulkan_source
                              ? "The Vulkan source-equivalent path requires the recovered descriptor manifest and no transform constants"
                          : stock_vulkan_abi_probe
                              ? "The Vulkan ABI probe requires the recovered descriptor manifest and no transform constants"
                              : "Native ksPerPixel state must not declare portable resources or transform constants"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        const PipelineVertexLayout& layout = request.pipeline->vertex_layout;
        const auto matches = [&](std::size_t index, PipelineVertexSemantic semantic,
                                 PipelineVertexAttributeFormat format,
                                 std::uint32_t location, std::uint32_t offset) {
            if (index >= layout.attributes.size()) return false;
            const auto& attribute = layout.attributes[index];
            return attribute.semantic == semantic && attribute.format == format &&
                   attribute.location == location && attribute.offset == offset;
        };
        if (layout.stride != stock_ks_per_pixel_vertex_stride_bytes ||
            layout.attributes.size() != 4U ||
            !matches(0U, PipelineVertexSemantic::position,
                     PipelineVertexAttributeFormat::float32x3, 0U, 0U) ||
            !matches(1U, PipelineVertexSemantic::normal,
                     PipelineVertexAttributeFormat::float32x3, 1U, 12U) ||
            !matches(2U, PipelineVertexSemantic::texcoord0,
                     PipelineVertexAttributeFormat::float32x2, 2U, 24U) ||
            !matches(3U, PipelineVertexSemantic::tangent,
                     PipelineVertexAttributeFormat::float32x3, 3U, 32U)) {
            diagnostic = {stock_vulkan_source
                              ? "indexed_stock_vulkan_source_vertex_layout_invalid"
                          : stock_vulkan_abi_probe
                              ? "indexed_stock_vulkan_abi_probe_vertex_layout_invalid"
                              : "indexed_stock_native_vertex_layout_invalid",
                          "Native ksPerPixel drawing requires the exact 44-byte POSITION, NORMAL, TEXCOORD0, TANGENT layout"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (stock_native) {
            const bool expected_a2c =
                request.stock_ks_per_pixel_native->resources->shader_program().source().variant() ==
                StockKsPerPixelVariant::alpha_to_coverage;
            if (packet.flags.blend_enabled || pipeline.blend.enabled ||
                packet.flags.alpha_to_coverage != expected_a2c ||
                pipeline.blend.alpha_to_coverage != expected_a2c ||
                (expected_a2c && target.samples != 4U)) {
                diagnostic = {"indexed_stock_native_variant_state_mismatch",
                              "Native ksPerPixel base and alpha-to-coverage packages require their exact blend and sample state"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
        }
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
    const bool has_reflection_cube =
        request.multimap_reflection_binding.cube.texture != nullptr;
    const bool has_reflection_sampler =
        request.multimap_reflection_binding.cube.sampler != nullptr;
    const bool has_reflection_constants =
        request.multimap_reflection_binding.constants.buffer != nullptr;
    const bool has_reflection_range =
        request.multimap_reflection_binding.constants.offset_bytes != 0U ||
        request.multimap_reflection_binding.constants.range_bytes != 0U;
    const bool shadow_declaration =
        pipeline_declares_directional_shadow_receiver(pipeline);
    const bool reflection_declaration =
        pipeline_declares_multimap_reflection(pipeline);
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
    if (!reflection_declaration &&
        (has_reflection_cube || has_reflection_sampler ||
         has_reflection_constants || has_reflection_range)) {
        diagnostic = {
            "indexed_multimap_reflection_binding_unexpected",
            "A pipeline without the portable MultiMap reflection extension cannot receive cubemap resources"};
        return IndexedStaticMeshDrawStatus::invalid_request;
    }
    // The probe manifest was validated above. Use the resource-free control
    // path only to prove that no request-local portable bindings overlap it.
    const IndexedPortableResourceLayout resource_layout =
        (stock_vulkan_source || stock_vulkan_abi_probe)
            ? IndexedPortableResourceLayout::resource_free
            : classify_indexed_portable_resource_layout(pipeline);
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
            has_reflection_cube || has_reflection_sampler ||
            has_reflection_constants || has_reflection_range ||
            request.resource_authority != IndexedResourceAuthority::packet_contract) {
            diagnostic = {"indexed_resource_binding_unexpected",
                          "A resource-free pipeline cannot receive explicit material bindings"};
            return IndexedStaticMeshDrawStatus::invalid_request;
        }
        if (!packet.resources.empty() && !stock_native &&
            !stock_vulkan_source) {
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
        if (reflection_declaration && !maps_declaration) {
            diagnostic = {
                "indexed_multimap_reflection_layout_unsupported",
                "The portable MultiMap reflection extension requires the txMaps material layout"};
            return IndexedStaticMeshDrawStatus::unsupported;
        }
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
        if (reflection_declaration != has_reflection_cube ||
            reflection_declaration != has_reflection_sampler ||
            reflection_declaration != has_reflection_constants ||
            (!reflection_declaration && has_reflection_range)) {
            diagnostic = {
                reflection_declaration
                    ? "indexed_multimap_reflection_binding_missing"
                    : "indexed_multimap_reflection_binding_unexpected",
                reflection_declaration
                    ? "The portable MultiMap reflection extension requires one cubemap, sampler, and constants buffer"
                    : "A pipeline without the portable MultiMap reflection extension cannot receive cubemap resources"};
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
        if (reflection_declaration) {
            const Texture& cube =
                *request.multimap_reflection_binding.cube.texture;
            const Sampler& cube_sampler =
                *request.multimap_reflection_binding.cube.sampler;
            const Buffer& reflection_constants =
                *request.multimap_reflection_binding.constants.buffer;
            if (&cube == &texture) {
                diagnostic = {
                    "indexed_multimap_reflection_feedback_loop",
                    "The color target cannot also be the reflection cubemap"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (cube.backend() != texture.backend() ||
                cube_sampler.backend() != texture.backend() ||
                reflection_constants.backend() != texture.backend()) {
                diagnostic = {
                    "indexed_multimap_reflection_backend_mismatch",
                    "Reflection cubemap resources and the color target must use the same backend"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            const TextureDescription& cube_description = cube.info().description;
            const auto cube_usage =
                static_cast<std::uint32_t>(cube_description.usage);
            if ((cube_usage &
                 static_cast<std::uint32_t>(TextureUsage::sampled)) == 0U) {
                diagnostic = {
                    "indexed_multimap_reflection_texture_usage_invalid",
                    "The reflection cubemap requires sampled usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if ((cube_usage &
                 static_cast<std::uint32_t>(TextureUsage::storage)) != 0U) {
                diagnostic = {
                    "indexed_multimap_reflection_texture_usage_unsupported",
                    "The portable reflection path rejects storage cubemaps"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            if (cube_description.shape != TextureShape::texture_cube ||
                cube_description.width == 0U ||
                cube_description.width != cube_description.height ||
                cube_description.mip_levels == 0U ||
                cube_description.array_layers != 1U ||
                cube_description.samples != 1U ||
                !portable_sampled_color_format(cube_description.format, true) ||
                (texture_format_is_compressed(cube_description.format) &&
                 cube_description.mutability != TextureMutability::immutable)) {
                diagnostic = {
                    "indexed_multimap_reflection_texture_description_unsupported",
                    "The portable reflection path requires one explicit single-sample square RGBA8, BGRA8, BC1, BC3, or BC7 cube"};
                return IndexedStaticMeshDrawStatus::unsupported;
            }
            Diagnostic cube_sampler_diagnostic;
            const SamplerStatus cube_sampler_status = validate_sampler_description(
                cube_sampler.info().description, cube_sampler_diagnostic);
            if (cube_sampler_status != SamplerStatus::ready) {
                diagnostic = {
                    cube_sampler_diagnostic.code.empty()
                        ? "indexed_multimap_reflection_sampler_invalid"
                        : "indexed_multimap_reflection_" +
                              cube_sampler_diagnostic.code,
                    cube_sampler_diagnostic.message};
                return cube_sampler_status == SamplerStatus::unsupported
                           ? IndexedStaticMeshDrawStatus::unsupported
                           : IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (cube_sampler.info().description.compare !=
                SamplerCompare::disabled) {
                diagnostic = {
                    "indexed_multimap_reflection_sampler_contract_invalid",
                    "Portable reflection cubemap sampling requires comparison disabled"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            const BufferDescription& constants_description =
                reflection_constants.info().description;
            if (constants_description.usage != BufferUsage::uniform) {
                diagnostic = {
                    "indexed_multimap_reflection_constants_usage_invalid",
                    "Reflection constants require exclusive uniform-buffer usage"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            const IndexedMaterialBufferBinding& constants =
                request.multimap_reflection_binding.constants;
            if (constants.offset_bytes %
                        portable_multimap_reflection_buffer_view_bytes !=
                    0U ||
                constants.range_bytes !=
                    portable_multimap_reflection_buffer_view_bytes) {
                diagnostic = {
                    "indexed_multimap_reflection_constants_alignment_invalid",
                    "Reflection constants require a 256-byte aligned offset and 256-byte range"};
                return IndexedStaticMeshDrawStatus::invalid_request;
            }
            if (constants.offset_bytes > constants_description.size_bytes ||
                static_cast<std::uint64_t>(constants.range_bytes) >
                    constants_description.size_bytes - constants.offset_bytes) {
                diagnostic = {
                    "indexed_multimap_reflection_constants_range_invalid",
                    "Reflection constants exceed the declared buffer size"};
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
            sampled.shape != TextureShape::texture_2d ||
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
                normal.shape != TextureShape::texture_2d ||
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
                maps.shape != TextureShape::texture_2d ||
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
                detail.shape != TextureShape::texture_2d ||
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
                normal_detail.shape != TextureShape::texture_2d ||
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
                    source.shape != TextureShape::texture_2d ||
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

IndexedStaticMeshDrawStatus validate_indexed_static_mesh_draw_request(
    const Texture& texture, const IndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic) {
    return validate_indexed_static_mesh_draw_request_internal(
        texture, request, diagnostic, false, false);
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
    if (pipeline.depth.test_enabled != pipeline.depth.write_enabled ||
        (pipeline.depth.test_enabled &&
         pipeline.depth.compare != PipelineCompareOperation::less_or_equal)) {
        diagnostic = {"overlay_line_depth_state_invalid",
                      "Overlay lines require depth test/write both disabled, or normal less-or-equal depth test/write"};
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
        validate_indexed_color_target(texture.info().description, diagnostic, true,
                                      !description.capture_rgba8);
    if (target_status != IndexedStaticMeshDrawStatus::ready)
        return target_status == IndexedStaticMeshDrawStatus::unsupported
                   ? IndexedStaticMeshBatchStatus::unsupported
                   : IndexedStaticMeshBatchStatus::invalid_request;
    const TextureDescription& target = texture.info().description;
    const IndexedStaticMeshBatchStatus target_subresource_status =
        validate_indexed_target_subresource(target, description.target_subresource, diagnostic);
    if (target_subresource_status != IndexedStaticMeshBatchStatus::ready)
        return target_subresource_status;
    if (target.shape == TextureShape::texture_cube && description.load_color) {
        diagnostic = {"indexed_static_mesh_batch_cube_load_unsupported",
                      "Cube indexed static-mesh targets cannot load a prior color slice"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    if (target.shape == TextureShape::texture_cube && description.resolve_target != nullptr) {
        diagnostic = {"indexed_static_mesh_batch_cube_resolve_unsupported",
                      "Cube indexed static-mesh targets cannot use a resolve target"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    const bool has_vulkan_abi_probe = std::any_of(
        description.draws.begin(), description.draws.end(),
        [](const IndexedStaticMeshDrawRequest& request) {
            return request.shader_authority ==
                   IndexedShaderAuthority::explicit_stock_ks_per_pixel_vulkan_abi_probe;
        });
    if (has_vulkan_abi_probe) {
        diagnostic = {
            "indexed_stock_vulkan_abi_probe_batch_unsupported",
            "The Vulkan ksPerPixel ABI probe is limited to single-draw validation"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    const std::size_t native_draw_count = static_cast<std::size_t>(std::count_if(
        description.draws.begin(), description.draws.end(),
        [](const IndexedStaticMeshDrawRequest& request) {
            return request.shader_authority ==
                   IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
        }));
    if (native_draw_count != 0U &&
        native_draw_count >
            max_stock_ks_per_pixel_native_batch_draws) {
        diagnostic = {
            "indexed_stock_native_batch_draw_limit",
            "The native ksPerPixel batch exceeds its D3D12 sampler-heap draw limit"};
        return IndexedStaticMeshBatchStatus::unsupported;
    }
    bool native_has_alpha_to_coverage = false;
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
            target.format == TextureFormat::bgra8_srgb ||
            target.format == TextureFormat::rgba16_sfloat;
        if (target.width == 0U || target.height == 0U ||
            target.mip_levels != 1U || target.array_layers != 1U ||
            target.shape != TextureShape::texture_2d ||
            (target_usage & required_target_usage) != required_target_usage) {
            diagnostic = {"indexed_static_mesh_batch_resolve_source_invalid",
                          "Batch resolve source must be a bounded color attachment and transfer source"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (!supported_format) {
            diagnostic = {"indexed_static_mesh_batch_resolve_format_unsupported",
                          "Batch resolve supports RGBA8, BGRA8, and RGBA16F color formats"};
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
        const bool allow_hdr_resolve_mips =
            !description.capture_rgba8 &&
            target.format == TextureFormat::rgba16_sfloat &&
            resolved.format == TextureFormat::rgba16_sfloat;
        if (resolved.width != target.width || resolved.height != target.height ||
            resolved.format != target.format ||
            (allow_hdr_resolve_mips ? !valid_texture_mip_count(resolved)
                                    : resolved.mip_levels != 1U) ||
            resolved.array_layers != 1U || resolved.samples != 1U ||
            resolved.shape != TextureShape::texture_2d) {
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
            validate_indexed_static_mesh_draw_request_internal(
                texture, effective, draw_diagnostic, true,
                !description.capture_rgba8);
        if (draw_status != IndexedStaticMeshDrawStatus::ready) {
            diagnostic = std::move(draw_diagnostic);
            return draw_status == IndexedStaticMeshDrawStatus::unsupported
                       ? IndexedStaticMeshBatchStatus::unsupported
                       : IndexedStaticMeshBatchStatus::invalid_request;
        }
        if (source.shader_authority ==
            IndexedShaderAuthority::explicit_stock_ks_per_pixel_native) {
            const StockKsPerPixelVariant variant =
                source.stock_ks_per_pixel_native->resources->shader_program()
                    .source()
                    .variant();
            const bool alpha_to_coverage =
                variant == StockKsPerPixelVariant::alpha_to_coverage;
            native_has_alpha_to_coverage =
                native_has_alpha_to_coverage || alpha_to_coverage;
            if ((variant != StockKsPerPixelVariant::base &&
                 !alpha_to_coverage) ||
             source.packet->flags.blend_enabled ||
             source.packet->flags.alpha_to_coverage != alpha_to_coverage ||
             source.packet->flags.wireframe || source.packet->flags.selected ||
             source.packet->shadow_only || source.pipeline->blend.enabled ||
             source.pipeline->blend.alpha_to_coverage != alpha_to_coverage ||
             source.pipeline->raster.fill == PipelineFillMode::wireframe) {
                diagnostic = {
                    "indexed_stock_native_batch_feature_unsupported",
                    "The native ksPerPixel batch supports opaque solid base and alpha-to-coverage draws"};
                return IndexedStaticMeshBatchStatus::unsupported;
            }
        }
    }
    if (native_draw_count != 0U) {
        const std::uint32_t target_samples =
            texture.info().description.samples;
        if ((native_has_alpha_to_coverage && target_samples != 4U) ||
            (!native_has_alpha_to_coverage && target_samples != 1U &&
             target_samples != 4U)) {
            diagnostic = {
                "indexed_stock_native_batch_target_samples_invalid",
                native_has_alpha_to_coverage
                    ? "Native ksPerPixelAT batches require a four-sample target"
                    : "Native base ksPerPixel batches require a one-sample or four-sample target"};
            return IndexedStaticMeshBatchStatus::unsupported;
        }
        if (target_samples == 4U && description.resolve_target == nullptr) {
            diagnostic = {
                "indexed_stock_native_batch_resolve_target_missing",
                "Four-sample native ksPerPixel batches require a retained single-sample resolve target"};
            return IndexedStaticMeshBatchStatus::invalid_request;
        }
    }
    diagnostic = {};
    return IndexedStaticMeshBatchStatus::ready;
}

bool validate_depth_only_indexed_pipeline_contract(
    const PipelineProgram& pipeline, DepthOnlyIndexedPipelineRole role,
    Diagnostic& diagnostic) {
    const PipelineValidationResult pipeline_validation =
        validate_pipeline(pipeline);
    if (!pipeline_validation.valid) {
        if (!pipeline_validation.diagnostics.empty()) {
            const auto& failure = pipeline_validation.diagnostics.front();
            diagnostic = {"depth_only_indexed_pipeline_" + failure.code,
                          failure.message};
        } else {
            diagnostic = {"depth_only_indexed_pipeline_invalid",
                          "Depth-only indexed pipeline validation failed without a diagnostic"};
        }
        return false;
    }
    if (!pipeline.targets.colors.empty() || !pipeline.targets.has_depth ||
        pipeline.targets.depth.format !=
            PipelineRenderTargetFormat::depth32_float ||
        pipeline.targets.depth.samples != 1U) {
        diagnostic = {"depth_only_indexed_pipeline_target_invalid",
                      "Depth-only indexed pipelines require exactly one single-sample D32 depth target and no color targets"};
        return false;
    }

    const bool alpha_tested =
        role == DepthOnlyIndexedPipelineRole::stock_alpha_tested_static;
    const bool skinned = role == DepthOnlyIndexedPipelineRole::skinned;
    const bool valid_alpha_resources =
        pipeline.resources.size() == 3U &&
        std::all_of(pipeline.resources.begin(), pipeline.resources.end(),
                    [](const PipelineResourceBinding& resource) {
                        if (resource.set != 0U) return false;
                        if (resource.binding == 0U)
                            return resource.kind ==
                                   PipelineResourceKind::sampled_texture;
                        if (resource.binding == 3U)
                            return resource.kind ==
                                   PipelineResourceKind::sampler;
                        if (resource.binding == 4U)
                            return resource.kind ==
                                   PipelineResourceKind::uniform_buffer;
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
    if ((!alpha_tested && !pipeline.resources.empty()) ||
        (alpha_tested && !valid_alpha_resources) || pipeline.blend.enabled ||
        pipeline.blend.alpha_to_coverage ||
        pipeline.raster.fill != PipelineFillMode::solid ||
        pipeline.transform_contract !=
            PipelineTransformContract::draw_matrices ||
        !pipeline.depth.test_enabled || !pipeline.depth.write_enabled ||
        pipeline.depth.compare != PipelineCompareOperation::less) {
        diagnostic = {"depth_only_indexed_pipeline_state_invalid",
                      alpha_tested
                          ? "Stock alpha-tested depth-only pipelines require exact t0/s3/b4 resources, solid fill, draw matrices, depth LESS test/write, and no blend"
                          : "Depth-only indexed pipelines require solid fill, draw matrices, depth LESS test/write, no blend, and no resources"};
        return false;
    }

    const auto vertex_shader_count = std::count_if(
        pipeline.shaders.begin(), pipeline.shaders.end(),
        [](const PipelineShaderModule& shader) {
            return shader.stage == PipelineShaderStage::vertex;
        });
    const auto fragment_shader_count = std::count_if(
        pipeline.shaders.begin(), pipeline.shaders.end(),
        [](const PipelineShaderModule& shader) {
            return shader.stage == PipelineShaderStage::fragment;
        });
    if ((!alpha_tested &&
         (pipeline.shaders.size() != 1U || vertex_shader_count != 1U)) ||
        (alpha_tested &&
         (pipeline.shaders.size() != 2U || vertex_shader_count != 1U ||
          fragment_shader_count != 1U))) {
        diagnostic = {"depth_only_indexed_shader_pair_invalid",
                      alpha_tested
                          ? "Stock alpha-tested depth-only pipelines require exactly one vertex and one fragment shader"
                          : "Depth-only indexed pipelines require exactly one vertex shader and no fragment shader"};
        return false;
    }

    const std::uint32_t expected_stride =
        (skinned ? 19U : 11U) * sizeof(float);
    const bool has_position = std::any_of(
        pipeline.vertex_layout.attributes.begin(),
        pipeline.vertex_layout.attributes.end(),
        [](const PipelineVertexAttribute& attribute) {
            return attribute.semantic == PipelineVertexSemantic::position &&
                   attribute.format ==
                       PipelineVertexAttributeFormat::float32x3 &&
                   attribute.location == 0U && attribute.offset == 0U;
        });
    if (pipeline.vertex_layout.stride != expected_stride || !has_position) {
        diagnostic = {
            "depth_only_indexed_pipeline_vertex_layout_invalid",
            skinned
                ? "Skinned depth-only pipelines require a 19-float stream with position at location zero"
                : "Static depth-only pipelines require an 11-float stream with position at location zero"};
        return false;
    }
    return true;
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
    const DepthOnlyIndexedPipelineRole pipeline_role =
        alpha_tested
            ? DepthOnlyIndexedPipelineRole::stock_alpha_tested_static
            : skinned ? DepthOnlyIndexedPipelineRole::skinned
                      : DepthOnlyIndexedPipelineRole::opaque_static;
    if (!validate_depth_only_indexed_pipeline_contract(
            pipeline, pipeline_role, diagnostic))
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    if (packet.flags.depth_test != pipeline.depth.test_enabled ||
        packet.flags.depth_write != pipeline.depth.write_enabled) {
        diagnostic = {"depth_only_indexed_packet_depth_state_mismatch",
                      "Depth-only packet depth flags must match the executable pipeline"};
        return DepthOnlyIndexedStaticMeshDrawStatus::invalid_request;
    }
    for (const auto& shader : pipeline.shaders) {
        const bool format_matches =
            depth.backend() == Backend::Vulkan
                ? shader.format == PipelineShaderFormat::spirv
                : shader.format == PipelineShaderFormat::dxbc ||
                      shader.format == PipelineShaderFormat::dxil;
        if (!format_matches) {
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
            texture.shape != TextureShape::texture_2d ||
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
    if (!std::isfinite(description.mip_lod_bias) ||
        description.mip_lod_bias < -16.0F ||
        description.mip_lod_bias > 16.0F) {
        diagnostic = {"sampler_lod_bias_invalid", "Sampler LOD bias must be finite and between -16 and 16"};
        return SamplerStatus::invalid_description;
    }
    if (!std::isfinite(description.min_lod) || !std::isfinite(description.max_lod) || description.min_lod < 0.0F ||
        description.max_lod < description.min_lod) {
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

enum class ContainerValidation : std::uint8_t {
    valid,
    invalid,
    unsupported,
};

ContainerValidation validate_dxbc_container(std::span<const std::byte> bytes,
                                             Diagnostic& diagnostic,
                                             ShaderBytecodeFormat* detected_format = nullptr) {
    detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    const detail::DxbcReaderStatus status = detail::inspect_dxbc_container(
        bytes, max_dxbc_chunks, inspection, error_offset);
    switch (status) {
    case detail::DxbcReaderStatus::ready: break;
    case detail::DxbcReaderStatus::truncated_header:
    case detail::DxbcReaderStatus::invalid_signature:
    case detail::DxbcReaderStatus::invalid_header_version:
        diagnostic = {"shader_dxbc_header_invalid", "DXBC container header fields are invalid"};
        return ContainerValidation::invalid;
    case detail::DxbcReaderStatus::declared_size_mismatch:
        diagnostic = {"shader_dxbc_size_invalid", "DXBC container size does not match the bytecode span"};
        return ContainerValidation::invalid;
    case detail::DxbcReaderStatus::invalid_chunk_count:
    case detail::DxbcReaderStatus::chunk_table_out_of_bounds:
        diagnostic = {"shader_dxbc_chunk_count_invalid", "DXBC chunk count exceeds the bounded container table"};
        return ContainerValidation::invalid;
    case detail::DxbcReaderStatus::chunk_header_out_of_bounds:
        diagnostic = {"shader_dxbc_offset_invalid", "DXBC chunk offset is outside the container"};
        return ContainerValidation::invalid;
    case detail::DxbcReaderStatus::chunk_payload_out_of_bounds:
    case detail::DxbcReaderStatus::program_chunk_truncated:
        diagnostic = {"shader_dxbc_chunk_invalid", "DXBC chunk size exceeds the container"};
        return ContainerValidation::invalid;
    case detail::DxbcReaderStatus::chunks_overlap:
        diagnostic = {"shader_dxbc_chunk_overlap", "DXBC chunks overlap"};
        return ContainerValidation::invalid;
    }
    if (inspection.program_format == detail::DxbcProgramFormat::none) {
        diagnostic = {"shader_dxbc_chunk_unsupported", "DXBC contains no supported shader chunk"};
        return ContainerValidation::unsupported;
    }
    if (inspection.program_format == detail::DxbcProgramFormat::mixed ||
        inspection.program_chunk_count != 1U) {
        diagnostic = {"shader_dxbc_program_ambiguous", "DXBC mixes legacy DXBC and DXIL program chunks"};
        return ContainerValidation::invalid;
    }
    if (detected_format != nullptr)
        *detected_format =
            inspection.program_format == detail::DxbcProgramFormat::legacy
                ? ShaderBytecodeFormat::dxbc
                : ShaderBytecodeFormat::dxil;
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
        Diagnostic diagnostic;
        return validate_dxbc_container(bytecode, diagnostic, &format) ==
               ContainerValidation::valid;
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
    const std::uint32_t signature = shader_word(description.bytecode);
    if (signature == 0x07230203U) {
        format = ShaderBytecodeFormat::spirv;
    } else if (signature == 0x43425844U) {
        const ContainerValidation container = validate_dxbc_container(
            description.bytecode, diagnostic, &format);
        if (container == ContainerValidation::invalid)
            return ShaderModuleStatus::invalid_description;
        if (container == ContainerValidation::unsupported)
            return ShaderModuleStatus::unsupported;
    } else {
        diagnostic = {"shader_bytecode_signature", "Shader module bytecode has no supported SPIR-V or Direct3D signature"};
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
    }
    return ShaderModuleStatus::ready;
}

StockKsPerPixelNativeShaderResult
allocate_stock_ks_per_pixel_native_shaders(
    Device& device,
    ValidatedStockKsPerPixelNativeProgram&& program) {
    if (program.validation_status() !=
        StockKsPerPixelNativeProgramStatus::ready) {
        return {StockKsPerPixelNativeShaderStatus::invalid_program,
                {"stock_native_shader_program_invalid",
                 "The owned native shader program is not in its validated state."},
                nullptr};
    }
    if (device.info().backend != Backend::D3D12) {
        return {StockKsPerPixelNativeShaderStatus::backend_unsupported,
                {"stock_native_shader_backend_unsupported",
                 "Vulkan cannot allocate the installed DXBC program. A source-equivalent SPIR-V program is required."},
                nullptr};
    }

    const auto description = [](ShaderStage stage,
                                std::span<const std::uint8_t> bytes) {
        return ShaderModuleDescription{
            stage,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(bytes.data()),
                bytes.size())};
    };
    const auto stage_failure = [](StockKsPerPixelNativeShaderStatus status,
                                  const char* fallback_code,
                                  const char* fallback_message,
                                  ShaderModuleResult& module) {
        Diagnostic diagnostic = std::move(module.diagnostic);
        if (diagnostic.code.empty()) diagnostic.code = fallback_code;
        if (diagnostic.message.empty()) diagnostic.message = fallback_message;
        return StockKsPerPixelNativeShaderResult{
            status, std::move(diagnostic), nullptr};
    };
    const auto valid_module = [](const ShaderModuleResult& result,
                                 ShaderStage stage,
                                 std::size_t size_bytes) {
        if (!result.ok() || result.shader_module->backend() != Backend::D3D12)
            return false;
        const ShaderModuleInfo& info = result.shader_module->info();
        return info.stage == stage && info.format == ShaderBytecodeFormat::dxbc &&
               info.size_bytes == size_bytes;
    };

    ShaderModuleResult vertex = device.create_shader_module(
        description(ShaderStage::vertex, program.vertex_shader()));
    if (vertex.status != ShaderModuleStatus::ready)
        return stage_failure(
            StockKsPerPixelNativeShaderStatus::vertex_shader_failed,
            "stock_native_vertex_shader_failed",
            "D3D12 did not allocate the validated native vertex shader.",
            vertex);
    if (!valid_module(vertex, ShaderStage::vertex,
                      program.vertex_shader().size())) {
        return {StockKsPerPixelNativeShaderStatus::invalid_shader_module,
                {"stock_native_vertex_shader_invalid",
                 "D3D12 returned an invalid native vertex shader object."},
                nullptr};
    }

    ShaderModuleResult pixel = device.create_shader_module(
        description(ShaderStage::fragment, program.pixel_shader()));
    if (pixel.status != ShaderModuleStatus::ready)
        return stage_failure(
            StockKsPerPixelNativeShaderStatus::pixel_shader_failed,
            "stock_native_pixel_shader_failed",
            "D3D12 did not allocate the validated native pixel shader.",
            pixel);
    if (!valid_module(pixel, ShaderStage::fragment,
                      program.pixel_shader().size())) {
        return {StockKsPerPixelNativeShaderStatus::invalid_shader_module,
                {"stock_native_pixel_shader_invalid",
                 "D3D12 returned an invalid native pixel shader object."},
                nullptr};
    }

    try {
        auto owned = std::unique_ptr<StockKsPerPixelNativeShaderProgram>(
            new StockKsPerPixelNativeShaderProgram(
                device, std::move(program), std::move(vertex.shader_module),
                std::move(pixel.shader_module)));
        return {StockKsPerPixelNativeShaderStatus::ready, {},
                std::move(owned)};
    } catch (const std::bad_alloc&) {
        return {StockKsPerPixelNativeShaderStatus::allocation_failed,
                {"stock_native_shader_program_allocation_failed",
                 "The native shader program owner allocation failed."},
                nullptr};
    }
}

StockKsPerPixelNativeConstantBufferResult
allocate_stock_ks_per_pixel_native_constant_buffers(
    Device& device,
    const StockKsPerPixelNativeConstantData& constants) {
    if (!valid_stock_ks_per_pixel_camera_constants(constants.camera) ||
        !valid_stock_ks_per_pixel_object_constants(constants.object) ||
        !valid_stock_ks_per_pixel_lighting_constants(constants.lighting) ||
        !valid_stock_directional_shadow_receiver_constants(
            constants.shadow_maps) ||
        !valid_stock_ks_per_pixel_material_constants(constants.material)) {
        return {StockKsPerPixelNativeConstantBufferStatus::invalid_constants,
                {"stock_native_constants_invalid",
                 "The native constant records contain invalid values."},
                nullptr};
    }
    constexpr std::size_t slot_count = static_cast<std::size_t>(
        StockKsPerPixelNativeConstantSlot::count);
    std::array<std::array<std::byte,
                          stock_ks_per_pixel_native_constant_buffer_view_bytes>,
               slot_count>
        records{};
    const auto copy_record = [&]<typename Record>(
                                 StockKsPerPixelNativeConstantSlot slot,
                                 const Record& record) {
        static_assert(sizeof(Record) <=
                      stock_ks_per_pixel_native_constant_buffer_view_bytes);
        std::memcpy(records[static_cast<std::size_t>(slot)].data(), &record,
                    sizeof(Record));
    };
    copy_record(StockKsPerPixelNativeConstantSlot::camera, constants.camera);
    copy_record(StockKsPerPixelNativeConstantSlot::object, constants.object);
    copy_record(StockKsPerPixelNativeConstantSlot::lighting,
                constants.lighting);
    copy_record(StockKsPerPixelNativeConstantSlot::shadow_maps,
                constants.shadow_maps);
    copy_record(StockKsPerPixelNativeConstantSlot::material,
                constants.material);

    const BufferDescription description{
        stock_ks_per_pixel_native_constant_buffer_view_bytes,
        BufferUsage::uniform, BufferMemory::host_visible,
        BufferMutability::mutable_data};
    constexpr std::array<const char*, slot_count> slot_names = {
        "cbCamera", "cbPerObject", "cbLighting", "cbShadowMaps",
        "cbMaterial"};
    std::array<std::unique_ptr<Buffer>, slot_count> buffers;
    for (std::size_t index = 0U; index < buffers.size(); ++index) {
        BufferResult created = device.create_buffer(description, records[index]);
        if (created.status != BufferStatus::ready) {
            Diagnostic diagnostic = std::move(created.diagnostic);
            if (diagnostic.code.empty())
                diagnostic.code = "stock_native_constant_buffer_failed";
            if (diagnostic.message.empty())
                diagnostic.message =
                    std::string("The render backend did not allocate ") +
                        slot_names[index] +
                    ".";
            return {StockKsPerPixelNativeConstantBufferStatus::buffer_failed,
                    std::move(diagnostic), nullptr};
        }
        if (!created.ok() ||
            created.buffer->backend() != device.info().backend ||
            created.buffer->info().description.size_bytes !=
                stock_ks_per_pixel_native_constant_buffer_view_bytes ||
            created.buffer->info().description.usage != BufferUsage::uniform ||
            created.buffer->info().description.memory !=
                BufferMemory::host_visible ||
            created.buffer->info().description.mutability !=
                BufferMutability::mutable_data) {
            return {StockKsPerPixelNativeConstantBufferStatus::invalid_buffer,
                    {"stock_native_constant_buffer_invalid",
                     std::string("The render backend returned an invalid ") +
                         slot_names[index] + " buffer."},
                    nullptr};
        }
        buffers[index] = std::move(created.buffer);
    }

    try {
        auto owned = std::unique_ptr<StockKsPerPixelNativeConstantBuffers>(
            new StockKsPerPixelNativeConstantBuffers(device,
                                                     std::move(buffers)));
        return {StockKsPerPixelNativeConstantBufferStatus::ready, {},
                std::move(owned)};
    } catch (const std::bad_alloc&) {
        return {StockKsPerPixelNativeConstantBufferStatus::allocation_failed,
                {"stock_native_constant_owner_allocation_failed",
                 "The native constant-buffer owner allocation failed."},
                nullptr};
    }
}

StockKsPerPixelNativeSamplerResult
allocate_stock_ks_per_pixel_native_samplers(
    Device& device,
    const StockKsPerPixelNativeSamplerSettings& settings) {
    if (!std::isfinite(settings.max_anisotropy) ||
        settings.max_anisotropy < 2.0F ||
        settings.max_anisotropy > 16.0F ||
        std::trunc(settings.max_anisotropy) != settings.max_anisotropy ||
        !std::isfinite(settings.mip_lod_bias) ||
        settings.mip_lod_bias < -16.0F ||
        settings.mip_lod_bias > 16.0F) {
        return {StockKsPerPixelNativeSamplerStatus::invalid_settings,
                {"stock_native_sampler_settings_invalid",
                 "Native stock sampler settings require integer anisotropy from 2 through 16 and LOD bias from -16 through 16."},
                nullptr};
    }
    if (device.info().backend != Backend::D3D12) {
        return {StockKsPerPixelNativeSamplerStatus::backend_unsupported,
                {"stock_native_sampler_backend_unsupported",
                 "The installed native stock sampler contract requires D3D12."},
                nullptr};
    }

    SamplerDescription linear;
    linear.min_filter = SamplerFilter::anisotropic;
    linear.mag_filter = SamplerFilter::anisotropic;
    linear.mip_filter = SamplerFilter::anisotropic;
    linear.address_u = SamplerAddressMode::repeat;
    linear.address_v = SamplerAddressMode::repeat;
    linear.address_w = SamplerAddressMode::repeat;
    linear.compare = SamplerCompare::disabled;
    linear.mip_lod_bias = settings.mip_lod_bias;
    linear.max_anisotropy = settings.max_anisotropy;
    linear.min_lod = 0.0F;
    linear.max_lod = std::numeric_limits<float>::max();

    SamplerDescription shadow;
    shadow.min_filter = SamplerFilter::linear;
    shadow.mag_filter = SamplerFilter::linear;
    shadow.mip_filter = SamplerFilter::nearest;
    shadow.address_u = SamplerAddressMode::clamp_to_edge;
    shadow.address_v = SamplerAddressMode::clamp_to_edge;
    shadow.address_w = SamplerAddressMode::clamp_to_edge;
    shadow.compare = SamplerCompare::less;
    shadow.max_anisotropy = 1.0F;
    shadow.min_lod = 0.0F;
    shadow.max_lod = std::numeric_limits<float>::max();

    constexpr std::size_t sampler_count = static_cast<std::size_t>(
        StockKsPerPixelNativeSamplerSlot::count);
    std::array<std::unique_ptr<Sampler>, sampler_count> samplers;
    const std::array descriptions = {linear, shadow};
    const std::array failure_statuses = {
        StockKsPerPixelNativeSamplerStatus::linear_sampler_failed,
        StockKsPerPixelNativeSamplerStatus::shadow_sampler_failed,
    };

    for (std::size_t index = 0U; index < sampler_count; ++index) {
        SamplerResult result = device.create_sampler(descriptions[index]);
        if (!result.ok()) {
            Diagnostic diagnostic = std::move(result.diagnostic);
            if (diagnostic.code.empty())
                diagnostic.code = "stock_native_sampler_failed";
            if (diagnostic.message.empty())
                diagnostic.message =
                    "The device did not create a required native stock sampler.";
            return {failure_statuses[index], std::move(diagnostic), nullptr};
        }
        const SamplerDescription& actual = result.sampler->info().description;
        if (result.sampler->backend() != device.info().backend ||
            actual.min_filter != descriptions[index].min_filter ||
            actual.mag_filter != descriptions[index].mag_filter ||
            actual.mip_filter != descriptions[index].mip_filter ||
            actual.address_u != descriptions[index].address_u ||
            actual.address_v != descriptions[index].address_v ||
            actual.address_w != descriptions[index].address_w ||
            actual.compare != descriptions[index].compare ||
            actual.mip_lod_bias != descriptions[index].mip_lod_bias ||
            actual.max_anisotropy != descriptions[index].max_anisotropy ||
            actual.min_lod != descriptions[index].min_lod ||
            actual.max_lod != descriptions[index].max_lod) {
            return {StockKsPerPixelNativeSamplerStatus::invalid_sampler,
                    {"stock_native_sampler_invalid",
                     "The device returned a sampler that does not preserve the recovered native contract."},
                    nullptr};
        }
        samplers[index] = std::move(result.sampler);
    }

    try {
        auto owned = std::unique_ptr<StockKsPerPixelNativeSamplers>(
            new StockKsPerPixelNativeSamplers(device, std::move(samplers)));
        return {StockKsPerPixelNativeSamplerStatus::ready, {},
                std::move(owned)};
    } catch (const std::bad_alloc&) {
        return {StockKsPerPixelNativeSamplerStatus::allocation_failed,
                {"stock_native_sampler_owner_allocation_failed",
                 "The native stock sampler owner allocation failed."},
                nullptr};
    }
}

StockKsPerPixelNativeDrawResourcesResult
make_stock_ks_per_pixel_native_draw_resources(
    Device& device,
    std::unique_ptr<StockKsPerPixelNativeShaderProgram> shader_program,
    std::unique_ptr<StockKsPerPixelNativeConstantBuffers> constant_buffers,
    std::unique_ptr<StockKsPerPixelNativeSamplers> samplers) {
    const auto fail = [](StockKsPerPixelNativeDrawResourcesStatus status,
                         const char* code, const char* message) {
        return StockKsPerPixelNativeDrawResourcesResult{
            status, {code, message}, nullptr};
    };
    if (device.info().backend != Backend::D3D12)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported,
                    "stock_native_draw_resources_backend_unsupported",
                    "Native draw resources require a D3D12 device.");
    if (shader_program == nullptr)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::shader_missing,
                    "stock_native_draw_resources_shader_missing",
                    "Native draw resources require an owned shader program.");
    if (constant_buffers == nullptr)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::constants_missing,
                    "stock_native_draw_resources_constants_missing",
                    "Native draw resources require owned constant buffers.");
    if (samplers == nullptr)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::samplers_missing,
                    "stock_native_draw_resources_samplers_missing",
                    "Native draw resources require owned samplers.");
    if (shader_program->device() != &device ||
        constant_buffers->device() != &device ||
        samplers->device() != &device)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::device_mismatch,
                    "stock_native_draw_resources_device_mismatch",
                    "Native draw resource owners must come from the supplied device.");
    if (!shader_program->ready() ||
        shader_program->source().validation_status() !=
            StockKsPerPixelNativeProgramStatus::ready)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::shader_invalid,
                    "stock_native_draw_resources_shader_invalid",
                    "Native draw resources require an intact validated shader program.");

    const ShaderModule& vertex_shader = shader_program->vertex_shader();
    const ShaderModule& pixel_shader = shader_program->pixel_shader();
    const auto valid_shader_module = [](const ShaderModule& module,
                                        ShaderStage stage,
                                        std::size_t size_bytes) {
        return module.backend() == Backend::D3D12 &&
               module.info().stage == stage &&
               module.info().format == ShaderBytecodeFormat::dxbc &&
               module.info().size_bytes == size_bytes;
    };
    if (!valid_shader_module(vertex_shader, ShaderStage::vertex,
                             shader_program->source().vertex_shader().size()) ||
        !valid_shader_module(pixel_shader, ShaderStage::fragment,
                             shader_program->source().pixel_shader().size()))
        return fail(StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported,
                    "stock_native_draw_resources_shader_backend_mismatch",
                    "Native draw resources require D3D12 DXBC vertex and pixel modules.");
    if (!constant_buffers->ready())
        return fail(StockKsPerPixelNativeDrawResourcesStatus::constants_invalid,
                    "stock_native_draw_resources_constants_invalid",
                    "Native draw resources require all five constant buffers.");
    for (std::size_t index = 0U;
         index < static_cast<std::size_t>(StockKsPerPixelNativeConstantSlot::count);
         ++index) {
        const Buffer* buffer = constant_buffers->buffer(
            static_cast<StockKsPerPixelNativeConstantSlot>(index));
        if (buffer == nullptr || buffer->backend() != Backend::D3D12)
            return fail(StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported,
                        "stock_native_draw_resources_constants_backend_mismatch",
                        "Native draw resources require D3D12 constant buffers.");
        const BufferDescription& description = buffer->info().description;
        if (description.size_bytes != stock_ks_per_pixel_native_constant_buffer_view_bytes ||
            description.usage != BufferUsage::uniform ||
            description.memory != BufferMemory::host_visible ||
            description.mutability != BufferMutability::mutable_data)
            return fail(StockKsPerPixelNativeDrawResourcesStatus::constants_invalid,
                        "stock_native_draw_resources_constants_invalid",
                        "Native draw resources require the exact 256-byte mutable uniform views.");
    }
    if (!samplers->ready())
        return fail(StockKsPerPixelNativeDrawResourcesStatus::samplers_invalid,
                    "stock_native_draw_resources_samplers_invalid",
                    "Native draw resources require both native samplers.");
    const Sampler* linear = samplers->sampler(
        StockKsPerPixelNativeSamplerSlot::linear);
    const Sampler* shadow = samplers->sampler(
        StockKsPerPixelNativeSamplerSlot::shadow);
    if (linear == nullptr || shadow == nullptr ||
        linear->backend() != Backend::D3D12 ||
        shadow->backend() != Backend::D3D12)
        return fail(StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported,
                    "stock_native_draw_resources_samplers_backend_mismatch",
                    "Native draw resources require D3D12 samplers.");
    const SamplerDescription& linear_description = linear->info().description;
    if (linear_description.min_filter != SamplerFilter::anisotropic ||
        linear_description.mag_filter != SamplerFilter::anisotropic ||
        linear_description.mip_filter != SamplerFilter::anisotropic ||
        linear_description.address_u != SamplerAddressMode::repeat ||
        linear_description.address_v != SamplerAddressMode::repeat ||
        linear_description.address_w != SamplerAddressMode::repeat ||
        linear_description.compare != SamplerCompare::disabled ||
        !std::isfinite(linear_description.mip_lod_bias) ||
        linear_description.mip_lod_bias < -16.0F ||
        linear_description.mip_lod_bias > 16.0F ||
        linear_description.max_anisotropy < 2.0F ||
        linear_description.max_anisotropy > 16.0F ||
        linear_description.min_lod != 0.0F ||
        linear_description.max_lod != std::numeric_limits<float>::max())
        return fail(StockKsPerPixelNativeDrawResourcesStatus::samplers_invalid,
                    "stock_native_draw_resources_sampler_contract_invalid",
                    "The native linear sampler does not match the recovered s0 contract.");
    const SamplerDescription& shadow_description = shadow->info().description;
    if (shadow_description.min_filter != SamplerFilter::linear ||
        shadow_description.mag_filter != SamplerFilter::linear ||
        shadow_description.mip_filter != SamplerFilter::nearest ||
        shadow_description.address_u != SamplerAddressMode::clamp_to_edge ||
        shadow_description.address_v != SamplerAddressMode::clamp_to_edge ||
        shadow_description.address_w != SamplerAddressMode::clamp_to_edge ||
        shadow_description.compare != SamplerCompare::less ||
        shadow_description.mip_lod_bias != 0.0F ||
        shadow_description.max_anisotropy != 1.0F ||
        shadow_description.min_lod != 0.0F ||
        shadow_description.max_lod != std::numeric_limits<float>::max())
        return fail(StockKsPerPixelNativeDrawResourcesStatus::samplers_invalid,
                    "stock_native_draw_resources_sampler_contract_invalid",
                    "The native shadow sampler does not match the recovered s1 contract.");
    try {
        auto resources = std::unique_ptr<StockKsPerPixelNativeDrawResources>(
            new StockKsPerPixelNativeDrawResources(
                device, std::move(shader_program), std::move(constant_buffers),
                std::move(samplers)));
        return {StockKsPerPixelNativeDrawResourcesStatus::ready, {},
                std::move(resources)};
    } catch (const std::bad_alloc&) {
        return fail(StockKsPerPixelNativeDrawResourcesStatus::allocation_failed,
                    "stock_native_draw_resources_allocation_failed",
                    "Native draw resource owner allocation failed.");
    }
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

const char* hdr_tone_map_status_name(HdrToneMapStatus status) noexcept {
    switch (status) {
    case HdrToneMapStatus::ready:
        return "ready";
    case HdrToneMapStatus::invalid_request:
        return "invalid_request";
    case HdrToneMapStatus::unsupported:
        return "unsupported";
    case HdrToneMapStatus::execution_failed:
        return "execution_failed";
    }
    return "unknown";
}

const char* hdr_luminance_status_name(HdrLuminanceStatus status) noexcept {
    switch (status) {
    case HdrLuminanceStatus::ready:
        return "ready";
    case HdrLuminanceStatus::invalid_request:
        return "invalid_request";
    case HdrLuminanceStatus::unsupported:
        return "unsupported";
    case HdrLuminanceStatus::execution_failed:
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

const char* stock_ks_per_pixel_native_shader_status_name(
    StockKsPerPixelNativeShaderStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelNativeShaderStatus::ready: return "ready";
    case StockKsPerPixelNativeShaderStatus::invalid_program:
        return "invalid_program";
    case StockKsPerPixelNativeShaderStatus::backend_unsupported:
        return "backend_unsupported";
    case StockKsPerPixelNativeShaderStatus::vertex_shader_failed:
        return "vertex_shader_failed";
    case StockKsPerPixelNativeShaderStatus::pixel_shader_failed:
        return "pixel_shader_failed";
    case StockKsPerPixelNativeShaderStatus::invalid_shader_module:
        return "invalid_shader_module";
    case StockKsPerPixelNativeShaderStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unknown";
}

const char* stock_ks_per_pixel_native_constant_buffer_status_name(
    StockKsPerPixelNativeConstantBufferStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelNativeConstantBufferStatus::ready: return "ready";
    case StockKsPerPixelNativeConstantBufferStatus::invalid_constants:
        return "invalid_constants";
    case StockKsPerPixelNativeConstantBufferStatus::backend_unsupported:
        return "backend_unsupported";
    case StockKsPerPixelNativeConstantBufferStatus::buffer_failed:
        return "buffer_failed";
    case StockKsPerPixelNativeConstantBufferStatus::invalid_buffer:
        return "invalid_buffer";
    case StockKsPerPixelNativeConstantBufferStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unknown";
}

const char* stock_ks_per_pixel_native_sampler_status_name(
    StockKsPerPixelNativeSamplerStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelNativeSamplerStatus::ready: return "ready";
    case StockKsPerPixelNativeSamplerStatus::invalid_settings:
        return "invalid_settings";
    case StockKsPerPixelNativeSamplerStatus::backend_unsupported:
        return "backend_unsupported";
    case StockKsPerPixelNativeSamplerStatus::linear_sampler_failed:
        return "linear_sampler_failed";
    case StockKsPerPixelNativeSamplerStatus::shadow_sampler_failed:
        return "shadow_sampler_failed";
    case StockKsPerPixelNativeSamplerStatus::invalid_sampler:
        return "invalid_sampler";
    case StockKsPerPixelNativeSamplerStatus::allocation_failed:
        return "allocation_failed";
    }
    return "unknown";
}

const char* stock_ks_per_pixel_native_draw_resources_status_name(
    StockKsPerPixelNativeDrawResourcesStatus status) noexcept {
    switch (status) {
    case StockKsPerPixelNativeDrawResourcesStatus::ready:
        return "ready";
    case StockKsPerPixelNativeDrawResourcesStatus::shader_missing:
        return "shader_missing";
    case StockKsPerPixelNativeDrawResourcesStatus::constants_missing:
        return "constants_missing";
    case StockKsPerPixelNativeDrawResourcesStatus::samplers_missing:
        return "samplers_missing";
    case StockKsPerPixelNativeDrawResourcesStatus::shader_invalid:
        return "shader_invalid";
    case StockKsPerPixelNativeDrawResourcesStatus::constants_invalid:
        return "constants_invalid";
    case StockKsPerPixelNativeDrawResourcesStatus::samplers_invalid:
        return "samplers_invalid";
    case StockKsPerPixelNativeDrawResourcesStatus::device_mismatch:
        return "device_mismatch";
    case StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported:
        return "backend_unsupported";
    case StockKsPerPixelNativeDrawResourcesStatus::allocation_failed:
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
