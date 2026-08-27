#include "apex/render/device.hpp"
#include "../src/render/half_float.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <stdexcept>
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
    explicit MinimalDevice(Backend backend)
        : info_{backend, "minimal", "luminance-test", 1U, 0U, 0U, 0U, 0U,
                true} {}

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
        return {TextureReadbackStatus::unsupported, {"minimal", "unused"},
                {}};
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
        return {ShaderModuleStatus::unsupported, {"minimal", "unused"},
                nullptr};
    }
    void wait_idle() noexcept override {}

private:
    DeviceInfo info_{};
};

TextureDescription luminance_description() {
    TextureDescription description;
    description.width = 4U;
    description.height = 2U;
    description.mip_levels = 3U;
    description.array_layers = 1U;
    description.format = TextureFormat::rgba16_sfloat;
    description.usage = TextureUsage::sampled |
                        TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    description.mutability = TextureMutability::mutable_data;
    description.samples = 1U;
    description.shape = TextureShape::texture_2d;
    description.access_policy = TextureAccessPolicy::render_then_sample;
    return description;
}

TextureDescription tone_map_destination_description() {
    TextureDescription description;
    description.width = 4U;
    description.height = 2U;
    description.mip_levels = 1U;
    description.array_layers = 1U;
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    description.mutability = TextureMutability::mutable_data;
    description.samples = 1U;
    description.shape = TextureShape::texture_2d;
    description.access_policy = TextureAccessPolicy::fixed_usage;
    return description;
}

TextureDescription hdr_batch_target_description(std::uint32_t samples,
                                               std::uint32_t mip_levels) {
    TextureDescription description = luminance_description();
    description.samples = samples;
    description.mip_levels = mip_levels;
    if (samples != 1U) {
        description.usage = TextureUsage::color_attachment |
                           TextureUsage::transfer_source;
        description.access_policy = TextureAccessPolicy::fixed_usage;
    }
    return description;
}

void accepts_exact_full_chain() {
    TestTexture source(luminance_description(), Backend::Vulkan);
    Diagnostic diagnostic;
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::ready &&
                diagnostic.code.empty() && diagnostic.message.empty(),
            "RGBA16F full mip chain is a valid luminance source");
    require(std::string_view(hdr_luminance_status_name(
                HdrLuminanceStatus::ready)) == "ready",
            "HDR luminance status names are exposed");
}

void rejects_shape_and_mip_contracts() {
    auto description = luminance_description();
    TestTexture source(description, Backend::Vulkan);
    Diagnostic diagnostic;

    description.width = 0U;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_dimensions_invalid",
            "zero luminance dimensions are rejected");

    description = luminance_description();
    description.width = max_texture_dimension + 1U;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_dimension_limit",
            "oversized luminance dimensions are rejected");

    description = luminance_description();
    description.mip_levels = 2U;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_mip_chain_invalid",
            "incomplete luminance mip chains are rejected");

    description = luminance_description();
    description.mip_levels = 4U;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_mip_chain_invalid",
            "excessive luminance mip chains are rejected");

    description = luminance_description();
    description.shape = TextureShape::texture_cube;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_source_shape_invalid",
            "cube luminance sources are rejected");

    description = luminance_description();
    description.array_layers = 2U;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_source_shape_invalid",
            "array luminance sources are rejected");

    description = luminance_description();
    description.samples = 4U;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_source_shape_invalid",
            "multisample luminance sources are rejected");
}

void rejects_format_usage_and_state_contracts() {
    auto description = luminance_description();
    TestTexture source(description, Backend::Vulkan);
    Diagnostic diagnostic;

    description.format = TextureFormat::rgba8_unorm;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::unsupported &&
                diagnostic.code == "hdr_luminance_source_format_unsupported",
            "non-HDR luminance sources are unsupported");

    description = luminance_description();
    description.usage = TextureUsage::sampled;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_source_usage_invalid",
            "unsuitable luminance source usage is rejected");

    description = luminance_description();
    description.mutability = TextureMutability::immutable;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_source_mutability_invalid",
            "immutable luminance sources are rejected");

    description = luminance_description();
    description.access_policy = TextureAccessPolicy::fixed_usage;
    source = TestTexture(description, Backend::Vulkan);
    require(validate_hdr_luminance_request(source, diagnostic) ==
                HdrLuminanceStatus::invalid_request &&
                diagnostic.code == "hdr_luminance_source_access_policy_invalid",
            "fixed-state luminance sources are rejected");
}

void tone_map_accepts_valid_source_mips_but_not_invalid_mips() {
    TestTexture source(luminance_description(), Backend::Vulkan);
    TestTexture destination(tone_map_destination_description(), Backend::Vulkan);
    Diagnostic diagnostic;
    require(validate_hdr_tone_map_request(source, destination, {}, diagnostic) ==
                HdrToneMapStatus::ready,
            "tone mapping accepts a valid multi-mip HDR source");

    auto invalid_description = luminance_description();
    invalid_description.mip_levels = 4U;
    TestTexture invalid_source(invalid_description, Backend::Vulkan);
    require(validate_hdr_tone_map_request(invalid_source, destination, {},
                                          diagnostic) ==
                HdrToneMapStatus::invalid_request &&
                diagnostic.code == "hdr_tone_map_source_shape_invalid",
            "tone mapping rejects an invalid HDR source mip chain");
}

