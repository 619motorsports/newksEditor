#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string environment_value(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string result(value, length == 0U ? 0U : length - 1U);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

void put_word(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + sizeof(std::uint32_t) <= bytes.size(), "shader fixture write out of bounds");
    bytes[offset] = std::byte{static_cast<unsigned char>(value & 0xffU)};
    bytes[offset + 1U] = std::byte{static_cast<unsigned char>((value >> 8U) & 0xffU)};
    bytes[offset + 2U] = std::byte{static_cast<unsigned char>((value >> 16U) & 0xffU)};
    bytes[offset + 3U] = std::byte{static_cast<unsigned char>((value >> 24U) & 0xffU)};
}

std::vector<std::byte> minimal_spirv_fixture() {
    // Minimal vertex module assembled with spirv-as --target-env vulkan1.0.
    // It is intentionally embedded so runtime backend tests do not depend on
    // shader compiler tools being installed on the test host.
    constexpr std::array<std::uint32_t, 29> words = {
        0x07230203U, 0x00010000U, 0x00070000U, 0x00000005U, 0x00000000U,
        0x00020011U, 0x00000001U, 0x0003000eU, 0x00000000U, 0x00000001U,
        0x0005000fU, 0x00000000U, 0x00000001U, 0x6e69616dU, 0x00000000U,
        0x00020013U, 0x00000002U, 0x00030021U, 0x00000003U, 0x00000002U,
        0x00050036U, 0x00000002U, 0x00000001U, 0x00000000U, 0x00000003U,
        0x000200f8U, 0x00000004U, 0x000100fdU, 0x00010038U,
    };
    std::vector<std::byte> bytes(words.size() * sizeof(std::uint32_t));
    for (std::size_t index = 0U; index < words.size(); ++index)
        put_word(bytes, index * sizeof(std::uint32_t), words[index]);
    return bytes;
}

std::vector<std::byte> minimal_dxbc_fixture() {
    std::vector<std::byte> bytes(48U);
    put_word(bytes, 0U, 0x43425844U); // DXBC
    put_word(bytes, 20U, 1U);
    put_word(bytes, 24U, static_cast<std::uint32_t>(bytes.size()));
    put_word(bytes, 28U, 1U);
    put_word(bytes, 32U, 36U);
    put_word(bytes, 36U, 0x58454853U); // SHEX
    put_word(bytes, 40U, 4U);
    put_word(bytes, 44U, 0U);
    return bytes;
}

void contract_names() {
    using apex::render::Backend;
    using apex::render::DeviceStatus;
    require(std::string(apex::render::backend_name(Backend::Vulkan)) == "Vulkan", "Vulkan backend name");
    require(std::string(apex::render::backend_name(Backend::D3D12)) == "Direct3D 12", "D3D12 backend name");
    require(std::string(apex::render::device_status_name(DeviceStatus::ready)) == "ready", "ready status name");
    require(std::string(apex::render::device_status_name(DeviceStatus::unavailable)) == "unavailable", "unavailable status name");
    require(std::string(apex::render::buffer_status_name(apex::render::BufferStatus::ready)) == "ready",
            "buffer ready status name");
    require(std::string(apex::render::buffer_status_name(apex::render::BufferStatus::upload_failed)) == "upload_failed",
            "buffer upload status name");
    require(std::string(apex::render::texture_status_name(apex::render::TextureStatus::ready)) == "ready",
            "texture ready status name");
    require(std::string(apex::render::texture_status_name(apex::render::TextureStatus::upload_failed)) == "upload_failed",
            "texture upload status name");
    require(std::string(apex::render::texture_readback_status_name(
                apex::render::TextureReadbackStatus::execution_failed)) == "execution_failed",
            "texture readback status name");
    require(std::string(apex::render::sampler_status_name(apex::render::SamplerStatus::ready)) == "ready",
            "sampler ready status name");
    require(std::string(apex::render::shader_module_status_name(apex::render::ShaderModuleStatus::unsupported)) ==
                "unsupported",
            "shader unsupported status name");
}

void contract_options() {
    using namespace apex::render;
    DeviceOptions options;
    options.headless = false;
    const AdapterResult adapters = enumerate_adapters(Backend::Vulkan, options);
    const DeviceResult device = create_device(Backend::Vulkan, options);
    require(adapters.status == DeviceStatus::invalid_options, "adapter invalid-options status");
    require(device.status == DeviceStatus::invalid_options, "device invalid-options status");
    require(adapters.diagnostic.code == "headless_required", "adapter headless diagnostic");
    require(device.diagnostic.code == "headless_required", "device headless diagnostic");

    options.headless = true;
    options.allow_software = false;
    options.prefer_software = true;
    require(enumerate_adapters(Backend::Vulkan, options).status == DeviceStatus::invalid_options,
            "contradictory software adapter options");
}

