#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/static_mesh_upload.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

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

std::vector<std::uint8_t> pipeline_shader_fixture() {
    const std::vector<std::byte> bytes = minimal_spirv_fixture();
    std::vector<std::uint8_t> result;
    result.reserve(bytes.size());
    for (const std::byte value : bytes) result.push_back(std::to_integer<std::uint8_t>(value));
    return result;
}

std::uint8_t hex_digit(char value) {
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
    throw std::runtime_error("invalid embedded shader hex");
}

std::vector<std::uint8_t> executable_vertex_shader() {
    constexpr std::string_view hex =
        "03022307000001000b000d001b0000000000000011000200010000000b00060001000000474c534c2e7374642e343530000000000e00030000000000010000000f00070000000000040000006d61696e000000000d00000012000000470003000b00000002000000480005000b000000000000000b00000000000000480005000b000000010000000b00000001000000480005000b000000020000000b00000003000000480005000b000000030000000b0000000400000047000400120000001e00000000000000130002000200000021000300030000000200000016000300060000002000000017000400070000000600000004000000150004000800000020000000000000002b0004000800000009000000010000001c0004000a00000006000000090000001e0006000b00000007000000060000000a0000000a000000200004000c000000030000000b0000003b0004000c0000000d00000003000000150004000e00000020000000010000002b0004000e0000000f0000000000000017000400100000000600000003000000200004001100000001000000100000003b0004001100000012000000010000002b00040006000000140000000000803f200004001900000003000000070000003600050002000000040000000000000003000000f8000200050000003d0004001000000013000000120000005100050006000000150000001300000000000000510005000600000016000000130000000100000051000500060000001700000013000000020000005000070007000000180000001500000016000000170000001400000041000500190000001a0000000d0000000f0000003e0003001a00000018000000fd00010038000100";
    require(hex.size() % 2U == 0U, "embedded vertex shader hex alignment");
    std::vector<std::uint8_t> result(hex.size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>((hex_digit(hex[index * 2U]) << 4U) |
                                                   hex_digit(hex[index * 2U + 1U]));
    return result;
}

std::vector<std::uint8_t> executable_transform_vertex_shader() {
    // Generated from tests/shaders/indexed_static_mesh.vert with:
    // glslangValidator -V --target-env vulkan1.0 -Os -g0 -S vert
    constexpr std::string_view hex =
        "03022307000001000b0008002a0000000000000011000200010000000b00060001000000474c534c2e7374642e343530000000000e00030000000000010000000f00070000000000040000006d61696e000000001500000022000000470003000b00000002000000480004000b0000000000000005000000480005000b000000000000000700000010000000480005000b000000000000002300000000000000480004000b0000000100000005000000480005000b000000010000000700000010000000480005000b00000001000000230000004000000047000400150000001e000000000000004700030020000000020000004800050020000000000000000b000000000000004800050020000000010000000b000000010000004800050020000000020000000b000000030000004800050020000000030000000b00000004000000130002000200000021000300030000000200000016000300060000002000000017000400070000000600000004000000180004000a00000007000000040000001e0004000b0000000a0000000a000000200004000c000000090000000b0000003b0004000c0000000d00000009000000150004000e00000020000000010000002b0004000e0000000f000000000000002000040010000000090000000a00000017000400130000000600000003000000200004001400000001000000130000003b0004001400000015000000010000002b00040006000000170000000000803f150004001d00000020000000000000002b0004001d0000001e000000010000001c0004001f000000060000001e0000001e0006002000000007000000060000001f0000001f000000200004002100000003000000200000003b0004002100000022000000030000002b0004000e0000002300000001000000200004002800000003000000070000003600050002000000040000000000000003000000f8000200050000004100050010000000110000000d0000000f0000003d0004000a00000012000000110000003d0004001300000016000000150000005100050006000000180000001600000000000000510005000600000019000000160000000100000051000500060000001a000000160000000200000050000700070000001b00000018000000190000001a0000001700000091000500070000001c000000120000001b0000004100050010000000240000000d000000230000003d0004000a0000002500000024000000910005000700000027000000250000001c000000410005002800000029000000220000000f0000003e0003002900000027000000fd00010038000100";
    require(hex.size() % 2U == 0U, "embedded transform shader hex alignment");
    std::vector<std::uint8_t> result(hex.size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            (hex_digit(hex[index * 2U]) << 4U) | hex_digit(hex[index * 2U + 1U]));
    }
    return result;
}

