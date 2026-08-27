#include "apex/render/device.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace apex::render;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TestTexture final : public Texture {
public:
    TestTexture(TextureDescription description, Backend backend)
        : backend_(backend), info_({description}) {}

    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }

private:
    Backend backend_ = Backend::Vulkan;
    TextureInfo info_{};
};

class MinimalDevice final : public Device {
public:
    explicit MinimalDevice(Backend backend) : info_{backend, "minimal", "test", 1U, 0U,
                                                    0U, 0U, 0U, true} {}

    const DeviceInfo& info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription&,
                               std::span<const std::byte>) override {
        return {BufferStatus::unsupported, {"minimal", "unused"}, nullptr};
    }

    BufferUpdateResult update_buffer(Buffer&, std::uint64_t,
                                      std::span<const std::byte>) override {
        return {BufferStatus::unsupported, {"minimal", "unused"}};
    }

    TextureResult create_texture(const TextureDescription&,
                                 const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"minimal", "unused"}, nullptr};
    }

    TextureUpdateResult update_texture(Texture&,
                                       const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"minimal", "unused"}};
    }

    TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"minimal", "unused"}, {}};
    }

    TriangleDrawResult draw_triangle_and_readback(
        Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"minimal", "unused"}, {}};
    }

    SamplerResult create_sampler(const SamplerDescription&) override {
        return {SamplerStatus::unsupported, {"minimal", "unused"}, nullptr};
    }

    ShaderModuleResult create_shader_module(
        const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"minimal", "unused"}, nullptr};
    }

    void wait_idle() noexcept override {}

private:
    DeviceInfo info_{};
};

TextureDescription source_description() {
    TextureDescription description;
    description.width = 16U;
    description.height = 8U;
    description.format = TextureFormat::rgba16_sfloat;
    description.usage = TextureUsage::sampled;
    description.mutability = TextureMutability::immutable;
    return description;
}

TextureDescription destination_description() {
    TextureDescription description;
    description.width = 16U;
    description.height = 8U;
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    description.mutability = TextureMutability::mutable_data;
    return description;
}

HdrToneMapStatus validate(const TextureDescription& source_description_value,
                          const TextureDescription& destination_description_value,
                          HdrToneMapParameters parameters = {},
                          Backend source_backend = Backend::Vulkan,
                          Backend destination_backend = Backend::Vulkan,
                          Diagnostic* output = nullptr) {
    TestTexture source(source_description_value, source_backend);
    TestTexture destination(destination_description_value, destination_backend);
    Diagnostic diagnostic;
    const HdrToneMapStatus status = validate_hdr_tone_map_request(
        source, destination, parameters, diagnostic);
    if (output != nullptr) *output = std::move(diagnostic);
    return status;
}

void accepts_valid_request() {
    Diagnostic diagnostic;
    require(validate(source_description(), destination_description(), {},
                     Backend::Vulkan, Backend::Vulkan, &diagnostic) ==
                HdrToneMapStatus::ready &&
                diagnostic.code.empty() && diagnostic.message.empty(),
            "valid HDR tone-map request");
}

void rejects_shape_and_dimension_requests() {
    TextureDescription source = source_description();
    TextureDescription destination = destination_description();

    source.width = 0U;
    require(validate(source, destination) == HdrToneMapStatus::invalid_request,
            "zero HDR source dimensions are rejected");
    source = source_description();

    source.shape = TextureShape::texture_cube;
    require(validate(source, destination) == HdrToneMapStatus::invalid_request,
            "cube HDR source is rejected");
    source = source_description();
    source.array_layers = 2U;
    require(validate(source, destination) == HdrToneMapStatus::invalid_request,
            "array HDR source is rejected");
    source = source_description();
    source.mip_levels = 2U;
    require(validate(source, destination) == HdrToneMapStatus::invalid_request,
            "mipped HDR source is rejected");
    source = source_description();
    source.samples = 4U;
    require(validate(source, destination) == HdrToneMapStatus::invalid_request,
            "multisample HDR source is rejected");

    destination.shape = TextureShape::texture_cube;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "cube display destination is rejected");
    destination = destination_description();
    destination.array_layers = 2U;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "array display destination is rejected");
    destination = destination_description();
    destination.mip_levels = 2U;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "mipped display destination is rejected");
    destination = destination_description();
    destination.samples = 4U;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "multisample display destination is rejected");

    destination = destination_description();
    destination.width += 1U;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "mismatched HDR dimensions are rejected");
}