class ContractBuffer final : public apex::render::Buffer {
public:
    explicit ContractBuffer(apex::render::BufferDescription description) : info_({description}) {}

    apex::render::Backend backend() const noexcept override { return apex::render::Backend::Vulkan; }
    const apex::render::BufferInfo& info() const noexcept override { return info_; }

private:
    apex::render::BufferInfo info_;
};

class ContractTexture final : public apex::render::Texture {
public:
    explicit ContractTexture(apex::render::TextureDescription description) : info_({description}) {}

    apex::render::Backend backend() const noexcept override { return apex::render::Backend::Vulkan; }
    const apex::render::TextureInfo& info() const noexcept override { return info_; }

private:
    apex::render::TextureInfo info_;
};

class ContractD3D12Texture final : public apex::render::Texture {
public:
    explicit ContractD3D12Texture(apex::render::TextureDescription description) : info_({description}) {}

    apex::render::Backend backend() const noexcept override { return apex::render::Backend::D3D12; }
    const apex::render::TextureInfo& info() const noexcept override { return info_; }

private:
    apex::render::TextureInfo info_;
};

void contract_buffer_limits() {
    using namespace apex::render;
    Diagnostic diagnostic;
    BufferDescription description;
    require(validate_buffer_description(description, 0U, diagnostic) == BufferStatus::invalid_description,
            "zero-sized buffer rejected");
    require(diagnostic.code == "buffer_size_zero", "zero-sized buffer diagnostic");
    description.size_bytes = max_buffer_bytes + 1U;
    description.usage = BufferUsage::vertex;
    require(validate_buffer_description(description, 0U, diagnostic) == BufferStatus::invalid_description,
            "oversized buffer rejected");
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
        require(diagnostic.code == "buffer_size_platform_limit", "platform buffer-size diagnostic");
    else
        require(diagnostic.code == "buffer_size_limit", "oversized buffer diagnostic");
    description.size_bytes = 16U;
    description.usage = static_cast<BufferUsage>(1U << 31U);
    require(validate_buffer_description(description, 0U, diagnostic) == BufferStatus::invalid_description,
            "unknown usage rejected");
    require(diagnostic.code == "buffer_usage_invalid", "unknown usage diagnostic");
    description.usage = BufferUsage::vertex;
    require(validate_buffer_description(description, 17U, diagnostic) == BufferStatus::invalid_description,
            "initial data limit rejected");
    require(diagnostic.code == "buffer_initial_data_too_large", "initial data diagnostic");

    ContractBuffer immutable({16U, BufferUsage::vertex, BufferMemory::host_visible, BufferMutability::immutable});
    require(validate_buffer_update(immutable, 0U, 1U, diagnostic) == BufferStatus::invalid_description,
            "immutable update rejected");
    require(diagnostic.code == "buffer_immutable", "immutable update diagnostic");
    ContractBuffer mutable_buffer({16U, BufferUsage::vertex, BufferMemory::host_visible, BufferMutability::mutable_data});
    require(validate_buffer_update(mutable_buffer, 15U, 2U, diagnostic) == BufferStatus::invalid_description,
            "out-of-range update rejected");
    require(diagnostic.code == "buffer_update_out_of_range", "out-of-range update diagnostic");
    require(validate_buffer_update(mutable_buffer, 0U, 0U, diagnostic) == BufferStatus::invalid_description,
            "empty update rejected");
    require(diagnostic.code == "buffer_update_empty", "empty update diagnostic");
}