std::vector<std::uint8_t> executable_fragment_shader() {
    constexpr std::string_view hex =
        "03022307000001000b000d000d0000000000000011000200010000000b00060001000000474c534c2e7374642e343530000000000e00030000000000010000000f00060004000000040000006d61696e000000000900000010000300040000000700000047000400090000001e00000000000000130002000200000021000300030000000200000016000300060000002000000017000400070000000600000004000000200004000800000003000000070000003b0004000800000009000000030000002b000400060000000a0000000000803f2b000400060000000b000000000000002c000700070000000c0000000a0000000b0000000b0000000a0000003600050002000000040000000000000003000000f8000200050000003e000300090000000c000000fd00010038000100";
    require(hex.size() % 2U == 0U, "embedded fragment shader hex alignment");
    std::vector<std::uint8_t> result(hex.size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>((hex_digit(hex[index * 2U]) << 4U) |
                                                   hex_digit(hex[index * 2U + 1U]));
    return result;
}

std::vector<std::uint8_t> executable_blue_fragment_shader() {
    // Generated from this exact source with:
    // glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag
    // spirv-val --target-env vulkan1.0
    // Source SHA-256: 6b3ae3948ecab07330849819ce22f015f015c44ad614e6dee89bbca1e167f821
    // SPIR-V SHA-256: b250bd205a32b206f37fdf169a37aa449b75a381376d46c56e08dfba21e3ce52
    // #version 450
    // layout(location = 0) out vec4 color;
    // void main() { color = vec4(0.0, 0.0, 1.0, 1.0); }
    constexpr std::string_view hex =
        "03022307000001000b0008000d0000000000000011000200010000000b00060001000000474c534c2e7374642e343530000000000e00030000000000010000000f00060004000000040000006d61696e000000000900000010000300040000000700000047000400090000001e00000000000000130002000200000021000300030000000200000016000300060000002000000017000400070000000600000004000000200004000800000003000000070000003b0004000800000009000000030000002b000400060000000a000000000000002b000400060000000b0000000000803f2c000700070000000c0000000a0000000a0000000b0000000b0000003600050002000000040000000000000003000000f8000200050000003e000300090000000c000000fd00010038000100";
    require(hex.size() % 2U == 0U, "embedded blue fragment shader hex alignment");
    std::vector<std::uint8_t> result(hex.size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>((hex_digit(hex[index * 2U]) << 4U) |
                                                   hex_digit(hex[index * 2U + 1U]));
    return result;
}

#if defined(_WIN32)
std::vector<std::uint8_t> executable_d3d_shader(std::string_view source, std::string_view profile) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source.data(), source.size(), "apex_triangle", nullptr, nullptr, "main",
                                      profile.data(), D3DCOMPILE_ENABLE_STRICTNESS, 0U, &bytecode, &errors);
    if (FAILED(result)) {
        const std::string message = errors == nullptr
                                        ? "D3DCompile failed"
                                        : std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                      errors->GetBufferSize());
        throw std::runtime_error(message);
    }
    const auto* begin = static_cast<const std::uint8_t*>(bytecode->GetBufferPointer());
    return {begin, begin + bytecode->GetBufferSize()};
}
#endif

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
    require(std::string(apex::render::triangle_draw_status_name(
                apex::render::TriangleDrawStatus::execution_failed)) == "execution_failed",
            "triangle draw status name");
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

class ContractD3D12Buffer final : public apex::render::Buffer {
public:
    explicit ContractD3D12Buffer(apex::render::BufferDescription description) : info_({description}) {}

