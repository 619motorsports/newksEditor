#include "apex/render/device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace apex::render;

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class TestTexture final : public Texture {
public:
    TestTexture(const TextureDescription description, const Backend backend)
        : info_({description}), backend_(backend) {}

    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }

private:
    TextureInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

TextureDescription source_description() {
    TextureDescription description;
    description.width = 16U;
    description.height = 8U;
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::sampled |
                        TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    description.mutability = TextureMutability::mutable_data;
    description.access_policy = TextureAccessPolicy::render_then_sample;
    return description;
}

TextureDescription destination_description() {
    TextureDescription description = source_description();
    description.usage = TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    description.access_policy = TextureAccessPolicy::fixed_usage;
    return description;
}

FxaaStatus validate(const TextureDescription& source_description_value,
                    const TextureDescription& destination_description_value,
                    const Backend source_backend = Backend::Vulkan,
                    const Backend destination_backend = Backend::Vulkan,
                    Diagnostic* output = nullptr) {
    TestTexture source(source_description_value, source_backend);
    TestTexture destination(destination_description_value, destination_backend);
    Diagnostic diagnostic;
    const FxaaStatus status =
        validate_fxaa_request(source, destination, diagnostic);
    if (output != nullptr)
        *output = std::move(diagnostic);
    return status;
}

void accepts_valid_request() {
    Diagnostic diagnostic;
    require(validate(source_description(), destination_description(),
                     Backend::Vulkan, Backend::Vulkan, &diagnostic) ==
                    FxaaStatus::ready &&
                diagnostic.code.empty(),
            "valid FXAA request is accepted");
    require(std::string_view(fxaa_status_name(FxaaStatus::ready)) == "ready" &&
                std::string_view(fxaa_status_name(
                    FxaaStatus::invalid_request)) == "invalid_request" &&
                std::string_view(fxaa_status_name(FxaaStatus::unsupported)) ==
                    "unsupported" &&
                std::string_view(fxaa_status_name(
                    FxaaStatus::execution_failed)) == "execution_failed",
            "FXAA status names are stable");
}

void rejects_malformed_requests() {
    TextureDescription source = source_description();
    TextureDescription destination = destination_description();

    source.width = 0U;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "zero FXAA source dimensions are rejected");
    source = source_description();
    source.width = max_texture_dimension + 1U;
    destination.width = source.width;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "oversized FXAA dimensions are rejected");
    source = source_description();
    destination = destination_description();

    source.format = TextureFormat::rgba16_sfloat;
    require(validate(source, destination) == FxaaStatus::unsupported,
            "HDR source is rejected by the RGBA8 FXAA contract");
    source = source_description();
    source.shape = TextureShape::texture_cube;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "cube FXAA source is rejected");
    source = source_description();
    source.array_layers = 2U;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "array FXAA source is rejected");
    source = source_description();
    source.mip_levels = 2U;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "mipped FXAA source is rejected");
    source = source_description();
    source.samples = 4U;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "multisample FXAA source is rejected");
    source = source_description();
    source.usage = TextureUsage::color_attachment |
                   TextureUsage::transfer_source;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "unsampled FXAA source is rejected");
    source = source_description();
    source.access_policy = TextureAccessPolicy::fixed_usage;
    require(validate(source, destination) == FxaaStatus::invalid_request,
            "fixed-usage FXAA source is rejected");

    destination.format = TextureFormat::rgba16_sfloat;
    require(validate(source_description(), destination) ==
                FxaaStatus::unsupported,
            "float FXAA destination is rejected");
    destination = destination_description();
    destination.shape = TextureShape::texture_cube;
    require(validate(source_description(), destination) ==
                FxaaStatus::invalid_request,
            "cube FXAA destination is rejected");
    destination = destination_description();
    destination.mip_levels = 2U;
    require(validate(source_description(), destination) ==
                FxaaStatus::invalid_request,
            "mipped FXAA destination is rejected");
    destination = destination_description();
    destination.samples = 4U;
    require(validate(source_description(), destination) ==
                FxaaStatus::invalid_request,
            "multisample FXAA destination is rejected");
    destination = destination_description();
    destination.usage = TextureUsage::color_attachment;
    require(validate(source_description(), destination) ==
                FxaaStatus::invalid_request,
            "non-readable FXAA destination is rejected");
    destination = destination_description();
    destination.mutability = TextureMutability::immutable;
    require(validate(source_description(), destination) ==
                FxaaStatus::invalid_request,
            "immutable FXAA destination is rejected");

    destination = destination_description();
    destination.width = source_description().width + 1U;
    require(validate(source_description(), destination) ==
                FxaaStatus::invalid_request,
            "mismatched FXAA dimensions are rejected");

    Diagnostic diagnostic;
    require(validate(source_description(), destination_description(),
                     Backend::Vulkan, Backend::D3D12, &diagnostic) ==
                FxaaStatus::unsupported &&
                diagnostic.code == "fxaa_backend_mismatch",
            "cross-backend FXAA textures are rejected");
    TestTexture aliased(source_description(), Backend::Vulkan);
    require(validate_fxaa_request(aliased, aliased, diagnostic) ==
                FxaaStatus::invalid_request &&
                diagnostic.code == "fxaa_alias_invalid",
            "aliased FXAA textures are rejected");
}

