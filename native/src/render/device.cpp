#include "apex/render/device.hpp"
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