void rejects_format_usage_and_mutability_requests() {
    TextureDescription source = source_description();
    TextureDescription destination = destination_description();

    source.format = TextureFormat::rgba8_unorm;
    require(validate(source, destination) == HdrToneMapStatus::unsupported,
            "non-HDR source format is rejected");
    source = source_description();
    source.usage = TextureUsage::transfer_source;
    require(validate(source, destination) == HdrToneMapStatus::invalid_request,
            "unsampled HDR source is rejected");

    destination.format = TextureFormat::r32_sfloat;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::unsupported,
            "non-display destination format is rejected");
    destination = destination_description();
    destination.usage = TextureUsage::color_attachment;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "non-readable display destination is rejected");
    destination.usage = TextureUsage::transfer_source;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "non-renderable display destination is rejected");
    destination = destination_description();
    destination.mutability = TextureMutability::immutable;
    require(validate(source_description(), destination) ==
                HdrToneMapStatus::invalid_request,
            "immutable display destination is rejected");
}

void rejects_parameters_backend_and_aliasing() {
    const TextureDescription source = source_description();
    const TextureDescription destination = destination_description();

    HdrToneMapParameters parameters;
    parameters.exposure = -1.0F;
    require(validate(source, destination, parameters) ==
                HdrToneMapStatus::invalid_request,
            "negative exposure is rejected");
    parameters = {};
    parameters.gamma = 0.0F;
    require(validate(source, destination, parameters) ==
                HdrToneMapStatus::invalid_request,
            "nonpositive gamma is rejected");
    parameters = {};
    parameters.saturation = -1.0F;
    require(validate(source, destination, parameters) ==
                HdrToneMapStatus::invalid_request,
            "negative saturation is rejected");
    parameters = {};
    parameters.curve_scale = -1.0F;
    require(validate(source, destination, parameters) ==
                HdrToneMapStatus::invalid_request,
            "negative curve scale is rejected");
    parameters = {};
    parameters.curve_shoulder = std::numeric_limits<float>::quiet_NaN();
    require(validate(source, destination, parameters) ==
                HdrToneMapStatus::invalid_request,
            "nonfinite curve shoulder is rejected");
    parameters = {};
    parameters.dither_scale = -1.0F;
    require(validate(source, destination, parameters) ==
                HdrToneMapStatus::invalid_request,
            "negative dither scale is rejected");

    require(validate(source, destination, {}, Backend::Vulkan, Backend::D3D12) ==
                HdrToneMapStatus::unsupported,
            "cross-backend HDR tone mapping is rejected");

    TestTexture aliased(source, Backend::Vulkan);
    Diagnostic diagnostic;
    require(validate_hdr_tone_map_request(aliased, aliased, {}, diagnostic) ==
                HdrToneMapStatus::invalid_request &&
                diagnostic.code == "hdr_tone_map_alias_invalid",
            "aliased HDR tone-map textures are rejected");
}

void status_names_are_stable() {
    require(std::string_view(hdr_tone_map_status_name(HdrToneMapStatus::ready)) ==
                "ready" &&
                std::string_view(hdr_tone_map_status_name(
                    HdrToneMapStatus::invalid_request)) == "invalid_request" &&
                std::string_view(hdr_tone_map_status_name(
                    HdrToneMapStatus::unsupported)) == "unsupported" &&
                std::string_view(hdr_tone_map_status_name(
                    HdrToneMapStatus::execution_failed)) == "execution_failed",
            "HDR tone-map status names");
}

void default_device_boundary_is_explicit() {
    MinimalDevice device(Backend::Vulkan);
    TestTexture source(source_description(), Backend::Vulkan);
    TestTexture destination(destination_description(), Backend::Vulkan);
    const HdrToneMapResult result =
        device.tone_map_hdr_texture(source, destination);
    require(!result.ok() && result.status == HdrToneMapStatus::unsupported &&
                result.diagnostic.code == "hdr_tone_map_unsupported",
            "neutral Device reports HDR tone-map execution unsupported");
}

