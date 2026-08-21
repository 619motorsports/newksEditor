#include "apex/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
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

bool contract_backend(apex::render::Backend backend) {
    using namespace apex::render;
    DeviceOptions options{};
    options.enable_validation = std::getenv("APEX_RENDER_VALIDATION") != nullptr;
    const char* requested_adapter = std::getenv("APEX_D3D12_ADAPTER");
    if (backend == Backend::D3D12 && requested_adapter && std::string_view(requested_adapter) == "warp")
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
        const char* requested = std::getenv("APEX_RENDER_BACKEND");
        if (requested && std::string(requested) == "vulkan") {
            return contract_backend(apex::render::Backend::Vulkan) ? 0 : 77;
        }
        if (requested && std::string(requested) != "d3d12") {
            std::cerr << "Unknown APEX_RENDER_BACKEND value: " << requested << '\n';
            return 2;
        }
        if (requested) {
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
