#include "apex/render/device.hpp"
#include "apex/core/sha256.hpp"

#include "../src/render/generated/portable_sky_spirv.hpp"
#include "../src/render/generated/d3d12_portable_sky_dxbc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class TestTexture final : public Texture {
public:
    TestTexture(const TextureDescription description, const Backend backend)
        : backend_(backend), info_({description}) {}

    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }

private:
    Backend backend_ = Backend::Vulkan;
    TextureInfo info_{};
};

CameraFrame camera(const Backend backend) {
    CameraFrame frame;
    frame.forward = {0.0F, 0.0F, -1.0F};
    frame.up = {0.0F, 1.0F, 0.0F};
    frame.fov_radians = 1.0471975512F;
    frame.aspect = 1.0F;
    frame.clip_space = backend == Backend::Vulkan ? CameraClipSpace::vulkan
                                                   : CameraClipSpace::d3d12;
    return frame;
}

PortableSkyParameters sky(const Backend backend) {
    PortableSkyParameters parameters;
    parameters.camera = camera(backend);
    parameters.horizon_color = {0.08F, 0.12F, 0.20F};
    parameters.sky_color = {0.52F, 0.66F, 0.90F};
    parameters.sun_color = {0.80F, 0.72F, 0.35F};
    parameters.sun_direction = {0.0F, 0.0F, -1.0F};
    return parameters;
}

TextureDescription ldr_target() {
    TextureDescription description;
    // Odd dimensions place one pixel center on NDC (0, 0). The production
    // sun lobe is narrower than one pixel around the center of an even target.
    description.width = 33U;
    description.height = 33U;
    description.format = TextureFormat::rgba8_unorm;
    description.usage = TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    description.mutability = TextureMutability::mutable_data;
    return description;
}

TextureDescription hdr_target(const std::uint32_t samples = 1U) {
    TextureDescription description;
    description.width = 32U;
    description.height = 32U;
    description.format = TextureFormat::rgba16_sfloat;
    description.usage = TextureUsage::color_attachment |
                        TextureUsage::transfer_source;
    if (samples == 1U)
        description.usage = description.usage | TextureUsage::sampled;
    description.mutability = TextureMutability::mutable_data;
    description.samples = samples;
    description.access_policy = samples == 1U
                                    ? TextureAccessPolicy::render_then_sample
                                    : TextureAccessPolicy::fixed_usage;
    description.mip_levels = samples == 1U ? 6U : 1U;
    return description;
}

IndexedStaticMeshBatchStatus validate_batch(
    const TextureDescription& target_description, const Backend backend,
    IndexedStaticMeshBatchDescription description, Diagnostic* output = nullptr) {
    TestTexture target(target_description, backend);
    Diagnostic diagnostic;
    const IndexedStaticMeshBatchStatus status =
        validate_indexed_static_mesh_batch_description(target, description,
                                                       diagnostic);
    if (output != nullptr)
        *output = std::move(diagnostic);
    return status;
}

void constants_pack_and_normalize() {
    PortableSkyParameters parameters;
    parameters.camera = camera(Backend::Vulkan);
    parameters.camera.forward = {0.0F, 0.0F, -2.0F};
    parameters.camera.up = {0.0F, 3.0F, 0.0F};
    parameters.camera.aspect = 2.0F;
    parameters.horizon_color = {1.0F, 2.0F, 3.0F};
    parameters.sky_color = {4.0F, 5.0F, 6.0F};
    parameters.sun_color = {7.0F, 8.0F, 9.0F};
    parameters.sun_direction = {0.0F, 2.0F, 0.0F};

    PortableSkyShaderConstants constants;
    Diagnostic diagnostic;
    require(build_portable_sky_shader_constants(parameters, constants,
                                                diagnostic),
            "portable sky constants accept finite basis");
    require(std::abs(constants.camera_forward_tan_half_fov[2U] + 1.0F) <
                1.0e-5F &&
                std::abs(constants.camera_forward_tan_half_fov[3U] -
                         std::tan(parameters.camera.fov_radians * 0.5F)) <
                    1.0e-5F,
            "portable sky forward and FOV are normalized and packed");
    require(std::abs(constants.camera_right_aspect[0U] - 1.0F) < 1.0e-5F &&
                std::abs(constants.camera_right_aspect[3U] - 2.0F) < 1.0e-5F &&
                std::abs(constants.camera_up[1U] - 1.0F) < 1.0e-5F,
            "portable sky right, up, and aspect are normalized and packed");
    require(constants.horizon_color[0U] == 1.0F &&
                constants.sky_color[2U] == 6.0F &&
                constants.sun_color[1U] == 8.0F &&
                std::abs(constants.sun_direction[1U] - 1.0F) < 1.0e-5F,
            "portable sky lighting and sun are packed");
    require(diagnostic.code.empty() && diagnostic.message.empty(),
            "portable sky constants clear diagnostics on success");
}