bool executes_backend(const Backend backend, const DeviceOptions& options) {
    DeviceResult created = create_device(backend, options);
    if (!created.ok()) {
        std::cout << "SKIP "
                  << (backend == Backend::Vulkan ? "Vulkan" : "D3D12")
                  << ": " << created.diagnostic.code << ": "
                  << created.diagnostic.message << '\n';
        return false;
    }

    TextureDescription source = source_description();
    source.width = 2U;
    source.height = 2U;
    constexpr std::array<std::uint16_t, 16U> hdr_pixels = {
        0x0000U, 0x0000U, 0x0000U, 0x3c00U,
        0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U,
        0x4400U, 0x4000U, 0x3800U, 0x3c00U,
        0x3400U, 0x3800U, 0x3a00U, 0x3800U,
    };
    TextureUploadPlan upload;
    upload.subresources.push_back(
        {0U, 0U, 2U, 2U, 2U * 4U * sizeof(std::uint16_t),
         std::as_bytes(std::span(hdr_pixels))});
    TextureResult hdr = created.device->create_texture(source, upload);
    require(hdr.ok(), "backend creates initialized HDR tone-map source");

    TextureDescription output = destination_description();
    output.width = 2U;
    output.height = 2U;
    output.usage = TextureUsage::sampled | TextureUsage::color_attachment |
                   TextureUsage::transfer_source;
    output.access_policy = TextureAccessPolicy::render_then_sample;
    TextureResult display = created.device->create_texture(output);
    require(display.ok(), "backend creates HDR tone-map display target");

    HdrToneMapResult result = created.device->tone_map_hdr_texture(
        *hdr.texture, *display.texture);
    if (!result.ok())
        throw std::runtime_error("backend HDR tone map failed: " +
                                 result.diagnostic.code + ": " +
                                 result.diagnostic.message);
    result = created.device->tone_map_hdr_texture(*hdr.texture,
                                                   *display.texture);
    require(result.ok(), "backend repeats HDR tone mapping with tracked states");

    TextureResult empty_hdr = created.device->create_texture(source);
    require(empty_hdr.ok(), "backend creates empty HDR source");
    result = created.device->tone_map_hdr_texture(*empty_hdr.texture,
                                                   *display.texture);
    require(!result.ok() &&
                result.status == HdrToneMapStatus::invalid_request &&
                result.diagnostic.code == "hdr_tone_map_source_uninitialized",
            "backend rejects uninitialized HDR source");

    TextureDescription multisample_source = source;
    multisample_source.samples = 4U;
    multisample_source.usage = TextureUsage::color_attachment |
                               TextureUsage::transfer_source;
    multisample_source.mutability = TextureMutability::mutable_data;
    TextureResult multisample_hdr =
        created.device->create_texture(multisample_source);
    require(multisample_hdr.ok(),
            "backend creates four-sample RGBA16F scene target");

    TextureDescription resolved_source = source;
    resolved_source.usage = TextureUsage::sampled |
                            TextureUsage::color_attachment |
                            TextureUsage::transfer_source;
    resolved_source.mutability = TextureMutability::mutable_data;
    resolved_source.access_policy = TextureAccessPolicy::render_then_sample;
    TextureResult resolved_hdr = created.device->create_texture(resolved_source);
    require(resolved_hdr.ok(),
            "backend creates sampled RGBA16F resolve target");

    IndexedStaticMeshBatchDescription batch;
    batch.clear_color = {4.0F, 2.0F, 1.0F, 1.0F};
    batch.resolve_target = resolved_hdr.texture.get();
    batch.capture_rgba8 = false;
    const IndexedStaticMeshBatchResult resolved =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *multisample_hdr.texture, batch);
    if (!resolved.ok())
        throw std::runtime_error("backend HDR resolve failed: " +
                                 resolved.diagnostic.code + ": " +
                                 resolved.diagnostic.message);
    result = created.device->tone_map_hdr_texture(
        *resolved_hdr.texture, *display.texture);
    if (!result.ok())
        throw std::runtime_error("resolved HDR tone map failed: " +
                                 result.diagnostic.code + ": " +
                                 result.diagnostic.message);
    require(created.device->tone_map_hdr_texture(
                *resolved_hdr.texture, *display.texture).ok(),
            "backend repeats resolved HDR tone mapping with tracked states");

    TextureDescription dither_source = source_description();
    dither_source.width = 4U;
    dither_source.height = 4U;
    constexpr std::uint16_t dither_source_value = 0x331dU;
    std::array<std::uint16_t, 64U> dither_pixels{};
    for (std::size_t index = 0U; index < dither_pixels.size(); index += 4U) {
        dither_pixels[index] = dither_source_value;
        dither_pixels[index + 1U] = dither_source_value;
        dither_pixels[index + 2U] = dither_source_value;
        dither_pixels[index + 3U] = 0x3c00U;
    }
    TextureUploadPlan dither_upload;
    dither_upload.subresources.push_back(
        {0U, 0U, 4U, 4U, 4U * 4U * sizeof(std::uint16_t),
         std::as_bytes(std::span(dither_pixels))});
    TextureResult dither_hdr =
        created.device->create_texture(dither_source, dither_upload);
    require(dither_hdr.ok(), "backend creates uniform HDR dither fixture");

    TextureDescription dither_output = destination_description();
    dither_output.width = 4U;
    dither_output.height = 4U;
    dither_output.usage = TextureUsage::sampled | TextureUsage::color_attachment |
                          TextureUsage::transfer_source;
    dither_output.access_policy = TextureAccessPolicy::render_then_sample;
    TextureResult dither_display = created.device->create_texture(dither_output);
    require(dither_display.ok(), "backend creates dither display target");
    result = created.device->tone_map_hdr_texture(
        *dither_hdr.texture, *dither_display.texture);
    require(result.ok(), "backend executes recovered fixed-phase dither");

    IndexedStaticMeshBatchDescription readback;
    readback.load_color = true;
    readback.capture_rgba8 = true;
    const IndexedStaticMeshBatchResult captured =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *dither_display.texture, readback);
    require(captured.ok(), "backend reads the retained dither target");
    constexpr std::array<std::uint8_t, 16U> expected_luma = {
        60U, 61U, 60U, 61U, 61U, 60U, 61U, 60U,
        60U, 61U, 60U, 61U, 61U, 60U, 61U, 60U};
    require(captured.rgba8.size() == expected_luma.size() * 4U,
            "dither readback has one RGBA8 pixel per source pixel");
    for (std::size_t index = 0U; index < expected_luma.size(); ++index) {
        const std::uint8_t expected = expected_luma[index];
        const std::size_t offset = index * 4U;
        require(std::to_integer<std::uint8_t>(captured.rgba8[offset]) == expected &&
                    std::to_integer<std::uint8_t>(
                        captured.rgba8[offset + 1U]) == expected &&
                    std::to_integer<std::uint8_t>(
                        captured.rgba8[offset + 2U]) == expected &&
                    std::to_integer<std::uint8_t>(captured.rgba8[offset + 3U]) == 255U,
                "backend matches the recovered 4x4 dither pattern");
    }

    HdrToneMapParameters undithered_parameters;
    undithered_parameters.dither_scale = 0.0F;
    result = created.device->tone_map_hdr_texture(
        *dither_hdr.texture, *dither_display.texture, undithered_parameters);
    require(result.ok(), "backend disables dither through its public parameter");
    const IndexedStaticMeshBatchResult undithered =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *dither_display.texture, readback);
    require(undithered.ok() && undithered.rgba8.size() == captured.rgba8.size(),
            "backend reads the undithered target");
    const std::uint8_t uniform =
        std::to_integer<std::uint8_t>(undithered.rgba8[0U]);
    for (std::size_t index = 0U; index < expected_luma.size(); ++index) {
        const std::size_t offset = index * 4U;
        require(std::to_integer<std::uint8_t>(undithered.rgba8[offset]) == uniform &&
                    std::to_integer<std::uint8_t>(
                        undithered.rgba8[offset + 1U]) == uniform &&
                    std::to_integer<std::uint8_t>(
                        undithered.rgba8[offset + 2U]) == uniform &&
                    std::to_integer<std::uint8_t>(
                        undithered.rgba8[offset + 3U]) == 255U,
                "undithered tone mapping remains spatially uniform");
    }
    return true;
}

}  // namespace

int main() {
    try {
        accepts_valid_request();
        rejects_shape_and_dimension_requests();
        rejects_format_usage_and_mutability_requests();
        rejects_parameters_backend_and_aliasing();
        status_names_are_stable();
        default_device_boundary_is_explicit();
        DeviceOptions vulkan_options;
        vulkan_options.enable_validation = true;
        bool executed = executes_backend(Backend::Vulkan, vulkan_options);
#if defined(_WIN32)
        DeviceOptions d3d12_options;
        d3d12_options.prefer_software = true;
        d3d12_options.allow_software = true;
        executed = executes_backend(Backend::D3D12, d3d12_options) || executed;
#endif
        if (!executed) return 77;
        std::cout << "hdr tone-map contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hdr tone-map contract tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