std::vector<std::uint8_t> make_fixture(const bool edge) {
    constexpr std::uint32_t width = 16U;
    constexpr std::uint32_t height = 8U;
    std::vector<std::uint8_t> pixels(width * height * 4U, 255U);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            // A one-pixel diagonal stair-step exercises both directional
            // search and diagonal taps. A perfectly vertical step can remain
            // unchanged under the recovered edge-offset rule.
            const bool bright = edge ? x >= 4U + y : true;
            const std::uint8_t value = bright ? 220U : 32U;
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[offset] = value;
            pixels[offset + 1U] = value;
            pixels[offset + 2U] = value;
        }
    }
    return pixels;
}

TextureUploadPlan upload_plan(const std::vector<std::uint8_t>& pixels) {
    TextureUploadPlan upload;
    upload.subresources.push_back({
        0U, 0U, 16U, 8U, 16U * 4U,
        std::as_bytes(std::span<const std::uint8_t>(pixels))});
    return upload;
}

std::vector<std::uint8_t> read_rgba8(Device& device, Texture& texture) {
    IndexedStaticMeshBatchDescription request;
    request.load_color = true;
    request.capture_rgba8 = true;
    const IndexedStaticMeshBatchResult result =
        device.draw_indexed_static_mesh_batch_and_readback(texture, request);
    if (!result.ok())
        throw std::runtime_error("FXAA readback failed: " +
                                 result.diagnostic.code + ": " +
                                 result.diagnostic.message);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(result.rgba8.size());
    for (const std::byte value : result.rgba8)
        bytes.push_back(std::to_integer<std::uint8_t>(value));
    return bytes;
}