void contract_texture_limits() {
    using namespace apex::render;
    Diagnostic diagnostic;
    const auto rgb565_info = texture_format_info(TextureFormat::r5g6b5_unorm);
    require(rgb565_info.classification == TextureFormatClass::uncompressed &&
                rgb565_info.bytes_per_pixel == 2U && texture_format_bytes_per_pixel(TextureFormat::r5g6b5_unorm) == 2U,
            "RGB565 texture metadata");
    const auto bc7_info = texture_format_info(TextureFormat::bc7_srgb);
    require(bc7_info.classification == TextureFormatClass::block_compressed && bc7_info.block_width == 4U &&
                bc7_info.block_height == 4U && bc7_info.block_bytes == 16U && bc7_info.srgb &&
                texture_format_is_compressed(TextureFormat::bc7_srgb),
            "BC7 texture metadata");
    TextureDescription description;
    TextureUploadPlan empty;
    require(validate_texture_description(description, empty, diagnostic) == TextureStatus::invalid_description,
            "zero-sized texture rejected");
    require(diagnostic.code == "texture_dimensions_invalid", "zero-sized texture diagnostic");
    description.width = max_texture_dimension + 1U;
    description.height = 1U;
    description.usage = TextureUsage::sampled;
    require(validate_texture_description(description, empty, diagnostic) == TextureStatus::invalid_description,
            "oversized texture rejected");
    require(diagnostic.code == "texture_dimension_limit", "oversized texture diagnostic");
    description.width = 1U;
    description.mip_levels = 2U;
    require(validate_texture_description(description, empty, diagnostic) == TextureStatus::invalid_description,
            "too many texture mips rejected");
    require(diagnostic.code == "texture_mip_limit", "texture mip diagnostic");
    description.mip_levels = 1U;
    description.format = static_cast<TextureFormat>(255U);
    require(validate_texture_description(description, empty, diagnostic) == TextureStatus::unsupported,
            "unknown texture format rejected");
    require(diagnostic.code == "texture_format_unknown", "unknown texture format diagnostic");
    description.format = TextureFormat::bc1_unorm;
    require(validate_texture_description(description, empty, diagnostic) == TextureStatus::unsupported,
            "compressed texture format rejected explicitly");
    require(diagnostic.code == "texture_compressed_format_unsupported",
            "compressed texture format diagnostic");
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::none;
    require(validate_texture_description(description, empty, diagnostic) == TextureStatus::invalid_description,
            "empty texture usage rejected");
    require(diagnostic.code == "texture_usage_invalid", "texture usage diagnostic");

    description.width = 2U;
    description.height = 2U;
    description.usage = TextureUsage::sampled;
    const std::array<std::byte, 16> pixels{};
    TextureUpload upload{0U, 0U, 2U, 2U, 7U, pixels};
    TextureUploadPlan plan{{upload}};
    require(validate_texture_description(description, plan, diagnostic) == TextureStatus::invalid_description,
            "short texture row pitch rejected");
    require(diagnostic.code == "texture_row_pitch_too_small", "row pitch diagnostic");
    upload.row_pitch = 8U;
    upload.data = std::span<const std::byte>(pixels.data(), 8U);
    plan.subresources[0] = upload;
    require(validate_texture_description(description, plan, diagnostic) == TextureStatus::invalid_description,
            "truncated texture upload rejected");
    require(diagnostic.code == "texture_upload_truncated", "truncated upload diagnostic");
    upload.data = pixels;
    plan.subresources[0] = upload;
    plan.subresources.push_back(upload);
    require(validate_texture_description(description, plan, diagnostic) == TextureStatus::invalid_description,
            "duplicate texture subresource rejected");
    require(diagnostic.code == "texture_subresource_duplicate", "duplicate subresource diagnostic");

    description.mutability = TextureMutability::immutable;
    ContractTexture immutable(description);
    TextureUploadPlan valid_plan{{TextureUpload{0U, 0U, 2U, 2U, 8U, pixels}}};
    require(validate_texture_upload_plan(description, valid_plan, diagnostic) == TextureStatus::ready,
            "valid texture upload plan accepted");
    require(validate_texture_update(immutable, valid_plan, diagnostic) == TextureStatus::invalid_description,
            "immutable texture update rejected");
    require(diagnostic.code == "texture_immutable", "immutable texture diagnostic");

    TextureDescription array_mips{4U, 2U, 2U, 2U, TextureFormat::r8_unorm,
                                  TextureUsage::sampled, TextureMemory::device_local,
                                  TextureMutability::mutable_data};
    const std::array<std::byte, 8> mip0{};
    const std::array<std::byte, 4> mip1{};
    TextureUploadPlan all_subresources{{
        TextureUpload{0U, 0U, 4U, 2U, 4U, mip0},
        TextureUpload{1U, 0U, 2U, 1U, 4U, mip1},
        TextureUpload{0U, 1U, 4U, 2U, 4U, mip0},
        TextureUpload{1U, 1U, 2U, 1U, 4U, mip1},
    }};
    require(validate_texture_description(array_mips, all_subresources, diagnostic) == TextureStatus::ready,
            "multi-mip array texture plan accepted");
    TextureUploadPlan partial{{TextureUpload{1U, 1U, 2U, 1U, 4U, mip1}}};
    require(validate_texture_upload_plan(array_mips, partial, diagnostic) == TextureStatus::ready,
            "partial texture update plan accepted");
    partial.subresources.push_back(partial.subresources.front());
    require(validate_texture_upload_plan(array_mips, partial, diagnostic) == TextureStatus::invalid_description &&
                diagnostic.code == "texture_subresource_duplicate",
            "duplicate array-mip subresource rejected");
}