    apex::render::Backend backend() const noexcept override { return apex::render::Backend::D3D12; }
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

void contract_triangle_draw_limits() {
    using namespace apex::render;
    Diagnostic diagnostic;
    const TextureDescription target_description{16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
                                                TextureUsage::color_attachment | TextureUsage::transfer_source,
                                                TextureMemory::device_local, TextureMutability::mutable_data};
    ContractTexture target(target_description);
    PipelineProgram pipeline;
    pipeline.name = "triangle-contract";
    pipeline.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.vertex_layout.stride = 12U;
    pipeline.vertex_layout.attributes.push_back({PipelineVertexSemantic::position,
                                                  PipelineVertexAttributeFormat::float32x3, 0U, 0U});
    pipeline.shaders.push_back({PipelineShaderStage::vertex, PipelineShaderFormat::spirv, pipeline_shader_fixture()});
    pipeline.shaders.push_back({PipelineShaderStage::fragment, PipelineShaderFormat::spirv, pipeline_shader_fixture()});
    const std::array<std::byte, 36> vertices{};
    TriangleDrawRequest request{&pipeline, vertices, 3U, 0U, 0U, {0.0F, 0.0F, 0.0F, 1.0F}};
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::ready,
            "bounded triangle request accepted");
    std::vector<std::uint8_t> truncated_spirv = pipeline_shader_fixture();
    truncated_spirv.resize(24U);
    pipeline.shaders[0].bytes = truncated_spirv;
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::invalid_request &&
                diagnostic.code == "triangle_shader_shader_spirv_instruction_truncated",
            "triangle truncated SPIR-V instruction rejected");
    pipeline.shaders[0].bytes = pipeline_shader_fixture();
    std::vector<std::byte> malformed_dxbc = minimal_dxbc_fixture();
    put_word(malformed_dxbc, 24U, 47U);
    put_word(malformed_dxbc, 40U, 3U);
    pipeline.shaders[0].format = PipelineShaderFormat::dxil;
    pipeline.shaders[0].bytes.clear();
    pipeline.shaders[0].bytes.reserve(malformed_dxbc.size());
    for (const std::byte value : malformed_dxbc) pipeline.shaders[0].bytes.push_back(std::to_integer<std::uint8_t>(value));
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::invalid_request &&
                diagnostic.code == "triangle_shader_shader_dxbc_size_invalid",
            "triangle malformed DXBC rejected");
    pipeline.shaders[0].format = PipelineShaderFormat::spirv;
    pipeline.shaders[0].bytes = pipeline_shader_fixture();
    request.vertex_count = 4U;
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::invalid_request &&
                diagnostic.code == "triangle_vertex_layout_invalid", "triangle vertex count rejected");
    request.vertex_count = 3U;
    const std::array<std::byte, 40> extra_vertex_data{};
    request.vertex_data = extra_vertex_data;
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::invalid_request &&
                diagnostic.code == "triangle_vertex_data_invalid", "triangle trailing vertex data rejected");
    request.vertex_data = vertices;
    pipeline.targets.colors[0].format = PipelineRenderTargetFormat::bgra8_unorm;
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::invalid_request &&
                diagnostic.code == "triangle_target_format_mismatch", "triangle target format mismatch rejected");
    pipeline.targets.colors[0].format = PipelineRenderTargetFormat::rgba8_unorm;
    pipeline.blend.alpha_to_coverage = true;
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::invalid_request &&
                diagnostic.code == "triangle_pipeline_a2c_sample_count", "triangle alpha-to-coverage rejected");
    pipeline.blend.alpha_to_coverage = false;
    pipeline.resources.push_back({PipelineResourceKind::sampled_texture, 0U, 0U, "texture"});
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::unsupported &&
                diagnostic.code == "triangle_pipeline_unsupported", "triangle resource bindings rejected explicitly");
    pipeline.resources.clear();
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    require(validate_triangle_draw_request(target, request, diagnostic) == TriangleDrawStatus::unsupported &&
                diagnostic.code == "triangle_pipeline_unsupported",
            "triangle transform contract rejected explicitly");
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

    PipelineProgram triangle_pipeline;
    triangle_pipeline.name = "embedded-triangle";
    triangle_pipeline.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    triangle_pipeline.raster.cull = PipelineCullMode::none;
    triangle_pipeline.depth.test_enabled = false;
    triangle_pipeline.depth.write_enabled = false;
    triangle_pipeline.vertex_layout.stride = 12U;
    triangle_pipeline.vertex_layout.attributes.push_back({PipelineVertexSemantic::position,
                                                           PipelineVertexAttributeFormat::float32x3, 0U, 0U});
    std::vector<std::uint8_t> vertex_shader;
    std::vector<std::uint8_t> fragment_shader;
    if (backend == Backend::Vulkan) {
        vertex_shader = executable_vertex_shader();
        fragment_shader = executable_fragment_shader();
        triangle_pipeline.shaders.push_back({PipelineShaderStage::vertex, PipelineShaderFormat::spirv,
                                              std::move(vertex_shader)});
        triangle_pipeline.shaders.push_back({PipelineShaderStage::fragment, PipelineShaderFormat::spirv,
                                              std::move(fragment_shader)});
    } else {
#if defined(_WIN32)
        constexpr std::string_view vertex_source =
            "struct Input { float3 position : POSITION; };"
            "struct Output { float4 position : SV_Position; };"
            "Output main(Input input) { Output output; output.position = float4(input.position, 1.0); return output; }";
        constexpr std::string_view fragment_source =
            "float4 main(float4 position : SV_Position) : SV_Target { return float4(1, 0, 0, 1); }";
        triangle_pipeline.shaders.push_back({PipelineShaderStage::vertex, PipelineShaderFormat::dxil,
                                              executable_d3d_shader(vertex_source, "vs_5_0")});
        triangle_pipeline.shaders.push_back({PipelineShaderStage::fragment, PipelineShaderFormat::dxil,
                                              executable_d3d_shader(fragment_source, "ps_5_0")});
#else
        require(false, "D3D12 executable shader test requires Windows D3DCompile");
#endif
    }
    const TextureDescription triangle_description{32U, 32U, 1U, 1U, TextureFormat::rgba8_unorm,
                                                   TextureUsage::color_attachment | TextureUsage::transfer_source,
                                                   TextureMemory::device_local, TextureMutability::mutable_data};
    TextureResult triangle_texture = device.device->create_texture(triangle_description);
    require(triangle_texture.ok(), "executable triangle target creation");
    const std::array<float, 9> triangle_vertices = {
        -0.75F, -0.75F, 0.0F,
        0.75F, -0.75F, 0.0F,
        0.0F, 0.75F, 0.0F,
    };
    const std::span<const std::byte> triangle_vertex_bytes(
        reinterpret_cast<const std::byte*>(triangle_vertices.data()), sizeof(triangle_vertices));
    const TriangleDrawRequest triangle_request{&triangle_pipeline, triangle_vertex_bytes, 3U, 0U, 0U,
                                                {0.0F, 0.0F, 0.0F, 1.0F}};
    TriangleDrawResult foreign_triangle;
    if (backend == Backend::Vulkan) {
        ContractD3D12Texture foreign_texture(triangle_description);
        foreign_triangle = device.device->draw_triangle_and_readback(foreign_texture, triangle_request);
    } else {
        ContractTexture foreign_texture(triangle_description);
        foreign_triangle = device.device->draw_triangle_and_readback(foreign_texture, triangle_request);
    }
    require(foreign_triangle.status == TriangleDrawStatus::unsupported &&
                foreign_triangle.diagnostic.code == "texture_backend_mismatch",
            "real backend rejects foreign triangle texture ownership");
    const TriangleDrawResult triangle_result =
        device.device->draw_triangle_and_readback(*triangle_texture.texture, triangle_request);
    require(triangle_result.ok() && triangle_result.rgba8.size() == 32U * 32U * 4U,
            "executable triangle draw/readback");
    const std::size_t center = (16U * 32U + 16U) * 4U;
    require(triangle_result.rgba8[center] == std::byte{255} && triangle_result.rgba8[center + 1U] == std::byte{0} &&
                triangle_result.rgba8[center + 2U] == std::byte{0} && triangle_result.rgba8[center + 3U] == std::byte{255},
            "triangle center pixel is shader red");
    require(triangle_result.rgba8[0] == std::byte{0} && triangle_result.rgba8[1] == std::byte{0} &&
                triangle_result.rgba8[2] == std::byte{0} && triangle_result.rgba8[3] == std::byte{255},
            "triangle outside pixel retains clear color");

    apex::formats::Kn5Node indexed_mesh;
    indexed_mesh.type = 2U;
    indexed_mesh.kind = "mesh";
    indexed_mesh.vertexStride = 11U;
    indexed_mesh.vertices = {
        -0.75F, -0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         0.75F, -0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         0.0F,   0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.0F,
    };
    indexed_mesh.indices = {0U, 1U, 2U};
    DrawPacket indexed_packet;
    indexed_packet.primitive = DrawPrimitiveKind::static_mesh;
    indexed_packet.vertex_count = 3U;
    indexed_packet.index_count = 3U;
    indexed_packet.vertex_stride_floats = 11U;
    indexed_packet.world_matrix = apex::scene::identity_matrix;
    indexed_packet.world_matrix[12] = 1.5F;
    indexed_packet.shader_execution_supported = true;
    indexed_packet.flags.depth_test = false;
    indexed_packet.flags.depth_write = false;
    StaticMeshUploadResult indexed_upload =
        upload_static_mesh(*device.device, indexed_mesh, indexed_packet);
    require(indexed_upload.ok(), "validated KN5 static-mesh upload");
    PipelineProgram indexed_pipeline = triangle_pipeline;
    indexed_pipeline.name = "indexed-transform-triangle";
    indexed_pipeline.vertex_layout.stride = 11U * sizeof(float);
    indexed_pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    if (backend == Backend::Vulkan) {
        indexed_pipeline.shaders[0].bytes = executable_transform_vertex_shader();
    } else {
#if defined(_WIN32)
        constexpr std::string_view transform_vertex_source =
            "cbuffer DrawMatrices : register(b0) { column_major float4x4 world; column_major float4x4 viewProjection; };"
            "struct Input { float3 position : POSITION; };"
            "struct Output { float4 position : SV_Position; };"
            "Output main(Input input) { Output output;"
            "output.position = mul(viewProjection, mul(world, float4(input.position, 1.0))); return output; }";
        indexed_pipeline.shaders[0].bytes =
            executable_d3d_shader(transform_vertex_source, "vs_5_0");
#endif
    }
    CameraFrameRequest indexed_camera_request;
    indexed_camera_request.eye = {0.0F, 0.0F, 2.0F};
    indexed_camera_request.target = {0.0F, 0.0F, 0.0F};
    indexed_camera_request.fov_radians = 1.5707963267948966F;
    indexed_camera_request.aspect = 1.0F;
    indexed_camera_request.near_plane = 0.1F;
    indexed_camera_request.far_plane = 10.0F;
    indexed_camera_request.clip_space = backend == Backend::Vulkan
                                            ? CameraClipSpace::vulkan
                                            : CameraClipSpace::d3d12;
    const CameraFrameResult indexed_camera = build_camera_frame(indexed_camera_request);
    require(indexed_camera.ok(), "backend-specific indexed camera frame");
    IndexedStaticMeshDrawRequest indexed_request =
        indexed_upload.upload->make_request(indexed_pipeline, *indexed_camera.frame);
    const BufferDescription indexed_vertex_description =
        indexed_upload.upload->vertex_buffer->info().description;
    ContractBuffer foreign_vulkan_buffer(indexed_vertex_description);
    ContractD3D12Buffer foreign_d3d12_buffer(indexed_vertex_description);
    if (backend == Backend::Vulkan)
        indexed_request.vertex_buffer = &foreign_d3d12_buffer;
    else
        indexed_request.vertex_buffer = &foreign_vulkan_buffer;
    const IndexedStaticMeshDrawResult foreign_indexed_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, indexed_request);
    require(foreign_indexed_result.status == IndexedStaticMeshDrawStatus::unsupported &&
                foreign_indexed_result.diagnostic.code == "indexed_static_mesh_backend_mismatch",
            "indexed static-mesh rejects foreign buffer ownership");
    indexed_request.vertex_buffer = indexed_upload.upload->vertex_buffer.get();
    const IndexedStaticMeshDrawResult indexed_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, indexed_request);
    require(indexed_result.ok() && indexed_result.rgba8.size() == 32U * 32U * 4U,
            "indexed static-mesh draw/readback");
    require(indexed_result.rgba8[center] == std::byte{0} &&
                indexed_result.rgba8[center + 1U] == std::byte{0} &&
                indexed_result.rgba8[center + 2U] == std::byte{0} &&
                indexed_result.rgba8[center + 3U] == std::byte{255},
            "indexed world transform moves geometry away from center");
    const std::size_t right = (16U * 32U + 28U) * 4U;
    require(indexed_result.rgba8[right] == std::byte{255} &&
                indexed_result.rgba8[right + 1U] == std::byte{0} &&
                indexed_result.rgba8[right + 2U] == std::byte{0} &&
                indexed_result.rgba8[right + 3U] == std::byte{255},
            "indexed world transform reaches expected right pixel");
    require(indexed_result.rgba8[0] == std::byte{0} && indexed_result.rgba8[1] == std::byte{0} &&
                indexed_result.rgba8[2] == std::byte{0} && indexed_result.rgba8[3] == std::byte{255},
            "indexed static-mesh outside pixel retains clear color");
    indexed_upload.upload->packet.world_matrix = apex::scene::identity_matrix;
    indexed_camera_request.eye = {1.5F, 0.0F, 2.0F};
    indexed_camera_request.target = {1.5F, 0.0F, 0.0F};
    const CameraFrameResult shifted_camera = build_camera_frame(indexed_camera_request);
    require(shifted_camera.ok(), "shifted indexed camera frame");
    const IndexedStaticMeshDrawRequest shifted_request =
        indexed_upload.upload->make_request(indexed_pipeline, *shifted_camera.frame);
    const IndexedStaticMeshDrawResult shifted_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, shifted_request);
    require(shifted_result.ok() && shifted_result.rgba8.size() == 32U * 32U * 4U,
            "shifted camera indexed draw/readback preserves tracked state");
    const std::size_t left = (16U * 32U + 4U) * 4U;
    require(shifted_result.rgba8[center] == std::byte{0} &&
                shifted_result.rgba8[left] == std::byte{255} &&
                shifted_result.rgba8[left + 1U] == std::byte{0} &&
                shifted_result.rgba8[left + 2U] == std::byte{0} &&
                shifted_result.rgba8[left + 3U] == std::byte{255},
            "camera view-projection moves geometry to the expected left pixel");

    // Exercise the persistent depth/load contract with two real draws per
    // frame. The depth attachment is deliberately kept alive across all
    // requests below; load_color and clear_depth therefore describe actual
    // attachment operations instead of an implementation-local shortcut.
    PipelineProgram depth_pipeline = indexed_pipeline;
    depth_pipeline.name = "indexed-depth-load";
    depth_pipeline.targets.has_depth = true;
    depth_pipeline.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    depth_pipeline.depth.test_enabled = true;
    depth_pipeline.depth.write_enabled = true;
    depth_pipeline.depth.compare = PipelineCompareOperation::less;

    auto make_depth_pipeline = [&](bool blue) {
        PipelineProgram pipeline = depth_pipeline;
        if (backend == Backend::Vulkan) {
            pipeline.shaders[1].bytes = blue ? executable_blue_fragment_shader() : executable_fragment_shader();
        } else {
#if defined(_WIN32)
            constexpr std::string_view blue_fragment_source =
                "float4 main(float4 position : SV_Position) : SV_Target { return float4(0, 0, 1, 1); }";
            constexpr std::string_view red_fragment_source =
                "float4 main(float4 position : SV_Position) : SV_Target { return float4(1, 0, 0, 1); }";
            pipeline.shaders[1].bytes = executable_d3d_shader(
                blue ? blue_fragment_source : red_fragment_source, "ps_5_0");
#else
            require(false, "D3D12 depth shader test requires Windows D3DCompile");
#endif
        }
        return pipeline;
    };
    PipelineProgram depth_red_pipeline = make_depth_pipeline(false);
    PipelineProgram depth_blue_pipeline = make_depth_pipeline(true);

    DepthAttachmentDescription depth_description;
    depth_description.width = 32U;
    depth_description.height = 32U;
    depth_description.samples = 1U;
    depth_description.format = DepthAttachmentFormat::d32_float;
    DepthAttachmentResult depth_result = device.device->create_depth_attachment(depth_description);
    require(depth_result.ok(), "persistent depth attachment creation");

    auto make_depth_packet = [&](float x, float z, bool write_enabled) {
        DrawPacket packet = indexed_packet;
        packet.world_matrix = apex::scene::identity_matrix;
        packet.world_matrix[12] = x;
        packet.world_matrix[14] = z;
        packet.flags.depth_test = true;
        packet.flags.depth_write = write_enabled;
        return packet;
    };
    auto make_depth_upload = [&](const DrawPacket& packet) {
        StaticMeshUploadResult result = upload_static_mesh(*device.device, indexed_mesh, packet);
        require(result.ok(), "persistent depth static-mesh upload");
        return result;
    };
    DrawPacket left_packet = make_depth_packet(-1.1F, 0.0F, true);
    DrawPacket right_packet = make_depth_packet(1.1F, 0.0F, true);
    StaticMeshUploadResult left_upload = make_depth_upload(left_packet);
    StaticMeshUploadResult right_upload = make_depth_upload(right_packet);

    auto make_depth_request = [&](const StaticMeshUploadResult& upload, const PipelineProgram& pipeline,
                                  bool load_color, bool clear_depth, float depth_clear_value = 1.0F) {
        IndexedStaticMeshDrawRequest request =
            upload.upload->make_request(pipeline, *indexed_camera.frame);
        request.depth_attachment = depth_result.attachment.get();
        request.load_color = load_color;
        request.clear_depth = clear_depth;
        request.depth_clear_value = depth_clear_value;
        return request;
    };
    auto make_batch_request = [&](const StaticMeshUploadResult& upload, const PipelineProgram& pipeline) {
        // Batch attachment/load/clear state belongs to the description. Keep
        // every request at the neutral defaults required by the contract.
        return upload.upload->make_request(pipeline, *indexed_camera.frame);
    };
    auto require_pixel = [&](const std::vector<std::byte>& rgba8, std::size_t x, std::size_t y,
                             std::byte red_value, std::byte green_value, std::byte blue_value,
                             std::string_view message) {
        require(rgba8.size() == 32U * 32U * 4U, "persistent depth readback size");
        const std::size_t offset = (y * 32U + x) * 4U;
        require(rgba8[offset] == red_value && rgba8[offset + 1U] == green_value &&
                    rgba8[offset + 2U] == blue_value && rgba8[offset + 3U] == std::byte{255},
                message);
    };

    // A second draw must load the first draw's color while its depth test is
    // independent at a different pixel location.
    IndexedStaticMeshDrawRequest left_request =
        make_depth_request(left_upload, depth_red_pipeline, false, true);
    TextureResult uninitialized_color = device.device->create_texture(triangle_description);
    require(uninitialized_color.ok(), "uninitialized color-load target creation");
    IndexedStaticMeshDrawRequest invalid_color_load = left_request;
    invalid_color_load.load_color = true;
    const IndexedStaticMeshDrawResult invalid_color_load_result =
        device.device->draw_indexed_static_mesh_and_readback(*uninitialized_color.texture,
                                                              invalid_color_load);
    require(invalid_color_load_result.status == IndexedStaticMeshDrawStatus::execution_failed &&
                invalid_color_load_result.diagnostic.code == "indexed_color_load_before_clear",
            "color load before initialization rejected");

    DepthAttachmentResult uninitialized_depth =
        device.device->create_depth_attachment(depth_description);
    require(uninitialized_depth.ok(), "uninitialized depth-load attachment creation");
    IndexedStaticMeshDrawRequest invalid_depth_load = left_request;
    invalid_depth_load.depth_attachment = uninitialized_depth.attachment.get();
    invalid_depth_load.clear_depth = false;
    const IndexedStaticMeshDrawResult invalid_depth_load_result =
        device.device->draw_indexed_static_mesh_and_readback(*uninitialized_color.texture,
                                                              invalid_depth_load);
    require(invalid_depth_load_result.status == IndexedStaticMeshDrawStatus::execution_failed &&
                invalid_depth_load_result.diagnostic.code == "indexed_depth_load_before_clear",
            "depth load before initialization rejected");

    const IndexedStaticMeshDrawResult left_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, left_request);
    require(left_result.ok(), "depth color-load first draw");
    IndexedStaticMeshDrawRequest right_request =
        make_depth_request(right_upload, depth_blue_pipeline, true, false);
    const IndexedStaticMeshDrawResult right_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, right_request);
    require(right_result.ok(), "depth color-load second draw");
    require_pixel(right_result.rgba8, 8U, 16U, std::byte{255}, std::byte{0}, std::byte{0},
                  "load_color preserves the first draw's red pixels");
    require_pixel(right_result.rgba8, 24U, 16U, std::byte{0}, std::byte{0}, std::byte{255},
                  "load_color records the second draw's blue pixels");

    DrawPacket near_packet = make_depth_packet(0.0F, 0.5F, true);
    DrawPacket far_packet = make_depth_packet(0.0F, -0.5F, true);
    StaticMeshUploadResult near_upload = make_depth_upload(near_packet);
    StaticMeshUploadResult far_upload = make_depth_upload(far_packet);

    // Far first, then near: LESS must allow the nearer red triangle to
    // replace the farther blue triangle.
    IndexedStaticMeshDrawRequest far_first_request =
        make_depth_request(far_upload, depth_blue_pipeline, false, true);
    const IndexedStaticMeshDrawResult far_first_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, far_first_request);
    require(far_first_result.ok(), "depth far-first draw");
    IndexedStaticMeshDrawRequest near_second_request =
        make_depth_request(near_upload, depth_red_pipeline, true, false);
    const IndexedStaticMeshDrawResult near_second_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, near_second_request);
    require(near_second_result.ok(), "depth near-second draw");
    require_pixel(near_second_result.rgba8, 16U, 16U, std::byte{255}, std::byte{0}, std::byte{0},
                  "LESS depth accepts the nearer second draw");

    // A new frame must clear the persistent depth attachment. Without that
    // clear, this farther triangle would fail against the preceding near
    // triangle even though the color attachment is reset.
    IndexedStaticMeshDrawRequest cleared_far_request =
        make_depth_request(far_upload, depth_blue_pipeline, false, true);
    const IndexedStaticMeshDrawResult cleared_far_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, cleared_far_request);
    require(cleared_far_result.ok(), "new-frame depth clear draw");
    require_pixel(cleared_far_result.rgba8, 16U, 16U, std::byte{0}, std::byte{0}, std::byte{255},
                  "new-frame depth clear admits the farther draw");

    // Near first, then far: the farther draw must fail LESS against the
    // nearer depth, while the color attachment remains loaded.
    IndexedStaticMeshDrawRequest near_first_request =
        make_depth_request(near_upload, depth_red_pipeline, false, true);
    const IndexedStaticMeshDrawResult near_first_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, near_first_request);
    require(near_first_result.ok(), "depth near-first draw");
    IndexedStaticMeshDrawRequest far_second_request =
        make_depth_request(far_upload, depth_blue_pipeline, true, false);
    const IndexedStaticMeshDrawResult far_second_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, far_second_request);
    require(far_second_result.ok(), "depth far-second draw");
    require_pixel(far_second_result.rgba8, 16U, 16U, std::byte{255}, std::byte{0}, std::byte{0},
                  "LESS depth rejects the farther second draw");

    // With depth writes disabled, both draws pass against the cleared depth
    // value; the later far draw therefore replaces the earlier near color.
    PipelineProgram depth_no_write_red_pipeline = make_depth_pipeline(false);
    PipelineProgram depth_no_write_blue_pipeline = make_depth_pipeline(true);
    depth_no_write_red_pipeline.name = "indexed-depth-no-write-red";
    depth_no_write_blue_pipeline.name = "indexed-depth-no-write-blue";
    depth_no_write_red_pipeline.depth.write_enabled = false;
    depth_no_write_blue_pipeline.depth.write_enabled = false;
    DrawPacket no_write_near_packet = make_depth_packet(0.0F, 0.5F, false);
    DrawPacket no_write_far_packet = make_depth_packet(0.0F, -0.5F, false);
    StaticMeshUploadResult no_write_near_upload = make_depth_upload(no_write_near_packet);
    StaticMeshUploadResult no_write_far_upload = make_depth_upload(no_write_far_packet);
    IndexedStaticMeshDrawRequest no_write_near_request =
        make_depth_request(no_write_near_upload, depth_no_write_red_pipeline, false, true);
    const IndexedStaticMeshDrawResult no_write_near_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, no_write_near_request);
    require(no_write_near_result.ok(), "depth-write-disabled near draw");
    IndexedStaticMeshDrawRequest no_write_far_request =
        make_depth_request(no_write_far_upload, depth_no_write_blue_pipeline, true, false);
    const IndexedStaticMeshDrawResult no_write_far_result =
        device.device->draw_indexed_static_mesh_and_readback(*triangle_texture.texture, no_write_far_request);
    require(no_write_far_result.ok(), "depth-write-disabled far draw");
    require_pixel(no_write_far_result.rgba8, 16U, 16U, std::byte{0}, std::byte{0}, std::byte{255},
                  "disabled depth writes allow the later far draw");

    // Ordered batches execute both translated draws in one render pass. The
    // triangles overlap at the center: red is submitted first and blue
    // second, so the center must show the later request while side samples
    // prove that both draws survived the batch readback.
    PipelineProgram batch_order_red_pipeline = depth_red_pipeline;
    PipelineProgram batch_order_blue_pipeline = depth_blue_pipeline;
    batch_order_red_pipeline.name = "indexed-batch-order-red";
    batch_order_blue_pipeline.name = "indexed-batch-order-blue";
    batch_order_red_pipeline.targets.has_depth = false;
    batch_order_blue_pipeline.targets.has_depth = false;
    batch_order_red_pipeline.depth.test_enabled = false;
    batch_order_red_pipeline.depth.write_enabled = false;
    batch_order_blue_pipeline.depth.test_enabled = false;
    batch_order_blue_pipeline.depth.write_enabled = false;
    DrawPacket batch_left_packet = make_depth_packet(-1.1F, 0.0F, false);
    DrawPacket batch_right_packet = make_depth_packet(1.1F, 0.0F, false);
    batch_left_packet.flags.depth_test = false;
    batch_right_packet.flags.depth_test = false;
    StaticMeshUploadResult batch_left_upload = make_depth_upload(batch_left_packet);
    StaticMeshUploadResult batch_right_upload = make_depth_upload(batch_right_packet);
    std::array<IndexedStaticMeshDrawRequest, 2> ordered_batch_draws = {
        make_batch_request(batch_left_upload, batch_order_red_pipeline),
        make_batch_request(batch_right_upload, batch_order_blue_pipeline),
    };
    IndexedStaticMeshBatchDescription ordered_batch_description;
    ordered_batch_description.draws = std::span<const IndexedStaticMeshDrawRequest>(ordered_batch_draws);
    ordered_batch_description.load_color = false;
    ordered_batch_description.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    ordered_batch_description.clear_depth = false;
    IndexedStaticMeshBatchDescription invalid_batch_color_load = ordered_batch_description;
    invalid_batch_color_load.load_color = true;
    const IndexedStaticMeshBatchResult invalid_batch_color_load_result =
        device.device->draw_indexed_static_mesh_batch_and_readback(*uninitialized_color.texture,
                                                                    invalid_batch_color_load);
    require(invalid_batch_color_load_result.status == IndexedStaticMeshBatchStatus::execution_failed &&
                invalid_batch_color_load_result.diagnostic.code == "indexed_color_load_before_clear",
            "batch color load before initialization rejected");
    const IndexedStaticMeshBatchResult ordered_batch_result =
        device.device->draw_indexed_static_mesh_batch_and_readback(*triangle_texture.texture,
                                                                    ordered_batch_description);
    require(ordered_batch_result.ok() && ordered_batch_result.rgba8.size() == 32U * 32U * 4U,
            "ordered indexed batch draw/readback");
    require_pixel(ordered_batch_result.rgba8, 8U, 16U, std::byte{255}, std::byte{0}, std::byte{0},
                  "ordered batch preserves the first translated red draw");
    require_pixel(ordered_batch_result.rgba8, 24U, 16U, std::byte{0}, std::byte{0}, std::byte{255},
                  "ordered batch preserves the second translated blue draw");
    require(ordered_batch_draws[0].depth_attachment == nullptr && !ordered_batch_draws[0].load_color &&
                !ordered_batch_draws[0].clear_depth && ordered_batch_draws[1].depth_attachment == nullptr &&
                !ordered_batch_draws[1].load_color && !ordered_batch_draws[1].clear_depth &&
                ordered_batch_draws[0].clear_color == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F} &&
                ordered_batch_draws[1].clear_color == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F},
            "ordered batch leaves caller requests attachment-neutral");

    // Reuse one immutable upload for two requests at the same position. The
    // later blue pipeline must replace red, making request order observable.
    DrawPacket batch_overlap_packet = make_depth_packet(0.0F, 0.0F, false);
    batch_overlap_packet.flags.depth_test = false;
    StaticMeshUploadResult batch_overlap_upload = make_depth_upload(batch_overlap_packet);
    std::array<IndexedStaticMeshDrawRequest, 2> overlap_batch_draws = {
        make_batch_request(batch_overlap_upload, batch_order_red_pipeline),
        make_batch_request(batch_overlap_upload, batch_order_blue_pipeline),
    };
    IndexedStaticMeshBatchDescription overlap_batch_description = ordered_batch_description;
    overlap_batch_description.draws = std::span<const IndexedStaticMeshDrawRequest>(overlap_batch_draws);
    const IndexedStaticMeshBatchResult overlap_batch_result =
        device.device->draw_indexed_static_mesh_batch_and_readback(*triangle_texture.texture,
                                                                    overlap_batch_description);
    require(overlap_batch_result.ok(), "overlapping ordered indexed batch draw/readback");
    require_pixel(overlap_batch_result.rgba8, 16U, 16U, std::byte{0}, std::byte{0}, std::byte{255},
                  "ordered batch preserves request order at overlap");

    // A single batch clear must apply to the first draw only. Near red is
    // submitted before far blue; if the implementation cleared depth again
    // for the second request, blue would incorrectly replace red under LESS.
    std::array<IndexedStaticMeshDrawRequest, 2> depth_batch_draws = {
        make_batch_request(near_upload, depth_red_pipeline),
        make_batch_request(far_upload, depth_blue_pipeline),
    };
    IndexedStaticMeshBatchDescription depth_batch_description;
    depth_batch_description.draws = std::span<const IndexedStaticMeshDrawRequest>(depth_batch_draws);
    depth_batch_description.depth_attachment = depth_result.attachment.get();
    depth_batch_description.load_color = false;
    depth_batch_description.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    depth_batch_description.clear_depth = true;
    depth_batch_description.depth_clear_value = 1.0F;
    IndexedStaticMeshBatchDescription invalid_batch_depth_load = depth_batch_description;
    invalid_batch_depth_load.depth_attachment = uninitialized_depth.attachment.get();
    invalid_batch_depth_load.clear_depth = false;
    const IndexedStaticMeshBatchResult invalid_batch_depth_load_result =
        device.device->draw_indexed_static_mesh_batch_and_readback(*triangle_texture.texture,
                                                                    invalid_batch_depth_load);
    require(invalid_batch_depth_load_result.status == IndexedStaticMeshBatchStatus::execution_failed &&
                invalid_batch_depth_load_result.diagnostic.code == "indexed_depth_load_before_clear",
            "batch depth load before initialization rejected");
    const IndexedStaticMeshBatchResult depth_batch_result =
        device.device->draw_indexed_static_mesh_batch_and_readback(*triangle_texture.texture,
                                                                    depth_batch_description);
    require(depth_batch_result.ok() && depth_batch_result.rgba8.size() == 32U * 32U * 4U,
            "depth indexed batch draw/readback");
    require_pixel(depth_batch_result.rgba8, 16U, 16U, std::byte{255}, std::byte{0}, std::byte{0},
                  "depth batch loads depth between ordered requests");
    require(depth_batch_draws[0].depth_attachment == nullptr && !depth_batch_draws[0].load_color &&
                !depth_batch_draws[0].clear_depth && depth_batch_draws[1].depth_attachment == nullptr &&
                !depth_batch_draws[1].load_color && !depth_batch_draws[1].clear_depth,
            "depth batch applies attachment and clear state without mutating requests");

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
        contract_triangle_draw_limits();
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