bool executes_backend(const Backend backend) {
    DeviceOptions options;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    DeviceResult created = create_device(backend, options);
    if (!created.ok()) {
        std::cout << "SKIP " << backend_name(backend) << ": "
                  << created.diagnostic.code << ": "
                  << created.diagnostic.message << '\n';
        return false;
    }
    Device& device = *created.device;

    const std::vector<std::uint8_t> flat_pixels = make_fixture(false);
    TextureDescription source_info = source_description();
    TextureResult source = device.create_texture(source_info,
                                                  upload_plan(flat_pixels));
    require(source.ok(), "backend creates the FXAA source fixture");
    TextureResult destination = device.create_texture(destination_description());
    require(destination.ok(), "backend creates the FXAA destination");
    const std::vector<std::uint8_t> source_before =
        read_rgba8(device, *source.texture);
    const FxaaResult flat_result =
        device.apply_fxaa(*source.texture, *destination.texture);
    if (!flat_result.ok())
        throw std::runtime_error("backend FXAA failed: " +
                                 flat_result.diagnostic.code + ": " +
                                 flat_result.diagnostic.message);
    const std::vector<std::uint8_t> flat_output =
        read_rgba8(device, *destination.texture);
    require(flat_output.size() == flat_pixels.size(),
            "FXAA flat readback has the expected size");
    for (std::size_t offset = 0U; offset < flat_output.size(); offset += 4U) {
        require(flat_output[offset] == 220U &&
                    flat_output[offset + 1U] == 220U &&
                    flat_output[offset + 2U] == 220U &&
                    flat_output[offset + 3U] == 255U,
                "FXAA early exit preserves a flat color and writes opaque alpha");
    }
    require(read_rgba8(device, *source.texture) == source_before,
            "FXAA preserves its source texture");

    for (const TextureFormat format : {
             TextureFormat::rgba8_srgb,
             TextureFormat::bgra8_unorm,
             TextureFormat::bgra8_srgb}) {
        TextureDescription format_description = destination_description();
        format_description.format = format;
        TextureResult format_destination =
            device.create_texture(format_description);
        require(format_destination.ok(),
                "backend creates each supported FXAA display format");
        require(device.apply_fxaa(*source.texture, *format_destination.texture)
                    .ok(),
                "backend applies FXAA to each supported display format");
        const std::vector<std::uint8_t> format_output =
            read_rgba8(device, *format_destination.texture);
        require(format_output.size() == flat_pixels.size(),
                "FXAA display-format readback has the expected size");
        for (std::size_t offset = 0U; offset < format_output.size();
             offset += 4U) {
            require(format_output[offset] == 220U &&
                        format_output[offset + 1U] == 220U &&
                        format_output[offset + 2U] == 220U &&
                        format_output[offset + 3U] == 255U,
                    "FXAA preserves encoded flat color across display formats");
        }
    }

    const std::vector<std::uint8_t> edge_pixels = make_fixture(true);
    TextureResult edge_source = device.create_texture(
        source_info, upload_plan(edge_pixels));
    require(edge_source.ok(), "backend creates the FXAA edge fixture");
    TextureResult edge_destination =
        device.create_texture(destination_description());
    require(edge_destination.ok(), "backend creates the FXAA edge destination");
    const std::vector<std::uint8_t> edge_before =
        read_rgba8(device, *edge_source.texture);
    const FxaaResult edge_result = device.apply_fxaa(
        *edge_source.texture, *edge_destination.texture);
    if (!edge_result.ok())
        throw std::runtime_error("backend FXAA edge failed: " +
                                 edge_result.diagnostic.code + ": " +
                                 edge_result.diagnostic.message);
    const std::vector<std::uint8_t> edge_output =
        read_rgba8(device, *edge_destination.texture);
    bool smoothed_boundary = false;
    for (std::size_t offset = 0U; offset < edge_output.size(); offset += 4U) {
        require(edge_output[offset + 3U] == 255U,
                "FXAA edge output writes opaque alpha");
        smoothed_boundary = smoothed_boundary ||
                            (edge_output[offset] > 32U &&
                             edge_output[offset] < 220U);
    }
    require(smoothed_boundary,
            "FXAA edge fixture produces a bounded smoothed boundary");
    require(read_rgba8(device, *edge_source.texture) == edge_before,
            "FXAA edge pass preserves its source texture");
    require(device.apply_fxaa(*edge_source.texture, *edge_destination.texture)
                    .ok(),
            "backend repeats the FXAA pass");
    require(read_rgba8(device, *edge_destination.texture) == edge_output,
            "repeated FXAA output is deterministic");

    // FXAA consumes the RGBA8 result of the HDR tone-map stage. This keeps
    // the recovered ordering explicit while the direct fixture above covers
    // the LDR-compatible API contract.
    TextureDescription hdr_description;
    hdr_description.width = 16U;
    hdr_description.height = 8U;
    hdr_description.format = TextureFormat::rgba16_sfloat;
    hdr_description.usage = TextureUsage::sampled |
                            TextureUsage::color_attachment |
                            TextureUsage::transfer_source;
    hdr_description.mutability = TextureMutability::mutable_data;
    hdr_description.access_policy = TextureAccessPolicy::render_then_sample;
    std::vector<std::uint16_t> hdr_pixels(16U * 8U * 4U, 0x3c00U);
    TextureUploadPlan hdr_upload;
    hdr_upload.subresources.push_back(
        {0U, 0U, 16U, 8U, 16U * 4U * sizeof(std::uint16_t),
         std::as_bytes(std::span<const std::uint16_t>(hdr_pixels))});
    TextureResult hdr_source =
        device.create_texture(hdr_description, hdr_upload);
    require(hdr_source.ok(), "backend creates the HDR FXAA source fixture");

    TextureDescription tone_map_description = destination_description();
    tone_map_description.usage = TextureUsage::sampled |
                                  TextureUsage::color_attachment |
                                  TextureUsage::transfer_source;
    tone_map_description.access_policy = TextureAccessPolicy::render_then_sample;
    TextureResult tone_map_target =
        device.create_texture(tone_map_description);
    require(tone_map_target.ok(), "backend creates the HDR FXAA intermediate");
    HdrToneMapParameters tone_map_parameters;
    tone_map_parameters.dither_scale = 0.0F;
    const HdrToneMapResult tone_map = device.tone_map_hdr_texture(
        *hdr_source.texture, *tone_map_target.texture, tone_map_parameters);
    require(tone_map.ok(), "backend tone maps before applying FXAA");
    TextureResult hdr_destination =
        device.create_texture(destination_description());
    require(hdr_destination.ok(), "backend creates the HDR FXAA output");
    const FxaaResult hdr_fxaa = device.apply_fxaa(
        *tone_map_target.texture, *hdr_destination.texture);
    require(hdr_fxaa.ok(), "backend applies FXAA after HDR tone mapping");
    const std::vector<std::uint8_t> hdr_output =
        read_rgba8(device, *hdr_destination.texture);
    require(!hdr_output.empty(), "HDR FXAA output is readable");
    for (std::size_t offset = 3U; offset < hdr_output.size(); offset += 4U)
        require(hdr_output[offset] == 255U,
                "HDR FXAA output writes opaque alpha");
    return true;
}

}  // namespace

int main() {
    try {
        accepts_valid_request();
        rejects_malformed_requests();
        bool executed = executes_backend(Backend::Vulkan);
#if defined(_WIN32)
        executed = executes_backend(Backend::D3D12) || executed;
#endif
        if (!executed)
            return 77;
        std::cout << "FXAA contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FXAA contract tests failed: " << error.what() << '\n';
        return 1;
    }
}