void batch_accepts_retained_hdr_mip_chains_only() {
    TestTexture hdr_target(hdr_batch_target_description(1U, 3U),
                           Backend::Vulkan);
    IndexedStaticMeshBatchDescription batch;
    batch.capture_rgba8 = false;
    Diagnostic diagnostic;
    require(validate_indexed_static_mesh_batch_description(
                hdr_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "retained single-sample HDR target accepts a valid mip chain");

    auto invalid_hdr_target_description =
        hdr_batch_target_description(1U, 4U);
    TestTexture invalid_hdr_target(invalid_hdr_target_description,
                                   Backend::Vulkan);
    require(validate_indexed_static_mesh_batch_description(
                invalid_hdr_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_invalid",
            "retained HDR target rejects an excessive mip chain");

    TestTexture ldr_target(tone_map_destination_description(),
                           Backend::Vulkan);
    auto invalid_ldr = tone_map_destination_description();
    invalid_ldr.mip_levels = 2U;
    TestTexture invalid_ldr_target(invalid_ldr, Backend::Vulkan);
    require(validate_indexed_static_mesh_batch_description(
                invalid_ldr_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_invalid",
            "retained LDR target remains one-mip");

    auto multisample_description = hdr_batch_target_description(4U, 1U);
    TestTexture multisample_target(multisample_description, Backend::Vulkan);
    auto resolve_description = hdr_batch_target_description(1U, 3U);
    TestTexture resolve_target(resolve_description, Backend::Vulkan);
    batch.resolve_target = &resolve_target;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "retained single-sample HDR resolve accepts a valid mip chain");

    auto invalid_multisample_description =
        hdr_batch_target_description(4U, 3U);
    TestTexture invalid_multisample_target(invalid_multisample_description,
                                           Backend::Vulkan);
    require(validate_indexed_static_mesh_batch_description(
                invalid_multisample_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "indexed_static_mesh_target_invalid",
            "retained multisample HDR source remains one-mip");

    auto invalid_resolve_description = resolve_description;
    invalid_resolve_description.mip_levels = 4U;
    TestTexture invalid_resolve(invalid_resolve_description, Backend::Vulkan);
    batch.resolve_target = &invalid_resolve;
    require(validate_indexed_static_mesh_batch_description(
                multisample_target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code ==
                    "indexed_static_mesh_batch_resolve_description_mismatch",
            "retained HDR resolve rejects an excessive mip chain");

    (void)ldr_target;
}

void default_device_boundary_is_explicit() {
    MinimalDevice device(Backend::Vulkan);
    TestTexture source(luminance_description(), Backend::Vulkan);
    const HdrLuminanceResult result = device.measure_hdr_luminance(source);
    require(!result.ok() && result.status == HdrLuminanceStatus::unsupported &&
                result.luminance == 0.0F &&
                result.diagnostic.code == "hdr_luminance_unsupported",
            "neutral Device reports luminance measurement unsupported");

    auto invalid_description = luminance_description();
    invalid_description.mip_levels = 2U;
    TestTexture invalid_source(invalid_description, Backend::Vulkan);
    const HdrLuminanceResult invalid =
        device.measure_hdr_luminance(invalid_source);
    require(!invalid.ok() &&
                invalid.status == HdrLuminanceStatus::invalid_request &&
                invalid.diagnostic.code == "hdr_luminance_mip_chain_invalid",
            "neutral Device validates luminance requests before dispatch");
}

void exposure_mode_values_are_stable() {
    require(static_cast<std::uint8_t>(HdrExposureMode::manual) == 0U &&
                static_cast<std::uint8_t>(HdrExposureMode::automatic) == 1U,
            "HDR exposure mode values are stable");
}

void decodes_backend_half_float_values() {
    using apex::render::detail::half_to_float;
    require(half_to_float(0x0000U) == 0.0F &&
                std::signbit(half_to_float(0x8000U)) &&
                half_to_float(0x3c00U) == 1.0F &&
                half_to_float(0x4000U) == 2.0F &&
                half_to_float(0x0001U) > 0.0F &&
                std::isinf(half_to_float(0x7c00U)) &&
                std::isnan(half_to_float(0x7e00U)),
            "backend half-float conversion handles finite and special values");
}

}  // namespace

int main() {
    try {
        accepts_exact_full_chain();
        rejects_shape_and_mip_contracts();
        rejects_format_usage_and_state_contracts();
        tone_map_accepts_valid_source_mips_but_not_invalid_mips();
        batch_accepts_retained_hdr_mip_chains_only();
        default_device_boundary_is_explicit();
        exposure_mode_values_are_stable();
        decodes_backend_half_float_values();
        std::cout << "hdr luminance contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hdr luminance contract tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