void contract_texture_readback_limits() {
    using namespace apex::render;
    Diagnostic diagnostic;
    const TextureDescription description{4U, 2U, 2U, 1U, TextureFormat::rgba8_unorm,
                                         TextureUsage::color_attachment | TextureUsage::transfer_source,
                                         TextureMemory::device_local, TextureMutability::mutable_data};
    ContractTexture texture(description);
    TextureClearReadbackRequest request{{0, 1, 0, 1}, 0U, 0U, 4U, 2U};
    require(validate_texture_clear_readback(texture, request, diagnostic) == TextureReadbackStatus::ready,
            "valid texture clear/readback request accepted");
    request.clear_color[0] = std::numeric_limits<float>::quiet_NaN();
    require(validate_texture_clear_readback(texture, request, diagnostic) == TextureReadbackStatus::invalid_request &&
                diagnostic.code == "texture_clear_color_non_finite", "non-finite clear color rejected");
    request.clear_color[0] = 0.0F;
    request.output_width = 2U;
    require(validate_texture_clear_readback(texture, request, diagnostic) == TextureReadbackStatus::invalid_request &&
                diagnostic.code == "texture_readback_dimensions", "readback output dimensions rejected");
    request.output_width = 4U;
    request.mip_level = 2U;
    require(validate_texture_clear_readback(texture, request, diagnostic) == TextureReadbackStatus::invalid_request &&
                diagnostic.code == "texture_readback_subresource_out_of_range", "readback mip range rejected");
    request.mip_level = 0U;
    TextureDescription array_mips{4U, 2U, 2U, 2U, TextureFormat::rgba8_unorm,
                                  TextureUsage::color_attachment | TextureUsage::transfer_source,
                                  TextureMemory::device_local, TextureMutability::mutable_data};
    ContractTexture array_mips_texture(array_mips);
    request.mip_level = 1U;
    request.array_layer = 1U;
    request.output_width = 2U;
    request.output_height = 1U;
    require(validate_texture_clear_readback(array_mips_texture, request, diagnostic) ==
                TextureReadbackStatus::ready,
            "array/mip readback subresource accepted");
    request.array_layer = 2U;
    require(validate_texture_clear_readback(array_mips_texture, request, diagnostic) ==
                TextureReadbackStatus::invalid_request &&
                diagnostic.code == "texture_readback_subresource_out_of_range",
            "array readback layer range enforced");
    request.array_layer = 1U;
    request.mip_level = 0U;
    request.array_layer = 0U;
    request.output_width = 4U;
    request.output_height = 2U;
    TextureDescription missing_usage = description;
    missing_usage.usage = TextureUsage::color_attachment;
    ContractTexture missing_usage_texture(missing_usage);
    require(validate_texture_clear_readback(missing_usage_texture, request, diagnostic) ==
                TextureReadbackStatus::invalid_request && diagnostic.code == "texture_readback_usage_invalid",
            "readback transfer usage required");
    TextureDescription unsupported_format = description;
    unsupported_format.format = TextureFormat::r8_unorm;
    ContractTexture unsupported_texture(unsupported_format);
    require(validate_texture_clear_readback(unsupported_texture, request, diagnostic) ==
                TextureReadbackStatus::unsupported && diagnostic.code == "texture_readback_format_unsupported",
            "unsupported readback format rejected");
    TextureDescription oversized = description;
    oversized.width = max_texture_dimension;
    oversized.height = max_texture_dimension;
    ContractTexture oversized_texture(oversized);
    TextureClearReadbackRequest oversized_request{{0, 1, 0, 1}, 0U, 0U,
                                                  max_texture_dimension, max_texture_dimension};
    require(validate_texture_clear_readback(oversized_texture, oversized_request, diagnostic) ==
                TextureReadbackStatus::invalid_request && diagnostic.code == "texture_readback_size_limit",
            "readback output byte budget enforced");
    ContractD3D12Texture foreign_texture(description);
    require(foreign_texture.backend() == Backend::D3D12 && texture.backend() == Backend::Vulkan,
            "backend ownership fixture");
}