void constants_reject_malformed_inputs() {
    const auto expect = [](PortableSkyParameters parameters,
                           const std::string_view expected_code) {
        PortableSkyShaderConstants constants;
        Diagnostic diagnostic;
        require(!build_portable_sky_shader_constants(parameters, constants,
                                                     diagnostic),
                "malformed portable sky constants are rejected");
        require(diagnostic.code == expected_code,
                "portable sky malformed diagnostic is stable");
    };

    PortableSkyParameters parameters = sky(Backend::Vulkan);
    parameters.camera.forward = {std::numeric_limits<float>::quiet_NaN(), 0.0F,
                                 -1.0F};
    expect(parameters, "portable_sky_camera_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.camera.fov_radians = 0.0F;
    expect(parameters, "portable_sky_camera_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.camera.fov_radians = 3.1415926536F;
    expect(parameters, "portable_sky_camera_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.camera.aspect = -1.0F;
    expect(parameters, "portable_sky_camera_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.camera.aspect = std::numeric_limits<float>::max();
    parameters.camera.fov_radians = 2.0F;
    expect(parameters, "portable_sky_camera_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.horizon_color[1U] = -0.01F;
    expect(parameters, "portable_sky_lighting_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.sky_color[0U] = 64001.0F;
    expect(parameters, "portable_sky_lighting_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.sky_color[0U] = 40000.0F;
    parameters.sun_color[0U] = 30000.0F;
    expect(parameters, "portable_sky_lighting_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.sun_direction = {0.0F, 0.0F, 0.0F};
    expect(parameters, "portable_sky_basis_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.camera.up = parameters.camera.forward;
    expect(parameters, "portable_sky_basis_invalid");
    parameters = sky(Backend::Vulkan);
    parameters.sun_direction[2U] = std::numeric_limits<float>::infinity();
    expect(parameters, "portable_sky_lighting_invalid");
}

void shader_source_and_artifact_drift() {
#ifndef APEX_NATIVE_SOURCE_DIR
#error "APEX_NATIVE_SOURCE_DIR must identify the native source tree"
#endif
    const auto read = [](const std::string_view relative) {
        std::ifstream input(std::string(APEX_NATIVE_SOURCE_DIR) + "/" +
                            std::string(relative), std::ios::binary);
        require(input.good(), "portable sky shader source is readable");
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                         std::istreambuf_iterator<char>());
    };
    const auto vertex = read("src/render/shaders/portable_sky.vert");
    const auto fragment = read("src/render/shaders/portable_sky.frag");
    const auto d3d12 = read("src/render/shaders/d3d12_portable_sky.hlsl");
    require(vertex.size() == 490U && fragment.size() == 1072U &&
                d3d12.size() == 1734U &&
                apex::core::sha256Hex(vertex) ==
                    "7e5c009607623bc1b01f57042271cb99c5c600aa5a2bfc9b6cdb8272ff7fb8d9" &&
                apex::core::sha256Hex(fragment) ==
                    "86e414aba978395cb042b9337ca0ab2b8c51f443daae4ddd75cb98bd29204fb2" &&
                apex::core::sha256Hex(d3d12) ==
                    "c0df0dfc3e855fb8fdd5340f64e983e7498e5ea7383abd6da2bc3d4ebd6b2a01",
            "portable sky GLSL and HLSL source identities are stable");

    using namespace apex::render::generated;
    const auto bytes = [](const auto& words) {
        return std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(words.data()),
            words.size() * sizeof(words[0]));
    };
    const auto vertex_spirv = bytes(std::span(portable_sky_vertex_spirv));
    const auto fragment_spirv = bytes(std::span(portable_sky_fragment_spirv));
    require(portable_sky_vertex_spirv_size == 932U &&
                portable_sky_fragment_spirv_size == 2072U &&
                vertex_spirv.size() % sizeof(std::uint32_t) == 0U &&
                fragment_spirv.size() % sizeof(std::uint32_t) == 0U &&
                apex::core::sha256Hex(vertex_spirv) ==
                    "ffc2bc06e4550d25e6806852aea92557a62bd6f18fb67985904ef220a45b2faf" &&
                apex::core::sha256Hex(fragment_spirv) ==
                    "f8e463c5c0a665ec60e8257d31ab63d779bd6451f224c56274fb5779c1507d06" &&
                detect_pipeline_shader_format(vertex_spirv) ==
                    PipelineShaderFormat::spirv &&
                detect_pipeline_shader_format(fragment_spirv) ==
                    PipelineShaderFormat::spirv,
            "portable sky generated SPIR-V artifacts have stable identities");

    const auto dxbc_bytes = [](const auto& artifact) {
        return std::span<const std::uint8_t>(artifact,
                                             std::size(artifact));
    };
    const auto vertex_dxbc = dxbc_bytes(
        apex::render::generated::d3d12_portable_sky_vertex_dxbc);
    const auto pixel_dxbc = dxbc_bytes(
        apex::render::generated::d3d12_portable_sky_pixel_dxbc);
    require(vertex_dxbc.size() == 784U && pixel_dxbc.size() == 2356U &&
                apex::core::sha256Hex(vertex_dxbc) ==
                    "a5b86ffe3e60ca8c16a9d0af0de5b493bb8a6ca688a01b4b6a9b3879fa7d4013" &&
                apex::core::sha256Hex(pixel_dxbc) ==
                    "c95bfecc93d22e2b494dd7528ad318ff85267a1b50ff81ef09f8eb4a602aad9b",
            "portable sky generated DXBC artifacts match HLSL source");
}

void batch_accepts_ldr_hdr_msaa_and_cube(const Backend backend) {
    IndexedStaticMeshBatchDescription batch;
    batch.sky = sky(backend);
    require(validate_batch(ldr_target(), backend, batch) ==
                IndexedStaticMeshBatchStatus::ready,
            "empty sky-only batch accepts LDR target");

    batch.capture_rgba8 = false;
    require(validate_batch(hdr_target(), backend, batch) ==
                IndexedStaticMeshBatchStatus::ready,
            "empty sky-only batch accepts full-chain HDR target");

    TextureDescription msaa = hdr_target(4U);
    TextureDescription resolve = hdr_target();
    batch.resolve_target = nullptr;
    require(validate_batch(msaa, backend, batch) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "four-sample retained HDR batch requires resolve target");
    TestTexture resolve_texture(resolve, backend);
    batch.resolve_target = &resolve_texture;
    require(validate_batch(msaa, backend, batch) ==
                IndexedStaticMeshBatchStatus::ready,
            "four-sample retained HDR batch accepts full-chain resolve");

    TextureDescription cube = ldr_target();
    cube.shape = TextureShape::texture_cube;
    cube.array_layers = 2U;
    batch = {};
    batch.sky = sky(backend);
    batch.target_subresource.array_layer = 1U;
    batch.target_subresource.cube_face = CubeFace::negative_z;
    require(validate_batch(cube, backend, batch) ==
                IndexedStaticMeshBatchStatus::ready,
            "empty sky-only batch accepts cube face subresource");
}

void batch_rejects_malformed_requests(const Backend backend) {
    IndexedStaticMeshBatchDescription batch;
    batch.sky = sky(backend);
    batch.sky->camera.clip_space =
        backend == Backend::Vulkan ? CameraClipSpace::d3d12
                                   : CameraClipSpace::vulkan;
    Diagnostic diagnostic;
    require(validate_batch(ldr_target(), backend, batch, &diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "portable_sky_clip_space_mismatch",
            "portable sky rejects backend clip-space mismatch");

    batch.sky = sky(backend);
    batch.target_subresource.cube_face = CubeFace::positive_x;
    require(validate_batch(ldr_target(), backend, batch) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "2D portable sky target rejects cube face");

    TextureDescription cube = ldr_target();
    cube.shape = TextureShape::texture_cube;
    batch.target_subresource = {};
    require(validate_batch(cube, backend, batch) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "cube portable sky target requires face");
    batch.target_subresource.cube_face = CubeFace::positive_x;
    batch.target_subresource.array_layer = cube.array_layers;
    require(validate_batch(cube, backend, batch) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "cube portable sky target bounds layer");

    TextureDescription excessive = hdr_target();
    excessive.mip_levels = 7U;
    batch.target_subresource = {};
    batch.capture_rgba8 = false;
    require(validate_batch(excessive, backend, batch) ==
                IndexedStaticMeshBatchStatus::invalid_request,
            "HDR target rejects excessive mip chain");
}

std::array<std::uint8_t, 4U> pixel(const std::vector<std::byte>& rgba,
                                   const std::uint32_t width,
                                   const std::uint32_t x,
                                   const std::uint32_t y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    require(offset + 4U <= rgba.size(), "portable sky readback pixel bounds");
    return {std::to_integer<std::uint8_t>(rgba[offset]),
            std::to_integer<std::uint8_t>(rgba[offset + 1U]),
            std::to_integer<std::uint8_t>(rgba[offset + 2U]),
            std::to_integer<std::uint8_t>(rgba[offset + 3U])};
}

bool exercise_backend(const Backend backend, const DeviceOptions& options) {
    DeviceResult created = create_device(backend, options);
    if (!created.ok()) {
        std::cout << "SKIP " << backend_name(backend) << ": "
                  << created.diagnostic.code << ": "
                  << created.diagnostic.message << '\n';
        return false;
    }

    TextureDescription description = ldr_target();
    TextureResult target = created.device->create_texture(description);
    require(target.ok(), "backend creates portable sky LDR target");

    IndexedStaticMeshBatchDescription batch;
    batch.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    batch.sky = sky(backend);
    const IndexedStaticMeshBatchResult first =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *target.texture, batch);
    require(first.ok() && first.rgba8.size() ==
                              static_cast<std::size_t>(description.width) *
                                  description.height * 4U,
            "backend executes empty portable sky-only batch");

    const auto zenith = pixel(first.rgba8, description.width, 16U, 1U);
    const auto horizon = pixel(first.rgba8, description.width, 16U, 30U);
    const auto sun_lobe = pixel(first.rgba8, description.width, 16U, 16U);
    const auto corner = pixel(first.rgba8, description.width, 1U, 1U);
    require(zenith[2U] > horizon[2U] + 25U,
            "portable sky zenith is brighter than horizon");
    require(sun_lobe[0U] > corner[0U] + 25U,
            "portable sky forward sun lobe is localized");
    require(zenith[3U] == 255U && horizon[3U] == 255U &&
                sun_lobe[3U] == 255U,
            "portable sky writes opaque pixels");

    batch.load_color = true;
    batch.sky.reset();
    const IndexedStaticMeshBatchResult retained =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *target.texture, batch);
    require(retained.ok() && retained.rgba8 == first.rgba8,
            "portable sky target retains exact source pixels on load");

    const IndexedStaticMeshBatchResult repeat =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *target.texture, [&] {
                IndexedStaticMeshBatchDescription repeated;
                repeated.sky = sky(backend);
                return repeated;
            }());
    require(repeat.ok() && repeat.rgba8 == first.rgba8,
            "portable sky repeat is deterministic");

    TextureDescription hdr_description = hdr_target();
    TextureResult hdr = created.device->create_texture(hdr_description);
    require(hdr.ok(), "backend creates portable sky HDR target");
    IndexedStaticMeshBatchDescription hdr_batch;
    hdr_batch.capture_rgba8 = false;
    hdr_batch.sky = sky(backend);
    const IndexedStaticMeshBatchResult hdr_result =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *hdr.texture, hdr_batch);
    require(hdr_result.ok() && hdr_result.rgba8.empty(),
            "backend executes empty portable sky HDR batch without capture");

    TextureDescription msaa_description = ldr_target();
    msaa_description.samples = 4U;
    TextureResult msaa = created.device->create_texture(msaa_description);
    require(msaa.ok(), "backend creates portable sky four-sample target");
    TextureDescription resolve_description = ldr_target();
    resolve_description.usage = TextureUsage::sampled |
                                TextureUsage::color_attachment |
                                TextureUsage::transfer_source;
    resolve_description.access_policy = TextureAccessPolicy::render_then_sample;
    TextureResult resolve = created.device->create_texture(resolve_description);
    require(resolve.ok(), "backend creates portable sky resolve target");
    IndexedStaticMeshBatchDescription msaa_batch;
    msaa_batch.capture_rgba8 = false;
    msaa_batch.resolve_target = resolve.texture.get();
    msaa_batch.sky = sky(backend);
    const IndexedStaticMeshBatchResult msaa_result =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *msaa.texture, msaa_batch);
    require(msaa_result.ok() && msaa_result.rgba8.empty(),
            "backend executes portable sky four-sample resolve");
    IndexedStaticMeshBatchDescription resolve_readback;
    resolve_readback.load_color = true;
    const IndexedStaticMeshBatchResult resolved_pixels =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *resolve.texture, resolve_readback);
    require(resolved_pixels.ok() && resolved_pixels.rgba8.size() ==
                                      first.rgba8.size(),
            "portable sky four-sample resolve is readable");
    const auto resolved_zenith = pixel(
        resolved_pixels.rgba8, resolve_description.width, 16U, 1U);
    const auto resolved_horizon = pixel(
        resolved_pixels.rgba8, resolve_description.width, 16U, 31U);
    require(resolved_zenith[2U] > resolved_horizon[2U] + 25U,
            "portable sky four-sample resolve retains the sky gradient");

    TextureDescription cube_description = ldr_target();
    cube_description.shape = TextureShape::texture_cube;
    TextureResult cube = created.device->create_texture(cube_description);
    require(cube.ok(), "backend creates portable sky cube target");
    IndexedStaticMeshBatchDescription cube_batch;
    cube_batch.sky = sky(backend);
    cube_batch.target_subresource.cube_face = CubeFace::negative_z;
    const IndexedStaticMeshBatchResult cube_result =
        created.device->draw_indexed_static_mesh_batch_and_readback(
            *cube.texture, cube_batch);
    require(cube_result.ok() && cube_result.rgba8.size() == first.rgba8.size(),
            "backend executes portable sky on a cube face");
    const auto cube_zenith = pixel(
        cube_result.rgba8, cube_description.width, 16U, 1U);
    const auto cube_horizon = pixel(
        cube_result.rgba8, cube_description.width, 16U, 31U);
    require(cube_zenith[2U] > cube_horizon[2U] + 25U,
            "portable sky cube-face readback retains the sky gradient");
    return true;
}

} // namespace

int main() {
    try {
        constants_pack_and_normalize();
        constants_reject_malformed_inputs();
        shader_source_and_artifact_drift();
        for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
            batch_accepts_ldr_hdr_msaa_and_cube(backend);
            batch_rejects_malformed_requests(backend);
        }

        bool exercised = false;
        DeviceOptions vulkan_options;
        vulkan_options.headless = true;
        vulkan_options.enable_validation = true;
        vulkan_options.allow_software = true;
        vulkan_options.prefer_software = true;
        exercised = exercise_backend(Backend::Vulkan, vulkan_options);
#if defined(_WIN32)
        DeviceOptions d3d12_options;
        d3d12_options.headless = true;
        d3d12_options.enable_validation = true;
        d3d12_options.allow_software = true;
        d3d12_options.prefer_software = true;
        exercised = exercise_backend(Backend::D3D12, d3d12_options) || exercised;
#endif
        if (!exercised)
            return 77;
        std::cout << "portable sky contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portable sky contract tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