void contract_sampler_shader_limits() {
    using namespace apex::render;
    Diagnostic diagnostic;
    SamplerDescription sampler;
    require(validate_sampler_description(sampler, diagnostic) == SamplerStatus::ready,
            "default sampler accepted");
    sampler.min_filter = SamplerFilter::anisotropic;
    require(validate_sampler_description(sampler, diagnostic) == SamplerStatus::invalid_description &&
                diagnostic.code == "sampler_anisotropy_invalid", "anisotropy requires a non-unit limit");
    sampler.min_filter = SamplerFilter::linear;
    sampler.max_anisotropy = 17.0F;
    require(validate_sampler_description(sampler, diagnostic) == SamplerStatus::invalid_description &&
                diagnostic.code == "sampler_anisotropy_invalid", "anisotropy limit enforced");
    sampler.max_anisotropy = 1.0F;
    sampler.min_lod = 4.0F;
    sampler.max_lod = 2.0F;
    require(validate_sampler_description(sampler, diagnostic) == SamplerStatus::invalid_description &&
                diagnostic.code == "sampler_lod_invalid", "sampler LOD ordering enforced");

    const std::array<std::byte, 3> short_bytecode{};
    ShaderModuleDescription shader{ShaderStage::vertex, short_bytecode};
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::invalid_description &&
                diagnostic.code == "shader_bytecode_alignment", "short shader alignment rejected");
    std::vector<std::byte> oversized_shader(max_shader_module_bytes + 4U);
    require(validate_shader_module_description({ShaderStage::vertex, oversized_shader}, diagnostic) ==
                ShaderModuleStatus::invalid_description && diagnostic.code == "shader_bytecode_size_limit",
            "shader bytecode size limit enforced");
    const auto spirv = minimal_spirv_fixture();
    shader.bytecode = spirv;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::ready,
            "structurally valid SPIR-V accepted");
    for (std::size_t prefix = 0; prefix < spirv.size(); ++prefix) {
        // A word-aligned prefix at an instruction boundary can be
        // structurally valid even when it is not a complete shader module.
        if (prefix >= 5U * sizeof(std::uint32_t) && prefix % sizeof(std::uint32_t) == 0U) continue;
        shader.bytecode = std::span<const std::byte>(spirv).first(prefix);
        require(validate_shader_module_description(shader, diagnostic) != ShaderModuleStatus::ready,
                "misaligned or header-truncated SPIR-V prefix rejected");
    }
    std::vector<std::byte> bad_spirv = spirv;
    put_word(bad_spirv, 12U, 0U);
    shader.bytecode = bad_spirv;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::invalid_description &&
                diagnostic.code == "shader_spirv_bound_invalid", "zero SPIR-V bound rejected");
    bad_spirv = spirv;
    put_word(bad_spirv, 16U, 1U);
    shader.bytecode = bad_spirv;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::invalid_description &&
                diagnostic.code == "shader_spirv_schema_invalid", "nonzero SPIR-V schema rejected");
    bad_spirv = spirv;
    put_word(bad_spirv, bad_spirv.size() - sizeof(std::uint32_t), 0x00020038U);
    shader.bytecode = bad_spirv;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::invalid_description &&
                diagnostic.code == "shader_spirv_instruction_truncated", "truncated SPIR-V instruction rejected");
    const std::array<std::byte, 4> reversed_magic = {
        std::byte{0x07}, std::byte{0x23}, std::byte{0x02}, std::byte{0x03}};
    shader.bytecode = reversed_magic;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::unsupported &&
                diagnostic.code == "shader_bytecode_signature", "non-little-endian SPIR-V rejected");
    const std::array<std::byte, 4> unknown = {
        std::byte{'N'}, std::byte{'O'}, std::byte{'P'}, std::byte{'E'}};
    shader.bytecode = unknown;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::unsupported &&
                diagnostic.code == "shader_bytecode_signature", "unknown shader signature rejected");
    const auto dxbc = minimal_dxbc_fixture();
    shader.bytecode = dxbc;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::ready,
            "structurally valid DXBC accepted");
    std::vector<std::byte> dxil_chunk = dxbc;
    put_word(dxil_chunk, 36U, 0x4C495844U); // DXIL chunk inside a DXBC container.
    shader.bytecode = dxil_chunk;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::ready,
            "DXIL chunk in DXBC accepted");
    for (std::size_t prefix = 0; prefix < dxbc.size(); ++prefix) {
        shader.bytecode = std::span<const std::byte>(dxbc).first(prefix);
        require(validate_shader_module_description(shader, diagnostic) != ShaderModuleStatus::ready,
                "truncated DXBC prefix rejected");
    }
    std::vector<std::byte> bad_dxbc = dxbc;
    put_word(bad_dxbc, 24U, 44U);
    shader.bytecode = bad_dxbc;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::invalid_description &&
                diagnostic.code == "shader_dxbc_size_invalid", "DXBC total size mismatch rejected");
    bad_dxbc = dxbc;
    put_word(bad_dxbc, 32U, 0U);
    shader.bytecode = bad_dxbc;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::invalid_description &&
                diagnostic.code == "shader_dxbc_offset_invalid", "DXBC invalid offset rejected");
    std::vector<std::byte> no_shader_chunk = dxbc;
    put_word(no_shader_chunk, 36U, 0x4D41504DU); // MAP0, structurally valid but unsupported.
    shader.bytecode = no_shader_chunk;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::unsupported &&
                diagnostic.code == "shader_dxbc_chunk_unsupported", "unsupported DXBC chunk rejected");
    const std::array<std::byte, 32> standalone_dxil = {
        std::byte{'D'}, std::byte{'X'}, std::byte{'I'}, std::byte{'L'},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    shader.bytecode = standalone_dxil;
    require(validate_shader_module_description(shader, diagnostic) == ShaderModuleStatus::unsupported &&
                diagnostic.code == "shader_bytecode_signature", "standalone DXIL rejected");
}

bool contract_backend(apex::render::Backend backend) {
    using namespace apex::render;
    DeviceOptions options{};
    options.enable_validation = !environment_value("APEX_RENDER_VALIDATION").empty();
    const std::string requested_adapter = environment_value("APEX_D3D12_ADAPTER");
    if (backend == Backend::D3D12 && requested_adapter == "warp")
        options.prefer_software = true;
    const AdapterResult adapters = enumerate_adapters(backend, options);
    const DeviceResult device = create_device(backend, options);
    if (!adapters.ok() || !device.ok()) {
        // Runtime backends are optional in CI. A missing loader, platform, or
        // driver is a skip, but it must always carry an actionable diagnostic.
        if (!adapters.ok()) {
            require(!adapters.diagnostic.code.empty(), "adapter diagnostic code");
            require(!adapters.diagnostic.message.empty(), "adapter diagnostic message");
        }
        if (!device.ok()) {
            require(!device.diagnostic.code.empty(), "device diagnostic code");
            require(!device.diagnostic.message.empty(), "device diagnostic message");
        }
        std::cout << "SKIP " << backend_name(backend) << ": " << device.diagnostic.code
                  << ": " << device.diagnostic.message << '\n';
        return false;
    }
    require(!adapters.adapters.empty(), "adapter list");
    require(device.device != nullptr, "created device");
    require(device.device->info().backend == backend, "created backend");
    require(!device.device->info().name.empty(), "device name");
    const SamplerResult sampler = device.device->create_sampler(SamplerDescription{});
    require(sampler.ok(), "real backend sampler creation");
    std::vector<std::unique_ptr<Sampler>> d3d_sampler_handles;
    if (backend == Backend::D3D12) {
        d3d_sampler_handles.reserve(2047U);
        for (std::size_t index = 0; index < 2047U; ++index) {
            SamplerResult extra = device.device->create_sampler(SamplerDescription{});
            require(extra.ok(), "D3D12 sampler capacity fixture allocation");
            d3d_sampler_handles.push_back(std::move(extra.sampler));
        }
        const SamplerResult exhausted = device.device->create_sampler(SamplerDescription{});
        require(exhausted.status == SamplerStatus::allocation_failed &&
                    exhausted.diagnostic.code == "d3d12_sampler_limit",
                "D3D12 sampler capacity limit");
    }
    const auto minimal_spirv = minimal_spirv_fixture();
    const ShaderModuleResult shader = device.device->create_shader_module({ShaderStage::vertex, minimal_spirv});
    if (backend == Backend::Vulkan)
        require(shader.ok(), "Vulkan structurally valid shader module creation");
    else
        require(shader.status == ShaderModuleStatus::unsupported &&
                    shader.diagnostic.code == "d3d12_shader_format_unsupported",
                "D3D12 rejects SPIR-V shader modules explicitly");
    const auto minimal_dxbc = minimal_dxbc_fixture();
    const ShaderModuleResult dxil = device.device->create_shader_module({ShaderStage::vertex, minimal_dxbc});
    if (backend == Backend::D3D12)
        require(dxil.ok(), "D3D12 immutable shader blob resource");
    else
        require(dxil.status == ShaderModuleStatus::unsupported &&
                    dxil.diagnostic.code == "vulkan_shader_format_unsupported",
                "Vulkan rejects DXIL shader modules explicitly");
    const std::array<std::byte, 4> initial = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    BufferDescription mutable_description{16U, BufferUsage::vertex, BufferMemory::host_visible,
                                          BufferMutability::mutable_data};
    BufferResult mutable_buffer = device.device->create_buffer(mutable_description, initial);
    require(mutable_buffer.ok(), "host-visible mutable buffer creation");
    require(mutable_buffer.buffer->info().description.size_bytes == 16U, "buffer size metadata");
    BufferUpdateResult update = device.device->update_buffer(*mutable_buffer.buffer, 4U, initial);
    require(update.ok(), "host-visible mutable buffer update");
    update = device.device->update_buffer(*mutable_buffer.buffer, 15U, initial);
    require(update.status == BufferStatus::invalid_description, "real backend rejects out-of-range update");
    BufferDescription immutable_description{16U, BufferUsage::vertex, BufferMemory::device_local,
                                             BufferMutability::immutable};
    BufferResult immutable_buffer = device.device->create_buffer(immutable_description, initial);
    require(immutable_buffer.ok(), "device-local immutable buffer upload");
    update = device.device->update_buffer(*immutable_buffer.buffer, 0U, initial);
    require(update.status == BufferStatus::invalid_description, "real backend rejects immutable update");
    const std::array<std::byte, 16> texture_pixels = {
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7},
        std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11},
        std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15},
    };
    TextureDescription mutable_texture_description{2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm,
                                                    TextureUsage::sampled, TextureMemory::device_local,
                                                    TextureMutability::mutable_data};
    TextureUploadPlan texture_uploads{{TextureUpload{0U, 0U, 2U, 2U, 8U, texture_pixels}}};
    TextureResult mutable_texture = device.device->create_texture(mutable_texture_description, texture_uploads);
    require(mutable_texture.ok(), "mutable texture creation and upload");
    TextureUpdateResult texture_update = device.device->update_texture(*mutable_texture.texture, texture_uploads);
    require(texture_update.ok(), "mutable texture update");
    TextureDescription immutable_texture_description = mutable_texture_description;
    immutable_texture_description.mutability = TextureMutability::immutable;
    TextureResult immutable_texture = device.device->create_texture(immutable_texture_description, texture_uploads);
    require(immutable_texture.ok(), "immutable texture creation and upload");
    texture_update = device.device->update_texture(*immutable_texture.texture, texture_uploads);
    require(texture_update.status == TextureStatus::invalid_description, "real backend rejects immutable texture update");
    TextureDescription host_texture_description = mutable_texture_description;
    host_texture_description.memory = TextureMemory::host_visible;
    TextureResult host_texture = device.device->create_texture(host_texture_description, texture_uploads);
    require(host_texture.status == TextureStatus::unsupported, "real backend rejects host-visible texture memory");
    TextureDescription clear_texture_description{2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm,
                                                  TextureUsage::color_attachment | TextureUsage::transfer_source,
                                                  TextureMemory::device_local, TextureMutability::mutable_data};
    TextureResult clear_texture = device.device->create_texture(clear_texture_description);
    require(clear_texture.ok(), "clear/readback texture creation");
    const TextureClearReadbackRequest clear_request{{0.0F, 1.0F, 0.0F, 1.0F}, 0U, 0U, 2U, 2U};
    TextureClearReadbackResult foreign_result;
    if (backend == Backend::Vulkan) {
        ContractD3D12Texture foreign_texture(clear_texture_description);
        foreign_result = device.device->clear_texture_and_readback(foreign_texture, clear_request);
    } else {
        ContractTexture foreign_texture(clear_texture_description);
        foreign_result = device.device->clear_texture_and_readback(foreign_texture, clear_request);
    }
    require(foreign_result.status == TextureReadbackStatus::unsupported &&
                foreign_result.diagnostic.code == "texture_backend_mismatch",
            "real backend rejects foreign texture ownership");
    const TextureClearReadbackResult clear_result =
        device.device->clear_texture_and_readback(*clear_texture.texture, clear_request);
    require(clear_result.ok() && clear_result.rgba8.size() == 16U, "real clear/readback execution");
    for (std::size_t pixel = 0; pixel < clear_result.rgba8.size(); pixel += 4U) {
        require(clear_result.rgba8[pixel] == std::byte{0} && clear_result.rgba8[pixel + 1U] == std::byte{255} &&
                    clear_result.rgba8[pixel + 2U] == std::byte{0} && clear_result.rgba8[pixel + 3U] == std::byte{255},
                "deterministic canonical RGBA8 clear pixels");
    }
    TextureClearReadbackRequest invalid_clear = clear_request;
    invalid_clear.clear_color[0] = std::numeric_limits<float>::infinity();
    const TextureClearReadbackResult invalid_clear_result =
        device.device->clear_texture_and_readback(*clear_texture.texture, invalid_clear);
    require(invalid_clear_result.status == TextureReadbackStatus::invalid_request &&
                invalid_clear_result.diagnostic.code == "texture_clear_color_non_finite",
            "real clear/readback validates clear color");
    const TextureClearReadbackRequest second_clear{{0.0F, 0.0F, 1.0F, 1.0F}, 0U, 0U, 2U, 2U};
    const TextureClearReadbackResult repeated_clear =
        device.device->clear_texture_and_readback(*clear_texture.texture, second_clear);
    require(repeated_clear.ok(), "repeated clear/readback preserves tracked state");
    require(repeated_clear.rgba8.size() == 16U && repeated_clear.rgba8[0] == std::byte{0} &&
                repeated_clear.rgba8[1] == std::byte{0} && repeated_clear.rgba8[2] == std::byte{255} &&
                repeated_clear.rgba8[3] == std::byte{255},
            "repeated clear/readback pixels");

    const TextureDescription bgra_description{3U, 2U, 1U, 1U, TextureFormat::bgra8_unorm,
                                               TextureUsage::color_attachment | TextureUsage::transfer_source,
                                               TextureMemory::device_local, TextureMutability::mutable_data};
    TextureResult bgra_texture = device.device->create_texture(bgra_description);
    require(bgra_texture.ok(), "BGRA clear/readback texture creation");
    const TextureClearReadbackRequest bgra_request{{1.0F, 0.0F, 0.0F, 1.0F}, 0U, 0U, 3U, 2U};
    const TextureClearReadbackResult bgra_result =
        device.device->clear_texture_and_readback(*bgra_texture.texture, bgra_request);
    require(bgra_result.ok() && bgra_result.rgba8.size() == 24U, "BGRA clear/readback execution");
    for (std::size_t pixel = 0; pixel < bgra_result.rgba8.size(); pixel += 4U)
        require(bgra_result.rgba8[pixel] == std::byte{255} && bgra_result.rgba8[pixel + 1U] == std::byte{0} &&
                    bgra_result.rgba8[pixel + 2U] == std::byte{0} && bgra_result.rgba8[pixel + 3U] == std::byte{255},
                "BGRA canonical red readback");

    const TextureDescription srgb_description{2U, 2U, 1U, 1U, TextureFormat::rgba8_srgb,
                                               TextureUsage::color_attachment | TextureUsage::transfer_source,
                                               TextureMemory::device_local, TextureMutability::mutable_data};
    TextureResult srgb_texture = device.device->create_texture(srgb_description);
    require(srgb_texture.ok(), "sRGB clear/readback texture creation");
    const TextureClearReadbackRequest srgb_request{{0.0F, 1.0F, 0.0F, 1.0F}, 0U, 0U, 2U, 2U};
    const TextureClearReadbackResult srgb_result =
        device.device->clear_texture_and_readback(*srgb_texture.texture, srgb_request);
    require(srgb_result.ok() && srgb_result.rgba8.size() == 16U, "sRGB clear/readback execution");
    for (std::size_t pixel = 0; pixel < srgb_result.rgba8.size(); pixel += 4U)
        require(srgb_result.rgba8[pixel] == std::byte{0} && srgb_result.rgba8[pixel + 1U] == std::byte{255} &&
                    srgb_result.rgba8[pixel + 2U] == std::byte{0} && srgb_result.rgba8[pixel + 3U] == std::byte{255},
                "sRGB canonical green readback");

    const TextureDescription array_description{4U, 4U, 2U, 2U, TextureFormat::rgba8_unorm,
                                                TextureUsage::color_attachment | TextureUsage::transfer_source,
                                                TextureMemory::device_local, TextureMutability::mutable_data};
    TextureResult array_texture = device.device->create_texture(array_description);
    require(array_texture.ok(), "array/mip clear/readback texture creation");
    const TextureClearReadbackRequest array_request{{1.0F, 0.0F, 1.0F, 1.0F}, 1U, 1U, 2U, 2U};
    const TextureClearReadbackResult array_result =
        device.device->clear_texture_and_readback(*array_texture.texture, array_request);
    require(array_result.ok() && array_result.rgba8.size() == 16U, "array/mip clear/readback execution");
    for (std::size_t pixel = 0; pixel < array_result.rgba8.size(); pixel += 4U)
        require(array_result.rgba8[pixel] == std::byte{255} && array_result.rgba8[pixel + 1U] == std::byte{0} &&
                    array_result.rgba8[pixel + 2U] == std::byte{255} && array_result.rgba8[pixel + 3U] == std::byte{255},
                "array/mip clear/readback pixels");
    if (backend == Backend::D3D12) {
        const BufferDescription incompatible{16U,
                                             BufferUsage::transfer_destination | BufferUsage::vertex,
                                             BufferMemory::device_local,
                                             BufferMutability::mutable_data};
        const BufferResult rejected = device.device->create_buffer(incompatible);
        require(rejected.status == BufferStatus::unsupported,
                "D3D12 rejects combined copy-destination/read usage");
        require(rejected.diagnostic.code == "d3d12_transfer_destination_exclusive",
                "D3D12 combined usage diagnostic");
        TextureDescription invalid_texture_usage = mutable_texture_description;
        invalid_texture_usage.usage = TextureUsage::sampled | TextureUsage::color_attachment;
        const TextureResult invalid_texture = device.device->create_texture(invalid_texture_usage, texture_uploads);
        require(invalid_texture.status == TextureStatus::unsupported,
                "D3D12 rejects combined render-target and sampled texture usage");
        require(invalid_texture.diagnostic.code == "d3d12_render_target_texture_exclusive",
                "D3D12 combined texture usage diagnostic");
    }
    device.device->wait_idle();
    std::cout << "PASS " << backend_name(backend) << ": " << device.device->info().name << '\n';
    return true;
}

} // namespace

int main() {
    try {
        contract_names();
        contract_options();
        contract_buffer_limits();
        contract_texture_limits();
        contract_texture_readback_limits();
        contract_sampler_shader_limits();
        const std::string requested = environment_value("APEX_RENDER_BACKEND");
        if (requested == "vulkan") {
            return contract_backend(apex::render::Backend::Vulkan) ? 0 : 77;
        }
        if (!requested.empty() && requested != "d3d12") {
            std::cerr << "Unknown APEX_RENDER_BACKEND value: " << requested << '\n';
            return 2;
        }
        if (!requested.empty()) {
            return contract_backend(apex::render::Backend::D3D12) ? 0 : 77;
        }
        (void)contract_backend(apex::render::Backend::Vulkan);
        (void)contract_backend(apex::render::Backend::D3D12);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "render backend tests failed: " << error.what() << '\n';
        return 1;
    }
}
